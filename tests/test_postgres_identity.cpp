#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include <gtest/gtest.h>

#include <liquid/identity.hpp>
#include <liquid/runtime.hpp>

#include <openssl/evp.h>

#include "lake/audit/audit.hpp"
#include "lake/control/postgres_store.hpp"
#include "webauthn_test_support.hpp"

namespace {

std::string base64url(const std::vector<std::byte>& bytes) {
    const auto* input = reinterpret_cast<const unsigned char*>(bytes.data());
    const std::size_t size = bytes.size();
    std::string encoded(((size + 2) / 3) * 4, '\0');
    const int encoded_size = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()), input, static_cast<int>(size));
    encoded.resize(static_cast<std::size_t>(encoded_size));
    while (encoded.ends_with('=')) {
        encoded.pop_back();
    }
    std::replace(encoded.begin(), encoded.end(), '+', '-');
    std::replace(encoded.begin(), encoded.end(), '/', '_');
    return encoded;
}

std::string signed_identity_document(std::string_view label) {
    const std::filesystem::path home =
        std::filesystem::temp_directory_path() /
        ("lake-identity-public-api-" + std::to_string(getpid()) + "-" + std::string(label));
    std::error_code error;
    std::filesystem::remove_all(home, error);
    setenv("LIQUID_HOME", home.c_str(), 1);
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    EXPECT_TRUE(signer) << signer.error().message;
    const std::filesystem::path document_path =
        home / "identity" / ((signer ? signer->identity().fingerprint : "") + ".identity.cbor");
    std::ifstream input(document_path, std::ios::binary);
    std::vector<std::byte> document;
    for (char character; input.get(character);) {
        document.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    const auto decoded = liquid::decode_identity_document(document);
    EXPECT_TRUE(decoded) << decoded.error().message;
    if (decoded) {
        const auto verified = liquid::verify_document(*decoded);
        EXPECT_TRUE(verified && *verified);
    }
    std::filesystem::remove_all(home, error);
    return base64url(document);
}

TEST(PostgresIdentity, SessionsTokensAndDeviceRevocation) {
    const char* uri = std::getenv("LAKE_TEST_DB_URI");
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }

    std::unique_ptr<lake::control::Store> store = lake::control::make_postgres_store(uri);
    const std::string email = "postgres-identity-" + std::to_string(getpid()) + "@example.test";
    const auto sign_up = store->sign_up_password({"lake",
                                                  email,
                                                  "correct horse battery staple",
                                                  "Postgres identity",
                                                  "integration test",
                                                  signed_identity_document("password")});
    ASSERT_TRUE(sign_up) << sign_up.error().message;

    const auto context = store->context_for(sign_up->secret);
    ASSERT_TRUE(context) << context.error().message;
    EXPECT_EQ(context->user_id, sign_up->session.user_id);

    const auto token = store->create_token(
        {context->user_id, "lake", "read sessions", "", {"list_sessions"}, 3600, false});
    ASSERT_TRUE(token) << token.error().message;
    EXPECT_TRUE(store->context_for(token->secret));

    const auto enrolled = store->enroll_mfa(context->user_id, "lake");
    ASSERT_TRUE(enrolled) << enrolled.error().message;
    ASSERT_EQ(enrolled->recovery_codes.size(), 8);
    const auto mfa_required = store->sign_in_password(
        {"lake", email, "correct horse battery staple", "", "integration test"});
    ASSERT_FALSE(mfa_required);
    EXPECT_EQ(mfa_required.error().code, "mfa_required");
    const auto verified = store->sign_in_totp({"lake",
                                               email,
                                               "correct horse battery staple",
                                               enrolled->recovery_codes[0],
                                               "integration test"});
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_TRUE(store->context_for(verified->secret));
    EXPECT_FALSE(store->sign_in_totp({"lake",
                                      email,
                                      "correct horse battery staple",
                                      enrolled->recovery_codes[0],
                                      "integration test"}));
    EXPECT_FALSE(store->disable_mfa(context->user_id, "lake", enrolled->recovery_codes[0]));
    const auto disabled = store->disable_mfa(context->user_id, "lake", enrolled->recovery_codes[1]);
    ASSERT_TRUE(disabled) << disabled.error().message;
    EXPECT_TRUE(store->sign_in_password(
        {"lake", email, "correct horse battery staple", "", "integration test"}));

    const std::string provider_name = "stub-" + std::to_string(static_cast<long long>(getpid()));
    const auto registered = store->register_oauth_provider({"lake",
                                                            provider_name,
                                                            "https://issuer.example.test",
                                                            "lake-test",
                                                            "oauth-client-secret",
                                                            {"openid", "email"}});
    ASSERT_TRUE(registered) << registered.error().message;
    const auto provider = store->get_oauth_provider("lake", provider_name);
    ASSERT_TRUE(provider) << provider.error().message;
    EXPECT_EQ(provider->client_secret, "oauth-client-secret");
    const std::string state = "state-" + std::to_string(getpid());
    const auto started = store->begin_oauth({"lake",
                                             provider_name,
                                             state,
                                             "pkce-verifier",
                                             "oidc-nonce",
                                             "integration browser",
                                             signed_identity_document("oauth")});
    ASSERT_TRUE(started) << started.error().message;
    const auto flow = store->consume_oauth(provider_name, state);
    ASSERT_TRUE(flow) << flow.error().message;
    EXPECT_EQ(flow->code_verifier, "pkce-verifier");
    EXPECT_EQ(flow->nonce, "oidc-nonce");
    EXPECT_FALSE(store->consume_oauth(provider_name, state));
    const auto oauth =
        store->sign_in_oauth({"lake",
                              provider_name,
                              "provider-subject",
                              "postgres-oauth-" + std::to_string(getpid()) + "@example.test",
                              "Postgres OAuth",
                              "integration browser",
                              flow->identity_document});
    ASSERT_TRUE(oauth) << oauth.error().message;
    const auto oauth_user = store->get_user(oauth->session.user_id);
    ASSERT_TRUE(oauth_user) << oauth_user.error().message;
    EXPECT_FALSE(oauth_user->taap_id.empty());

    lake::test::SoftwareAuthenticator authenticator;
    const auto registration = store->begin_passkey_registration(
        context->user_id, "lake", "lake.example", "https://lake.example");
    ASSERT_TRUE(registration) << registration.error().message;
    const auto attestation = authenticator.registration(
        registration->challenge, registration->rp_id, registration->origin);
    const auto passkey = store->register_passkey(
        {context->user_id,
         "lake",
         registration->challenge,
         attestation.client_data_json,
         attestation.authenticator_data,
         "integration passkey",
         lake::audit::IdentityOptions{
             .instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"}});
    ASSERT_TRUE(passkey) << passkey.error().message;
    EXPECT_FALSE(store->register_passkey({context->user_id,
                                          "lake",
                                          registration->challenge,
                                          attestation.client_data_json,
                                          attestation.authenticator_data,
                                          "integration passkey"}));
    const auto devices = store->list_devices(context->user_id);
    ASSERT_TRUE(devices) << devices.error().message;
    const auto passkey_device = std::ranges::find_if(
        *devices, [](const auto& device) { return !device.passkey_credential_id.empty(); });
    ASSERT_NE(passkey_device, devices->end());
    EXPECT_EQ(passkey_device->passkey_credential_id, attestation.credential_id);
    EXPECT_TRUE(lake::audit::verify_passkey_binding(*store,
                                                    passkey_device->passkey_binding_attestation,
                                                    context->taap_id,
                                                    attestation.credential_id,
                                                    "lake"));

    const auto assertion =
        store->begin_passkey_sign_in("lake", "lake.example", "https://lake.example");
    ASSERT_TRUE(assertion) << assertion.error().message;
    const auto signed_assertion =
        authenticator.assertion(assertion->challenge, assertion->rp_id, assertion->origin, 1);
    const auto passkey_session = store->sign_in_passkey({"lake",
                                                         assertion->challenge,
                                                         signed_assertion.credential_id,
                                                         signed_assertion.client_data_json,
                                                         signed_assertion.authenticator_data,
                                                         signed_assertion.signature,
                                                         "integration authenticator"});
    ASSERT_TRUE(passkey_session) << passkey_session.error().message;
    EXPECT_TRUE(store->context_for(passkey_session->secret));
    EXPECT_FALSE(store->sign_in_passkey({"lake",
                                         assertion->challenge,
                                         signed_assertion.credential_id,
                                         signed_assertion.client_data_json,
                                         signed_assertion.authenticator_data,
                                         signed_assertion.signature,
                                         "integration authenticator"}));

    const auto revoke = store->revoke_device(context->user_id, context->device_id);
    ASSERT_TRUE(revoke) << revoke.error().message;
    EXPECT_FALSE(store->context_for(sign_up->secret));
}

TEST(PostgresIdentity, SsoFederationAndScimDeactivation) {
    const char* uri = std::getenv("LAKE_TEST_DB_URI");
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    setenv(
        "LAKE_INSTANCE_KEK", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 1);
    std::unique_ptr<lake::control::Store> store = lake::control::make_postgres_store(uri);
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto organization = store->create_organization(
        {"taap:1:ed25519:sso-org-" + suffix, "SSO integration organization", ""});
    ASSERT_TRUE(organization) << organization.error().message;
    const std::string scim_token = "lake-stub-scim-token-" + suffix;

    const auto provider = store->register_sso_provider({organization->id,
                                                        "lake",
                                                        "stub-sso-" + suffix,
                                                        "oidc",
                                                        "https://oidc.stub.example/" + suffix,
                                                        "",
                                                        "",
                                                        "lake-stub-client",
                                                        "lake-stub-secret",
                                                        scim_token,
                                                        {{"email", "mail"}},
                                                        false});
    ASSERT_TRUE(provider) << provider.error().message;
    const auto authenticated = store->authenticate_scim(scim_token);
    ASSERT_TRUE(authenticated) << authenticated.error().message;
    EXPECT_EQ(authenticated->id, provider->id);
    EXPECT_FALSE(store->authenticate_scim("wrong-scim-token"));
    ASSERT_TRUE(store->test_sso_provider(provider->id));
    ASSERT_TRUE(store->create_federation_issuer({organization->id, provider->issuer, "email"}));

    const auto created = store->upsert_scim_user({provider->id,
                                                  "stub-user-" + suffix,
                                                  "sso-user-" + suffix + "@example.test",
                                                  "SSO user",
                                                  "taap:1:ed25519:sso-user-" + suffix,
                                                  true,
                                                  {{"department", "engineering"}}});
    ASSERT_TRUE(created) << created.error().message;
    const auto updated = store->upsert_scim_user({provider->id,
                                                  "stub-user-" + suffix,
                                                  "sso-user-" + suffix + "@example.test",
                                                  "Updated SSO user",
                                                  "taap:1:ed25519:sso-user-" + suffix,
                                                  true,
                                                  {{"department", "identity"}}});
    ASSERT_TRUE(updated) << updated.error().message;
    EXPECT_EQ(updated->user_id, created->user_id);
    const auto users = store->list_scim_users(provider->id, 100);
    ASSERT_TRUE(users) << users.error().message;
    ASSERT_EQ(users->size(), 1);
    EXPECT_EQ(users->at(0).external_id, created->external_id);
    const auto read_user = store->get_scim_user(provider->id, created->user_id);
    ASSERT_TRUE(read_user) << read_user.error().message;
    EXPECT_EQ(read_user->external_id, created->external_id);

    const auto group = store->upsert_scim_group({provider->id,
                                                 "stub-group-" + suffix,
                                                 "stub-sso-team-" + suffix,
                                                 "SSO team",
                                                 {"stub-user-" + suffix},
                                                 {}});
    ASSERT_TRUE(group) << group.error().message;
    const auto groups = store->list_scim_groups(provider->id, 100);
    ASSERT_TRUE(groups) << groups.error().message;
    ASSERT_EQ(groups->size(), 1);
    const auto read_group = store->get_scim_group(provider->id, group->team_id);
    ASSERT_TRUE(read_group) << read_group.error().message;
    EXPECT_EQ(read_group->external_id, group->external_id);
    const auto issued = store->sign_in_sso({provider->id,
                                            provider->issuer,
                                            "stub-user-" + suffix,
                                            "sso-user-" + suffix + "@example.test",
                                            "Updated SSO user",
                                            "",
                                            "integration browser",
                                            {{"email", "sso-user-" + suffix + "@example.test"}}});
    ASSERT_TRUE(issued) << issued.error().message;
    EXPECT_TRUE(store->context_for(issued->secret));

    const auto deactivated = store->upsert_scim_user({provider->id,
                                                      "stub-user-" + suffix,
                                                      "sso-user-" + suffix + "@example.test",
                                                      "Updated SSO user",
                                                      "taap:1:ed25519:sso-user-" + suffix,
                                                      false,
                                                      {}});
    ASSERT_TRUE(deactivated) << deactivated.error().message;
    EXPECT_FALSE(store->context_for(issued->secret));
    ASSERT_TRUE(store->delete_scim_user(provider->id, created->user_id));
    ASSERT_TRUE(store->delete_scim_group(provider->id, group->team_id));
    EXPECT_FALSE(store->get_scim_group(provider->id, group->team_id));
    const auto logs = store->list_sso_provisioning_logs(provider->id, 100);
    ASSERT_TRUE(logs) << logs.error().message;
    EXPECT_TRUE(std::ranges::any_of(*logs, [](const lake::control::ProvisioningLog& log) {
        return log.resource == "user" && log.operation == "deactivate";
    }));
}

} // namespace
