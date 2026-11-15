// SPDX-License-Identifier: MIT

#include "audit_test_support.hpp"
#include "postgres_fixture.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>

#include <liquid/attestation.hpp>
#include <liquid/identity.hpp>

#include "identity/taap.hpp"

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/agents.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/pki.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/telemetry/metrics.hpp"
#include "lake/transparency/log.hpp"
#include "temporary_directory.hpp"

namespace {

using nlohmann::json;

struct PkeyDeleter {
    void operator()(EVP_PKEY* key) const {
        EVP_PKEY_free(key);
    }
};

struct ContextDeleter {
    void operator()(EVP_MD_CTX* context) const {
        EVP_MD_CTX_free(context);
    }
};

using Pkey = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using Context = std::unique_ptr<EVP_MD_CTX, ContextDeleter>;

constexpr std::string_view instance_kek =
    "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

std::vector<std::byte> bytes(std::string_view input) {
    std::vector<std::byte> output;
    output.reserve(input.size());
    for (const char value : input) {
        output.push_back(static_cast<std::byte>(value));
    }
    return output;
}

std::string base64url(std::span<const std::byte> input) {
    std::string output(((input.size() + 2) / 3) * 4, '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()),
                                     reinterpret_cast<const unsigned char*>(input.data()),
                                     static_cast<int>(input.size()));
    if (size < 0) {
        return {};
    }
    output.resize(static_cast<std::size_t>(size));
    std::ranges::replace(output, '+', '-');
    std::ranges::replace(output, '/', '_');
    while (output.ends_with('=')) {
        output.pop_back();
    }
    return output;
}

std::vector<std::byte> from_base64url(std::string input) {
    std::ranges::replace(input, '-', '+');
    std::ranges::replace(input, '_', '/');
    while (input.size() % 4 != 0) {
        input.push_back('=');
    }
    std::vector<std::byte> output((input.size() / 4) * 3);
    const int size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(output.data()),
                                     reinterpret_cast<const unsigned char*>(input.data()),
                                     static_cast<int>(input.size()));
    if (size < 0) {
        return {};
    }
    std::size_t padding = 0;
    if (input.ends_with('=')) {
        ++padding;
    }
    if (input.ends_with("==")) {
        ++padding;
    }
    output.resize(static_cast<std::size_t>(size) - padding);
    return output;
}

std::vector<std::byte> sign_drip(std::span<const std::byte> seed,
                                 std::string_view session_taap_id,
                                 std::string_view signed_at,
                                 std::span<const std::byte> body) {
    Pkey key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519,
                                          nullptr,
                                          reinterpret_cast<const unsigned char*>(seed.data()),
                                          seed.size()));
    if (!key) {
        return {};
    }
    std::vector<std::byte> input = bytes("TAAP/1 drip");
    input.push_back(std::byte{0});
    input.insert(input.end(), body.begin(), body.end());
    Context context(EVP_MD_CTX_new());
    std::array<unsigned char, 64> raw{};
    std::size_t size = raw.size();
    if (!context || EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1 ||
        EVP_DigestSign(context.get(),
                       raw.data(),
                       &size,
                       reinterpret_cast<const unsigned char*>(input.data()),
                       input.size()) != 1) {
        return {};
    }
    std::vector<std::byte> signature_bytes;
    signature_bytes.reserve(size);
    for (std::size_t index = 0; index < size; ++index) {
        signature_bytes.push_back(std::byte{raw[index]});
    }
    auto encoded = liquid::encode_attestation_signature(
        {.algorithm = "ed25519",
         .signing_key_identifier = std::string(session_taap_id),
         .signing_key_label = "agent-session",
         .signature_purpose = "drip",
         .signed_at = std::string(signed_at),
         .bytes = std::move(signature_bytes)});
    return encoded ? std::move(*encoded) : std::vector<std::byte>{};
}

std::string create_customer(lake::control::Store& store, std::string suffix) {
    const auto owner = store.create_user({"taap:1:ed25519:agent-owner-" + suffix,
                                          "agent-owner-" + suffix + "@example.test",
                                          "Agent owner"});
    EXPECT_TRUE(owner) << owner.error().message;
    if (!owner) {
        return {};
    }
    const auto customer =
        store.create_customer({"individual", owner->id, "", "agent-customer-" + suffix});
    EXPECT_TRUE(customer) << customer.error().message;
    return customer ? customer->id : std::string{};
}

