// An identity document is public, so holding one proves nothing: password sign-up and a first
// OAuth sign-in bind a document to a new account only with a fresh signature by its key (lake#931,
// docs/api/identity.md#proving-you-hold-the-key).
#include "identity_proof_test_support.hpp"
#include "postgres_fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/rest.hpp"
#include "lake/api/web_ui.hpp"
#include "lake/control/own_identity.hpp"
#include "lake/control/postgres_store.hpp"

namespace {

using lake::control::IdentityProof;
using lake::test::TestIdentity;
using nlohmann::json;

constexpr std::string_view instance_kek =
    "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

std::array<unsigned char, 32> instance_kek_bytes() {
    std::array<unsigned char, 32> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<unsigned char>(
            std::stoi(std::string(instance_kek.substr(index * 2, 2)), nullptr, 16));
    }
    return bytes;
}

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

class PostgresIdentityProof : public testing::Test {
protected:
    void SetUp() override {
        const char* configured = lake::test::postgres_uri();
        if (configured == nullptr || *configured == '\0') {
            GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
        }
        uri = configured;
        store = lake::control::make_postgres_store(uri, std::string(instance_kek));
        suffix = lake::control::new_uuid_v4().substr(0, 8);
    }

    lake::control::NewPasswordUser
    sign_up(const std::string& label, const std::string& document, IdentityProof proof = {}) {
        lake::control::NewPasswordUser user{"lake",
                                            label + "-" + suffix + "@example.test",
                                            "correct horse battery staple",
                                            label,
                                            "browser",
                                            document};
        user.identity_proof = std::move(proof);
        return user;
    }

    // A flow of a provider registered for this test, starting a first sign-in with `document`.
    lake::control::OAuthFlow
    oauth_flow(const std::string& document, IdentityProof proof, const std::string& state) {
        const std::string provider = "stub-" + suffix;
        EXPECT_TRUE(store->register_oauth_provider(
            {"lake", provider, "https://issuer.example.test", "lake-test", "secret", {"openid"}}));
        return {"lake",
                provider,
                state + "-" + suffix,
                "pkce-verifier",
                "oidc-nonce",
                "browser",
                document,
                std::move(proof)};
    }

    int users_with(const std::string& taap_id) {
        pqxx::connection connection(uri);
        pqxx::read_transaction tx(connection);
        return tx.exec_params("SELECT count(*) FROM users WHERE taap_id = $1", taap_id)[0][0]
            .as<int>();
    }

    std::string uri;
    std::unique_ptr<lake::control::Store> store;
    std::string suffix;
};

TEST_F(PostgresIdentityProof, SignUpWithAnotherIdentitysDocumentAndNoProofIsRefused) {
    TestIdentity victim = lake::test::test_identity("victim-" + suffix);
    const auto taken = store->sign_up_password(sign_up("attacker", victim.document));
    ASSERT_FALSE(taken);
    EXPECT_EQ(taken.error().code, "identity_proof_required");
    EXPECT_EQ(users_with(victim.signer.identity().taap_id), 0);

    // The same through the capability, as REST and MCP call it: 403, nothing created.
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::ApiContext context;
    context.store = store.get();
    context.instance_kek = std::string(instance_kek);
    context.request_id = lake::control::new_uuid_v4();
    const auto answered = liquid::runtime::block_on(
        registry.call_async("sign_up_password",
                            {{"email", "attacker-api-" + suffix + "@example.test"},
                             {"password", "correct horse battery staple"},
                             {"identity_document", victim.document}},
                            context));
    ASSERT_TRUE(answered);
    EXPECT_EQ(answered->error.code, "identity_proof_required");
    EXPECT_EQ(lake::api::rest_status(*answered), 403);
    EXPECT_EQ(users_with(victim.signer.identity().taap_id), 0);
}

TEST_F(PostgresIdentityProof, AGenuineProofEnrollsTheIdentity) {
    TestIdentity person = lake::test::test_identity("person-" + suffix);
    const auto enrolled = store->sign_up_password(
        sign_up("person", person.document, lake::test::prove_identity(*store, "lake", person)));
    ASSERT_TRUE(enrolled) << enrolled.error().message;
    const auto principal = store->context_for(enrolled->secret);
    ASSERT_TRUE(principal) << principal.error().message;
    EXPECT_EQ(principal->taap_id, person.signer.identity().taap_id);
}

TEST_F(PostgresIdentityProof, AProofByAnotherKeyIsRefused) {
    TestIdentity victim = lake::test::test_identity("victim-" + suffix);
    TestIdentity attacker = lake::test::test_identity("attacker-" + suffix);
    // The challenge is for the victim's document; the attacker can only sign it with their own key.
    const auto issued = store->begin_identity_proof("lake", victim.document);
    ASSERT_TRUE(issued) << issued.error().message;
    const std::string signed_output =
        lake::test::liquid_sign_artifact(attacker.signer, issued->challenge);
    for (const std::string& signature :
         {signed_output, signed_output.substr(0, signed_output.find("\nsigner="))}) {
        const auto taken = store->sign_up_password(
            sign_up("attacker", victim.document, {issued->nonce, issued->expires_at, signature}));
        ASSERT_FALSE(taken);
        EXPECT_EQ(taken.error().code, "unauthorized") << taken.error().message;
    }
    EXPECT_EQ(users_with(victim.signer.identity().taap_id), 0);
}

