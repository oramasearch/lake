// SPDX-License-Identifier: MIT

// Managed keys and using your own identity (lake#762,
// docs/api/identity.md#your-own-identity).
#include "audit_test_support.hpp"
#include "identity_proof_test_support.hpp"
#include "personal_customer_test_support.hpp"
#include "postgres_fixture.hpp"
#include "repository_test_support.hpp"
#include "temporary_directory.hpp"

#include <ctime>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include "identity/taap.hpp"
#include <liquid/liquid.hpp>

#include "identity/taap.hpp"

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/server.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/own_identity.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/postgres_store.hpp"

namespace {

using lake::api::ApiContext;
using lake::api::ApiResult;
using lake::api::Registry;
using nlohmann::json;

constexpr std::string_view instance_kek =
    "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

// LIQUID_HOME is process-wide: each signer is opened under its own home, and the previous value
// comes back when the scope ends.
class LiquidHome {
public:
    explicit LiquidHome(const std::filesystem::path& path) {
        if (const char* value = std::getenv("LIQUID_HOME")) {
            previous_ = value;
        }
        setenv("LIQUID_HOME", path.c_str(), 1);
    }
    ~LiquidHome() {
        if (previous_) {
            setenv("LIQUID_HOME", previous_->c_str(), 1);
        } else {
            unsetenv("LIQUID_HOME");
        }
    }

private:
    std::optional<std::string> previous_;
};

struct Person {
    liquid::signer signer;
    std::string document; // what `liquid identity show --document` prints
};

std::string base64url(std::span<const std::byte> bytes) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string output;
    unsigned buffer = 0;
    int bits = 0;
    for (const std::byte value : bytes) {
        buffer = (buffer << 8U) | std::to_integer<unsigned>(value);
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            output.push_back(alphabet[(buffer >> bits) & 0x3fU]);
        }
    }
    if (bits > 0) {
        output.push_back(alphabet[(buffer << (6 - bits)) & 0x3fU]);
    }
    return output;
}

Person person_at(const std::filesystem::path& home) {
    LiquidHome scope(home);
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    EXPECT_TRUE(signer) << (signer ? "" : signer.error().message);
    auto document = signer->document();
    EXPECT_TRUE(document);
    auto encoded = liquid::encode_identity_document(*document);
    EXPECT_TRUE(encoded);
    return Person{std::move(*signer), base64url(*encoded)};
}

// What `liquid sign artifact <challenge>` prints: the canonical signature object as hex, and the
// signer (src/cli/sign.cpp in liquid).
std::string liquid_sign_artifact(liquid::signer& signer, const std::string& challenge) {
    const auto cbor = json::to_cbor(json(challenge));
    std::vector<std::byte> body(cbor.size());
    std::ranges::transform(cbor, body.begin(), [](std::uint8_t b) { return std::byte{b}; });
    auto signed_value = liquid::runtime::block_on(signer.sign("artifact", std::move(body)));
    EXPECT_TRUE(signed_value);
    auto encoded = liquid::encode_attestation_signature(*signed_value);
    EXPECT_TRUE(encoded);
    std::string hex;
    for (const std::byte value : *encoded) {
        static constexpr std::string_view digits = "0123456789abcdef";
        hex.push_back(digits[std::to_integer<unsigned>(value) >> 4U]);
        hex.push_back(digits[std::to_integer<unsigned>(value) & 0x0fU]);
    }
    return "signature=" + hex + "\nsigner=" + signer.identity().taap_id + "\n";
}

class Harness {
public:
    explicit Harness(lake::control::Store& store) : store_(store) {
        lake::api::register_core_capabilities(registry_, "test");
    }

    ApiResult call(const std::string& name, json input, ApiContext& context) {
        context.store = &store_;
        context.instance_kek = std::string(instance_kek);
        context.request_id = lake::control::new_uuid_v4();
        auto completed = liquid::runtime::block_on(registry_.call_async(name, input, context));
        return completed ? std::move(*completed)
                         : ApiResult::failure("store_error", completed.error().message);
    }

    // Signs up without an identity document and answers the session's context.
    ApiContext sign_up(const std::string& email) {
        ApiContext anonymous;
        const ApiResult created = call("sign_up_password",
                                       {{"email", email},
                                        {"password", "correct horse battery staple"},
                                        {"device_name", "laptop"}},
                                       anonymous);
        EXPECT_TRUE(created.ok) << created.error.message;
        return signed_in(created.value.value("secret", ""));
    }

    ApiContext signed_in(const std::string& secret) {
        ApiContext context;
        const auto principal = store_.context_for(secret);
        EXPECT_TRUE(principal) << (principal ? "" : principal.error().message);
        if (!principal) {
            return context;
        }
        context.authorized = true;
        context.session_credential = true;
        context.acting_user_id = principal->user_id;
        context.acting_identity = principal->taap_id;
        context.credential_kind = principal->kind;
        context.credential_session_id = principal->session_id;
        context.pool_id = principal->pool_id;
        context.scopes = principal->scopes;
        context.authentication_method = "session";
        return context;
    }

private:
    lake::control::Store& store_;
    Registry registry_;
};