void grant_dispatcher(lake::control::Store& store,
                      const std::string& customer_id,
                      const std::string& dispatcher,
                      const std::string& role = "admin") {
    const auto object = store.find_policy_object("customer", customer_id);
    ASSERT_TRUE(object) << object.error().message;
    const auto granted = store.create_grant(
        {dispatcher, object->id, role, "agent-test-grant", "admin", "agent-test"});
    ASSERT_TRUE(granted) << granted.error().message;
}

lake::control::PkiCertificateRecord register_agent(lake::control::Store& store,
                                                   const std::string& customer_id,
                                                   const std::string& agent,
                                                   const std::string& expires_at,
                                                   const std::string& profile = "agent") {
    lake::control::pki::Service pki;
    if (!store.get_pki_authority(customer_id, std::string(instance_kek))) {
        const auto authority = pki.create_authority(store, customer_id, instance_kek);
        EXPECT_TRUE(authority) << authority.error().message;
    }
    const auto certificate = store.create_pki_certificate({customer_id,
                                                           profile,
                                                           agent,
                                                           lake::control::new_uuid_v4(),
                                                           "test-public-certificate",
                                                           expires_at,
                                                           {}});
    EXPECT_TRUE(certificate) << certificate.error().message;
    return certificate.value_or(lake::control::PkiCertificateRecord{});
}

void exercise_agent_session(lake::control::Store& store,
                            const std::string& customer_id,
                            const std::string& instance,
                            bool verify_audit_chain) {
    const auto root = lake::test::fixture_path(instance);
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    lake::transparency::TiledLog log(root, instance);
    std::chrono::sys_seconds now =
        std::chrono::sys_days{std::chrono::year{2026} / std::chrono::September / 5} +
        std::chrono::hours{12};
    lake::control::agents::Service agents([&now] { return now; });
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::ApiContext context;
    context.store = &store;
    context.agents = &agents;
    context.transparency_log = &log;
    context.authorized = true;
    context.operator_credential = true;
    context.acting_identity = "admin";
    context.instance_kek = std::string(instance_kek);
    const std::string durable_agent = "taap:1:ed25519:durable-agent-" + instance;
    const std::string dispatcher = "taap:1:ed25519:dispatcher-" + instance;
    grant_dispatcher(store, customer_id, dispatcher);
    register_agent(store, customer_id, durable_agent, "2027-01-01T00:00:00Z");
    context.acting_identity = dispatcher;
    context.operator_credential = false;

    auto minted = registry.call("mint_agent_session_key",
                                {{"customer_id", customer_id},
                                 {"dispatcher_taap_id", dispatcher},
                                 {"durable_agent_taap_id", durable_agent},
                                 {"lifetime_seconds", 60},
                                 {"scopes", json::array({"drip:write", "build:read"})}},
                                context);
    ASSERT_TRUE(minted.ok) << minted.error.code << ": " << minted.error.message;
    ASSERT_TRUE(minted.value.contains("private_key"));
    const std::string session_id = minted.value.at("agent_session_id").get<std::string>();
    const std::string session_taap_id = minted.value.at("session_taap_id").get<std::string>();
    const auto public_key = from_base64url(minted.value.at("public_key").get<std::string>());
    const auto expected_fingerprint = liquid::identity_detail::key_fingerprint(
        public_key, minted.value.at("issued_at").get<std::string>(), "agent-session");
    ASSERT_TRUE(expected_fingerprint) << expected_fingerprint.error().message;
    EXPECT_EQ(session_taap_id, "taap:1:ed25519:" + *expected_fingerprint);
    const auto stored = store.get_agent_session(customer_id, session_id);
    ASSERT_TRUE(stored) << stored.error().message;
    EXPECT_EQ(stored->durable_agent_taap_id, durable_agent);
    EXPECT_EQ(stored->dispatcher_taap_id, dispatcher);
    EXPECT_EQ(stored->transparency_index, 0U);

    const auto encoded_attestation =
        from_base64url(minted.value.at("attestation").get<std::string>());
    const auto attestation = liquid::decode_attestation(encoded_attestation);
    ASSERT_TRUE(attestation) << attestation.error().message;
    EXPECT_EQ(attestation->profile, "delegation/1");
    EXPECT_EQ(attestation->subject_kind, liquid::attestation_subject_kind::actor);
    EXPECT_TRUE(std::ranges::any_of(attestation->attributes, [&](const auto& attribute) {
        const auto* value = std::get_if<std::string>(&attribute.value);
        return attribute.name == "dispatcher" && value && *value == dispatcher;
    }));
    const auto state = log.state();
    ASSERT_TRUE(state) << state.error().message;
    EXPECT_EQ(state->tree_size, 1U);
    EXPECT_EQ(state->root_hash, stored->transparency_root);
    const auto scan = log.scan_identity(dispatcher, state->tree_size, state->root_hash);
    ASSERT_TRUE(scan) << scan.error().message;
    ASSERT_EQ(scan->matches.size(), 1U);
    EXPECT_EQ(scan->matches.front().attestation, stored->attestation);

    const std::vector<std::byte> body = bytes("canonical agent drip");
    const auto seed = from_base64url(minted.value.at("private_key").get<std::string>());
    ASSERT_EQ(seed.size(), 32U);
    const auto signature = sign_drip(seed, session_taap_id, stored->issued_at, body);
    ASSERT_FALSE(signature.empty());
    context.request_id.clear();
    auto verified = registry.call("verify_agent_session_drip",
                                  {{"agent_session_id", session_id},
                                   {"body", base64url(body)},
                                   {"customer_id", customer_id},
                                   {"signature", base64url(signature)}},
                                  context);
    ASSERT_TRUE(verified.ok) << verified.error.code << ": " << verified.error.message;
    EXPECT_TRUE(verified.value.at("verified").get<bool>());
    EXPECT_EQ(verified.value.at("durable_agent_taap_id"), durable_agent);

    std::vector<std::byte> altered = body;
    altered.back() ^= std::byte{1};
    context.request_id.clear();
    const auto tampered = registry.call("verify_agent_session_drip",
                                        {{"agent_session_id", session_id},
                                         {"body", base64url(altered)},
                                         {"customer_id", customer_id},
                                         {"signature", base64url(signature)}},
                                        context);
    EXPECT_FALSE(tampered.ok);
    EXPECT_EQ(tampered.error.code, "unauthorized");

    now += std::chrono::seconds{61};
    context.request_id.clear();
    const auto expired = registry.call("verify_agent_session_drip",
                                       {{"agent_session_id", session_id},
                                        {"body", base64url(body)},
                                        {"customer_id", customer_id},
                                        {"signature", base64url(signature)}},
                                       context);
    EXPECT_FALSE(expired.ok);
    EXPECT_EQ(expired.error.code, "unauthorized");
    EXPECT_EQ(expired.error.message, "agent session key has expired");

    const auto events =
        lake::test::audit_events(store, {.object = "agent_session/" + session_id, .limit = 100});
    ASSERT_TRUE(events) << events.error().message;
    EXPECT_TRUE(std::ranges::any_of(*events, [](const lake::audit::Event& event) {
        return event.draft.kind == "agent_session" &&
               event.draft.action == "mint_agent_session_key" && event.draft.outcome == "success" &&
               !event.draft.details.contains("private_key");
    }));
    EXPECT_TRUE(std::ranges::any_of(*events, [](const lake::audit::Event& event) {
        return event.draft.kind == "agent_session" &&
               event.draft.action == "verify_agent_session_drip" &&
               event.draft.outcome == "success";
    }));
    if (verify_audit_chain) {
        EXPECT_TRUE(lake::audit::verify(store, *events));
    }
    std::filesystem::remove_all(root);
}

