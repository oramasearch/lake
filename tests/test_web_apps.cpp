#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/agent_bus.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/platform.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "temporary_directory.hpp"

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

class UnusedWorkspaceCaller final : public lake::control::WorkspaceCaller {
public:
    liquid::runtime::task<lake::control::ZettaControlResult<json>>
    call(std::string, std::string, std::string, bool, json) override {
        co_return lake::control::ZettaControlResult<json>(json::object());
    }
};

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
    lake::api::Registry registry;
    std::shared_ptr<lake::control::MemoryAgentBus> raw_bus =
        std::make_shared<lake::control::MemoryAgentBus>();
    std::shared_ptr<lake::control::AgentTaskBus> bus =
        std::make_shared<lake::control::AgentTaskBus>(store, raw_bus, std::vector<std::string>{});
    std::filesystem::path service_dir =
        lake::test::fixture_path("web-app-services-" + lake::control::new_uuid_v4());
    std::shared_ptr<UnusedWorkspaceCaller> workspace_caller =
        std::make_shared<UnusedWorkspaceCaller>();
    std::shared_ptr<lake::control::WorkspaceService> workspaces;
    std::shared_ptr<FakeLoki> loki = std::make_shared<FakeLoki>();
    std::shared_ptr<lake::control::PlatformService> platform;

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
        workspaces = std::make_shared<lake::control::WorkspaceService>(
            lake::control::WorkspaceConfig{service_dir.string(), 5}, store, workspace_caller);
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
        context.apps_wildcard = "apps.example.test";
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
        context.pool_id = principal.pool_id;
        context.scopes = principal.scopes;
        context.request_id = lake::control::new_uuid_v4();
        input["customer_id"] = customer_id;
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
                     {"mode", "source"},
                     {"source",
                      {{"repository", "https://example.test/team/app.git"},
                       {"revision", "main"},
                       {"path", "web"}}},
                     {"framework", "node"},
                     {"port", 3000},
                     {"bindings", {{"zetta", true}, {"services", json::array()}}},
                     {"env", {{"LOG_LEVEL", "info"}}}});
    }
};

} // namespace

