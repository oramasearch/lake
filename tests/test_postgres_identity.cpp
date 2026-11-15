#include "postgres_fixture.hpp"
#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <string>
#include <thread>

#include <unistd.h>

#include <gtest/gtest.h>

#include <liquid/identity.hpp>
#include <liquid/runtime.hpp>

#include <openssl/evp.h>
#include <pqxx/pqxx>

#include "identity_proof_test_support.hpp"
#include "lake/api/apps.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/handles.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/thread.hpp"
#include "personal_customer_test_support.hpp"
#include "repository_test_support.hpp"
#include "stream_fixture.hpp"
#include "temporary_directory.hpp"
#include "webauthn_test_support.hpp"

namespace {

constexpr std::string_view test_instance_kek =
    "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

// A new identity's document, and a proof of its key for sign-up or a first OAuth sign-in in
// `pool_id` (lake#931).
struct ProvenDocument {
    std::string document;
    lake::control::IdentityProof proof;
};

ProvenDocument
proven_document(lake::control::Store& store, const std::string& pool_id, const std::string& label) {
    lake::test::TestIdentity identity = lake::test::test_identity(label);
    return {identity.document, lake::test::prove_identity(store, pool_id, identity)};
}

// A Lake-pool password sign-up with a proven document.
lake::control::NewPasswordUser proven_sign_up(lake::control::Store& store,
                                              std::string email,
                                              std::string display_name,
                                              std::string device_name,
                                              const std::string& label) {
    ProvenDocument proven = proven_document(store, "lake", label);
    lake::control::NewPasswordUser user{"lake",
                                        std::move(email),
                                        "correct horse battery staple",
                                        std::move(display_name),
                                        std::move(device_name),
                                        std::move(proven.document)};
    user.identity_proof = std::move(proven.proof);
    return user;
}

lake::api::ApiResult claim_recovery_delivery(lake::api::Registry& registry,
                                             lake::api::ApiContext& context,
                                             const std::string& recovery_id) {
    for (;;) {
        const auto page = registry.call("claim_recovery_deliveries", {{"limit", 1}}, context);
        if (!page.ok) {
            return page;
        }
        const auto& items = page.value.at("items");
        for (const auto& item : items) {
            if (item.at("recovery_id") == recovery_id) {
                return lake::api::ApiResult::success(item);
            }
        }
        if (items.empty()) {
            return lake::api::ApiResult::failure("not_found", "recovery delivery was not queued");
        }
    }
}

class PostgresProvisioning : public testing::Test {
protected:
    void SetUp() override {
        const char* configured_uri = lake::test::postgres_uri();
        if (configured_uri == nullptr || *configured_uri == '\0') {
            GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
        }
        uri = configured_uri;
        store = lake::control::make_postgres_store(uri, std::string(test_instance_kek));
        input = proven_sign_up(*store,
                               "provisioning-" + lake::control::new_uuid_v4() + "@example.test",
                               "Personal customer",
                               "browser",
                               lake::control::new_uuid_v4());
    }

