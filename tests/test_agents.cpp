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
#include "lake/control/postgres_store.hpp"
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
        const auto* principal = std::get_if<std::string>(&attribute.value);
        return attribute.name == "principal" && principal && *principal == durable_agent;
    }));
    const auto state = log.state();
    ASSERT_TRUE(state) << state.error().message;
    EXPECT_EQ(state->tree_size, 1U);
    EXPECT_EQ(state->root_hash, stored->transparency_root);
    const auto scan = log.scan_identity(durable_agent, state->tree_size, state->root_hash);
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
        store.list_audit_events({.object = "agent_session/" + session_id, .limit = 100});
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

} // namespace

TEST(AgentSessions, MintsAttributesUsesAndExpiresSessionKey) {
    lake::control::InMemoryStore store;
    const std::string customer_id = create_customer(store, "memory");
    ASSERT_FALSE(customer_id.empty());
    exercise_agent_session(store, customer_id, "agent-memory", true);
}

TEST(PostgresAgentSessions, PersistsLoggedSessionAndDurableAttribution) {
    const char* uri = std::getenv("LAKE_TEST_DB_URI");
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