TEST(WebApps, DesiredStateStatusAndQuotasFollowTheContract) {
    WebApps apps;
    const auto created = apps.create();
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_EQ(created.value["host"], "reports.apps.example.test");
    EXPECT_EQ(created.value["auth"], "lake");
    EXPECT_EQ(created.value["generation"], 1);
    EXPECT_EQ(created.value["bindings"]["zetta"]["secret_ref"], "workspace-token");
    EXPECT_FALSE(created.value.dump().contains("workspace-admin-token"));

    const std::string id = created.value.at("id");
    const auto audit_events = apps.store->list_audit_events({.limit = 100});
    ASSERT_TRUE(audit_events) << audit_events.error().message;
    EXPECT_TRUE(std::ranges::any_of(*audit_events, [&](const lake::audit::Event& event) {
        return event.draft.action == "create_app" && event.draft.object == "app/" + id &&
               event.draft.principal == apps.owner.taap_id && event.draft.outcome == "success";
    }));
    ASSERT_TRUE(apps.store->set_web_app_status(
        id, "Ready", "https://reports.apps.example.test", true, "available", 1));
    const auto observed = apps.call("get_app", {{"app_id", id}});
    ASSERT_TRUE(observed.ok) << observed.error.message;
    EXPECT_EQ(observed.value["phase"], "Ready");
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

TEST(WebApps, CapabilitiesAreCustomerScopedAndValidateSecretsHostsAndDeletes) {
    WebApps apps;
    for (const std::string& read :
         {"list_apps", "get_app", "get_app_credentials", "app_logs", "receive_app_events"}) {
        ASSERT_EQ(apps.registry.find(read)->action, "read") << read;
    }
    for (const std::string& write : {"create_app",
                                     "deploy_app",
                                     "rollback_app",
                                     "delete_app",
                                     "set_app_env",
                                     "unset_app_env",
                                     "restart_app",
                                     "scale_app",
                                     "stop_app",
                                     "start_app",
                                     "bind_service_to_app",
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

    const auto created = apps.create();
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string id = created.value.at("id");
    const auto foreign = apps.call(apps.other, "get_app", {{"app_id", id}});
    EXPECT_EQ(foreign.error.code, "unauthorized");
    const auto secret = apps.call("set_app_env",
                                  {{"app_id", id},
                                   {"key", "AWS_SECRET_ACCESS_KEY"},
                                   {"value", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"}});
    EXPECT_EQ(secret.error.code, "secret_required");
    const auto reference = apps.call(
        "set_app_env", {{"app_id", id}, {"key", "DATABASE_URL"}, {"secret_ref", "db-url"}});
    ASSERT_TRUE(reference.ok) << reference.error.message;
    EXPECT_FALSE(reference.value["env"].contains("DATABASE_URL"));
    EXPECT_EQ(reference.value["secret_env_refs"]["DATABASE_URL"], "db-url");
    EXPECT_TRUE(apps.call("unset_app_env", {{"app_id", id}, {"key", "DATABASE_URL"}}).ok);

    const auto deployed =
        apps.call("deploy_app", {{"app_id", id}, {"image", "registry.example.test/reports:v2"}});
    ASSERT_TRUE(deployed.ok) << deployed.error.message;
    EXPECT_EQ(deployed.value["mode"], "image");
    const auto rolled_back = apps.call(
        "rollback_app",
        {{"app_id", id},
         {"source", {{"repository", "https://example.test/team/app.git"}, {"revision", "v1"}}}});
    ASSERT_TRUE(rolled_back.ok) << rolled_back.error.message;
    EXPECT_EQ(rolled_back.value["source"]["revision"], "v1");
    EXPECT_EQ(apps.call("delete_app", {{"app_id", id}, {"confirm", "wrong"}}).error.code,
              "invalid_input");
    const auto deleted = apps.call("delete_app", {{"app_id", id}, {"confirm", "reports"}});
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(deleted.value["desired"], "absent");
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
    EXPECT_TRUE(bound.value["bindings"]["services"][0].contains("secret_ref"));
    EXPECT_FALSE(bound.value.dump().contains("password"));

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
    EXPECT_EQ(received.value["events"].back()["generation"], 2);
    EXPECT_EQ(received.value["events"].back()["principal"], apps.owner.taap_id);
}

TEST(WebApps, LakeMintsScopedCredentialsAndOnlyTheOperatorCanResolveThem) {
    WebApps apps;
    const auto created = apps.create();
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string id = created.value.at("id");

    const auto credentials = apps.store->web_app_credentials(id);
    ASSERT_TRUE(credentials) << credentials.error().message;
    ASSERT_EQ(credentials->size(), 2U);
    const auto workspace =
        std::ranges::find(*credentials, "workspace", [](const auto& value) { return value.kind; });
    const auto events =
        std::ranges::find(*credentials, "events", [](const auto& value) { return value.kind; });
    ASSERT_NE(workspace, credentials->end());
    ASSERT_NE(events, credentials->end());
    EXPECT_TRUE(workspace->value.starts_with(lake::control::web_app_workspace_credential_prefix));
    EXPECT_EQ(workspace->scopes,
              std::vector<std::string>({"workspace_query", "workspace_openapi"}));
    EXPECT_TRUE(events->value.starts_with(lake::control::web_app_event_credential_prefix));
    EXPECT_EQ(events->scopes, std::vector<std::string>({"publish_app_event"}));
    EXPECT_FALSE(created.value.dump().contains(workspace->value));
    EXPECT_FALSE(created.value.dump().contains(events->value));

    const auto workspace_principal = apps.store->context_for(workspace->value);
    ASSERT_TRUE(workspace_principal) << workspace_principal.error().message;
    EXPECT_EQ(workspace_principal->kind, "app_workspace");
    EXPECT_EQ(workspace_principal->workload_id, id);
    EXPECT_EQ(workspace_principal->workload_customer_id, apps.customer_id);
    EXPECT_EQ(apps.call(*workspace_principal, "get_app", {{"app_id", id}}).error.code,
              "unauthorized");

    const auto event_principal = apps.store->context_for(events->value);
    ASSERT_TRUE(event_principal) << event_principal.error().message;
    const auto published =
        apps.call(*event_principal, "publish_app_event", {{"app_id", id}, {"type", "rollout"}});
    ASSERT_TRUE(published.ok) << published.error.message;
    const auto second = apps.create("second");
    ASSERT_TRUE(second.ok) << second.error.message;
    EXPECT_EQ(apps.call(*event_principal,
                        "publish_app_event",
                        {{"app_id", second.value.at("id")}, {"type", "pod"}})
                  .error.code,
              "unauthorized");

    EXPECT_EQ(apps.call("get_app_credentials", {{"app_id", id}}).error.code, "unauthorized");
    lake::api::ApiContext operator_context;
    operator_context.store = apps.store.get();
    operator_context.authorized = true;
    operator_context.operator_credential = true;
    operator_context.acting_identity = "admin";
    operator_context.request_id = lake::control::new_uuid_v4();
    const auto resolved = apps.registry.call("get_app_credentials",
                                             {{"customer_id", apps.customer_id}, {"app_id", id}},
                                             operator_context);
    ASSERT_TRUE(resolved.ok) << resolved.error.message;
    EXPECT_EQ(resolved.value["credentials"].size(), 2U);

    ASSERT_TRUE(apps.call("delete_app", {{"app_id", id}, {"confirm", "reports"}}).ok);
    EXPECT_FALSE(apps.store->context_for(workspace->value));
    EXPECT_TRUE(apps.store->context_for(events->value));
    ASSERT_TRUE(apps.store->set_web_app_status(id, "Deleted", "", false, "", 2));
    EXPECT_FALSE(apps.store->context_for(events->value));
}