TEST_F(PostgresIdentityProof, AChallengeForAnotherPoolOrNonceIsRefused) {
    TestIdentity person = lake::test::test_identity("person-" + suffix);
    IdentityProof elsewhere = lake::test::prove_identity(*store, "other-pool-" + suffix, person);
    const auto refused = store->sign_up_password(sign_up("person", person.document, elsewhere));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "unauthorized") << refused.error().message;

    IdentityProof altered = lake::test::prove_identity(*store, "lake", person);
    altered.nonce += "x";
    const auto changed = store->sign_up_password(sign_up("person", person.document, altered));
    ASSERT_FALSE(changed);
    EXPECT_EQ(changed.error().code, "unauthorized") << changed.error().message;
    EXPECT_EQ(users_with(person.signer.identity().taap_id), 0);
}

TEST_F(PostgresIdentityProof, AnExpiredChallengeIsRefused) {
    TestIdentity person = lake::test::test_identity("person-" + suffix);
    const auto document = lake::control::own_identity::verify_document(person.document);
    ASSERT_TRUE(document) << document.error().message;
    // A challenge only this Lake could have made, signed by the right key, but expired.
    const std::int64_t expired = unix_now() - 1;
    const auto kek = instance_kek_bytes();
    const std::string challenge = lake::control::own_identity::enrollment_challenge(
        kek, "lake", *document, "expired-nonce", expired);
    ASSERT_FALSE(challenge.empty());
    const auto refused = store->sign_up_password(sign_up(
        "person",
        person.document,
        {"expired-nonce", expired, lake::test::liquid_sign_artifact(person.signer, challenge)}));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "unauthorized");
    EXPECT_NE(refused.error().message.find("expired"), std::string::npos);
    EXPECT_EQ(users_with(person.signer.identity().taap_id), 0);
}

TEST_F(PostgresIdentityProof, AProofIsUsedOnce) {
    TestIdentity person = lake::test::test_identity("person-" + suffix);
    const IdentityProof proof = lake::test::prove_identity(*store, "lake", person);
    ASSERT_TRUE(store->begin_oauth(oauth_flow(person.document, proof, "first")));
    const auto again = store->begin_oauth(oauth_flow(person.document, proof, "second"));
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, "unauthorized");
    const auto signed_up = store->sign_up_password(sign_up("person", person.document, proof));
    ASSERT_FALSE(signed_up);
    EXPECT_EQ(signed_up.error().code, "unauthorized");
    EXPECT_EQ(users_with(person.signer.identity().taap_id), 0);
    // The spent proof is never a flow the callback can consume.
    EXPECT_FALSE(store->consume_oauth("stub-" + suffix, "identity_proof:"));

    // A proof a sign-up spent cannot start an OAuth flow, and a retried sign-up is a conflict.
    TestIdentity other = lake::test::test_identity("other-" + suffix);
    const IdentityProof other_proof = lake::test::prove_identity(*store, "lake", other);
    ASSERT_TRUE(store->sign_up_password(sign_up("other", other.document, other_proof)));
    const auto reused = store->begin_oauth(oauth_flow(other.document, other_proof, "third"));
    ASSERT_FALSE(reused);
    EXPECT_EQ(reused.error().code, "unauthorized");
    const auto retried = store->sign_up_password(sign_up("other", other.document, other_proof));
    ASSERT_FALSE(retried);
    EXPECT_EQ(retried.error().code, "conflict");
}

TEST_F(PostgresIdentityProof, AFirstOAuthSignInNeedsTheProofToo) {
    TestIdentity victim = lake::test::test_identity("victim-" + suffix);
    const auto refused = store->begin_oauth(oauth_flow(victim.document, {}, "unproven"));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "identity_proof_required");
    // No flow was created, so no callback can bind the document.
    EXPECT_FALSE(store->consume_oauth("stub-" + suffix, "unproven-" + suffix));

    const auto started = store->begin_oauth(
        oauth_flow(victim.document, lake::test::prove_identity(*store, "lake", victim), "proven"));
    ASSERT_TRUE(started) << started.error().message;
    const auto flow = store->consume_oauth("stub-" + suffix, "proven-" + suffix);
    ASSERT_TRUE(flow) << flow.error().message;
    const auto signed_in = store->sign_in_oauth({"lake",
                                                 "stub-" + suffix,
                                                 "subject-" + suffix,
                                                 "victim-" + suffix + "@example.test",
                                                 "Victim",
                                                 "browser",
                                                 flow->identity_document});
    ASSERT_TRUE(signed_in) << signed_in.error().message;
    EXPECT_EQ(users_with(victim.signer.identity().taap_id), 1);
}

TEST(IdentityProofPage, KeepsTheFormEscapedAndNeverEchoesThePassword) {
    const lake::control::Application application{"app-1",
                                                 "customer-1",
                                                 "pool-1",
                                                 "Example",
                                                 {},
                                                 {},
                                                 "",
                                                 json::object(),
                                                 "created",
                                                 "updated"};
    const std::string html = lake::api::web_ui_application_identity_proof(
        application,
        {.action = "/identity/sign-up",
         .challenge = "sha256:abc",
         .nonce = "nonce\"><script>",
         .expires_at = 1700000000,
         .fields = {{"email", "a\"><script>x()</script>@example.test"},
                    {"identity_document", "document"}},
         .password = true},
        "");
    EXPECT_NE(html.find("action=\"/apps/app-1/identity/sign-up\""), std::string::npos);
    EXPECT_NE(html.find("liquid sign artifact sha256:abc"), std::string::npos);
    EXPECT_NE(html.find("name=\"identity_expires_at\" value=\"1700000000\""), std::string::npos);
    EXPECT_NE(html.find("name=\"identity_signature\""), std::string::npos);
    EXPECT_NE(html.find("name=\"identity_document\" value=\"document\""), std::string::npos);
    EXPECT_EQ(html.find("<script>"), std::string::npos);
    EXPECT_NE(html.find("type=\"password\" name=\"password\""), std::string::npos);
    EXPECT_EQ(html.find("name=\"password\" value="), std::string::npos);
}

} // namespace