const lake::audit::Event* find_event(const std::vector<lake::audit::Event>& events,
                                     const std::string& action,
                                     const std::string& outcome) {
    const auto found = std::ranges::find_if(events, [&](const lake::audit::Event& event) {
        return event.draft.action == action && event.draft.outcome == outcome;
    });
    return found == events.end() ? nullptr : &*found;
}

// The whole contract, against either store. Returns the account's user id.
std::string expect_use_own_identity(lake::control::Store& store, const std::string& suffix) {
    Harness harness(store);
    const auto home = lake::test::fixture_path("own-identity-" + suffix);
    Person own = person_at(home / "own");
    Person stranger = person_at(home / "stranger");

    // Sign-up without a document: a managed key Lake holds.
    ApiContext person = harness.sign_up("own-" + suffix + "@example.test");
    const std::string account = person.acting_identity;
    EXPECT_TRUE(account.starts_with("taap:1:ed25519:"));
    const ApiResult before = harness.call("account_identity", json::object(), person);
    EXPECT_TRUE(before.ok) << before.error.message;
    EXPECT_EQ(before.value.at("key"), "managed");
    EXPECT_EQ(before.value.at("taap_id"), account);
    EXPECT_TRUE(before.value.at("managed_key").at("held").get<bool>());
    EXPECT_TRUE(store.managed_key(person.acting_user_id));

    // A repository and the ownership grant, which must stay with the account.
    const auto customer = lake::test::personal_customer(store, person.acting_user_id);
    EXPECT_TRUE(customer) << (customer ? "" : customer.error().message);
    const auto repository =
        store.create_repository({customer->id,
                                 "kept",
                                 "",
                                 "private",
                                 lake::test::repository_workspace(store, customer->id)});
    EXPECT_TRUE(repository) << (repository ? "" : repository.error().message);
    const auto customer_object = store.find_policy_object("customer", customer->id);
    EXPECT_TRUE(customer_object);

    // Begin: the document is verified and a challenge issued for this account.
    const ApiResult begun =
        harness.call("begin_own_identity", {{"identity_document", own.document}}, person);
    EXPECT_TRUE(begun.ok) << begun.error.message;
    const std::string challenge = begun.value.value("challenge", "");
    EXPECT_TRUE(challenge.starts_with("sha256:"));
    EXPECT_EQ(begun.value.at("command"), "liquid sign artifact " + challenge);
    EXPECT_EQ(begun.value.at("own_taap_id"), own.signer.identity().taap_id);

    // Not a successor: a signature by another identity, and a signature over another challenge,
    // are refused, and each refusal spends the challenge.
    const ApiResult wrong_signer = harness.call(
        "use_own_identity",
        {{"challenge", challenge}, {"signature", liquid_sign_artifact(stranger.signer, challenge)}},
        person);
    EXPECT_FALSE(wrong_signer.ok);
    EXPECT_EQ(wrong_signer.error.code, "unauthorized");
    const ApiResult replayed = harness.call(
        "use_own_identity",
        {{"challenge", challenge}, {"signature", liquid_sign_artifact(own.signer, challenge)}},
        person);
    EXPECT_FALSE(replayed.ok) << "a challenge is used once";

    const ApiResult second =
        harness.call("begin_own_identity", {{"identity_document", own.document}}, person);
    EXPECT_TRUE(second.ok) << second.error.message;
    const std::string second_challenge = second.value.value("challenge", "");
    std::string other_challenge_signature = liquid_sign_artifact(own.signer, challenge);
    // Without the signer line the hex alone is accepted, and still must match this challenge.
    other_challenge_signature =
        other_challenge_signature.substr(10, other_challenge_signature.find('\n') - 10);
    const ApiResult wrong_challenge =
        harness.call("use_own_identity",
                     {{"challenge", second_challenge}, {"signature", other_challenge_signature}},
                     person);
    EXPECT_FALSE(wrong_challenge.ok);
    EXPECT_EQ(wrong_challenge.error.code, "unauthorized");
    EXPECT_TRUE(store.managed_key(person.acting_user_id)) << "nothing changed on a refusal";

    // An API token cannot do it, only the signed-in person.
    {
        ApiContext token = person;
        token.credential_kind = "api";
        const ApiResult refused =
            harness.call("begin_own_identity", {{"identity_document", own.document}}, token);
        EXPECT_FALSE(refused.ok);
        EXPECT_EQ(refused.error.code, "unauthorized");
    }

    // The real thing.
    const ApiResult third =
        harness.call("begin_own_identity", {{"identity_document", own.document}}, person);
    EXPECT_TRUE(third.ok) << third.error.message;
    const std::string third_challenge = third.value.value("challenge", "");
    const ApiResult used =
        harness.call("use_own_identity",
                     {{"challenge", third_challenge},
                      {"signature", liquid_sign_artifact(own.signer, third_challenge)}},
                     person);
    EXPECT_TRUE(used.ok) << used.error.message;
    EXPECT_EQ(used.value.at("key"), "own");
    EXPECT_EQ(used.value.at("taap_id"), account) << "the account keeps its identifier";
    EXPECT_EQ(used.value.at("signs_as"), own.signer.identity().taap_id);
    EXPECT_FALSE(used.value.at("managed_key").at("held").get<bool>());
    EXPECT_FALSE(used.value.at("managed_key").at("destroyed_at").is_null());

    // The managed seed is gone; the link verifies with both keys.
    EXPECT_FALSE(store.managed_key(person.acting_user_id));
    const auto identity = store.account_identity(person.acting_user_id);
    EXPECT_TRUE(identity);
    EXPECT_TRUE(lake::control::own_identity::verify_link(
        identity->link_document, identity->managed_public_key, own.document))
        << identity->link_document.dump();
    json tampered = identity->link_document;
    tampered["body"]["next"] = stranger.signer.identity().taap_id;
    EXPECT_FALSE(lake::control::own_identity::verify_link(
        tampered, identity->managed_public_key, own.document));

    // Grants, repositories and the account's identifier stay; the own identity acts for it.
    ApiContext anonymous;
    const ApiResult signed_in_again = harness.call("sign_in_password",
                                                   {{"email", "own-" + suffix + "@example.test"},
                                                    {"password", "correct horse battery staple"}},
                                                   anonymous);
    EXPECT_TRUE(signed_in_again.ok) << signed_in_again.error.message;
    const auto session_again = store.context_for(signed_in_again.value.value("secret", ""));
    EXPECT_TRUE(session_again);
    EXPECT_EQ(session_again->taap_id, account);
    const auto grants = store.list_grants({customer_object->id}, account, 10);
    EXPECT_TRUE(grants);
    EXPECT_EQ(grants->size(), 1U);
    lake::control::AuthorizationCache cache;
    const auto allowed =
        lake::control::authorize(store, account, customer_object->id, "write", cache);
    EXPECT_TRUE(allowed && allowed->allowed);
    const auto repositories = store.list_repositories(customer->id, 10);
    EXPECT_TRUE(repositories);
    EXPECT_TRUE(std::ranges::any_of(*repositories,
                                    [&](const auto& value) { return value.id == repository->id; }));
    const auto acts_for = store.account_for_identity(own.signer.identity().taap_id);
    EXPECT_TRUE(acts_for);
    EXPECT_EQ(*acts_for, account);
    // A change signed with either key reads as the account's name and handle (lake#1326); a key
    // no account acts for is absent, and never an email.
    {
        const auto user = store.get_user(person.acting_user_id);
        EXPECT_TRUE(user);
        const auto names = store.signer_names(
            {account, own.signer.identity().taap_id, stranger.signer.identity().taap_id, ""});
        EXPECT_TRUE(names) << (names ? "" : names.error().message);
        if (names && user && customer) {
            EXPECT_EQ(names->size(), 2U);
            for (const std::string& key : {account, own.signer.identity().taap_id}) {
                EXPECT_TRUE(names->contains(key)) << key;
                if (!names->contains(key)) {
                    continue;
                }
                EXPECT_EQ(names->at(key).kind, "user");
                EXPECT_EQ(names->at(key).display_name, user->display_name);
                EXPECT_EQ(names->at(key).handle, customer->name);
                EXPECT_NE(names->at(key).display_name, user->email);
            }
            EXPECT_FALSE(names->contains(stranger.signer.identity().taap_id));
        }
    }

    // Signing in with the own key reaches the same account (sign_in_taap).
    {
        const std::int64_t issued_at = std::chrono::duration_cast<std::chrono::seconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count();
        const std::string nonce = "own-identity-nonce-" + suffix;
        const std::string body = "lake\n" + nonce + "\n" + std::to_string(issued_at);
        std::vector<std::byte> bytes(body.size());
        std::ranges::transform(body, bytes.begin(), [](char c) { return std::byte(c); });
        auto signature =
            liquid::runtime::block_on(own.signer.sign("lake_sign_in", std::move(bytes)));
        EXPECT_TRUE(signature);
        ApiContext visitor;
        const ApiResult taap =
            harness.call("sign_in_taap",
                         {{"identity_document", own.document},
                          {"nonce", nonce},
                          {"issued_at", issued_at},
                          {"signature",
                           {{"algorithm", signature->algorithm},
                            {"signing_key_identifier", signature->signing_key_identifier},
                            {"signing_key_label", signature->signing_key_label},
                            {"signature_purpose", signature->signature_purpose},
                            {"signed_at", signature->signed_at},
                            {"bytes", base64url(signature->bytes)}}}},
                         visitor);
        EXPECT_TRUE(taap.ok) << taap.error.message;
        const auto taap_session = store.context_for(taap.value.value("secret", ""));
        EXPECT_TRUE(taap_session);
        EXPECT_EQ(taap_session->user_id, person.acting_user_id);
        EXPECT_EQ(taap_session->taap_id, account);
    }

    // Again: refused, the account already uses its own identity.
    const ApiResult again =
        harness.call("begin_own_identity", {{"identity_document", own.document}}, person);
    EXPECT_FALSE(again.ok);
    EXPECT_EQ(again.error.code, "conflict");

    // Another account cannot take the same identity.
    ApiContext other = harness.sign_up("other-" + suffix + "@example.test");
    const ApiResult taken =
        harness.call("begin_own_identity", {{"identity_document", own.document}}, other);
    EXPECT_FALSE(taken.ok);
    EXPECT_EQ(taken.error.code, "conflict");

    // Audit: each step is recorded with the identities it moved between.
    const auto events = lake::test::audit_events(store, {.limit = 500});
    EXPECT_TRUE(events);
    const auto* begin_event = find_event(*events, "begin_own_identity", "success");
    const auto* use_event = find_event(*events, "use_own_identity", "success");
    EXPECT_NE(begin_event, nullptr);
    EXPECT_NE(use_event, nullptr);
    if (use_event != nullptr) {
        EXPECT_EQ(use_event->draft.principal, account);
        EXPECT_EQ(use_event->draft.details.at("previous"), account);
        EXPECT_EQ(use_event->draft.details.at("next"), own.signer.identity().taap_id);
        EXPECT_EQ(use_event->draft.details.at("managed_seed_destroyed"), true);
    }
    EXPECT_NE(find_event(*events, "use_own_identity", "denied"), nullptr);
    return person.acting_user_id;
}

} // namespace