    std::string uri;
    std::unique_ptr<lake::control::Store> store;
    lake::control::NewPasswordUser input;
};

TEST_F(PostgresProvisioning, MfaVerificationLocksTheAccountBeforeItsFactors) {
    const auto signed_up = store->sign_up_password(input);
    ASSERT_TRUE(signed_up) << signed_up.error().message;
    const auto factor = store->enroll_mfa(
        signed_up->session.user_id, "lake", {signed_up->session.id, input.password});
    ASSERT_TRUE(factor) << factor.error().message;
    auto authenticator = lake::control::make_postgres_store(uri, std::string(test_instance_kek));
    std::optional<lake::control::Result<lake::control::IssuedSession>> authenticated;
    pqxx::connection blocker(uri), observer(uri);
    lake::Thread verifier;
    bool waiting = false;
    bool factor_available = false;
    std::string factor_error;
    {
        // Account deletion holds this row before removing credentials. MFA must wait without
        // holding a factor row which deletion would need in turn.
        pqxx::work deleting(blocker);
        ASSERT_EQ(deleting
                      .exec_params("SELECT 1 FROM users WHERE id = $1::uuid FOR UPDATE",
                                   signed_up->session.user_id)
                      .size(),
                  1u);
        verifier = lake::Thread([&] {
            authenticated = authenticator->sign_in_totp({"lake",
                                                         input.email,
                                                         input.password,
                                                         factor->recovery_codes[0],
                                                         "lock-order test"});
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                pqxx::read_transaction read(observer);
                waiting =
                    read.exec("SELECT EXISTS (SELECT 1 FROM pg_stat_activity "
                              "WHERE datname = current_database() AND pid <> pg_backend_pid() "
                              "AND wait_event_type = 'Lock')")[0][0]
                        .as<bool>();
            }
            if (waiting)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        try {
            factor_available =
                !deleting
                     .exec_params("SELECT 1 FROM credentials WHERE user_id = $1::uuid AND kind IN "
                                  "('password', 'totp', 'recovery') "
                                  "AND revoked_at IS NULL FOR UPDATE NOWAIT",
                                  signed_up->session.user_id)
                     .empty();
        } catch (const pqxx::sql_error& error) {
            factor_error = error.what();
        }
        deleting.abort();
    }
    verifier.join();
    EXPECT_TRUE(waiting) << "authentication should wait for the account transaction";
    EXPECT_TRUE(factor_available) << factor_error;
    ASSERT_TRUE(authenticated.has_value());
    ASSERT_TRUE(*authenticated) << authenticated->error().message;
}

TEST_F(PostgresProvisioning, OwnsOneCustomerAndCanCreateFirstRepository) {
    const auto signed_up = store->sign_up_password(input);
    ASSERT_TRUE(signed_up) << signed_up.error().message;
    const auto principal = store->context_for(signed_up->secret);
    ASSERT_TRUE(principal) << principal.error().message;
    const auto customer = lake::test::personal_customer(*store, principal->user_id);
    ASSERT_TRUE(customer) << customer.error().message;
    EXPECT_TRUE(lake::control::valid_handle(customer->name));
    EXPECT_EQ(customer->customer_type, "individual");
    const auto quota = store->storage_quota(customer->id);
    ASSERT_TRUE(quota);
    EXPECT_FALSE(quota->has_value());
    const auto object = store->find_policy_object("customer", customer->id);
    ASSERT_TRUE(object);
    const auto grants = store->list_grants({object->id}, principal->taap_id, 10);
    ASSERT_TRUE(grants);
    ASSERT_EQ(grants->size(), 1U);
    EXPECT_EQ(grants->front().role, "admin");
    EXPECT_TRUE(grants->front().attestation_digest.starts_with("sign_up_password/"));

    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::ApiContext context;
    context.store = store.get();
    context.authorized = true;
    context.session_credential = true;
    context.acting_user_id = principal->user_id;
    context.acting_identity = principal->taap_id;
    context.scopes = principal->scopes;
    context.instance_kek = std::string(test_instance_kek);
    const auto customers = registry.call("list_customers", {{"limit", 1000}}, context);
    ASSERT_TRUE(customers.ok) << customers.error.message;
    ASSERT_EQ(customers.value.at("items").size(), 1U);
    const auto repository =
        registry.call("create_repository",
                      {{"customer_id", customer->id},
                       {"workspace_id", lake::test::repository_workspace(*store, customer->id)},
                       {"name", "first-repository"}},
                      context);
    ASSERT_TRUE(repository.ok) << repository.error.message;
    EXPECT_TRUE(registry.call("get_repository", {{"id", repository.value.at("id")}}, context).ok);
    EXPECT_FALSE(registry
                     .call("create_customer",
                           {{"customer_type", "individual"}, {"user_id", principal->user_id}},
                           context)
                     .ok);
    const auto retry = store->sign_up_password(input);
    ASSERT_FALSE(retry);
    EXPECT_EQ(retry.error().code, "conflict");
    EXPECT_TRUE(store->sign_in_password({"lake", input.email, input.password, "", "browser"}));
    EXPECT_EQ(lake::test::personal_customer(*store, principal->user_id)->id, customer->id);
    input.email = "another-" + input.email;
    EXPECT_FALSE(store->sign_up_password(input));
    ProvenDocument other_document = proven_document(*store, "lake", lake::control::new_uuid_v4());
    input.identity_document = other_document.document;
    input.identity_proof = other_document.proof;
    const auto other = store->sign_up_password(input);
    ASSERT_TRUE(other);
    const auto other_customer = lake::test::personal_customer(*store, other->session.user_id);
    ASSERT_TRUE(other_customer);
    EXPECT_FALSE(registry
                     .call("create_repository",
                           {{"customer_id", other_customer->id},
                            {"workspace_id",
                             lake::test::repository_workspace(*store, other_customer->id)},
                            {"name", "forbidden"}},
                           context)
                     .ok);
}

TEST_F(PostgresProvisioning, ConcurrentRetriesCreateOneCustomer) {
    auto other_store = lake::control::make_postgres_store(uri, std::string(test_instance_kek));
    std::barrier start(2);
    auto attempt = std::async(std::launch::async, [&] {
        start.arrive_and_wait();
        return other_store->sign_up_password(input);
    });
    start.arrive_and_wait();
    const auto first = store->sign_up_password(input);
    const auto second = attempt.get();
    ASSERT_NE(first.has_value(), second.has_value());
    EXPECT_EQ((first ? second.error() : first.error()).code, "conflict");
    const auto& issued = first ? *first : *second;
    pqxx::connection connection(uri);
    pqxx::read_transaction tx(connection);
    EXPECT_EQ(
        tx.exec_params("SELECT count(*) FROM users WHERE email = $1", input.email)[0][0].as<int>(),
        1);
    EXPECT_EQ(tx.exec_params("SELECT count(*) FROM customers WHERE user_id = $1::uuid",
                             issued.session.user_id)[0][0]
                  .as<int>(),
              1);
    EXPECT_EQ(tx.exec_params("SELECT count(*) FROM sessions WHERE user_id = $1::uuid",
                             issued.session.user_id)[0][0]
                  .as<int>(),
              1);
}

TEST_F(PostgresProvisioning, SessionFailureRollsBackUserCustomerAndGrantThenRetrySucceeds) {
    pqxx::connection connection(uri);
    std::vector<long long> counts_before;
    const auto counts = [&] {
        pqxx::read_transaction tx(connection);
        std::vector<long long> result;
        for (const std::string table : {"users",
                                        "customers",
                                        "objects",
                                        "grants",
                                        "identity_documents",
                                        "credentials",
                                        "devices",
                                        "sessions"}) {
            result.push_back(tx.exec("SELECT count(*) FROM " + table)[0][0].as<long long>());
        }
        return result;
    };
    counts_before = counts();
    {
        pqxx::work tx(connection);
        tx.exec("CREATE FUNCTION issue_119_refuse_session() RETURNS trigger LANGUAGE plpgsql AS $$ "
                "BEGIN RAISE EXCEPTION 'injected sign-up session failure'; END $$");
        tx.exec("CREATE TRIGGER issue_119_refuse_session BEFORE INSERT ON sessions "
                "FOR EACH ROW EXECUTE FUNCTION issue_119_refuse_session()");
        tx.commit();
    }
    const auto failed = store->sign_up_password(input);
    {
        pqxx::work tx(connection);
        tx.exec("DROP TRIGGER issue_119_refuse_session ON sessions");
        tx.exec("DROP FUNCTION issue_119_refuse_session()");
        tx.commit();
    }
    EXPECT_EQ(counts(), counts_before);
    ASSERT_FALSE(failed);
    EXPECT_NE(failed.error().message.find("injected sign-up session failure"), std::string::npos);
    {
        pqxx::read_transaction tx(connection);
        EXPECT_EQ(tx.exec_params("SELECT count(*) FROM users WHERE email = $1", input.email)[0][0]
                      .as<int>(),
                  0);
        EXPECT_EQ(
            tx.exec_params("SELECT count(*) FROM credentials WHERE subject = $1", input.email)[0][0]
                .as<int>(),
            0);
        EXPECT_EQ(tx.exec("SELECT count(*) FROM objects o LEFT JOIN customers c ON c.id::text = "
                          "o.external_id "
                          "WHERE o.object_type = 'customer' AND c.id IS NULL")[0][0]
                      .as<int>(),
                  0);
    }
    const auto retry = store->sign_up_password(input);
    ASSERT_TRUE(retry) << retry.error().message;
    EXPECT_TRUE(lake::test::personal_customer(*store, retry->session.user_id));
}

TEST_F(PostgresProvisioning, MalformedDocumentExplainsRecoveryWithoutCreatingUser) {
    const std::string valid = input.identity_document;
    for (const auto& document : {std::string("{}"), std::string("="), valid.substr(0, 40)}) {
        input.identity_document = document;
        const auto result = store->sign_up_password(input);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, "invalid_input");
        EXPECT_NE(result.error().message.find("liquid identity show --document"),
                  std::string::npos);
    }
    pqxx::connection connection(uri);
    pqxx::read_transaction tx(connection);
    EXPECT_EQ(
        tx.exec_params("SELECT count(*) FROM users WHERE email = $1", input.email)[0][0].as<int>(),
        0);
}

TEST(PostgresApplications, CapabilitiesPersistKeysUsersOrganizationsAndWebhooks) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }

    std::shared_ptr<lake::control::Store> store =
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
    // The application's events live in its workspace Zetta (lake#356, lake#357).
    const auto zetta = std::make_shared<lake::test::StreamCaller>();
    const auto service_dir = lake::test::fixture_path("postgres-app-workspaces-" + suffix);
    const auto workspaces = std::make_shared<lake::control::WorkspaceService>(
        lake::control::WorkspaceConfig{service_dir.string(), 0}, store, zetta);
    context.workspaces = workspaces.get();
    const std::string workspace_id = lake::test::ready_workspace(
        *store, service_dir, customer->id, "postgres-app-" + suffix, uri);
    const auto call = [&](const std::string& name,
                          nlohmann::json input = nlohmann::json::object()) {
        context.request_id = lake::control::new_uuid_v4();
        auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(completed);
        return completed ? *completed : lake::api::ApiResult::failure("store_error", "incomplete");
    };

    const auto registered =
        call("register_application",
             {{"customer_id", customer->id},
              {"workspace_id", workspace_id},
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
    // The settings live in the application's workspace (lake#351); the routing row holds none.
    const auto themed = call("update_application_theme",
                             {{"application_id", application_id},
                              {"workspace_id", workspace_id},
                              {"theme", {{"accent_color", "#778899"}}}});
    ASSERT_TRUE(themed.ok) << themed.error.message;
    EXPECT_EQ(themed.value.at("name"), "Postgres application");
    EXPECT_EQ(call("get_application", {{"id", application_id}}).value.at("theme").at("accent_color"),
              "#778899");
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        // The routing row has no settings columns (migration 0070 dropped them).
        EXPECT_EQ(tx.exec_params("SELECT count(*) FROM auth_applications WHERE id = $1::uuid",
                                 application_id)[0][0]
                      .as<int>(),
                  1);
        EXPECT_TRUE(tx.exec("SELECT 1 FROM information_schema.columns WHERE table_schema = "
                            "current_schema() AND table_name = 'auth_applications' AND "
                            "column_name IN ('name', 'allowed_origins', 'redirect_uris', 'theme')")
                        .empty());
    }
    EXPECT_TRUE(call("rotate_application_signing_key",
                     {{"application_id", application_id}, {"workspace_id", workspace_id}})
                    .ok);
    const auto jwks = call("application_jwks", {{"application_id", application_id}});
    ASSERT_TRUE(jwks.ok) << jwks.error.message;
    EXPECT_EQ(jwks.value.at("keys").size(), 2);

    const auto webhook =
        call("create_application_webhook",
             {{"application_id", application_id},
              {"workspace_id", workspace_id},
              {"url", "https://hooks.example.test/postgres"},
              {"events", nlohmann::json::array({"session.started", "organization.created"})}});
    ASSERT_TRUE(webhook.ok) << webhook.error.message;
    const auto webhooks = call("list_application_webhooks",
                               {{"application_id", application_id}, {"workspace_id", workspace_id}});
    ASSERT_TRUE(webhooks.ok) << webhooks.error.message;
    ASSERT_EQ(webhooks.value.at("items").size(), 1U);
    EXPECT_FALSE(webhooks.value.at("items").at(0).contains("secret"));

    const ProvenDocument app_user = proven_document(*store, pool_id, "application-" + suffix);
    const auto sign_up =
        call("sign_up_password",
             {{"pool_id", pool_id},
              {"email", "postgres-app-user-" + suffix + "@example.test"},
              {"password", "correct horse battery staple"},
              {"display_name", "Postgres application user"},
              {"identity_document", app_user.document},
              {"identity_proof", lake::test::identity_proof_json(app_user.proof)}});
    ASSERT_TRUE(sign_up.ok) << sign_up.error.message;
    EXPECT_TRUE(sign_up.value.contains("access_token"));
    const auto users = call("list_application_users",
                            {{"application_id", application_id}, {"workspace_id", workspace_id}});
    ASSERT_TRUE(users.ok) << users.error.message;
    ASSERT_EQ(users.value.at("items").size(), 1);
    const std::string user_id = users.value.at("items").at(0).at("user_id");
    EXPECT_TRUE(call("export_application_user",
                     {{"application_id", application_id},
                      {"workspace_id", workspace_id},
                      {"user_id", user_id}})
                    .ok);
    EXPECT_TRUE(call("issue_application_jwt",
                     {{"application_id", application_id},
                      {"workspace_id", workspace_id},
                      {"user_id", user_id}})
                    .ok);

    const auto organization = call("create_application_organization",
                                   {{"application_id", application_id},
                                    {"workspace_id", workspace_id},
                                    {"taap_id", "taap:1:ed25519:postgres-app-org-" + suffix},
                                    {"name", "Postgres organization"},
                                    {"metadata", {{"plan", "test"}}}});
    ASSERT_TRUE(organization.ok) << organization.error.message;
    const std::string organization_id = organization.value.at("id");
    const auto events = call(
        "list_application_webhook_events",
        {{"application_id", application_id}, {"workspace_id", workspace_id}, {"limit", 1}});
    ASSERT_TRUE(events.ok) << events.error.message;
    ASSERT_EQ(events.value.at("items").size(), 1U);
    const auto subscribed = events.value.at("items").front();
    EXPECT_EQ(subscribed.at("event"), "organization.created");

    EXPECT_TRUE(call("list_application_organizations",
                     {{"application_id", application_id}, {"workspace_id", workspace_id}})
                    .ok);
    EXPECT_TRUE(call("update_application_organization",
                     {{"application_id", application_id},
                      {"workspace_id", workspace_id},
                      {"organization_id", organization_id},
                      {"name", "Updated organization"},
                      {"metadata", {{"plan", "updated"}}}})
                    .ok);

    EXPECT_TRUE(call("retry_application_webhook",
                     {{"application_id", application_id},
                      {"workspace_id", workspace_id},
                      {"event_id", subscribed.at("id")},
                      {"webhook_id", webhook.value.at("id")}})
                    .ok);

    // The application's workspace never changes (migration 0049's routing trigger), and nothing of
    // its events is written to the retired tables.
    {
        const std::string other = lake::test::ready_workspace(
            *store, service_dir, customer->id, "postgres-app-other-" + suffix, uri);
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        EXPECT_THROW(tx.exec_params("UPDATE auth_applications SET workspace_id = $2::uuid "
                                    "WHERE id = $1::uuid",
                                    application_id, other),
                     pqxx::check_violation);
    }
    {
        // Its events, keys and webhooks are in its workspace Zetta, sealed (lake#352, lake#355);
        // migration 0070 dropped the retired tables.
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        for (const std::string table :
             {"auth_app_webhook_events", "auth_app_signing_keys", "auth_app_webhooks"}) {
            EXPECT_TRUE(tx.exec("SELECT to_regclass('" + table + "')")[0][0].is_null()) << table;
        }
        EXPECT_EQ(zetta->records["http://postgres-app-" + suffix + ".test"]
                      .rows("auth_app_signing_keys")
                      .size(),
                  1U);
        EXPECT_EQ(zetta->records["http://postgres-app-" + suffix + ".test"]
                      .rows("auth_app_webhooks")
                      .size(),
                  1U);
    }
    EXPECT_TRUE(call("delete_application_organization",
                     {{"application_id", application_id},
                      {"workspace_id", workspace_id},
                      {"organization_id", organization_id}})
                    .ok);
    EXPECT_TRUE(call("suspend_application_user",
                     {{"application_id", application_id},
                      {"workspace_id", workspace_id},
                      {"user_id", user_id}})
                    .ok);
    EXPECT_FALSE(call("sign_in_password",
                      {{"pool_id", pool_id},
                       {"email", "postgres-app-user-" + suffix + "@example.test"},
                       {"password", "correct horse battery staple"}})
                     .ok);
    EXPECT_TRUE(call("delete_application_user",
                     {{"application_id", application_id},
                      {"workspace_id", workspace_id},
                      {"user_id", user_id}})
                    .ok);
}

