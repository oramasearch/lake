#include "audit_test_support.hpp"
#include "personal_customer_test_support.hpp"
#include "postgres_fixture.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <openssl/evp.h>

#include <liquid/identity.hpp>
#include <liquid/runtime.hpp>
#include <liquid/transparency.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/pki.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/transparency/log.hpp"
#include "temporary_directory.hpp"

namespace {

using lake::api::ApiContext;
using lake::api::ApiResult;
using lake::api::Registry;
using nlohmann::json;

constexpr std::string_view instance_kek =
    "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

std::string base64url(std::span<const std::byte> bytes) {
    std::string output(((bytes.size() + 2) / 3) * 4, '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()),
                                     reinterpret_cast<const unsigned char*>(bytes.data()),
                                     static_cast<int>(bytes.size()));
    output.resize(static_cast<std::size_t>(size));
    while (output.ends_with('=')) {
        output.pop_back();
    }
    std::ranges::replace(output, '+', '-');
    std::ranges::replace(output, '/', '_');
    return output;
}

std::vector<std::byte> from_base64url(std::string value) {
    std::ranges::replace(value, '-', '+');
    std::ranges::replace(value, '_', '/');
    while (value.size() % 4 != 0) {
        value.push_back('=');
    }
    std::vector<std::byte> output((value.size() / 4) * 3);
    const int size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(output.data()),
                                     reinterpret_cast<const unsigned char*>(value.data()),
                                     static_cast<int>(value.size()));
    if (size < 0) {
        return {};
    }
    std::size_t padding = 0;
    if (value.ends_with('=')) {
        ++padding;
    }
    if (value.ends_with("==")) {
        ++padding;
    }
    output.resize(static_cast<std::size_t>(size) - padding);
    return output;
}

std::array<std::byte, 32> hash_from_hex(std::string_view value) {
    std::array<std::byte, 32> output{};
    for (std::size_t index = 0; index < output.size(); ++index) {
        unsigned byte = 0;
        const auto [end, error] =
            std::from_chars(value.data() + index * 2, value.data() + index * 2 + 2, byte, 16);
        EXPECT_EQ(error, std::errc{});
        EXPECT_EQ(end, value.data() + index * 2 + 2);
        output[index] = std::byte{static_cast<unsigned char>(byte)};
    }
    return output;
}

std::vector<std::string> token_parts(const std::string& token) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= token.size()) {
        const std::size_t end = token.find('.', start);
        parts.push_back(
            token.substr(start, end == std::string::npos ? token.size() - start : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return parts;
}

bool verifies_with_jwks(const std::string& token, const json& jwks) {
    const auto parts = token_parts(token);
    if (parts.size() != 3) {
        return false;
    }
    const auto header_bytes = from_base64url(parts[0]);
    const json decoded_header = json::parse(
        std::string(reinterpret_cast<const char*>(header_bytes.data()), header_bytes.size()));
    const auto found = std::ranges::find_if(jwks.at("keys"), [&](const json& key) {
        return key.at("kid") == decoded_header.at("kid");
    });
    if (found == jwks.at("keys").end()) {
        return false;
    }
    const auto public_key = from_base64url(found->at("x"));
    const auto signature = from_base64url(parts[2]);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519,
                                    nullptr,
                                    reinterpret_cast<const unsigned char*>(public_key.data()),
                                    public_key.size()),
        EVP_PKEY_free);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                    EVP_MD_CTX_free);
    const std::string signed_value = parts[0] + "." + parts[1];
    return key && context &&
           EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) == 1 &&
           EVP_DigestVerify(context.get(),
                            reinterpret_cast<const unsigned char*>(signature.data()),
                            signature.size(),
                            reinterpret_cast<const unsigned char*>(signed_value.data()),
                            signed_value.size()) == 1;
}

json token_claims(const std::string& token) {
    const auto parts = token_parts(token);
    const auto claims = from_base64url(parts.at(1));
    return json::parse(std::string(reinterpret_cast<const char*>(claims.data()), claims.size()));
}