TEST(OwnIdentity, SignUpWithoutADocumentGetsAManagedKey) {
    lake::control::InMemoryStore store;
    Harness harness(store);
    ApiContext person = harness.sign_up("managed@example.test");
    const auto identity = store.account_identity(person.acting_user_id);
    ASSERT_TRUE(identity) << identity.error().message;
    EXPECT_EQ(identity->key, "managed");
    EXPECT_EQ(identity->taap_id, person.acting_identity);
    const auto key = store.managed_key(person.acting_user_id);
    ASSERT_TRUE(key);
    EXPECT_EQ(key->taap_id, person.acting_identity);
    // The managed key is the instance key's: it can sign, and the signature verifies.
    const json body = {{"probe", 1}};
    const auto signature = lake::audit::sign_with_managed_key(
        key->wrapped_seed,
        lake::audit::effective_instance_kek(store, std::string(instance_kek)),
        "probe",
        body);
    ASSERT_TRUE(signature) << signature.error().message;
    EXPECT_TRUE(lake::audit::verify_managed_signature(key->public_key, "probe", body, *signature));
    EXPECT_TRUE(lake::test::personal_customer(store, person.acting_user_id));
}

TEST(OwnIdentity, MemoryStoreRotatesToTheOwnIdentity) {
    lake::control::InMemoryStore store;
    expect_use_own_identity(store, lake::control::new_uuid_v4().substr(0, 8));
}