TEST(PostgresIdentity, SessionsTokensAndDeviceRevocation) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }

    std::unique_ptr<lake::control::Store> store =
        lake::control::make_postgres_store(uri, std::string(test_instance_kek));
    const std::string email = "postgres-identity-" + std::to_string(getpid()) + "@example.test";
    const auto sign_up = store->sign_up_password(
        proven_sign_up(*store, email, "Postgres identity", "integration test", "password"));
    ASSERT_TRUE(sign_up) << sign_up.error().message;

    const auto context = store->context_for(sign_up->secret);
    ASSERT_TRUE(context) << context.error().message;
    EXPECT_EQ(context->user_id, sign_up->session.user_id);

    const auto token = store->create_token(
        {context->user_id, "lake", "read sessions", "", {"list_sessions"}, 3600, false});
    ASSERT_TRUE(token) << token.error().message;
    EXPECT_TRUE(store->context_for(token->secret));

    const auto enrolled = store->enroll_mfa(
        context->user_id, "lake", {sign_up->session.id, "correct horse battery staple"});
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
    const ProvenDocument oauth_document = proven_document(*store, "lake", "oauth");
    const auto started = store->begin_oauth({"lake",
                                             provider_name,
                                             state,
                                             "pkce-verifier",
                                             "oidc-nonce",
                                             "integration browser",
                                             oauth_document.document,
                                             oauth_document.proof,
                                             "browser-binding"});
    ASSERT_TRUE(started) << started.error().message;
    EXPECT_FALSE(store->consume_oauth(provider_name, state, ""));
    EXPECT_FALSE(store->consume_oauth(provider_name, state, "another-browser"));
    EXPECT_FALSE(store->consume_oauth("another-provider", state, "browser-binding"));
    const auto flow = store->consume_oauth(provider_name, state, "browser-binding");
    ASSERT_TRUE(flow) << flow.error().message;
    EXPECT_EQ(flow->code_verifier, "pkce-verifier");
    EXPECT_EQ(flow->nonce, "oidc-nonce");
    EXPECT_FALSE(store->consume_oauth(provider_name, state, "browser-binding"));
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
    const char* uri = lake::test::postgres_uri();
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
    const auto logs = store->list_sso_provisioning_logs(provider->id, 1);
    ASSERT_TRUE(logs) << logs.error().message;
    ASSERT_EQ(logs->size(), 1U);
    EXPECT_EQ(logs->front().external_id, created->external_id);
    EXPECT_EQ(logs->front().resource, "user");
    EXPECT_EQ(logs->front().operation, "deactivate");

    EXPECT_FALSE(store->context_for(issued->secret));
    ASSERT_TRUE(store->delete_scim_user(provider->id, created->user_id));
    ASSERT_TRUE(store->delete_scim_group(provider->id, group->team_id));
    EXPECT_FALSE(store->get_scim_group(provider->id, group->team_id));
}

