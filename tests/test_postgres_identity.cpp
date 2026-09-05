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
#include <pqxx/pqxx>

#include "lake/api/apps.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/postgres_store.hpp"
#include "webauthn_test_support.hpp"

namespace {

constexpr std::string_view test_instance_kek =
    "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

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

TEST(PostgresApplications, CapabilitiesPersistKeysUsersOrganizationsAndWebhooks) {
    const char* uri = std::getenv("LAKE_TEST_DB_URI");
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }

    std::unique_ptr<lake::control::Store> store =
        lake::control::make_postgres_store(uri, std::string(test_instance_kek));
    const std::string suffix = std::to_string(static_cast<long long>(getpid()));
    const auto owner = store->create_user({"taap:1:ed25519:postgres-app-owner-" + suffix,
                                           "postgres-app-owner-" + suffix + "@example.test",
                                           "Application owner"});
    ASSERT_TRUE(owner) << owner.error().message;
    const auto customer =
        store->create_customer({"individual", owner->id, "", "postgres-app-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;

    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::ApiContext context;
    context.store = store.get();
    context.authorized = true;
    context.operator_credential = true;
    context.acting_identity = "admin";
    context.instance_kek = std::string(test_instance_kek);
    context.public_host = "lake.example.test";
    context.request_origin = "https://lake.example.test";
    context.webhook_sender = [](const std::string&, const nlohmann::json&, const std::string&) {
        return lake::api::WebhookDeliveryResult{.status = 204};
    };
    const auto call = [&](const std::string& name,
                          nlohmann::json input = nlohmann::json::object()) {
        context.request_id = lake::control::new_uuid_v4();
        return registry.call(name, input, context);
    };

    const auto registered =
        call("register_application",
             {{"customer_id", customer->id},
              {"name", "Postgres application"},
              {"allowed_origins", nlohmann::json::array({"https://postgres-client.example.test"})},
              {"redirect_uris",
               nlohmann::json::array({"https://postgres-client.example.test/callback"})},
              {"custom_sign_in_domain", "login-" + suffix + ".example.test"},
              {"theme", {{"accent_color", "#445566"}}}});
    ASSERT_TRUE(registered.ok) << registered.error.message;
    const std::string application_id = registered.value.at("id");
    const std::string pool_id = registered.value.at("pool_id");
    EXPECT_TRUE(call("get_application", {{"id", application_id}}).ok);
    EXPECT_TRUE(call("list_applications", {{"customer_id", customer->id}}).ok);
    EXPECT_TRUE(call("update_application_theme",
                     {{"application_id", application_id}, {"theme", {{"accent_color", "#778899"}}}})
                    .ok);
    EXPECT_TRUE(call("rotate_application_signing_key", {{"application_id", application_id}}).ok);
    const auto jwks = call("application_jwks", {{"application_id", application_id}});
    ASSERT_TRUE(jwks.ok) << jwks.error.message;
    EXPECT_EQ(jwks.value.at("keys").size(), 2);

    const auto webhook =
        call("create_application_webhook",
             {{"application_id", application_id},
              {"url", "https://hooks.example.test/postgres"},
              {"events", nlohmann::json::array({"session.started", "organization.created"})}});
    ASSERT_TRUE(webhook.ok) << webhook.error.message;
    EXPECT_TRUE(call("list_application_webhooks", {{"application_id", application_id}}).ok);

    const auto sign_up =
        call("sign_up_password",
             {{"pool_id", pool_id},
              {"email", "postgres-app-user-" + suffix + "@example.test"},
              {"password", "correct horse battery staple"},
              {"display_name", "Postgres application user"},
              {"identity_document", signed_identity_document("application-" + suffix)}});
    ASSERT_TRUE(sign_up.ok) << sign_up.error.message;
    EXPECT_TRUE(sign_up.value.contains("access_token"));
    const auto users = call("list_application_users", {{"application_id", application_id}});
    ASSERT_TRUE(users.ok) << users.error.message;
    ASSERT_EQ(users.value.at("items").size(), 1);
    const std::string user_id = users.value.at("items").at(0).at("user_id");
    EXPECT_TRUE(
        call("export_application_user", {{"application_id", application_id}, {"user_id", user_id}})
            .ok);
    EXPECT_TRUE(
        call("issue_application_jwt", {{"application_id", application_id}, {"user_id", user_id}})
            .ok);

    const auto organization = call("create_application_organization",
                                   {{"application_id", application_id},
                                    {"taap_id", "taap:1:ed25519:postgres-app-org-" + suffix},
                                    {"name", "Postgres organization"},
                                    {"metadata", {{"plan", "test"}}}});
    ASSERT_TRUE(organization.ok) << organization.error.message;
    const std::string organization_id = organization.value.at("id");
    EXPECT_TRUE(call("list_application_organizations", {{"application_id", application_id}}).ok);
    EXPECT_TRUE(call("update_application_organization",
                     {{"application_id", application_id},
                      {"organization_id", organization_id},
                      {"name", "Updated organization"},
                      {"metadata", {{"plan", "updated"}}}})
                    .ok);

    const auto events =
        call("list_application_webhook_events", {{"application_id", application_id}});
    ASSERT_TRUE(events.ok) << events.error.message;
    ASSERT_FALSE(events.value.at("items").empty());
    const auto subscribed = std::ranges::find_if(events.value.at("items"), [](const auto& event) {
        return event.at("event") == "organization.created";
    });
    ASSERT_NE(subscribed, events.value.at("items").end());
    EXPECT_TRUE(call("retry_application_webhook",
                     {{"application_id", application_id},
                      {"event_id", subscribed->at("id")},
                      {"webhook_id", webhook.value.at("id")}})
                    .ok);

    EXPECT_TRUE(call("delete_application_organization",
                     {{"application_id", application_id}, {"organization_id", organization_id}})
                    .ok);
    EXPECT_TRUE(
        call("suspend_application_user", {{"application_id", application_id}, {"user_id", user_id}})
            .ok);
    EXPECT_FALSE(call("sign_in_password",
                      {{"pool_id", pool_id},
                       {"email", "postgres-app-user-" + suffix + "@example.test"},
                       {"password", "correct horse battery staple"}})
                     .ok);
    EXPECT_TRUE(
        call("delete_application_user", {{"application_id", application_id}, {"user_id", user_id}})
            .ok);
}

TEST(PostgresIdentity, SessionsTokensAndDeviceRevocation) {
    const char* uri = std::getenv("LAKE_TEST_DB_URI");
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }

    std::unique_ptr<lake::control::Store> store =
        lake::control::make_postgres_store(uri, std::string(test_instance_kek));
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
    std::unique_ptr<lake::control::Store> store =
        lake::control::make_postgres_store(uri, std::string(test_instance_kek));
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

TEST(PostgresOfflineRecovery, GrantsRecoveryPathsAndRepositoryKeyCustody) {
    const char* uri = std::getenv("LAKE_TEST_DB_URI");
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    std::unique_ptr<lake::control::Store> store =
        lake::control::make_postgres_store(uri, std::string(test_instance_kek));
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto create_account = [&](const std::string& label) {
        return store->sign_up_password({"lake",
                                        label + "-" + suffix + "@example.test",
                                        "correct horse battery staple",
                                        label,
                                        label + " integration device",
                                        signed_identity_document(label + "-" + suffix)});
    };
    const auto target = create_account("offline-target");
    const auto first_admin = create_account("offline-admin-one");
    const auto second_admin = create_account("offline-admin-two");
    const auto contact = create_account("offline-contact");
    ASSERT_TRUE(target) << target.error().message;
    ASSERT_TRUE(first_admin) << first_admin.error().message;
    ASSERT_TRUE(second_admin) << second_admin.error().message;
    ASSERT_TRUE(contact) << contact.error().message;
    const auto target_principal = store->context_for(target->secret);
    const auto first_principal = store->context_for(first_admin->secret);
    const auto second_principal = store->context_for(second_admin->secret);
    const auto contact_principal = store->context_for(contact->secret);
    ASSERT_TRUE(target_principal) << target_principal.error().message;
    ASSERT_TRUE(first_principal) << first_principal.error().message;
    ASSERT_TRUE(second_principal) << second_principal.error().message;
    ASSERT_TRUE(contact_principal) << contact_principal.error().message;

    const auto context_for = [&](const lake::control::Principal& principal) {
        lake::api::ApiContext context;
        context.store = store.get();
        context.authorized = true;
        context.session_credential = true;
        context.acting_user_id = principal.user_id;
        context.acting_identity = principal.taap_id;
        context.credential_device_id = principal.device_id;
        context.scopes = principal.scopes;
        context.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
        return context;
    };
    lake::api::ApiContext target_context = context_for(*target_principal);
    lake::api::ApiContext first_context = context_for(*first_principal);
    lake::api::ApiContext second_context = context_for(*second_principal);
    lake::api::ApiContext contact_context = context_for(*contact_principal);
    lake::api::ApiContext anonymous;
    anonymous.store = store.get();
    anonymous.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    lake::api::ApiContext operator_context = anonymous;
    operator_context.authorized = true;
    operator_context.operator_credential = true;
    operator_context.acting_identity = "admin";

    const auto customer =
        store->create_customer({"individual", target_principal->user_id, "", "offline-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository =
        store->create_repository({customer->id, "encrypted-" + suffix, "", "private"});
    ASSERT_TRUE(repository) << repository.error().message;
    const auto object = store->find_policy_object("repository", repository->id);
    ASSERT_TRUE(object) << object.error().message;
    ASSERT_TRUE(store->create_grant({target_principal->taap_id,
                                     object->id,
                                     "write",
                                     "offline-postgres-attestation",
                                     "admin",
                                     "offline-postgres"}));
    const std::string scope = "repository:" + repository->id + ":read";
    const lake::api::ApiResult grant = registry.call("issue_offline_grant",
                                                     {{"device_id", target_principal->device_id},
                                                      {"scopes", nlohmann::json::array({scope})},
                                                      {"duration_seconds", 3600}},
                                                     target_context);
    ASSERT_TRUE(grant.ok) << grant.error.message;
    EXPECT_TRUE(registry.call("list_offline_grants", nlohmann::json::object(), target_context).ok);
    EXPECT_TRUE(registry
                    .call("check_cached_read",
                          {{"credential", grant.value["credential"]}, {"scope", scope}},
                          anonymous)
                    .ok);
    const lake::api::ApiResult revoked =
        registry.call("revoke_offline_grant", {{"id", grant.value["id"]}}, target_context);
    ASSERT_TRUE(revoked.ok) << revoked.error.message;
    EXPECT_TRUE(registry
                    .call("check_cached_read",
                          {{"credential", grant.value["credential"]}, {"scope", scope}},
                          anonymous)
                    .ok);

    const auto enrolled = store->enroll_mfa(target_principal->user_id, "lake");
    ASSERT_TRUE(enrolled) << enrolled.error().message;
    const lake::api::ApiResult code =
        registry.call("recover_with_code",
                      {{"email", "offline-target-" + suffix + "@example.test"},
                       {"code", enrolled->recovery_codes.front()}},
                      anonymous);
    ASSERT_TRUE(code.ok) << code.error.message;
    EXPECT_TRUE(store->context_for(code.value["secret"]));

    const lake::api::ApiResult email =
        registry.call("begin_email_recovery",
                      {{"email", "offline-target-" + suffix + "@example.test"}},
                      anonymous);
    ASSERT_TRUE(email.ok) << email.error.message;
    {
        pqxx::connection connection(uri);
        pqxx::work transaction(connection);
        transaction.exec("UPDATE offline_recovery_attempts SET ready_at = now() - interval '1 "
                         "second' WHERE id = $1::uuid",
                         pqxx::params{email.value["id"].get<std::string>()});
        transaction.commit();
    }
    const lake::api::ApiResult email_claimed =
        registry.call("claim_recovery_deliveries", {{"limit", 1000}}, operator_context);
    ASSERT_TRUE(email_claimed.ok) << email_claimed.error.message;
    const auto email_delivery =
        std::ranges::find_if(email_claimed.value["items"], [&](const nlohmann::json& item) {
            return item["recovery_id"] == email.value["id"];
        });
    ASSERT_NE(email_delivery, email_claimed.value["items"].end());
    const lake::api::ApiResult email_restored =
        registry.call("complete_recovery",
                      {{"recovery_id", email.value["id"]}, {"token", (*email_delivery)["token"]}},
                      anonymous);
    ASSERT_TRUE(email_restored.ok) << email_restored.error.message;
    EXPECT_TRUE(store->context_for(email_restored.value["secret"]));

    const auto organization = store->create_organization(
        {"taap:1:ed25519:offline-org-" + suffix, "Offline recovery organization", ""});
    ASSERT_TRUE(organization) << organization.error().message;
    const auto add_member = [&](const lake::control::Principal& principal,
                                const std::string& role) {
        return store->create_membership({organization->id,
                                         "",
                                         "user",
                                         principal.user_id,
                                         principal.taap_id,
                                         role,
                                         "offline-membership-attestation",
                                         "admin",
                                         "offline-membership"});
    };
    ASSERT_TRUE(add_member(*target_principal, "member"));
    ASSERT_TRUE(add_member(*first_principal, "admin"));
    ASSERT_TRUE(add_member(*second_principal, "admin"));
    const lake::api::ApiResult admin_started = registry.call(
        "begin_admin_recovery",
        {{"organization_id", organization->id}, {"user_id", target_principal->user_id}},
        first_context);
    ASSERT_TRUE(admin_started.ok) << admin_started.error.message;
    EXPECT_EQ(registry
                  .call("approve_admin_recovery",
                        {{"recovery_id", admin_started.value["id"]}},
                        first_context)
                  .error.code,
              "conflict");
    const lake::api::ApiResult admin_approved = registry.call(
        "approve_admin_recovery", {{"recovery_id", admin_started.value["id"]}}, second_context);
    ASSERT_TRUE(admin_approved.ok) << admin_approved.error.message;
    EXPECT_EQ(admin_approved.value["approval_count"], 2);
    const lake::api::ApiResult admin_claimed =
        registry.call("claim_recovery_deliveries", {{"limit", 1000}}, operator_context);
    ASSERT_TRUE(admin_claimed.ok) << admin_claimed.error.message;
    const auto admin_delivery =
        std::ranges::find_if(admin_claimed.value["items"], [&](const nlohmann::json& item) {
            return item["recovery_id"] == admin_started.value["id"];
        });
    ASSERT_NE(admin_delivery, admin_claimed.value["items"].end());
    const lake::api::ApiResult admin_restored = registry.call(
        "complete_recovery",
        {{"recovery_id", admin_started.value["id"]}, {"token", (*admin_delivery)["token"]}},
        anonymous);
    ASSERT_TRUE(admin_restored.ok) << admin_restored.error.message;
    EXPECT_TRUE(store->context_for(admin_restored.value["secret"]));
    EXPECT_TRUE(
        registry.call("list_recovery_notifications", nlohmann::json::object(), target_context).ok);

    const lake::api::ApiResult missing = registry.call(
        "repository_recovery_status", {{"repository_id", repository->id}}, operator_context);
    ASSERT_TRUE(missing.ok) << missing.error.message;
    EXPECT_FALSE(missing.value["recoverable"]);
    const lake::api::ApiResult configured =
        registry.call("configure_recovery_contact",
                      {{"repository_id", repository->id},
                       {"contact_taap_id", contact_principal->taap_id},
                       {"wrapped_key", "postgres-client-wrapped-key"}},
                      operator_context);
    ASSERT_TRUE(configured.ok) << configured.error.message;
    const lake::api::ApiResult recovered = registry.call(
        "recover_repository_key", {{"repository_id", repository->id}}, contact_context);
    ASSERT_TRUE(recovered.ok) << recovered.error.message;
    EXPECT_EQ(recovered.value["wrapped_key"], "postgres-client-wrapped-key");
}

} // namespace