TEST(OwnIdentity, PostgresStoreRotatesToTheOwnIdentity) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    auto store = lake::control::make_postgres_store(uri, std::string(instance_kek));
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const std::string user_id = expect_use_own_identity(*store, suffix);

    pqxx::connection connection(uri);
    pqxx::read_transaction tx(connection);
    const auto managed = tx.exec_params(
        "SELECT wrapped_seed IS NULL, seed_destroyed_at IS NOT NULL, public_key <> '' "
        "FROM custodial_identities WHERE user_id = $1::uuid",
        user_id);
    ASSERT_EQ(managed.size(), 1U);
    EXPECT_TRUE(managed[0][0].as<bool>()) << "the managed seed is destroyed";
    EXPECT_TRUE(managed[0][1].as<bool>());
    EXPECT_TRUE(managed[0][2].as<bool>()) << "its public half stays to verify the link";
    const auto links =
        tx.exec_params("SELECT count(*) FROM identity_links WHERE user_id = $1::uuid", user_id);
    EXPECT_EQ(links[0][0].as<int>(), 1);
    const auto documents =
        tx.exec_params("SELECT count(*) FROM identity_documents WHERE user_id = $1::uuid", user_id);
    EXPECT_EQ(documents[0][0].as<int>(), 0) << "a managed sign-up registers no document";
}

TEST(OwnIdentity, PostgresSignUpWithoutADocumentHoldsAManagedKey) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    auto store = lake::control::make_postgres_store(uri, std::string(instance_kek));
    Harness harness(*store);
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    ApiContext person = harness.sign_up("managed-" + suffix + "@example.test");
    const auto identity = store->account_identity(person.acting_user_id);
    ASSERT_TRUE(identity) << identity.error().message;
    EXPECT_EQ(identity->key, "managed");
    EXPECT_TRUE(identity->managed_seed_held);
    EXPECT_TRUE(lake::test::personal_customer(*store, person.acting_user_id));
    pqxx::connection connection(uri);
    pqxx::read_transaction tx(connection);
    const auto rows = tx.exec_params(
        "SELECT c.taap_id = u.taap_id, c.wrapped_seed <> '' FROM custodial_identities c "
        "JOIN users u ON u.id = c.user_id WHERE u.id = $1::uuid",
        person.acting_user_id);
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_TRUE(rows[0][0].as<bool>());
    EXPECT_TRUE(rows[0][1].as<bool>());
    // An application pool still requires the document.
    ApiContext anonymous;
    const ApiResult application = harness.call("sign_up_password",
                                               {{"pool_id", "no-such-pool"},
                                                {"email", "app-" + suffix + "@example.test"},
                                                {"password", "correct horse battery staple"}},
                                               anonymous);
    EXPECT_FALSE(application.ok);
}

TEST(OwnIdentity, AClerkSignInStartsWithAManagedKey) {
    lake::control::InMemoryStore store;
    const auto minted = lake::audit::mint_custodial_identity(instance_kek);
    ASSERT_TRUE(minted);
    const auto issued = store.sign_in_clerk({"user_managed",
                                             "clerk-managed@example.test",
                                             "Clerk Person",
                                             "browser",
                                             minted->taap_id,
                                             minted->public_key,
                                             minted->wrapped_seed,
                                             static_cast<std::int64_t>(std::time(nullptr))});
    ASSERT_TRUE(issued) << issued.error().message;
    const auto principal = store.context_for(issued->secret);
    ASSERT_TRUE(principal);
    const auto identity = store.account_identity(principal->user_id);
    ASSERT_TRUE(identity);
    EXPECT_EQ(identity->key, "managed");
    EXPECT_EQ(identity->taap_id, minted->taap_id);
}

