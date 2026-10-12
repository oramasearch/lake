#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/agent_bus.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/platform.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "temporary_directory.hpp"
#include "stream_fixture.hpp"
#include "repository_test_support.hpp"

namespace {

using json = nlohmann::json;

lake::control::ClerkIdentity minted(std::string subject) {
    const auto identity = lake::audit::mint_custodial_identity("web-app-test-kek");
    return lake::control::ClerkIdentity{subject,
                                        subject + "@example.test",
                                        "",
                                        "browser",
                                        identity->taap_id,
                                        identity->public_key,
                                        identity->wrapped_seed};
}

class FakeLoki final : public lake::control::LokiSource {
public:
    std::string requested_namespace;

    liquid::runtime::task<lake::control::PlatformResult<json>>
    query(std::string namespace_name, std::string, int, std::string) override {
        requested_namespace = std::move(namespace_name);
        co_return lake::control::PlatformResult<json>(
            json{{"items", json::array({{{"line", "ready"}}})}, {"next_since", "2"}});
    }
};

struct WebApps {
    std::shared_ptr<lake::control::InMemoryStore> store =
        std::make_shared<lake::control::InMemoryStore>();
    lake::control::Principal owner;
    lake::control::Principal other;
    std::string customer_id;
    std::string repository_id;
    lake::api::Registry registry;
    std::shared_ptr<lake::control::MemoryAgentBus> raw_bus =
        std::make_shared<lake::control::MemoryAgentBus>();
    std::shared_ptr<lake::control::AgentTaskBus> bus =
        std::make_shared<lake::control::AgentTaskBus>(store, raw_bus, std::vector<std::string>{});
    std::filesystem::path service_dir =
        lake::test::fixture_path("web-app-services-" + lake::control::new_uuid_v4());
    std::unique_ptr<lake::test::StreamWorkspace> workspace;
    std::shared_ptr<lake::control::WorkspaceService> workspaces;
    std::shared_ptr<FakeLoki> loki = std::make_shared<FakeLoki>();
    std::shared_ptr<lake::control::PlatformService> platform;
    std::string apps_wildcard = "apps.example.test";
    std::string apps_reserved_names;

    WebApps() {
        std::filesystem::create_directories(service_dir);
        const auto owner_session = store->sign_in_clerk(minted("app-owner"));
        const auto other_session = store->sign_in_clerk(minted("app-other"));
        EXPECT_TRUE(owner_session);
        EXPECT_TRUE(other_session);
        owner = *store->context_for(owner_session->secret);
        other = *store->context_for(other_session->secret);
        const auto customers = store->list_customers(2, owner.user_id);
        EXPECT_TRUE(customers);
        customer_id = customers->front().id;
        const auto repository =
            store->create_repository({customer_id, "app-source", "", "private",
                                      lake::test::repository_workspace(*store, customer_id)});
        EXPECT_TRUE(repository);
        repository_id = repository->id;
        workspace = std::make_unique<lake::test::StreamWorkspace>(store, customer_id);
        workspaces = workspace->service;
        platform = std::make_shared<lake::control::PlatformService>(
            store,
            nullptr,
            loki,
            lake::control::PlatformConfig{"test", std::chrono::seconds(15), "", ""});
        lake::api::register_core_capabilities(registry, "test");
    }

    lake::api::ApiResult call(const lake::control::Principal& principal,
                              const std::string& name,
                              json input,
                              std::size_t app_limit = 3,
                              std::size_t replica_limit = 4) {
        lake::api::ApiContext context;
        context.store = store.get();
        context.workspaces = workspaces.get();
        context.platform = platform.get();
        context.agent_bus = bus.get();
        context.apps_wildcard = apps_wildcard;
        context.apps_reserved_names = apps_reserved_names;
        context.apps_per_customer = app_limit;
        context.app_replicas_per_customer = replica_limit;
        context.authorized = true;
        context.session_credential = true;
        context.acting_user_id = principal.user_id;
        context.acting_identity = principal.taap_id;
        context.credential_session_id = principal.session_id;
        context.credential_kind = principal.kind;
        context.credential_workload_id = principal.workload_id;
        context.credential_customer_id = principal.workload_customer_id;
        context.credential_repository_id = principal.workload_repository_id;
        context.credential_revision = principal.workload_revision;
        context.credential_token_hash = principal.workload_token_hash;
        context.pool_id = principal.pool_id;
        context.scopes = principal.scopes;
        context.request_id = lake::control::new_uuid_v4();
        if (registry.find(name)->input_schema["properties"].contains("customer_id") &&
            principal.kind != "app_events" && !input.contains("customer_id")) {
            input["customer_id"] = customer_id;
        }
        if (registry.find(name)->input_schema["properties"].contains("workspace_id") &&
            !input.contains("workspace_id")) {
            input["workspace_id"] = workspace->id;
        }
        auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(completed);
        return std::move(*completed);
    }