TEST(IdentityExit, PostgresProvesIdentityPkiAndDynamicSecrets) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    const std::filesystem::path identity_home =
        lake::test::fixture_path("identity-exit-liquid-home");
    ASSERT_EQ(setenv("LIQUID_HOME", identity_home.c_str(), 1), 0);
    auto signer_result = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    ASSERT_TRUE(signer_result) << signer_result.error().message;
    liquid::signer signer = std::move(*signer_result);
    const liquid::actor actor = signer.identity();
    const std::filesystem::path document_path =
        identity_home / "identity" / (actor.fingerprint + ".identity.cbor");
    std::ifstream document_input(document_path, std::ios::binary);
    ASSERT_TRUE(document_input);
    std::vector<std::byte> document;
    for (char value; document_input.get(value);) {
        document.push_back(std::byte{static_cast<unsigned char>(value)});
    }
    ASSERT_FALSE(document.empty());

    auto store = lake::control::make_postgres_store(uri, std::string(instance_kek));
    lake::control::pki::Service pki;
    Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    ApiContext context;
    context.store = store.get();
    context.instance_kek = std::string(instance_kek);
    context.public_host = "lake.example.test";
    context.request_origin = "https://lake.example.test";
    context.pki = &pki;
    const auto call = [&](const std::string& name, json input = json::object()) {
        context.request_id = lake::control::new_uuid_v4();
        return registry.call(name, input, context);
    };

    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const ApiResult signed_up = call("sign_up_password",
                                     {{"email", "identity-exit-" + suffix + "@example.test"},
                                      {"password", "correct horse battery staple"},
                                      {"display_name", "Identity exit"},
                                      {"identity_document", base64url(document)}});
    ASSERT_TRUE(signed_up.ok) << signed_up.error.message;
    const std::int64_t issued_at = std::chrono::duration_cast<std::chrono::seconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();
    const std::string nonce = "identity-exit-nonce-" + suffix;
    const std::string assertion_body = "lake\n" + nonce + "\n" + std::to_string(issued_at);
    std::vector<std::byte> assertion_bytes(
        reinterpret_cast<const std::byte*>(assertion_body.data()),
        reinterpret_cast<const std::byte*>(assertion_body.data() + assertion_body.size()));
    auto signature_result =
        liquid::runtime::block_on(signer.sign("lake_sign_in", std::move(assertion_bytes)));
    ASSERT_TRUE(signature_result) << signature_result.error().message;
    const liquid::signature& signature = *signature_result;
    const json signature_json = {{"algorithm", signature.algorithm},
                                 {"signing_key_identifier", signature.signing_key_identifier},
                                 {"signing_key_label", signature.signing_key_label},
                                 {"signature_purpose", signature.signature_purpose},
                                 {"signed_at", signature.signed_at},
                                 {"bytes", base64url(signature.bytes)}};
    const json sign_in_input = {{"identity_document", base64url(document)},
                                {"nonce", nonce},
                                {"issued_at", issued_at},
                                {"signature", signature_json}};
    const ApiResult signed_in = call("sign_in_taap", sign_in_input);
    ASSERT_TRUE(signed_in.ok) << signed_in.error.message;
    EXPECT_EQ(signed_in.value.at("session").at("kind"), "taap");
    const ApiResult replayed = call("sign_in_taap", sign_in_input);
    EXPECT_FALSE(replayed.ok);
    EXPECT_EQ(replayed.error.code, "unauthorized");

    const auto principal = store->context_for(signed_in.value.at("secret"));
    ASSERT_TRUE(principal) << principal.error().message;
    const auto customer = lake::test::personal_customer(*store, principal->user_id);
    ASSERT_TRUE(customer) << customer.error().message;
    const auto organization = store->create_organization(
        {"taap:1:ed25519:identity-exit-org-" + suffix, "Identity exit", customer->id});
    ASSERT_TRUE(organization) << organization.error().message;
    const auto customer_object = store->find_policy_object("customer", customer->id);
    ASSERT_TRUE(customer_object) << customer_object.error().message;
    const auto grants = store->list_grants({customer_object->id}, principal->taap_id, 10);
    ASSERT_TRUE(grants) << grants.error().message;
    ASSERT_EQ(grants->size(), 1U);
    EXPECT_EQ(grants->front().role, "admin");
    context.authorized = true;
    context.session_credential = true;
    context.acting_user_id = principal->user_id;
    context.acting_identity = principal->taap_id;
    context.authentication_method = "taap";
    context.scopes = {"*"};

    const ApiResult application =
        call("register_application",
             {{"customer_id", customer->id},
              {"name", "Identity exit relying party"},
              {"allowed_origins", json::array({"https://rp.example.test"})},
              {"redirect_uris", json::array({"https://rp.example.test/callback"})}});
    ASSERT_TRUE(application.ok) << application.error.message;
    const std::string application_id = application.value.at("id");
    const std::string verifier = "identity-exit-pkce-verifier-" + suffix;
    const std::string oidc_nonce = "identity-exit-oidc-nonce-" + suffix;
    const ApiResult authorization =
        call("begin_application_authorization",
             {{"application_id", application_id},
              {"redirect_uri", "https://rp.example.test/callback"},
              {"code_challenge", lake::control::identity_crypto::sha256_base64url(verifier)},
              {"code_challenge_method", "S256"},
              {"state", "rp-state"},
              {"nonce", oidc_nonce}});
    ASSERT_TRUE(authorization.ok) << authorization.error.message;
    const ApiResult tokens = call("exchange_application_authorization_code",
                                  {{"application_id", application_id},
                                   {"code", authorization.value.at("code")},
                                   {"redirect_uri", "https://rp.example.test/callback"},
                                   {"code_verifier", verifier}});
    ASSERT_TRUE(tokens.ok) << tokens.error.message;
    const ApiResult jwks = call("application_jwks", {{"application_id", application_id}});
    ASSERT_TRUE(jwks.ok) << jwks.error.message;
    EXPECT_TRUE(verifies_with_jwks(tokens.value.at("id_token"), jwks.value));
    const json claims = token_claims(tokens.value.at("id_token"));
    EXPECT_EQ(claims.at("iss"), "https://lake.example.test/v1/apps/" + application_id);
    EXPECT_EQ(claims.at("aud"), application_id);
    EXPECT_EQ(claims.at("nonce"), oidc_nonce);

    ASSERT_TRUE(call("create_pki_authority", {{"customer_id", customer->id}}).ok);
    const ApiResult certificate = call("issue_agent_certificate",
                                       {{"customer_id", customer->id},
                                        {"taap_id", actor.taap_id},
                                        {"public_key", base64url(actor.public_key)},
                                        {"lifetime_seconds", 300}});
    ASSERT_TRUE(certificate.ok) << certificate.error.message;
    EXPECT_TRUE(
        call("check_agent_certificate", {{"certificate_id", certificate.value.at("id")}}).ok);
    const ApiResult rotated = call("rotate_agent_certificate",
                                   {{"certificate_id", certificate.value.at("id")},
                                    {"public_key", base64url(actor.public_key)},
                                    {"lifetime_seconds", 300}});
    ASSERT_TRUE(rotated.ok) << rotated.error.message;
    EXPECT_EQ(rotated.value.at("renewed_from_id"), certificate.value.at("id"));
    ASSERT_TRUE(
        call("revoke_agent_certificate", {{"certificate_id", certificate.value.at("id")}}).ok);
    const ApiResult revoked_check =
        call("check_agent_certificate", {{"certificate_id", certificate.value.at("id")}});
    EXPECT_FALSE(revoked_check.ok);
    EXPECT_EQ(revoked_check.error.code, "unauthorized");
    EXPECT_TRUE(call("check_agent_certificate", {{"certificate_id", rotated.value.at("id")}}).ok);
    const auto certificate_events = lake::test::audit_events(*store, {.limit = 100});
    ASSERT_TRUE(certificate_events) << certificate_events.error().message;
    const auto count_certificate_events = [&](std::string_view action,
                                              std::string_view outcome = "success") {
        return std::ranges::count_if(*certificate_events, [&](const lake::audit::Event& event) {
            return event.draft.customer_id == customer->id && event.draft.kind == "certificate" &&
                   event.draft.action == action && event.draft.outcome == outcome;
        });
    };
    EXPECT_EQ(count_certificate_events("issue_agent_certificate"), 1);
    EXPECT_EQ(count_certificate_events("rotate_agent_certificate"), 1);
    EXPECT_EQ(count_certificate_events("revoke_agent_certificate"), 1);
    EXPECT_EQ(count_certificate_events("check_agent_certificate"), 2);
    EXPECT_EQ(count_certificate_events("check_agent_certificate", "denied"), 1);

    const ApiResult mount = call("create_secret_mount",
                                 {{"customer_id", customer->id},
                                  {"organization_id", organization->id},
                                  {"name", "identity-exit"}});
    ASSERT_TRUE(mount.ok) << mount.error.message;
    lake::transparency::TiledLog log(lake::test::fixture_path("identity-exit-log"), "lake");
    context.transparency_log = &log;
    const ApiResult lease =
        call("lease_dynamic_secret",
             {{"mount_id", mount.value.at("id")}, {"role", "reader"}, {"lifetime_seconds", 300}});
    ASSERT_TRUE(lease.ok) << lease.error.message;
    const std::string lease_id = lease.value.at("lease").at("id");
    EXPECT_TRUE(lease.value.at("value").contains("username"));
    const ApiResult renewed =
        call("renew_dynamic_secret", {{"lease_id", lease_id}, {"lifetime_seconds", 300}});
    ASSERT_TRUE(renewed.ok) << renewed.error.message;
    EXPECT_EQ(renewed.value.at("renewals"), 1);
    const ApiResult revoked = call("revoke_dynamic_secret", {{"lease_id", lease_id}});
    ASSERT_TRUE(revoked.ok) << revoked.error.message;
    EXPECT_FALSE(revoked.value.at("revoked_at").get<std::string>().empty());

    const auto events =
        lake::test::audit_events(*store, {.object = "dynamic_secret/" + lease_id, .limit = 10});
    ASSERT_TRUE(events) << events.error().message;
    ASSERT_EQ(events->size(), 3);
    const auto all_audit_events = lake::test::audit_events(*store, {.limit = 100});
    ASSERT_TRUE(all_audit_events) << all_audit_events.error().message;
    const auto audit_verified = lake::audit::verify(*store, *all_audit_events);
    ASSERT_TRUE(audit_verified) << audit_verified.error().message;
    const auto state = log.state();
    ASSERT_TRUE(state) << state.error().message;
    ASSERT_EQ(state->tree_size, 3);
    const auto tile = log.tile(0, 3);
    ASSERT_TRUE(tile) << tile.error().message;
    liquid::transparency::tile liquid_tile{.level = 0,
                                           .index = 0,
                                           .width = 3,
                                           .sha256 = hash_from_hex(tile->sha256),
                                           .bytes = from_base64url(tile->encoded)};
    const auto entries = liquid_tile.entries();
    ASSERT_TRUE(entries) << entries.error().message;
    const auto proof = log.inclusion_proof(2, state->tree_size);
    ASSERT_TRUE(proof) << proof.error().message;
    liquid::transparency::checkpoint checkpoint;
    checkpoint.tree_size = state->tree_size;
    checkpoint.entry_tile_width = lake::transparency::transparency_tile_width;
    checkpoint.hash_tile_width = lake::transparency::transparency_tile_width;
    checkpoint.root_hash = hash_from_hex(state->root_hash);
    std::vector<liquid::transparency::hash> audit_path;
    for (const std::string& hash : proof->audit_path) {
        audit_path.push_back(hash_from_hex(hash));
    }
    const liquid::transparency::inclusion_proof liquid_proof{.leaf_index = 2,
                                                             .tree_size = state->tree_size,
                                                             .root_hash = checkpoint.root_hash,
                                                             .audit_path = audit_path,
                                                             .bound_checkpoint = checkpoint};
    const auto verified = liquid_proof.verify(entries->at(2), checkpoint);
    EXPECT_TRUE(verified) << verified.error().message;
}

} // namespace