TEST(OwnIdentity, ARotatedSuccessorDocumentIsRefused) {
    const auto home = lake::test::fixture_path("own-identity-rotated");
    LiquidHome scope(home);
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    ASSERT_TRUE(signer) << signer.error().message;
    auto first = signer->document();
    ASSERT_TRUE(first);
    auto first_encoded = liquid::encode_identity_document(*first);
    ASSERT_TRUE(first_encoded);
    EXPECT_TRUE(lake::control::own_identity::verify_document(base64url(*first_encoded)));
    auto successor = liquid::runtime::block_on(signer->rotate());
    ASSERT_TRUE(successor) << successor.error().message;
    auto successor_encoded = liquid::encode_identity_document(*successor);
    ASSERT_TRUE(successor_encoded);
    const auto refused =
        lake::control::own_identity::verify_document(base64url(*successor_encoded));
    EXPECT_FALSE(refused);
    EXPECT_FALSE(lake::control::own_identity::verify_document("not a document"));
}

namespace {

// What a client sends to sign_in_taap: the signed body and the signature as JSON.
nlohmann::json
taap_sign_in_signature(liquid::signer& signer, const std::string& nonce, std::int64_t issued_at) {
    const std::string body = "lake\n" + nonce + "\n" + std::to_string(issued_at);
    std::vector<std::byte> bytes(body.size());
    std::ranges::transform(body, bytes.begin(), [](char c) { return std::byte(c); });
    auto signature = liquid::runtime::block_on(signer.sign("lake_sign_in", std::move(bytes)));
    EXPECT_TRUE(signature);
    return {{"algorithm", signature->algorithm},
            {"signing_key_identifier", signature->signing_key_identifier},
            {"signing_key_label", signature->signing_key_label},
            {"signature_purpose", signature->signature_purpose},
            {"signed_at", signature->signed_at},
            {"bytes", base64url(signature->bytes)}};
}

} // namespace

// liquid#383: a self-signed identity document that carries another person's TAAP identifier with
// the attacker's own key must not sign in as that person. Only an initial document whose
// identifier is derived from its own key proves the name.
TEST(OwnIdentity, ASignInDocumentNamingAnotherIdentityIsRefused) {
    auto victim = person_at(lake::test::fixture_path("taap-sign-in-victim"));
    auto attacker = person_at(lake::test::fixture_path("taap-sign-in-attacker"));
    const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    const std::string nonce = "taap-sign-in-forgery-nonce";

    const auto honest = lake::control::identity_crypto::verify_taap_sign_in(
        victim.document, "lake", nonce, now, taap_sign_in_signature(victim.signer, nonce, now));
    ASSERT_TRUE(honest) << honest.error();
    EXPECT_EQ(honest->taap_id, victim.signer.identity().taap_id);

    auto forged = attacker.signer.document();
    ASSERT_TRUE(forged);
    forged->subject.taap_id = victim.signer.identity().taap_id;
    forged->subject.fingerprint = victim.signer.identity().fingerprint;
    auto body = liquid::identity_detail::canonical_document(*forged, false);
    ASSERT_TRUE(body);
    auto resigned = liquid::runtime::block_on(attacker.signer.sign("identity_document", *body));
    ASSERT_TRUE(resigned);
    forged->signatures = {*resigned};
    auto forged_encoded = liquid::encode_identity_document(*forged);
    ASSERT_TRUE(forged_encoded);
    const auto refused = lake::control::identity_crypto::verify_taap_sign_in(
        base64url(*forged_encoded),
        "lake",
        nonce,
        now,
        taap_sign_in_signature(attacker.signer, nonce, now));
    EXPECT_FALSE(refused) << "signed in as " << refused->taap_id;
    EXPECT_FALSE(lake::control::own_identity::verify_document(base64url(*forged_encoded)));
}

// LSR-198 (lake#1004): the replay key is what the signature authenticates. An extra member in the
// signature object is refused, and the same signed assertion gives the same key however its
// signature object is ordered.
TEST(OwnIdentity, ATaapAssertionHasOneReplayKeyAndNoUnknownSignatureMembers) {
    auto person = person_at(lake::test::fixture_path("taap-sign-in-replay"));
    const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    const std::string nonce = "taap-sign-in-replay-nonce";
    const json signature = taap_sign_in_signature(person.signer, nonce, now);
    const auto first = lake::control::identity_crypto::verify_taap_sign_in(
        person.document, "lake", nonce, now, signature);
    ASSERT_TRUE(first) << first.error();

    json padded = signature;
    padded["padding"] = "a fresh assertion id, once";
    EXPECT_FALSE(lake::control::identity_crypto::verify_taap_sign_in(
        person.document, "lake", nonce, now, padded));

    json reordered = json::object();
    for (auto it = signature.rbegin(); it != signature.rend(); ++it) {
        reordered[it.key()] = it.value();
    }
    const auto again = lake::control::identity_crypto::verify_taap_sign_in(
        person.document, "lake", nonce, now, reordered);
    ASSERT_TRUE(again) << again.error();
    EXPECT_EQ(again->assertion_id, first->assertion_id);
}