    lake::api::ApiResult call(const std::string& name, json input = json::object()) {
        return call(owner, name, std::move(input));
    }

    lake::api::ApiResult create(std::string name = "reports") {
        return call("create_app",
                    {{"name", std::move(name)},
                     {"mode", "runtime"},
                     {"source",
                      {{"repository", repository_id},
                       {"revision", "main"},
                       {"path", "web"},
                       {"entrypoint", "server:app"}}},
                     {"framework", "node"},
                     {"port", 3000},
                     {"bindings", {{"zetta", true}, {"services", json::array()}}},
                     {"env", {{"LOG_LEVEL", "info"}}}});
    }
};

} // namespace

TEST(WebAppSessions, SignedClaimsAreBoundAndExpireAfterTwelveHours) {
    constexpr std::string_view instance_kek =
        "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    constexpr std::int64_t issued_at = 1'700'000'000;
    const std::string app_id = lake::control::new_uuid_v4();
    const std::string user_id = lake::control::new_uuid_v4();
    const std::string workspace_id = lake::control::new_uuid_v4();

    const auto token = lake::control::issue_web_app_session(
        instance_kek, app_id, user_id, workspace_id, issued_at);
    ASSERT_TRUE(token) << token.error();
    const auto claims = lake::control::verify_web_app_session(instance_kek, *token, issued_at + 1);
    ASSERT_TRUE(claims) << claims.error();
    EXPECT_EQ(claims->app_id, app_id);
    EXPECT_EQ(claims->user_id, user_id);
    EXPECT_EQ(claims->workspace_id, workspace_id);
    EXPECT_EQ(claims->issued_at, issued_at);
    EXPECT_EQ(claims->expires_at, issued_at + 12 * 60 * 60);

    std::string tampered = *token;
    tampered.front() = tampered.front() == 'A' ? 'B' : 'A';
    EXPECT_FALSE(lake::control::verify_web_app_session(instance_kek, tampered, issued_at + 1));
    EXPECT_FALSE(
        lake::control::verify_web_app_session(instance_kek, *token, issued_at + 12 * 60 * 60));
}

TEST(WebApps, DesiredStateStatusAndQuotasFollowTheContract) {
    WebApps apps;
    const auto created = apps.create();
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_EQ(created.value["host"], "reports.apps.example.test");
    EXPECT_EQ(created.value["auth"], "lake");
    EXPECT_EQ(created.value["generation"], 1);
    EXPECT_EQ(created.value["bindings"]["zetta"], json({{"scopes", {"read"}}}));
    EXPECT_EQ(created.value["source"]["entrypoint"], "server:app");
    EXPECT_FALSE(created.value.dump().contains("workspace-admin-token"));

    const std::string id = created.value.at("id");
    const auto audit_events = apps.store->list_audit_events({.limit = 100});
    ASSERT_TRUE(audit_events) << audit_events.error().message;
    EXPECT_TRUE(std::ranges::any_of(*audit_events, [&](const lake::audit::Event& event) {
        return event.draft.action == "create_app" && event.draft.object == "app/" + id &&
               event.draft.principal == apps.owner.taap_id && event.draft.outcome == "success";
    }));
    ASSERT_TRUE(apps.store->set_web_app_status(
        id, "Running", "https://reports.apps.example.test", true, "available", 1));
    const auto observed = apps.call("get_app", {{"app_id", id}});
    ASSERT_TRUE(observed.ok) << observed.error.message;
    EXPECT_EQ(observed.value["phase"], "Running");
    EXPECT_EQ(observed.value["observed_generation"], 1);
    EXPECT_TRUE(observed.value["ready"]);

    const auto scaled = apps.call("scale_app", {{"app_id", id}, {"replicas", 4}});
    ASSERT_TRUE(scaled.ok) << scaled.error.message;
    EXPECT_EQ(scaled.value["replicas"], 4);
    EXPECT_EQ(scaled.value["generation"], 2);
    const auto over_quota = apps.call("create_app",
                                      {{"name", "second"},
                                       {"mode", "image"},
                                       {"image", "registry.example.test/second:v1"},
                                       {"framework", "container"},
                                       {"port", 8080}});
    EXPECT_EQ(over_quota.error.code, "quota_exceeded");

    const auto stopped = apps.call("stop_app", {{"app_id", id}});
    ASSERT_TRUE(stopped.ok) << stopped.error.message;
    EXPECT_EQ(stopped.value["desired"], "stopped");
    EXPECT_EQ(apps.call("start_app", {{"app_id", id}}).value["desired"], "running");
    EXPECT_TRUE(apps.call("restart_app", {{"app_id", id}}).ok);
}