// lake#234: a key that never expires, allowed only by the customer's opt-in and the instance
// switch, still verifies years later, and stops the moment it is revoked.
void exercise_non_expiring_session(lake::control::Store& store,
                                   const std::string& customer_id,
                                   const std::string& instance) {
    const auto root = lake::test::fixture_path(instance);
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    lake::transparency::TiledLog log(root, instance);
    std::chrono::sys_seconds now =
        std::chrono::sys_days{std::chrono::year{2026} / std::chrono::September / 5} +
        std::chrono::hours{12};
    lake::control::agents::Service agents([&now] { return now; });
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::ApiContext context;
    context.store = &store;
    context.agents = &agents;
    context.transparency_log = &log;
    context.authorized = true;
    context.operator_credential = true;
    context.acting_identity = "admin";
    context.instance_kek = std::string(instance_kek);
    const json request{{"customer_id", customer_id},
                       {"dispatcher_taap_id", "taap:1:ed25519:dispatcher-" + instance},
                       {"durable_agent_taap_id", "taap:1:ed25519:durable-" + instance},
                       {"never_expires", true},
                       {"scopes", json::array({"drip:write"})}};
    const std::string dispatcher = request.at("dispatcher_taap_id");
    grant_dispatcher(store, customer_id, dispatcher);
    register_agent(store, customer_id, request.at("durable_agent_taap_id"), "2027-01-01T00:00:00Z");
    context.acting_identity = dispatcher;
    context.operator_credential = false;

    const auto off = registry.call("mint_agent_session_key", request, context);
    ASSERT_FALSE(off.ok) << "off by default";
    EXPECT_NE(off.error.message.find("allow_non_expiring_credentials"), std::string::npos);

    context.request_id.clear();
    const auto allowed =
        registry.call("set_customer_credential_policy",
                      {{"customer_id", customer_id}, {"allow_non_expiring_credentials", true}},
                      context);
    ASSERT_TRUE(allowed.ok) << allowed.error.message;
    context.non_expiring_credentials_allowed = false;
    context.request_id.clear();
    const auto instance_off = registry.call("mint_agent_session_key", request, context);
    ASSERT_FALSE(instance_off.ok) << "the instance switch overrides the customer";
    EXPECT_NE(instance_off.error.message.find("LAKE_ALLOW_NON_EXPIRING_CREDENTIALS"),
              std::string::npos);
    context.non_expiring_credentials_allowed = true;

    context.request_id.clear();
    const auto minted = registry.call("mint_agent_session_key", request, context);
    ASSERT_TRUE(minted.ok) << minted.error.code << ": " << minted.error.message;
    EXPECT_EQ(minted.value.at("expires_at"), "never");
    EXPECT_TRUE(minted.value.at("never_expires").get<bool>());
    const std::string session_id = minted.value.at("agent_session_id");
    const auto stored = store.get_agent_session(customer_id, session_id);
    ASSERT_TRUE(stored) << stored.error().message;
    EXPECT_TRUE(stored->never_expires());
    const std::vector<std::byte> body = bytes("a drip years later");
    const auto seed = from_base64url(minted.value.at("private_key").get<std::string>());
    const auto signature = sign_drip(
        seed, minted.value.at("session_taap_id").get<std::string>(), stored->issued_at, body);
    const json drip{{"agent_session_id", session_id},
                    {"body", base64url(body)},
                    {"customer_id", customer_id},
                    {"signature", base64url(signature)}};

    now += std::chrono::days{3650};
    context.request_id.clear();
    const auto later = registry.call("verify_agent_session_drip", drip, context);
    ASSERT_TRUE(later.ok) << later.error.code << ": " << later.error.message;

    context.request_id.clear();
    const auto revoked =
        registry.call("revoke_agent_session",
                      {{"customer_id", customer_id}, {"agent_session_id", session_id}},
                      context);
    ASSERT_TRUE(revoked.ok) << revoked.error.message;
    EXPECT_FALSE(revoked.value.at("revoked_at").get<std::string>().empty());
    context.request_id.clear();
    const auto refused = registry.call("verify_agent_session_drip", drip, context);
    ASSERT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.message, "agent session key has been revoked");
    std::filesystem::remove_all(root);
}