// A liquid push signed by the person's own key, linked to their account, is attributed to the
// account in the push audit.
TEST(OwnIdentity, APushFromTheLinkedKeyIsAttributedToTheAccount) {
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto home = lake::test::fixture_path("own-identity-push-" + suffix);
    auto store = std::make_shared<lake::control::InMemoryStore>();
    Harness harness(*store);
    Person own = person_at(home / "own");
    ApiContext person = harness.sign_up("push-" + suffix + "@example.test");
    const std::string account = person.acting_identity;
    const ApiResult begun =
        harness.call("begin_own_identity", {{"identity_document", own.document}}, person);
    ASSERT_TRUE(begun.ok) << begun.error.message;
    const std::string challenge = begun.value.at("challenge");
    const ApiResult used = harness.call(
        "use_own_identity",
        {{"challenge", challenge}, {"signature", liquid_sign_artifact(own.signer, challenge)}},
        person);
    ASSERT_TRUE(used.ok) << used.error.message;

    const auto customer = lake::test::personal_customer(*store, person.acting_user_id);
    ASSERT_TRUE(customer);
    const auto repository =
        store->create_repository({customer->id,
                                  "pushed",
                                  "",
                                  "private",
                                  lake::test::repository_workspace(*store, customer->id)});
    ASSERT_TRUE(repository) << repository.error().message;
    // liquid pushes with a bearer: here, a password sign-in's session.
    ApiContext anonymous;
    const ApiResult token = harness.call("sign_in_password",
                                         {{"email", "push-" + suffix + "@example.test"},
                                          {"password", "correct horse battery staple"}},
                                         anonymous);
    ASSERT_TRUE(token.ok) << token.error.message;

    lake::api::ServerOptions options;
    options.port = 0;
    options.node_name = "own-identity-push";
    options.instance_kek = std::string(instance_kek);
    options.store_root = (home / "lake").string();
    options.store = store;
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    LiquidHome scope(home / "own");
    auto source = liquid::runtime::block_on(liquid::repository::create(home / "source"));
    ASSERT_TRUE(source) << source.error().message;
    const std::string text = "signed by my own key";
    std::vector<std::byte> bytes(text.size());
    std::ranges::transform(text, bytes.begin(), [](char c) { return std::byte(c); });
    ASSERT_TRUE(liquid::runtime::block_on(source->write("own.txt", std::move(bytes))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("own")));
    liquid::sync_endpoint endpoint(
        "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/repositories/" + repository->id,
        liquid::sync_endpoint_options{.bearer_token = token.value.at("secret")});
    const auto pushed = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(pushed) << pushed.error().message;
    server.stop();

    const auto events = lake::test::audit_events(*store, {.limit = 500});
    ASSERT_TRUE(events);
    const auto* change = find_event(*events, "push_change", "success");
    ASSERT_NE(change, nullptr);
    EXPECT_EQ(change->draft.principal, account);
    EXPECT_EQ(change->draft.details.at("author"), own.signer.identity().taap_id);
    EXPECT_EQ(change->draft.details.at("author_account"), account);
    EXPECT_EQ(change->draft.details.at("author_is_caller"), true);
}

namespace {

std::vector<std::byte> base64url_bytes(const std::string& encoded) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::vector<std::byte> output;
    unsigned buffer = 0;
    int bits = 0;
    for (const char c : encoded) {
        const auto index = alphabet.find(c);
        if (index == std::string_view::npos) {
            return {};
        }
        buffer = (buffer << 6U) | static_cast<unsigned>(index);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(std::byte((buffer >> bits) & 0xffU));
        }
    }
    return output;
}

std::string encoded_document(const liquid::identity_document& document) {
    const auto encoded = liquid::encode_identity_document(document);
    EXPECT_TRUE(encoded);
    return encoded ? base64url(*encoded) : std::string{};
}

liquid::identity_document decoded_document(const std::string& encoded) {
    const auto document = liquid::decode_identity_document(base64url_bytes(encoded));
    EXPECT_TRUE(document);
    return document ? *document : liquid::identity_document{};
}

// Replaces a document's signatures with one self-signature by `signer`'s current key.
liquid::identity_document signed_by(liquid::identity_document document, liquid::signer& signer) {
    document.signatures.clear();
    auto body = liquid::identity_detail::canonical_document(document, false);
    EXPECT_TRUE(body);
    auto signature = liquid::runtime::block_on(signer.sign("identity_document", std::move(*body)));
    EXPECT_TRUE(signature) << (signature ? "" : signature.error().message);
    document.signatures.push_back(std::move(*signature));
    return document;
}

// The attack of lake#917: the attacker's own key and document, with the victim's
// identifier written in and self-signed again with the attacker's key.
std::string forged_document(Person& attacker, const std::string& victim_taap_id) {
    liquid::identity_document document = decoded_document(attacker.document);
    document.subject.taap_id = victim_taap_id;
    document.subject.fingerprint = victim_taap_id.substr(victim_taap_id.rfind(':') + 1);
    return encoded_document(signed_by(std::move(document), attacker.signer));
}