TEST(PostgresOfflineRecovery, GrantsRecoveryPathsAndRepositoryKeyCustody) {
    const char* uri = lake::test::postgres_uri();
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
        return store->sign_up_password(proven_sign_up(*store,
                                                      label + "-" + suffix + "@example.test",
                                                      label,
                                                      label + " integration device",
                                                      label + "-" + suffix));
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

    const auto customer = lake::test::personal_customer(*store, target_principal->user_id);
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository =
        store->create_repository({customer->id, "encrypted-" + suffix, "", "private",
                                  lake::test::repository_workspace(*store, customer->id)});
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

    const auto enrolled = store->enroll_mfa(
        target_principal->user_id, "lake", {target->session.id, "correct horse battery staple"});
    ASSERT_TRUE(enrolled) << enrolled.error().message;
    const lake::api::ApiResult code = *liquid::runtime::block_on(
        registry.call_async("recover_with_code",
                            {{"email", "offline-target-" + suffix + "@example.test"},
                             {"code", enrolled->recovery_codes.front()}},
                            anonymous));
    ASSERT_TRUE(code.ok) << code.error.message;
    EXPECT_TRUE(store->context_for(code.value["secret"]));

    const lake::api::ApiResult email =
        registry.call("begin_email_recovery",
                      {{"email", "offline-target-" + suffix + "@example.test"}},
                      anonymous);
    ASSERT_TRUE(email.ok) << email.error.message;
    // lake#774: the answer names no attempt; the attempt id is what the recovery email carries.
    EXPECT_FALSE(email.value.contains("id"));
    std::string email_recovery_id;
    {
        pqxx::connection connection(uri);
        pqxx::work transaction(connection);
        email_recovery_id = transaction
                                .exec("SELECT id::text FROM offline_recovery_attempts WHERE method "
                                      "= 'email' ORDER BY created_at DESC LIMIT 1")[0][0]
                                .as<std::string>();
        transaction.exec("UPDATE offline_recovery_attempts SET ready_at = now() - interval '1 "
                         "second' WHERE id = $1::uuid",
                         pqxx::params{email_recovery_id});
        transaction.commit();
    }
    const auto email_delivery =
        claim_recovery_delivery(registry, operator_context, email_recovery_id);
    ASSERT_TRUE(email_delivery.ok) << email_delivery.error.message;
    const lake::api::ApiResult email_restored = *liquid::runtime::block_on(registry.call_async(
        "complete_recovery",
        {{"recovery_id", email_recovery_id}, {"token", email_delivery.value["token"]}},
        anonymous));
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
    const auto admin_delivery =
        claim_recovery_delivery(registry, operator_context, admin_started.value["id"]);
    ASSERT_TRUE(admin_delivery.ok) << admin_delivery.error.message;
    const lake::api::ApiResult admin_restored = *liquid::runtime::block_on(registry.call_async(
        "complete_recovery",
        {{"recovery_id", admin_started.value["id"]}, {"token", admin_delivery.value["token"]}},
        anonymous));
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

// lake#870: no stored identity may be the operator's. Every creation path refuses `admin`, and a
// session whose user carries it anyway (a row written outside lake) is not accepted.
TEST(PostgresIdentity, OperatorPrincipalIsNeverAPersonsIdentity) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    std::unique_ptr<lake::control::Store> store =
        lake::control::make_postgres_store(uri, std::string(test_instance_kek));
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);

    for (const std::string refused : {"admin", "agent-service:ask", "taap:", "taap:has space"}) {
        const auto user = store->create_user({refused, "", "Refused"});
        ASSERT_FALSE(user) << refused;
        EXPECT_EQ(user.error().code, "invalid_input");
        const auto organization = store->create_organization({refused, "Refused", ""});
        ASSERT_FALSE(organization) << refused;
        EXPECT_EQ(organization.error().code, "invalid_input");
    }

    const auto organization = store->create_organization(
        {"taap:1:ed25519:operator-org-" + suffix, "Operator principal organization", ""});
    ASSERT_TRUE(organization) << organization.error().message;
    const auto provider = store->register_sso_provider({organization->id,
                                                        "lake",
                                                        "jit-" + suffix,
                                                        "oidc",
                                                        "https://oidc.attacker.example/" + suffix,
                                                        "",
                                                        "",
                                                        "client",
                                                        "secret",
                                                        "scim-" + suffix,
                                                        {{"email", "mail"}},
                                                        true});
    ASSERT_TRUE(provider) << provider.error().message;

    const auto scim = store->upsert_scim_user(
        {provider->id, "scim-admin", "", "Operator", "admin", true, nlohmann::json::object()});
    ASSERT_FALSE(scim);
    EXPECT_EQ(scim.error().code, "invalid_input");
    const auto jit = store->sign_in_sso({provider->id,
                                         provider->issuer,
                                         "jit-admin",
                                         "",
                                         "Operator",
                                         "admin",
                                         "browser",
                                         nlohmann::json::object()});
    ASSERT_FALSE(jit);
    EXPECT_EQ(jit.error().code, "invalid_input");

    // lake#872 (migration 0109_principal_ids): the database refuses the row however it is written.
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        EXPECT_THROW(tx.exec("INSERT INTO users (taap_id, display_name) VALUES ('admin', 'P')"),
                     pqxx::check_violation);
    }
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        EXPECT_THROW(tx.exec("INSERT INTO organizations (taap_id) VALUES ('share-link:1')"),
                     pqxx::check_violation);
    }
    // A row from before the constraint (it is NOT VALID, so old rows are not scanned): the
    // session issues, but no request can use it.
    std::string user_id;
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec("ALTER TABLE users DROP CONSTRAINT users_taap_id_format");
        user_id = tx.exec("INSERT INTO users (taap_id, display_name) VALUES ('admin', 'Planted') "
                          "RETURNING id::text")[0][0]
                      .as<std::string>();
        tx.exec("ALTER TABLE users ADD CONSTRAINT users_taap_id_format CHECK (taap_id IS NULL OR "
                "(taap_id ~ '^taap:[!-~]+$' AND length(taap_id) <= 512)) NOT VALID");
        tx.exec_params("INSERT INTO scim_users (provider_id, external_id, user_id, attributes) "
                       "VALUES ($1::uuid, 'planted', $2::uuid, '{}'::jsonb)",
                       provider->id,
                       user_id);
        tx.commit();
    }
    const auto planted = store->sign_in_sso({provider->id,
                                             provider->issuer,
                                             "planted",
                                             "",
                                             "",
                                             "",
                                             "browser",
                                             nlohmann::json::object()});
    ASSERT_TRUE(planted) << planted.error().message;
    const auto principal = store->context_for(planted->secret);
    ASSERT_FALSE(principal);
    EXPECT_EQ(principal.error().code, "not_found");
}

} // namespace