// Exercise the registry and the signing service against the same customer-scoped registration.
void exercise_delegation_authority(lake::control::Store& store, const std::string& suffix) {
    const std::string customer = create_customer(store, "authority-" + suffix);
    const std::string other_customer = create_customer(store, "foreign-" + suffix);
    const std::string dispatcher = "taap:1:ed25519:writer-" + suffix;
    const std::string agent = "taap:1:ed25519:registered-" + suffix;
    const std::string foreign_agent = "taap:1:ed25519:foreign-" + suffix;
    const std::string unknown_agent = "taap:1:ed25519:unknown-" + suffix;
    grant_dispatcher(store, customer, dispatcher, "write");
    const auto certificate = register_agent(store, customer, agent, "2027-01-01T00:00:00Z");
    register_agent(store, other_customer, foreign_agent, "2027-01-01T00:00:00Z");
    register_agent(store, customer, unknown_agent, "2027-01-01T00:00:00Z", "client");
    const auto now = std::chrono::sys_days{std::chrono::year{2026} / std::chrono::September / 5};
    lake::control::agents::Service agents([now] { return now; });
    const auto root = lake::test::fixture_path("agent-authority-" + suffix);
    std::filesystem::create_directories(root);
    lake::transparency::TiledLog log(root, "authority-" + suffix);
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::ApiContext context;
    context.store = &store;
    context.agents = &agents;
    context.transparency_log = &log;
    context.authorized = true;
    context.acting_identity = dispatcher;
    context.instance_kek = std::string(instance_kek);
    json request{{"customer_id", customer},
                 {"dispatcher_taap_id", dispatcher},
                 {"durable_agent_taap_id", agent},
                 {"scopes", json::array({"drip:write"})}};
    const auto refused = [&](const json& input) {
        context.request_id.clear();
        const auto result = registry.call("mint_agent_session_key", input, context);
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.error.code, "unauthorized") << result.error.message;
        EXPECT_FALSE(result.value.contains("private_key"));
        const auto state = log.state();
        ASSERT_TRUE(state) << state.error().message;
        EXPECT_EQ(state->tree_size, 0U);
    };
    auto forged = request;
    forged["dispatcher_taap_id"] = "taap:1:ed25519:victim";
    refused(forged);
    // Operator credentials have no acting TAAP identity and cannot name a dispatcher either.
    context.acting_identity = "admin";
    context.operator_credential = true;
    refused(request);
    context.acting_identity = dispatcher;
    context.operator_credential = false;
    for (const auto& unregistered :
         {foreign_agent, unknown_agent, std::string("taap:1:ed25519:absent")}) {
        auto input = request;
        input["durable_agent_taap_id"] = unregistered;
        refused(input);
    }
    ASSERT_TRUE(store.revoke_pki_certificate(certificate.id));
    refused(request);
    // A certificate that expires at the issuance instant is already expired.
    register_agent(store, customer, agent, "2026-09-05T00:00:00Z");
    refused(request);
    // The same checks protect direct service callers and never-expiring session keys.
    const lake::audit::IdentityOptions options{.instance_kek = std::string(instance_kek)};
    lake::control::agents::MintRequest mint_request{.customer_id = customer,
                                                    .durable_agent_taap_id = agent,
                                                    .dispatcher_taap_id = dispatcher,
                                                    .scopes = {"drip:write"},
                                                    .never_expires = true};
    auto direct = agents.mint(store, mint_request, dispatcher, options);
    ASSERT_FALSE(direct);
    EXPECT_EQ(direct.error().code, "unauthorized");
    register_agent(store, customer, agent, "2027-01-01T00:00:00Z");
    for (const std::string identity : {"", "admin", "taap:1:ed25519:other"}) {
        direct = agents.mint(store, mint_request, identity, options);
        ASSERT_FALSE(direct);
        EXPECT_EQ(direct.error().code, "unauthorized");
    }
    direct = agents.mint(store, mint_request, dispatcher, options);
    ASSERT_TRUE(direct) << direct.error().message;
    context.request_id.clear();
    const auto minted = registry.call("mint_agent_session_key", request, context);
    ASSERT_TRUE(minted.ok) << minted.error.message;
    EXPECT_EQ(minted.value.at("dispatcher_taap_id"), dispatcher);
    EXPECT_EQ(minted.value.at("durable_agent_taap_id"), agent);
}

} // namespace