TEST(WebApps, ReservedNamesAreRefusedAndOrdinaryNamesUseTheZone) {
    WebApps apps;
    apps.apps_wildcard = "oramadev.net";
    apps.apps_reserved_names = "customer-portal, internal";
    for (const char* value : {"lake",
                              "www",
                              "api",
                              "app",
                              "apps",
                              "admin",
                              "mcp",
                              "docs",
                              "status",
                              "mail",
                              "smtp",
                              "auth",
                              "login",
                              "sso",
                              "id",
                              "cdn",
                              "static",
                              "assets",
                              "grafana",
                              "prometheus",
                              "argocd",
                              "registry",
                              "git",
                              "dev",
                              "staging",
                              "prod",
                              "test",
                              "lake-worker",
                              "orama",
                              "orama-search",
                              "customer-portal",
                              "internal"}) {
        const std::string name = value;
        const auto refused = apps.create(name);
        EXPECT_EQ(refused.error.code, "invalid_input") << name;
        EXPECT_EQ(refused.error.message, name + " is reserved") << name;
        EXPECT_EQ(refused.error.path, "/name") << name;
    }

    const auto created = apps.create("reports");
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_EQ(created.value["host"], "reports.oramadev.net");
}

TEST(WebApps, CapabilitiesAreCustomerScopedAndValidateSecretsHostsAndDeletes) {
    WebApps apps;
    for (const std::string& read : {"list_apps", "get_app", "app_logs", "receive_app_events"}) {
        ASSERT_EQ(apps.registry.find(read)->action, "read") << read;
    }
    for (const std::string& write : {"create_app",
                                     "deploy_app",
                                     "rollback_app",
                                     "delete_app",
                                     "set_app_env",
                                     "unset_app_env",
                                     "restart_app",
                                     "set_app_auth",
                                     "scale_app",
                                     "stop_app",
                                     "start_app",
                                     "set_app_zetta_binding",
                                     "bind_service_to_app",
                                     "unbind_service_from_app",
                                     "create_service_and_bind_to_app",
                                     "publish_app_event"}) {
        ASSERT_EQ(apps.registry.find(write)->action, "write") << write;
    }

    const auto wrong_host = apps.call("create_app",
                                      {{"name", "wrong-host"},
                                       {"mode", "image"},
                                       {"image", "registry.example.test/app:v1"},
                                       {"framework", "container"},
                                       {"port", 8080},
                                       {"host", "app.example.test"}});
    EXPECT_EQ(wrong_host.error.code, "invalid_input");
    const auto invalid_label = apps.call("create_app",
                                         {{"name", "bad-label"},
                                          {"mode", "image"},
                                          {"image", "registry.example.test/app:v1"},
                                          {"framework", "container"},
                                          {"port", 8080},
                                          {"host", "bad-.apps.example.test"}});
    EXPECT_EQ(invalid_label.error.code, "invalid_input");
    // Agents send "host": "" when the person did not ask for an address: that means the default.
    const auto empty_host = apps.call("create_app",
                                      {{"name", "empty-host"},
                                       {"mode", "image"},
                                       {"image", "registry.example.test/app:v1"},
                                       {"framework", "container"},
                                       {"port", 8080},
                                       {"host", ""}});
    ASSERT_TRUE(empty_host.ok) << empty_host.error.message;
    EXPECT_EQ(empty_host.value["host"], "empty-host.apps.example.test");

    const auto created = apps.create();
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string id = created.value.at("id");
    const auto foreign = apps.call(apps.other, "get_app", {{"app_id", id}});
    EXPECT_EQ(foreign.error.code, "unauthorized");
    lake::api::ApiContext anonymous;
    anonymous.store = apps.store.get();
    EXPECT_EQ(apps.registry
                  .call("get_app", {{"app_id", id}, {"customer_id", apps.customer_id}}, anonymous)
                  .error.code,
              "unauthenticated");
    const auto secret = apps.call("set_app_env",
                                  {{"app_id", id},
                                   {"key", "AWS_SECRET_ACCESS_KEY"},
                                   {"value", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"}});
    EXPECT_EQ(secret.error.code, "secret_required");
    // A Kubernetes Secret name is no longer a reference: an app names a lake secret (lake#625,
    // MintSealedSecret.AnAppNamesALakeSecretItsWorkspaceHolds covers the accepted forms).
    const auto reference = apps.call(
        "set_app_env", {{"app_id", id}, {"key", "DATABASE_URL"}, {"secret_ref", "db-url"}});
    EXPECT_EQ(reference.error.code, "invalid_input");
    EXPECT_EQ(reference.error.path, "/secret_ref");
    EXPECT_TRUE(apps.call("unset_app_env", {{"app_id", id}, {"key", "DATABASE_URL"}}).ok);

    EXPECT_EQ(apps.call("set_app_auth", {{"app_id", id}, {"auth", "none"}}).error.code,
              "invalid_input");
    const auto public_app =
        apps.call("set_app_auth", {{"app_id", id}, {"auth", "none"}, {"confirm_public", true}});
    ASSERT_TRUE(public_app.ok) << public_app.error.message;
    EXPECT_EQ(public_app.value["auth"], "none");
    EXPECT_TRUE(apps.call("set_app_auth", {{"app_id", id}, {"auth", "lake"}}).ok);

    const auto detached = apps.call("set_app_zetta_binding", {{"app_id", id}, {"attached", false}});
    ASSERT_TRUE(detached.ok) << detached.error.message;
    EXPECT_TRUE(detached.value["bindings"]["zetta"].is_null());
    const auto attached = apps.call("set_app_zetta_binding", {{"app_id", id}, {"attached", true}});
    ASSERT_TRUE(attached.ok) << attached.error.message;
    EXPECT_EQ(attached.value["bindings"]["zetta"]["scopes"], json::array({"read"}));

    const auto deployed =
        apps.call("deploy_app", {{"app_id", id}, {"image", "registry.example.test/reports:v2"}});
    ASSERT_TRUE(deployed.ok) << deployed.error.message;
    EXPECT_EQ(deployed.value["mode"], "image");
    const auto rolled_back = apps.call(
        "rollback_app",
        {{"app_id", id}, {"source", {{"repository", apps.repository_id}, {"revision", "v1"}}}});
    ASSERT_TRUE(rolled_back.ok) << rolled_back.error.message;
    EXPECT_EQ(rolled_back.value["source"]["revision"], "v1");
    EXPECT_EQ(apps.call("delete_app", {{"app_id", id}, {"confirm", "wrong"}}).error.code,
              "invalid_input");
    const auto deleted = apps.call("delete_app", {{"app_id", id}, {"confirm", "reports"}});
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(deleted.value["desired"], "deleted");
}

TEST(WebApps, ServiceBindingsLogsAndLifecycleEventsCarryReferencesAndPrincipal) {
    WebApps apps;
    const auto created = apps.create();
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string id = created.value.at("id");

    const auto bound = apps.call("create_service_and_bind_to_app",
                                 {{"app_id", id}, {"kind", "postgres"}, {"name", "orders"}});
    ASSERT_TRUE(bound.ok) << bound.error.message;
    ASSERT_EQ(bound.value["bindings"]["services"].size(), 1U);
    EXPECT_EQ(bound.value["bindings"]["services"][0], bound.value["service"]["id"]);
    EXPECT_FALSE(bound.value.dump().contains("password"));
    const std::string service_id = bound.value["service"]["id"];
    const auto unbound =
        apps.call("unbind_service_from_app", {{"app_id", id}, {"service_id", service_id}});
    ASSERT_TRUE(unbound.ok) << unbound.error.message;
    EXPECT_TRUE(unbound.value["bindings"]["services"].empty());
    EXPECT_EQ(apps.call("unbind_service_from_app", {{"app_id", id}, {"service_id", service_id}})
                  .error.code,
              "not_found");

    const auto logs = apps.call("app_logs", {{"app_id", id}});
    ASSERT_TRUE(logs.ok) << logs.error.message;
    EXPECT_EQ(logs.value["items"][0]["line"], "ready");
    EXPECT_EQ(apps.loki->requested_namespace, lake::control::web_app_namespace(id));

    const auto published = apps.call(
        "publish_app_event", {{"app_id", id}, {"type", "rollout"}, {"message", "complete"}});
    ASSERT_TRUE(published.ok) << published.error.message;
    const auto received = apps.call("receive_app_events", {{"app_id", id}});
    ASSERT_TRUE(received.ok) << received.error.message;
    ASSERT_GE(received.value["events"].size(), 3U);
    EXPECT_EQ(received.value["events"][0]["type"], "create");
    EXPECT_EQ(received.value["events"].back()["type"], "rollout");
    EXPECT_EQ(received.value["events"].back()["generation"], 3);
    EXPECT_EQ(received.value["events"].back()["principal"], apps.owner.taap_id);
}

TEST(WebApps, OperatorHashesRestrictEveryPurposeAndRotationRevokesOldTokens) {
    WebApps apps;
    const auto created = apps.create();
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string id = created.value.at("id");
    const auto second = apps.create("second");
    ASSERT_TRUE(second.ok) << second.error.message;
    const auto foreign =
        apps.store->create_repository({apps.customer_id, "other-source", "", "public",
                                       lake::test::repository_workspace(
                                           *apps.store, apps.customer_id)});
    ASSERT_TRUE(foreign);
    EXPECT_EQ(created.value["mode"], "runtime");
    EXPECT_FALSE(created.value.contains("token_hashes_json"));
    ASSERT_TRUE(apps.store->get_web_app(id)->token_hashes.empty());

    const json hashes{{"source", lake::control::identity_crypto::sha256_hex("operator-source")},
                      {"zetta", lake::control::identity_crypto::sha256_hex("operator-zetta")},
                      {"events", lake::control::identity_crypto::sha256_hex("operator-events")},
                      {"pull", lake::control::identity_crypto::sha256_hex("operator-pull")}};
    ASSERT_TRUE(apps.store->set_web_app_token_hashes(id, hashes));
    ASSERT_FALSE(apps.store->context_for("unknown"));
    for (const std::string purpose : {"source", "zetta", "events", "pull"}) {
        const auto principal = apps.store->context_for("operator-" + purpose);
        ASSERT_TRUE(principal) << principal.error().message;
        EXPECT_EQ(principal->kind, "app_" + purpose);
        EXPECT_EQ(principal->workload_id, id);
        EXPECT_EQ(principal->workload_customer_id, apps.customer_id);
        EXPECT_EQ(
            apps.call(*principal, "get_app", {{"app_id", id}, {"customer_id", apps.customer_id}})
                .error.code,
            "unauthorized");
        EXPECT_EQ(apps.call(*principal, "server_info", json::object()).error.code, "unauthorized");
        EXPECT_EQ(apps.call(*principal, "server_info", nullptr).error.code, "invalid_input");
        EXPECT_EQ(
            apps.call(*principal,
                      "delete_app",
                      {{"app_id", id}, {"customer_id", apps.customer_id}, {"confirm", "reports"}})
                .error.code,
            "unauthorized");
        const auto different_app =
            apps.call(*principal,
                      "publish_app_event",
                      {{"app_id", second.value.at("id")}, {"event", {{"phase", "Running"}}}});
        EXPECT_EQ(different_app.error.code, "unauthorized");
    }

    const auto source = *apps.store->context_for("operator-source");
    EXPECT_EQ(apps.call(source,
                        "repository_tree",
                        {{"id", apps.repository_id}, {"ref", "other-revision"}})
                  .error.code,
              "unauthorized");
    EXPECT_EQ(
        apps.call(source, "repository_tree", {{"id", foreign->id}, {"ref", "main"}}).error.code,
        "unauthorized");
    EXPECT_EQ(apps.call(source,
                        "repository_file",
                        {{"id", foreign->id}, {"ref", "main"}, {"path", "app.toml"}})
                  .error.code,
              "unauthorized");
    EXPECT_EQ(apps.call(source, "workspace_query", {{"capability", "list_datasets"}}).error.code,
              "unauthorized");

    ASSERT_TRUE(apps.call("start_workspace").ok);
    const auto services = apps.store->list_services(apps.customer_id);
    ASSERT_TRUE(services);
    const auto workspace =
        std::ranges::find_if(*services, [](const auto& service) { return service.is_default; });
    ASSERT_NE(workspace, services->end());
    ASSERT_TRUE(
        apps.store->set_service_status(workspace->id, "Ready", "", "http://workspace.test:4800"));
    std::ofstream(apps.service_dir / (workspace->slug + ".json"))
        << json{{"kind", "zetta"}, {"token", "workspace-server-token"}}.dump();
    const auto zetta = *apps.store->context_for("operator-zetta");
    EXPECT_TRUE(apps.call(zetta, "workspace_query", {{"capability", "list_datasets"}}).ok);
    EXPECT_TRUE(apps.call(zetta, "workspace_sql", {{"statement", "SELECT * FROM data.rows"}}).ok);
    EXPECT_FALSE(apps.call(zetta, "workspace_query", {{"capability", "create_project"}}).ok);
    EXPECT_FALSE(apps.call(zetta, "workspace_sql", {{"statement", "DROP TABLE data.rows"}}).ok);
    EXPECT_EQ(apps.call(zetta, "workspace_act", {{"capability", "create_project"}}).error.code,
              "unauthorized");
    EXPECT_EQ(
        apps.call(zetta,
                  "workspace_query",
                  {{"customer_id", lake::control::new_uuid_v4()}, {"capability", "list_datasets"}})
            .error.code,
        "unauthorized");
    EXPECT_EQ(apps.call(zetta, "repository_tree", {{"id", apps.repository_id}, {"ref", "main"}})
                  .error.code,
              "unauthorized");

    const auto events = *apps.store->context_for("operator-events");
    const auto published =
        apps.call(events,
                  "publish_app_event",
                  {{"app_id", id}, {"event", {{"phase", "Running"}, {"ready", true}}}});
    ASSERT_TRUE(published.ok) << published.error.message;
    EXPECT_EQ(
        apps.call(events, "receive_app_events", {{"app_id", id}, {"customer_id", apps.customer_id}})
            .error.code,
        "unauthorized");
    EXPECT_EQ(
        apps.call(events,
                  "publish_app_event",
                  {{"app_id", id}, {"customer_id", lake::control::new_uuid_v4()}, {"type", "pod"}})
            .error.code,
        "unauthorized");
    const auto received = apps.call("receive_app_events", {{"app_id", id}});
    ASSERT_TRUE(received.ok);
    EXPECT_EQ(received.value["events"].back()["principal"], "app:" + id + ":events");
    EXPECT_EQ(received.value["events"].back()["status"]["phase"], "Running");

    auto rotated = hashes;
    rotated["source"] = lake::control::identity_crypto::sha256_hex("replacement-source");
    ASSERT_TRUE(apps.store->set_web_app_token_hashes(id, rotated));
    EXPECT_FALSE(apps.store->context_for("operator-source"));
    EXPECT_TRUE(apps.store->context_for("replacement-source"));
    EXPECT_EQ(apps.call(source, "repository_tree", {{"id", apps.repository_id}, {"ref", "main"}})
                  .error.code,
              "unauthorized");

    lake::control::WebAppPatch revoke;
    revoke.rotate_tokens = true;
    ASSERT_TRUE(apps.store->update_web_app(id, revoke));
    EXPECT_FALSE(apps.store->context_for("operator-events"));
    EXPECT_TRUE(apps.store->get_web_app(id)->token_hashes.empty());
    ASSERT_TRUE(apps.store->set_web_app_token_hashes(id, hashes));
    ASSERT_TRUE(apps.call("delete_app", {{"app_id", id}, {"confirm", "reports"}}).ok);
    EXPECT_FALSE(apps.store->context_for("operator-source"));
    EXPECT_FALSE(apps.store->context_for("operator-zetta"));
    EXPECT_FALSE(apps.store->context_for("operator-pull"));
    EXPECT_TRUE(apps.store->context_for("operator-events"));
    ASSERT_TRUE(apps.store->purge_deleted_web_apps());
    EXPECT_TRUE(apps.store->get_web_app(id));
    ASSERT_TRUE(apps.store->set_web_app_status(id, "Deleted", "", false, "", 4));
    EXPECT_FALSE(apps.store->context_for("operator-events"));
    ASSERT_TRUE(apps.store->purge_deleted_web_apps());
    EXPECT_FALSE(apps.store->get_web_app(id));
    EXPECT_TRUE(apps.store->get_web_app(second.value.at("id")));
}


TEST(WebApps, ServiceBindingsAcceptAUniqueServiceName) {
    // Agents and people name the service; lake stores its id (live 2026-09-30: the
    // ApplicationAgent bound `e2e-notes-db` by name and create_app answered not_found).
    WebApps apps;
    const auto created = apps.create();
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string id = created.value.at("id");
    const auto bound = apps.call("create_service_and_bind_to_app",
                                 {{"app_id", id}, {"kind", "postgres"}, {"name", "orders"}});
    ASSERT_TRUE(bound.ok) << bound.error.message;
    const std::string service_id = bound.value["service"]["id"];
    ASSERT_TRUE(apps.call("unbind_service_from_app", {{"app_id", id}, {"service_id", service_id}})
                    .ok);

    const auto by_name =
        apps.call("bind_service_to_app", {{"app_id", id}, {"service_id", "orders"}});
    ASSERT_TRUE(by_name.ok) << by_name.error.message;
    ASSERT_EQ(by_name.value["bindings"]["services"].size(), 1U);
    EXPECT_EQ(by_name.value["bindings"]["services"][0], service_id);
    EXPECT_EQ(apps.call("bind_service_to_app", {{"app_id", id}, {"service_id", "no-such-db"}})
                  .error.code,
              "not_found");
}

namespace {

// A store of the fixture's workspace, written as create_store writes it (the provider table is
// create_store's to check; a bucket is not offered on this Lake yet, lake#664).
std::string make_store(WebApps& apps,
                       const std::string& type,
                       const std::string& provider,
                       const std::string& name,
                       const std::string& workspace_id = {}) {
    lake::control::NewManagedStore store{apps.customer_id, type, provider, name, "taap:x"};
    store.workspace_id = workspace_id.empty() ? apps.workspace->id : workspace_id;
    const auto created = apps.store->create_store(store);
    EXPECT_TRUE(created) << created.error().message;
    return created ? created->id : std::string();
}

const json* store_named(const json& listed, std::string_view name) {
    for (const json& store : listed.at("stores")) {
        if (store.at("name") == name) {
            return &store;
        }
    }
    return nullptr;
}

} // namespace

// lake#666: a service and an app name the stores they mount; list_stores says who mounts each and
// how much of it is used, and a mounted store is not deleted.
TEST(WebApps, ServicesAndAppsMountStorageAndListStoresSaysWho) {
    WebApps apps;
    const std::string uploads = make_store(apps, "volume", "cluster", "uploads");
    const std::string media = make_store(apps, "object", "s3", "media");
    const std::string notes = make_store(apps, "object", "zetta", "notes");
    const auto elsewhere_workspace =
        apps.store->create_workspace({apps.customer_id, "other", "dev", ""});
    ASSERT_TRUE(elsewhere_workspace) << elsewhere_workspace.error().message;
    const std::string elsewhere =
        make_store(apps, "volume", "cluster", "elsewhere", elsewhere_workspace->id);

    // Only a kind whose pods the operator builds mounts storage.
    const auto postgres =
        apps.call("create_service",
                  {{"kind", "postgres"},
                   {"name", "orders"},
                   {"stores", {{{"store_id", uploads}, {"mount_path", "/data"}}}}});
    ASSERT_FALSE(postgres.ok);
    EXPECT_EQ(postgres.error.code, "invalid_input");
    EXPECT_EQ(postgres.error.path, "/stores");
    EXPECT_NE(postgres.error.message.find("Postgres"), std::string::npos) << postgres.error.message;
    const auto kinds = apps.call("list_service_kinds", json::object());
    ASSERT_TRUE(kinds.ok) << kinds.error.message;
    for (const json& kind : kinds.value.at("kinds")) {
        EXPECT_EQ(kind.at("mounts_stores"),
                  kind.at("kind") == "sqlite" || kind.at("kind") == "duckdb" ||
                      kind.at("kind") == "polars")
            << kind.dump();
    }

    // A volume needs an absolute path; a store of another workspace is not found.
    const auto pathless =
        apps.call("create_service",
                  {{"kind", "sqlite"}, {"name", "db"}, {"stores", {{{"store_id", uploads}}}}});
    ASSERT_FALSE(pathless.ok);
    EXPECT_EQ(pathless.error.path, "/stores/0/mount_path");
    EXPECT_EQ(apps.call("create_service",
                        {{"kind", "sqlite"},
                         {"name", "db"},
                         {"stores", {{{"store_id", uploads}, {"mount_path", "data"}}}}})
                  .error.path,
              "/stores/0/mount_path");
    EXPECT_EQ(apps.call("create_service",
                        {{"kind", "sqlite"},
                         {"name", "db"},
                         {"stores", {{{"store_id", uploads}, {"mount_path", "/a/../etc"}}}}})
                  .error.code,
              "invalid_input");
    const auto foreign =
        apps.call("create_service",
                  {{"kind", "sqlite"},
                   {"name", "db"},
                   {"stores", {{{"store_id", elsewhere}, {"mount_path", "/data"}}}}});
    ASSERT_FALSE(foreign.ok);
    EXPECT_EQ(foreign.error.code, "not_found");
    EXPECT_EQ(foreign.error.path, "/stores/0/store_id");

    const auto db = apps.call("create_service",
                              {{"kind", "sqlite"},
                               {"name", "db"},
                               {"stores", {{{"store_id", uploads}, {"mount_path", "/data"}}}}});
    ASSERT_TRUE(db.ok) << db.error.message;
    ASSERT_EQ(db.value.at("stores").size(), 1U);
    EXPECT_EQ(db.value["stores"][0]["name"], "uploads");
    EXPECT_EQ(db.value["stores"][0]["mount_path"], "/data");
    EXPECT_EQ(db.value["stores"][0]["read_only"], false);
    const std::string db_id = db.value.at("id");

    // A volume is mounted by one workload; the conflict names who mounts it.
    const auto created = apps.create();
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string app_id = created.value.at("id");
    EXPECT_EQ(created.value.at("stores"), json::array());
    const auto taken = apps.call(
        "set_app_stores",
        {{"app_id", app_id}, {"stores", {{{"store_id", uploads}, {"mount_path", "/data"}}}}});
    ASSERT_FALSE(taken.ok);
    EXPECT_EQ(taken.error.code, "conflict");
    EXPECT_NE(taken.error.message.find("service db"), std::string::npos) << taken.error.message;

    // A bucket has no path; a store Lake runs is not mounted; a store is named once.
    EXPECT_EQ(apps.call("set_app_stores",
                        {{"app_id", app_id},
                         {"stores", {{{"store_id", media}, {"mount_path", "/media"}}}}})
                  .error.path,
              "/stores/0/mount_path");
    const auto lake_run =
        apps.call("set_app_stores", {{"app_id", app_id}, {"stores", {{{"store_id", notes}}}}});
    ASSERT_FALSE(lake_run.ok);
    EXPECT_EQ(lake_run.error.code, "invalid_input");
    EXPECT_EQ(
        apps.call("set_app_stores",
                  {{"app_id", app_id}, {"stores", {{{"store_id", media}}, {{"store_id", media}}}}})
            .error.path,
        "/stores/1/store_id");
    const std::int64_t before = apps.store->get_web_app(app_id)->generation;
    const auto mounted =
        apps.call("set_app_stores",
                  {{"app_id", app_id}, {"stores", {{{"store_id", media}, {"read_only", true}}}}});
    ASSERT_TRUE(mounted.ok) << mounted.error.message;
    EXPECT_EQ(apps.store->get_web_app(app_id)->generation, before + 1)
        << "the operator rolls the app's pods";
    ASSERT_EQ(mounted.value.at("stores").size(), 1U);
    EXPECT_EQ(mounted.value["stores"][0]["name"], "media");
    EXPECT_EQ(mounted.value["stores"][0]["read_only"], true);
    const auto read = apps.call("get_app", {{"app_id", app_id}});
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.value.at("stores"), mounted.value.at("stores"));
    const auto listed_apps = apps.call("list_apps", json::object());
    ASSERT_TRUE(listed_apps.ok) << listed_apps.error.message;
    EXPECT_EQ(listed_apps.value["apps"][0]["stores"][0]["store_id"], media);

    // list_stores answers who mounts each, and the used space once the operator measured it.
    ASSERT_TRUE(apps.store->set_store_used_bytes(uploads, 5 * 1024 * 1024));
    const auto listed = apps.call("list_stores", json::object());
    ASSERT_TRUE(listed.ok) << listed.error.message;
    const json* volume = store_named(listed.value, "uploads");
    ASSERT_NE(volume, nullptr);
    EXPECT_EQ(volume->at("mounted_by"),
              json::array({{{"kind", "service"},
                            {"id", db_id},
                            {"name", "db"},
                            {"mount_path", "/data"},
                            {"read_only", false}}}));
    EXPECT_EQ(volume->at("used_bytes"), 5 * 1024 * 1024);
    EXPECT_TRUE(volume->at("used_bytes_at").is_string());
    const json* bucket = store_named(listed.value, "media");
    ASSERT_NE(bucket, nullptr);
    EXPECT_EQ(bucket->at("mounted_by")[0]["kind"], "app");
    EXPECT_EQ(bucket->at("mounted_by")[0]["name"], "reports");
    EXPECT_TRUE(bucket->at("used_bytes").is_null()) << "not measured";
    EXPECT_TRUE(bucket->at("used_bytes_at").is_null());
    const auto services = apps.call("list_services", json::object());
    ASSERT_TRUE(services.ok) << services.error.message;
    for (const json& service : services.value.at("services")) {
        EXPECT_EQ(service.at("stores").size(), service.at("id") == db_id ? 1U : 0U);
    }

    // A mounted store is not deleted: the conflict names the workloads.
    const auto refused = apps.call("delete_store", {{"store_id", uploads}, {"confirm", "uploads"}});
    ASSERT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "conflict");
    EXPECT_NE(refused.error.message.find("service db"), std::string::npos) << refused.error.message;
    EXPECT_EQ(apps.store->get_store(uploads)->desired, "present");

    // set_service_stores replaces what a service mounts and rolls it; [] mounts nothing.
    const std::int64_t service_generation = apps.store->get_service(db_id)->generation;
    const auto cleared =
        apps.call("set_service_stores", {{"service_id", db_id}, {"stores", json::array()}});
    ASSERT_TRUE(cleared.ok) << cleared.error.message;
    EXPECT_EQ(cleared.value.at("stores"), json::array());
    EXPECT_EQ(apps.store->get_service(db_id)->generation, service_generation + 1);
    ASSERT_TRUE(apps.call("set_service_stores",
                          {{"service_id", db_id},
                           {"stores", {{{"store_id", uploads}, {"mount_path", "/var/db"}}}}})
                    .ok);
    const auto other = apps.call("create_service", {{"kind", "postgres"}, {"name", "pg"}});
    ASSERT_TRUE(other.ok) << other.error.message;
    EXPECT_EQ(apps.call("set_service_stores",
                        {{"service_id", other.value.at("id")}, {"stores", {{{"store_id", media}}}}})
                  .error.code,
              "invalid_input");

    // Deleting the service and the app removes their mounts; the stores can then go.
    ASSERT_TRUE(apps.call("delete_service", {{"service_id", db_id}, {"confirm", "db"}}).ok);
    ASSERT_TRUE(apps.call("delete_app", {{"app_id", app_id}, {"confirm", "reports"}}).ok);
    const auto after = apps.call("list_stores", json::object());
    ASSERT_TRUE(after.ok) << after.error.message;
    EXPECT_EQ(store_named(after.value, "uploads")->at("mounted_by"), json::array());
    EXPECT_EQ(store_named(after.value, "media")->at("mounted_by"), json::array());
    EXPECT_TRUE(apps.call("delete_store", {{"store_id", uploads}, {"confirm", "uploads"}}).ok);
    // A store being deleted is not mounted.
    const auto gone = apps.create("second");
    ASSERT_TRUE(gone.ok) << gone.error.message;
    EXPECT_EQ(apps.call("set_app_stores",
                        {{"app_id", gone.value.at("id")},
                         {"stores", {{{"store_id", uploads}, {"mount_path", "/data"}}}}})
                  .error.code,
              "invalid_input");
}