json sign_in_taap_input(liquid::signer& signer,
                        const std::string& document,
                        const std::vector<std::string>& chain = {}) {
    const std::int64_t issued_at = std::chrono::duration_cast<std::chrono::seconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();
    const std::string nonce = "taap-binding-" + lake::control::new_uuid_v4();
    const std::string body = "lake\n" + nonce + "\n" + std::to_string(issued_at);
    std::vector<std::byte> bytes(body.size());
    std::ranges::transform(body, bytes.begin(), [](char c) { return std::byte(c); });
    auto signature = liquid::runtime::block_on(signer.sign("lake_sign_in", std::move(bytes)));
    EXPECT_TRUE(signature);
    json input{{"identity_document", document},
               {"nonce", nonce},
               {"issued_at", issued_at},
               {"signature",
                {{"algorithm", signature->algorithm},
                 {"signing_key_identifier", signature->signing_key_identifier},
                 {"signing_key_label", signature->signing_key_label},
                 {"signature_purpose", signature->signature_purpose},
                 {"signed_at", signature->signed_at},
                 {"bytes", base64url(signature->bytes)}}}};
    if (!chain.empty()) {
        input["identity_chain"] = chain;
    }
    return input;
}

// lake#917, against either store: a TAAP sign-in succeeds only with the key bound to the
// registered identity, its own first key or a rotation that key signed. A document naming the
// victim's identifier with the attacker's key is refused, issues no session, and is audited.
void expect_taap_sign_in_binds_the_key(lake::control::Store& store,
                                       const std::string& suffix,
                                       bool refuses_unbound_registration) {
    Harness harness(store);
    const auto home = lake::test::fixture_path("taap-binding-" + suffix);
    Person victim = person_at(home / "victim");
    Person attacker = person_at(home / "attacker");
    const std::string victim_id = victim.signer.identity().taap_id;

    // Password sign-up with a document on PostgreSQL also needs a fresh signature by the
    // document's key (lake#931); the in-memory store asks for none.
    const auto proven = [&](json input, liquid::signer& signer, const std::string& document) {
        if (refuses_unbound_registration) {
            const auto issued = store.begin_identity_proof("lake", document);
            if (issued) {
                input["identity_proof"] = lake::test::identity_proof_json(
                    {issued->nonce,
                     issued->expires_at,
                     lake::test::liquid_sign_artifact(signer, issued->challenge)});
            }
        }
        return input;
    };

    // The victim registers their identity at sign-up.
    ApiContext anonymous;
    const ApiResult registered = harness.call("sign_up_password",
                                              proven({{"email", "victim-" + suffix + "@example.test"},
                                                      {"password", "correct horse battery staple"},
                                                      {"identity_document", victim.document}},
                                                     victim.signer,
                                                     victim.document),
                                              anonymous);
    ASSERT_TRUE(!refuses_unbound_registration ||
                store.begin_identity_proof("lake", victim.document))
        << "the victim's own first document is issued a challenge";
    ASSERT_TRUE(registered.ok) << registered.error.message;
    const ApiContext victim_session = harness.signed_in(registered.value.value("secret", ""));
    ASSERT_EQ(victim_session.acting_identity, victim_id);

    // Their own key signs in.
    ApiContext visitor;
    const ApiResult own_key =
        harness.call("sign_in_taap", sign_in_taap_input(victim.signer, victim.document), visitor);
    ASSERT_TRUE(own_key.ok) << own_key.error.message;
    EXPECT_EQ(store.context_for(own_key.value.value("secret", ""))->user_id,
              victim_session.acting_user_id);

    // liquid refuses the forged document: its identifier is not the one its key derives
    // (liquid#383, liquid#448). Lake refuses it on its own as well (bound_taap_key), whatever
    // liquid answers.
    const std::string forged = forged_document(attacker, victim_id);
    const auto liquid_accepts = liquid::verify_document(decoded_document(forged));
    EXPECT_FALSE(liquid_accepts && *liquid_accepts)
        << "liquid accepted a document naming another identity's identifier";
    ApiContext intruder;
    const ApiResult impersonated =
        harness.call("sign_in_taap", sign_in_taap_input(attacker.signer, forged), intruder);
    EXPECT_FALSE(impersonated.ok) << "a replacement key signed in as the victim";
    EXPECT_EQ(impersonated.error.code, "unauthorized");
    EXPECT_FALSE(impersonated.value.contains("secret"));
    // The victim's document with the attacker's signature, and the forged document behind the
    // victim's real one as a chain, are refused too.
    ApiContext intruder_again;
    EXPECT_FALSE(harness
                     .call("sign_in_taap",
                           sign_in_taap_input(attacker.signer, victim.document),
                           intruder_again)
                     .ok);
    EXPECT_FALSE(harness
                     .call("sign_in_taap",
                           sign_in_taap_input(attacker.signer, forged, {victim.document, forged}),
                           intruder_again)
                     .ok);
    // Registering the forged document never gives an account the victim's identifier: PostgreSQL
    // refuses it, and the in-memory store, which never trusted a document's claim, gives the new
    // account an identifier of its own.
    // On PostgreSQL no challenge is issued for the forged document (it is not a first document), so
    // it reaches sign-up without a proof, where the binding check refuses it first.
    const ApiResult claimed = harness.call("sign_up_password",
                                           proven({{"email", "claimer-" + suffix + "@example.test"},
                                                   {"password", "correct horse battery staple"},
                                                   {"identity_document", forged}},
                                                  attacker.signer,
                                                  forged),
                                           anonymous);
    EXPECT_EQ(claimed.ok, !refuses_unbound_registration) << claimed.error.message;
    if (claimed.ok) {
        EXPECT_NE(harness.signed_in(claimed.value.value("secret", "")).acting_identity, victim_id);
    } else {
        EXPECT_EQ(claimed.error.code, "invalid_input") << claimed.error.message;
    }

    // A rotation the old key signed is bound: the new key signs in with the chain, not without.
    // The first key is kept aside (a copy of the victim's liquid home taken before the rotation),
    // as a leaked first key would be.
    std::filesystem::copy(home / "victim", home / "victim-first-key",
                          std::filesystem::copy_options::recursive);
    Person first_key = person_at(home / "victim-first-key");
    ASSERT_EQ(first_key.document, victim.document);
    liquid::identity_document successor;
    {
        LiquidHome scope(home / "victim");
        auto rotated = liquid::runtime::block_on(victim.signer.rotate());
        ASSERT_TRUE(rotated) << rotated.error().message;
        successor = *rotated;
    }
    const std::string successor_document = encoded_document(successor);
    ApiContext rotated_visitor;
    const ApiResult successor_alone = harness.call(
        "sign_in_taap", sign_in_taap_input(victim.signer, successor_document), rotated_visitor);
    EXPECT_FALSE(successor_alone.ok);
    EXPECT_NE(successor_alone.error.message.find("identity_chain"), std::string::npos)
        << successor_alone.error.message;
    const ApiResult with_chain =
        harness.call("sign_in_taap",
                     sign_in_taap_input(
                         victim.signer, successor_document, {victim.document, successor_document}),
                     rotated_visitor);
    ASSERT_TRUE(with_chain.ok) << with_chain.error.message;
    EXPECT_EQ(store.context_for(with_chain.value.value("secret", ""))->user_id,
              victim_session.acting_user_id);

    // lake#1024: that sign-in recorded the chain's head, so the replaced first key, alone, no
    // longer signs in; the current key with the chain still does.
    ApiContext replaced_visitor;
    const ApiResult replaced_key = harness.call(
        "sign_in_taap", sign_in_taap_input(first_key.signer, victim.document), replaced_visitor);
    EXPECT_FALSE(replaced_key.ok) << "a replaced key signed in after the rotation was recorded";
    EXPECT_EQ(replaced_key.error.code, "unauthorized");
    EXPECT_NE(replaced_key.error.message.find("rotated"), std::string::npos)
        << replaced_key.error.message;
    EXPECT_FALSE(replaced_key.value.contains("secret"));
    const ApiResult again_with_chain =
        harness.call("sign_in_taap",
                     sign_in_taap_input(
                         victim.signer, successor_document, {victim.document, successor_document}),
                     rotated_visitor);
    EXPECT_TRUE(again_with_chain.ok) << again_with_chain.error.message;

    // A successor that declares the old key compromised and is signed only by the new key passes
    // liquid's verify_chain, but nothing the old key signed binds it: refused.
    liquid::identity_document declared = successor;
    declared.compromised_keys = {
        {decoded_document(victim.document).subject.key_fingerprint, successor.updated_at, "lost"}};
    const std::string declared_document = encoded_document(signed_by(declared, victim.signer));
    const std::vector<liquid::identity_document> declared_chain{
        decoded_document(victim.document), decoded_document(declared_document)};
    EXPECT_TRUE(liquid::verify_chain(declared_chain))
        << "liquid accepts a compromise rotation without the old key";
    const ApiResult unbound_rotation = harness.call(
        "sign_in_taap",
        sign_in_taap_input(victim.signer, declared_document, {victim.document, declared_document}),
        rotated_visitor);
    EXPECT_FALSE(unbound_rotation.ok);
    EXPECT_NE(unbound_rotation.error.message.find("not signed by the key it replaces"),
              std::string::npos)
        << unbound_rotation.error.message;

    // Each refusal is in the audit log (outcome `denied`), naming the identity it claimed.
    const auto events = lake::test::audit_events(store, {.limit = 500});
    ASSERT_TRUE(events) << events.error().message;
    const auto refusals = std::ranges::count_if(*events, [&](const lake::audit::Event& event) {
        return event.draft.action == "sign_in_taap" && event.draft.outcome == "denied" &&
               event.draft.details.value("claimed_taap_id", "") == victim_id &&
               !event.draft.details.value("reason", "").empty();
    });
    EXPECT_GE(refusals, 4);
}

} // namespace

TEST(TaapSignIn, MemoryStoreBindsTheKeyToTheRegisteredIdentity) {
    lake::control::InMemoryStore store;
    expect_taap_sign_in_binds_the_key(store, lake::control::new_uuid_v4().substr(0, 8), false);
}

TEST(TaapSignIn, PostgresStoreBindsTheKeyToTheRegisteredIdentity) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    auto store = lake::control::make_postgres_store(uri, std::string(instance_kek));
    expect_taap_sign_in_binds_the_key(*store, lake::control::new_uuid_v4().substr(0, 8), true);
}