TEST(AgentSessions, RefusesUnprovenDispatcherAndUnregisteredDurableAgent) {
    lake::telemetry::Metrics metrics;
    auto store = lake::telemetry::instrument_store(std::make_shared<lake::control::InMemoryStore>(),
                                                   metrics);
    exercise_delegation_authority(*store, "memory");
}

TEST(PostgresAgentSessions, RefusesUnprovenDispatcherAndUnregisteredDurableAgent) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    lake::telemetry::Metrics metrics;
    auto store = lake::telemetry::instrument_store(
        lake::control::make_postgres_store(uri, std::string(instance_kek)), metrics);
    exercise_delegation_authority(*store, lake::control::new_uuid_v4().substr(0, 8));
}

TEST(AgentSessions, MintsAttributesUsesAndExpiresSessionKey) {
    lake::control::InMemoryStore store;
    const std::string customer_id = create_customer(store, "memory");
    ASSERT_FALSE(customer_id.empty());
    exercise_agent_session(store, customer_id, "agent-memory", true);
}

TEST(PostgresAgentSessions, PersistsLoggedSessionAndDurableAttribution) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    auto store = lake::control::make_postgres_store(uri, std::string(instance_kek));
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4();
    const std::string customer_id = create_customer(*store, suffix);
    ASSERT_FALSE(customer_id.empty());
    exercise_agent_session(*store, customer_id, "agent-postgres-" + suffix, false);
}

TEST(AgentSessions, NeverExpiringKeysNeedTheOptInAndStayRevocable) {
    lake::control::InMemoryStore store;
    const std::string customer_id = create_customer(store, "memory-never");
    ASSERT_FALSE(customer_id.empty());
    exercise_non_expiring_session(store, customer_id, "agent-memory-never");
}

TEST(PostgresAgentSessions, NeverExpiringKeysNeedTheOptInAndStayRevocable) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    auto store = lake::control::make_postgres_store(uri, std::string(instance_kek));
    const std::string suffix = lake::control::new_uuid_v4();
    const std::string customer_id = create_customer(*store, "never-" + suffix.substr(0, 8));
    ASSERT_FALSE(customer_id.empty());
    exercise_non_expiring_session(*store, customer_id, "agent-postgres-never-" + suffix);
}
