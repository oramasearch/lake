#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/web_ui.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "temporary_directory.hpp"

namespace {

using json = nlohmann::json;
using lake::control::ZettaControlResult;

constexpr std::string_view customer = "0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0";

class FakeCaller final : public lake::control::WorkspaceCaller {
public:
    std::string url, token, capability;
    bool read = false;
    json input;
    json value = json::object();
    std::map<std::string, json> answers; // per capability, when set; else `value`
    std::vector<std::pair<std::string, json>> calls;

    liquid::runtime::task<ZettaControlResult<json>> call(std::string called_url,
                                                         std::string called_token,
                                                         std::string called_capability,
                                                         bool called_read,
                                                         json called_input) override {
        url = std::move(called_url);
        token = std::move(called_token);
        capability = std::move(called_capability);
        read = called_read;
        input = std::move(called_input);
        calls.emplace_back(capability, input);
        const auto answer = answers.find(capability);
        co_return ZettaControlResult<json>(answer == answers.end() ? value : answer->second);
    }
};

lake::control::ClerkIdentity minted(std::string subject) {
    const auto identity = lake::audit::mint_custodial_identity("workspace-test-kek");
    return lake::control::ClerkIdentity{subject,
                                        subject + "@example.test",
                                        "",
                                        "browser",
                                        identity->taap_id,
                                        identity->public_key,
                                        identity->wrapped_seed};
}

// A store with one signed-in person and their personal customer, and the service over it.
struct Workspaces {
    std::filesystem::path dir =
        lake::test::fixture_path("service-connections-" + lake::control::new_uuid_v4());
    std::shared_ptr<lake::control::InMemoryStore> store =
        std::make_shared<lake::control::InMemoryStore>();
    std::shared_ptr<FakeCaller> caller = std::make_shared<FakeCaller>();
    std::shared_ptr<lake::control::WorkspaceService> service;
    std::string customer_id, secret;

    explicit Workspaces(std::size_t limit = 5) {
        std::filesystem::create_directories(dir);
        service = std::make_shared<lake::control::WorkspaceService>(
            lake::control::WorkspaceConfig{dir.string(), limit}, store, caller);
        const auto session = store->sign_in_clerk(minted("owner"));
        EXPECT_TRUE(session);
        secret = session->secret;
        const auto principal = store->context_for(secret);
        const auto customers = store->list_customers(10, principal->user_id);
        customer_id = customers->front().id;
    }
    // What the operator does once a service is Ready.
    void deliver(const lake::control::ManagedService& service, json connection) {
        std::ofstream(dir / (service.slug + ".json")) << connection.dump();
    }
    template <class Task> auto run(Task task) {
        auto completed = liquid::runtime::block_on(std::move(task));
        EXPECT_TRUE(completed);
        return std::move(*completed);
    }
};

} // namespace

TEST(Workspaces, SlugIsDerivedFromTheCustomerId) {
    EXPECT_EQ(lake::control::workspace_slug(customer), "ws-0f1e2d3c4b5a");
    EXPECT_EQ(lake::control::workspace_slug("0F1E2D3C-4B5A-6978-8796-A5B4C3D2E1F0"),
              "ws-0f1e2d3c4b5a");
    EXPECT_EQ(lake::control::workspace_slug("not-a-uuid"), "");
    EXPECT_EQ(lake::control::workspace_slug("0f1e2d3c-4b5a"), "");
}

TEST(Workspaces, TheWorkspaceIsTheDefaultZettaServiceAndReadyNeedsTheOperator) {
    Workspaces w;
    auto state = w.run(w.service->state(w.customer_id));
    ASSERT_TRUE(state);
    EXPECT_FALSE(state->requested);
    EXPECT_FALSE(state->ready());

    auto requested = w.run(w.service->request(w.customer_id, "taap:owner"));
    ASSERT_TRUE(requested);
    EXPECT_TRUE(requested->requested);
    EXPECT_EQ(requested->slug, lake::control::workspace_slug(w.customer_id))
        << "the first workspace keeps decision 45's name, so its cluster is adopted";
    ASSERT_TRUE(w.run(w.service->request(w.customer_id)));
    const auto services = w.store->list_services(w.customer_id);
    ASSERT_TRUE(services);
    ASSERT_EQ(services->size(), 1U) << "asking again creates nothing";
    const lake::control::ManagedService workspace = services->front();
    EXPECT_EQ(workspace.kind, "zetta");
    EXPECT_TRUE(workspace.is_default);
    EXPECT_EQ(workspace.created_by_taap_id, "taap:owner");

    ASSERT_TRUE(w.store->set_service_status(workspace.id, "Ready", "", "http://ws.test:4800"));
    state = w.run(w.service->state(w.customer_id));
    ASSERT_TRUE(state);
    EXPECT_EQ(state->phase, "Ready");
    EXPECT_FALSE(state->ready()) << "no credential delivered yet";
    w.deliver(workspace, {{"kind", "zetta"}, {"token", "workspace-admin-token"}});
    state = w.run(w.service->state(w.customer_id));
    ASSERT_TRUE(state);
    EXPECT_TRUE(state->ready());
    EXPECT_EQ(state->to_json()["ready"], true);
}

TEST(Workspaces, DeletingTheWorkspaceWithdrawsItAndARestartGetsANewSlug) {
    Workspaces w;
    auto refused = w.run(w.service->withdraw(w.customer_id));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "not_found") << "nothing to delete before a request";

    auto first = w.run(w.service->request(w.customer_id));
    ASSERT_TRUE(first);
    ASSERT_TRUE(w.store->set_service_status(first->service_id, "Ready", "", "http://a:4800"));
    auto withdrawn = w.run(w.service->withdraw(w.customer_id));
    ASSERT_TRUE(withdrawn);
    EXPECT_FALSE(withdrawn->requested);
    const auto old = w.store->get_service(first->service_id);
    ASSERT_TRUE(old);
    EXPECT_EQ(old->desired, "absent");
    EXPECT_EQ(old->generation, 2);

    auto state = w.run(w.service->state(w.customer_id));
    ASSERT_TRUE(state);
    EXPECT_FALSE(state->requested) << "a deleting workspace is not the workspace";
    auto second = w.run(w.service->request(w.customer_id));
    ASSERT_TRUE(second);
    EXPECT_NE(second->service_id, first->service_id);
    EXPECT_TRUE(second->slug.starts_with("svc-")) << "a slug is never reused";
    EXPECT_EQ(second->phase, "") << "the old Ready does not carry over";

    ASSERT_TRUE(w.store->set_service_status(first->service_id, "Deleted", "", ""));
    const auto listed = w.store->list_services(w.customer_id);
    ASSERT_TRUE(listed);
    EXPECT_EQ(listed->size(), 1U) << "a finished deletion is history";
}

TEST(Workspaces, ServicesAreNamedPerCustomerAndCapped) {
    Workspaces w(2);
    lake::control::NewManagedService postgres{w.customer_id, "postgres", "crm", "taap:owner"};
    const auto created = w.service->create_service(postgres);
    ASSERT_TRUE(created);
    EXPECT_TRUE(created->slug.starts_with("svc-"));
    EXPECT_EQ(created->desired, "present");
    const auto duplicate = w.service->create_service(postgres);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, "conflict");
    ASSERT_TRUE(w.service->create_service({w.customer_id, "zetta", "search", ""}));
    const auto over = w.service->create_service({w.customer_id, "zetta", "third", ""});
    ASSERT_FALSE(over);
    EXPECT_EQ(over.error().code, "quota_exceeded");

    const auto deleted = w.service->delete_service(w.customer_id, created->id);
    ASSERT_TRUE(deleted);
    EXPECT_EQ(deleted->desired, "absent");
    EXPECT_TRUE(w.service->create_service({w.customer_id, "zetta", "third", ""}))
        << "a deleted service frees its place";
    const auto again = w.service->delete_service(w.customer_id, created->id);
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, "conflict");
    const auto foreign =
        w.service->delete_service("11111111-2222-3333-4444-555555555555", created->id);
    ASSERT_FALSE(foreign);
    EXPECT_EQ(foreign.error().code, "not_found") << "another customer cannot see the service";
}

TEST(Workspaces, CallsReachOnlyTheCustomersOwnCluster) {
    Workspaces w;
    auto refused = w.run(w.service->call(w.customer_id, "list_projects", true, {}));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "workspace_not_ready");

    auto requested = w.run(w.service->request(w.customer_id));
    ASSERT_TRUE(requested);
    const auto workspace = w.store->get_service(requested->service_id);
    ASSERT_TRUE(w.store->set_service_status(
        workspace->id, "Ready", "", "http://prod-ws-own-zetta-client.lake.svc.cluster.local:4800"));
    w.deliver(*workspace, {{"kind", "zetta"}, {"token", "workspace-admin-token"}});
    w.caller->value = {{"projects", json::array({"retention"})}};
    auto called =
        w.run(w.service->call(w.customer_id, "get_project", true, {{"name", "retention"}}));
    ASSERT_TRUE(called);
    EXPECT_EQ((*called)["projects"][0], "retention");
    EXPECT_EQ(w.caller->url, "http://prod-ws-own-zetta-client.lake.svc.cluster.local:4800");
    EXPECT_EQ(w.caller->token, "workspace-admin-token");
    EXPECT_EQ(w.caller->capability, "get_project");
    EXPECT_TRUE(w.caller->read);
    EXPECT_EQ(w.caller->input["name"], "retention");
}

TEST(Workspaces, CapabilitiesAreAllowListedAndAuthorizedOnTheCustomer) {
    Workspaces w;
    lake::control::InMemoryStore& store = *w.store;
    const auto other = store.sign_in_clerk(minted("other"));
    ASSERT_TRUE(other);
    const auto owner_principal = store.context_for(w.secret);
    const auto other_principal = store.context_for(other->secret);
    ASSERT_TRUE(owner_principal);
    ASSERT_TRUE(other_principal);
    const std::string owner_customer = w.customer_id;

    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    const auto call =
        [&](const lake::control::Principal& principal, const std::string& name, json input) {
            lake::api::ApiContext context;
            context.store = &store;
            context.workspaces = w.service.get();
            context.authorized = true;
            context.acting_user_id = principal.user_id;
            context.acting_identity = principal.taap_id;
            context.request_id = lake::control::new_uuid_v4();
            auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
            EXPECT_TRUE(completed);
            return std::move(*completed);
        };

    const auto started =
        call(*owner_principal, "start_workspace", {{"customer_id", owner_customer}});
    ASSERT_TRUE(started.ok) << started.error.message;
    EXPECT_EQ(started.value["workspace"], lake::control::workspace_slug(owner_customer));

    const auto foreign =
        call(*other_principal, "workspace_status", {{"customer_id", owner_customer}});
    EXPECT_FALSE(foreign.ok) << "another customer's workspace is not readable";
    const auto foreign_start =
        call(*other_principal, "start_workspace", {{"customer_id", owner_customer}});
    EXPECT_FALSE(foreign_start.ok);

    const auto admin_surface = call(*owner_principal,
                                    "workspace_query",
                                    {{"customer_id", owner_customer},
                                     {"capability", "sql"},
                                     {"input", {{"statement", "SELECT 1"}}}});
    ASSERT_FALSE(admin_surface.ok);
    EXPECT_EQ(admin_surface.error.code, "invalid_input");
    const auto action_as_read =
        call(*owner_principal,
             "workspace_query",
             {{"customer_id", owner_customer}, {"capability", "create_project"}});
    EXPECT_FALSE(action_as_read.ok) << "an action is not reachable through workspace_query";
    const auto not_ready = call(*owner_principal,
                                "workspace_query",
                                {{"customer_id", owner_customer}, {"capability", "list_projects"}});
    ASSERT_FALSE(not_ready.ok);
    EXPECT_EQ(not_ready.error.code, "workspace_not_ready");
    for (const char* action : {"create_connection",
                               "delete_connection",
                               "create_dataset_from_query",
                               "upload_dataset",
                               "delete_project"}) {
        const auto allowed = call(*owner_principal,
                                  "workspace_act",
                                  {{"customer_id", owner_customer}, {"capability", action}});
        EXPECT_EQ(allowed.error.code, "workspace_not_ready") << action << " is an allowed action";
    }

    // Services.
    const auto created = call(*owner_principal,
                              "create_service",
                              {{"customer_id", owner_customer},
                               {"kind", "postgres"},
                               {"name", "crm"},
                               {"storage_gi", 20}});
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_EQ(created.value["storage_gi"], 20);
    const std::string service_id = created.value["id"];
    const auto bad_name =
        call(*owner_principal,
             "create_service",
             {{"customer_id", owner_customer}, {"kind", "zetta"}, {"name", "Bad"}});
    EXPECT_FALSE(bad_name.ok);
    EXPECT_FALSE(call(*owner_principal,
                      "create_service",
                      {{"customer_id", owner_customer},
                       {"kind", "sqlite"},
                       {"name", "notes"},
                       {"instances", 2}})
                     .ok)
        << "SQLite has one writer";
    for (const char* kind : {"kafka", "clickhouse", "tidb", "cassandra", "valkey", "sqlite"}) {
        const auto catalog = call(
            *owner_principal,
            "create_service",
            {{"customer_id", owner_customer}, {"kind", kind}, {"name", std::string("c-") + kind}});
        ASSERT_TRUE(catalog.ok) << kind << ": " << catalog.error.message;
        EXPECT_EQ(catalog.value["kind"], kind);
        // Deleted at once, so the per-customer cap is not what this loop tests.
        ASSERT_TRUE(store.withdraw_service(catalog.value["id"]));
    }
    EXPECT_FALSE(call(*owner_principal,
                      "create_service",
                      {{"customer_id", owner_customer}, {"kind", "mongodb"}, {"name", "s"}})
                     .ok)
        << "a kind outside the catalog is refused";
    const auto listed = call(*owner_principal, "list_services", {{"customer_id", owner_customer}});
    ASSERT_TRUE(listed.ok);
    EXPECT_EQ(listed.value["services"].size(), 8U) << "the workspace, crm and six deleting";
    EXPECT_FALSE(call(*other_principal, "list_services", {{"customer_id", owner_customer}}).ok);
    EXPECT_FALSE(call(*other_principal,
                      "create_service",
                      {{"customer_id", owner_customer}, {"kind", "zetta"}, {"name", "x"}})
                     .ok);

    const auto early = call(*owner_principal,
                            "service_connection",
                            {{"customer_id", owner_customer}, {"service_id", service_id}});
    ASSERT_FALSE(early.ok);
    EXPECT_EQ(early.error.code, "workspace_not_ready");
    const auto service = store.get_service(service_id);
    ASSERT_TRUE(store.set_service_status(service_id, "Ready", "", "prod-crm-rw:5432"));
    w.deliver(*service,
              {{"kind", "postgres"}, {"host", "prod-crm-rw"}, {"user", "app"}, {"password", "pw"}});
    const auto connection = call(*owner_principal,
                                 "service_connection",
                                 {{"customer_id", owner_customer}, {"service_id", service_id}});
    ASSERT_TRUE(connection.ok) << connection.error.message;
    EXPECT_EQ(connection.value["password"], "pw");
    EXPECT_EQ(listed.value.dump().find("pw"), std::string::npos);
    EXPECT_FALSE(connection.value.contains("kind"));

    // A SQLite service's credential is its bearer token.
    const auto sqlite =
        call(*owner_principal,
             "create_service",
             {{"customer_id", owner_customer}, {"kind", "sqlite"}, {"name", "notes"}});
    ASSERT_TRUE(sqlite.ok) << sqlite.error.message;
    const std::string sqlite_id = sqlite.value["id"];
    ASSERT_TRUE(store.set_service_status(sqlite_id, "Ready", "", "http://notes:8080"));
    w.deliver(*store.get_service(sqlite_id),
              {{"kind", "sqlite"},
               {"endpoint", "http://notes:8080"},
               {"token", "t0k"},
               {"database", "main"}});
    const auto sqlite_connection =
        call(*owner_principal,
             "service_connection",
             {{"customer_id", owner_customer}, {"service_id", sqlite_id}});
    ASSERT_TRUE(sqlite_connection.ok) << sqlite_connection.error.message;
    EXPECT_EQ(sqlite_connection.value["token"], "t0k");
    ASSERT_TRUE(store.withdraw_service(sqlite_id));
    const auto workspace_token =
        call(*owner_principal,
             "service_connection",
             {{"customer_id", owner_customer}, {"service_id", started.value["service_id"]}});
    EXPECT_FALSE(workspace_token.ok) << "a Zetta admin token is never handed out";

    const auto unconfirmed_service =
        call(*owner_principal,
             "delete_service",
             {{"customer_id", owner_customer}, {"service_id", service_id}, {"confirm", "yes"}});
    EXPECT_FALSE(unconfirmed_service.ok);
    const auto deleted_service =
        call(*owner_principal,
             "delete_service",
             {{"customer_id", owner_customer}, {"service_id", service_id}, {"confirm", "crm"}});
    ASSERT_TRUE(deleted_service.ok) << deleted_service.error.message;
    EXPECT_EQ(deleted_service.value["desired"], "absent");

    const std::string slug = lake::control::workspace_slug(owner_customer);
    const auto foreign_delete = call(
        *other_principal, "delete_workspace", {{"customer_id", owner_customer}, {"confirm", slug}});
    EXPECT_FALSE(foreign_delete.ok) << "another customer cannot delete the workspace";
    const auto unconfirmed = call(*owner_principal,
                                  "delete_workspace",
                                  {{"customer_id", owner_customer}, {"confirm", "yes"}});
    ASSERT_FALSE(unconfirmed.ok);
    EXPECT_EQ(unconfirmed.error.code, "invalid_input");
    const auto deleted = call(
        *owner_principal, "delete_workspace", {{"customer_id", owner_customer}, {"confirm", slug}});
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(deleted.value["requested"], false);
}

TEST(Workspaces, PagesGateOnTheWorkspaceAndRenderZettaResults) {
    lake::api::WorkspacePage page;
    page.section = "projects";
    page.customer_id = std::string(customer);
    page.customers = {{std::string(customer), "grand-lantern-42", true},
                      {"11111111-2222-3333-4444-555555555555", "acme", false}};
    std::string html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("Start your workspace"), std::string::npos);
    EXPECT_NE(html.find("action=\"/workspace/start\""), std::string::npos);
    EXPECT_NE(html.find("grand-lantern-42 (your personal space)"), std::string::npos);
    EXPECT_NE(html.find("<p class=\"eyebrow\">Design</p>"), std::string::npos);

    page.state = {{"requested", true}, {"phase", "Provisioning"}, {"ready", false}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("Preparing your workspace"), std::string::npos);
    EXPECT_NE(html.find("http-equiv=\"refresh\""), std::string::npos);

    page.state = {{"requested", true}, {"phase", "Ready"}, {"ready", true}};
    page.data["list_projects"] = {{"projects", json::array({"retention<review>"})}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("retention&lt;review&gt;"), std::string::npos);
    EXPECT_EQ(html.find("retention<review>"), std::string::npos);
    EXPECT_NE(html.find("action=\"/design/projects/create\""), std::string::npos);
    EXPECT_EQ(html.find("action=\"/workspace/delete\""), std::string::npos)
        << "the delete form needs the workspace id from the state";
    page.state["workspace"] = "ws-0f1e2d3c4b5a";
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("action=\"/workspace/delete\""), std::string::npos);
    EXPECT_NE(html.find("name=\"confirm\" value=\"ws-0f1e2d3c4b5a\" required"), std::string::npos);

    page.state = {{"requested", true},
                  {"phase", "Failed"},
                  {"ready", false},
                  {"workspace", "ws-0f1e2d3c4b5a"},
                  {"message", "workspace quota reached (3)"}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("as many workspaces as it allows"), std::string::npos);
    EXPECT_NE(html.find(">Cancel workspace</button>"), std::string::npos);
    page.state = {
        {"requested", true}, {"phase", "Ready"}, {"ready", true}, {"workspace", "ws-0f1e2d3c4b5a"}};

    page.section = "project";
    page.subject = "retention";
    page.data["get_project"] = {
        {"graphs",
         json::array({{{"id", "g1"}, {"name", "proposed plan"}, {"approval", "proposed"}},
                      {{"id", "g2"}, {"name", "approved plan"}, {"approval", "approved"}}})}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find(">Approve</button>"), std::string::npos);
    EXPECT_NE(html.find(">Check and run</button>"), std::string::npos);
    EXPECT_NE(html.find("action=\"/design/project/delete\""), std::string::npos);
    EXPECT_LT(html.find("/design/project/approve"), html.find("/design/project/run"))
        << "the proposed plan offers Approve, the approved one Check and run";
    page.subject.clear();

    page.section = "run";
    page.subject = "r1";
    page.data["get_run"] = {{"status", "running"}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("The report appears when the run finishes"), std::string::npos);
    page.data["get_run"] = {{"status", "succeeded"}};
    page.data["get_run_report"] = {{"model", {{"auc", 0.9}}}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("<h2>Report</h2>"), std::string::npos);
    page.subject.clear();

    page.section = "data";
    page.data["list_connections"] = json::object();
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("action=\"/train/data/connect\""), std::string::npos);
    EXPECT_NE(html.find("action=\"/train/data/upload\" enctype=\"multipart/form-data\""),
              std::string::npos)
        << "a file can be uploaded without any connection";
    EXPECT_NE(html.find("type=\"password\" name=\"token\""), std::string::npos);
    EXPECT_EQ(html.find("action=\"/train/data/pull\""), std::string::npos)
        << "no dataset can be pulled before a connection exists";
    page.data["list_connections"] = {
        {"connections",
         json::array(
             {{{"name", "warehouse"},
               {"connector", {{"type", "postgres"}, {"host", "db.test"}, {"database", "crm"}}}}})}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("db.test/crm"), std::string::npos);
    EXPECT_NE(html.find("action=\"/train/data/pull\""), std::string::npos);
    EXPECT_NE(html.find("<option>warehouse</option>"), std::string::npos);
    EXPECT_NE(html.find("action=\"/train/data/disconnect\""), std::string::npos);

    page.section = "models";
    page.data["list_models"] = {{"models", json::array({{{"name", "churn"}, {"kind", "gbdt"}}})}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("<p class=\"eyebrow\">Ship</p>"), std::string::npos);
    EXPECT_NE(html.find("action=\"/ship/models/publish\""), std::string::npos);

    page.unavailable = "Design, Train and Ship are not available on this lake yet";
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("not available on this lake yet"), std::string::npos);
    EXPECT_EQ(html.find("Start your workspace"), std::string::npos);
}

TEST(Workspaces, ServicesPageListsEveryServiceWithoutCredentials) {
    lake::api::WorkspacePage page;
    page.section = "services";
    page.customer_id = std::string(customer);
    page.data["list_services"] = {{"services", json::array()}, {"limit", 5}};
    std::string html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("No services yet."), std::string::npos);
    EXPECT_NE(html.find("action=\"/services/create\""), std::string::npos);
    EXPECT_NE(html.find("may run 5 services"), std::string::npos);
    EXPECT_EQ(html.find("Start your workspace"), std::string::npos)
        << "the Services page does not wait for the workspace";

    page.data["list_services"]["services"] =
        json::array({{{"id", "a"},
                      {"name", "workspace"},
                      {"kind", "zetta"},
                      {"default", true},
                      {"instances", 1},
                      {"storage_gi", 10},
                      {"desired", "present"},
                      {"phase", "Ready"},
                      {"endpoint", "http://prod-ws-1-zetta-client:4800"}},
                     {{"id", "b"},
                      {"name", "crm"},
                      {"kind", "postgres"},
                      {"instances", 2},
                      {"storage_gi", 20},
                      {"desired", "present"},
                      {"phase", "Ready"},
                      {"endpoint", "prod-svc-1-rw:5432"}},
                     {{"id", "c"},
                      {"name", "old"},
                      {"kind", "postgres"},
                      {"instances", 1},
                      {"storage_gi", 10},
                      {"desired", "absent"},
                      {"phase", "Ready"}}});
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("workspace <span class=\"muted\">(workspace)</span>"), std::string::npos);
    EXPECT_NE(html.find("2 × 20 GiB"), std::string::npos);
    EXPECT_NE(html.find("prod-svc-1-rw:5432"), std::string::npos);
    EXPECT_NE(html.find(">Deleting<"), std::string::npos) << "an absent service is deleting";
    EXPECT_NE(html.find("action=\"/services/connection\""), std::string::npos);
    EXPECT_NE(html.find(">Delete crm</button>"), std::string::npos);
    EXPECT_EQ(html.find(">Delete old</button>"), std::string::npos);
    EXPECT_NE(html.find("http-equiv=\"refresh\""), std::string::npos) << "one is still changing";
    page.data["list_services"]["services"][1]["connected"] = false;
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find(">Connecting<"), std::string::npos)
        << "Ready but not yet delivered to lake's mount";
    EXPECT_EQ(html.find("action=\"/services/connection\""), std::string::npos);
    page.data["list_services"]["services"][1]["connected"] = true;

    page.data["service_connection"] = {{"name", "crm"}, {"uri", "postgresql://app:pw@host/app"}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("postgresql://app:pw@host/app"), std::string::npos);
    page.data["service_connection"] = {{"name", "events"},
                                       {"bootstrap", "prod-svc-1-kafka-bootstrap:9092"},
                                       {"sasl_mechanism", "SCRAM-SHA-512"}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("<dt>bootstrap</dt>"), std::string::npos)
        << "every kind's connection fields are shown";
    for (const char* option :
         {"<option value=\"kafka\">Kafka — ready in about 3 minutes</option>",
          "<option value=\"clickhouse\">ClickHouse — ready in about 3 minutes</option>",
          "<option value=\"tidb\">TiDB — ready in about 4 minutes</option>",
          "<option value=\"cassandra\">Cassandra — ready in about 10 minutes</option>",
          "<option value=\"valkey\">Valkey — ready in about 2 minutes (preview, not for "
          "production)"}) {
        EXPECT_NE(html.find(option), std::string::npos) << option;
    }
    // A starting service says how long its kind usually takes and how long it has been going.
    page.data["list_services"]["services"] =
        json::array({{{"id", "d"},
                      {"name", "events"},
                      {"kind", "cassandra"},
                      {"instances", 1},
                      {"storage_gi", 2},
                      {"desired", "present"},
                      {"phase", "Provisioning"},
                      {"message", "waiting for the Cassandra datacenter"},
                      {"created_at", "2020-01-01T00:00:00Z"}},
                     {{"id", "e"},
                      {"name", "cache"},
                      {"kind", "valkey"},
                      {"instances", 1},
                      {"storage_gi", 2},
                      {"desired", "present"},
                      {"phase", "Ready"},
                      {"connected", true}}});
    page.data.erase("service_connection");
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("Usually ready in about 10 minutes; started "), std::string::npos);
    EXPECT_NE(html.find("taking longer than usual"), std::string::npos)
        << "a service far past its usual time says so";
    EXPECT_EQ(html.find("Usually ready in about 2 minutes"), std::string::npos)
        << "a Ready service shows no start hint";
}

TEST(Workspaces, NavigationOffersDesignTrainAndShip) {
    const std::string home = lake::api::web_ui_home(true);
    for (const std::string_view label : {"<p class=\"nav-label\">Design</p>",
                                         "<p class=\"nav-label\">Train</p>",
                                         "<p class=\"nav-label\">Ship</p>",
                                         "href=\"/design/projects\"",
                                         "href=\"/train/data\"",
                                         "href=\"/train/runs\"",
                                         "href=\"/ship/models\"",
                                         "href=\"/ship/indexes\"",
                                         "href=\"/deployments\"",
                                         "href=\"/services\""}) {
        EXPECT_NE(home.find(label), std::string::npos) << label;
    }
    EXPECT_EQ(lake::api::web_ui_home().find("/design/projects"), std::string::npos);
    lake::api::WebUiViewer viewer{true};
    viewer.path = "/train/run";
    EXPECT_NE(lake::api::web_ui_home(viewer).find("href=\"/train/runs\" aria-current=\"page\""),
              std::string::npos);
}

TEST(Workspaces, PackagesPageListsTheRegistriesWithInstallCommands) {
    const json catalog{{"oci", json::array({{{"name", "scoring"}, {"versions", {"v1", "v2"}}}})},
                       {"npm", json::array({{{"name", "@acme/ui<x>"}, {"versions", {"1.0.0"}}}})},
                       {"python", json::array()}};
    const std::string html =
        lake::api::web_ui_packages(catalog, true, "https://lake.example.test", true);
    EXPECT_NE(html.find("docker pull lake.example.test/scoring:v2"), std::string::npos);
    EXPECT_NE(html.find("npm install @acme/ui&lt;x&gt; --registry https://lake.example.test"),
              std::string::npos);
    EXPECT_NE(html.find("None published."), std::string::npos);
    EXPECT_NE(lake::api::web_ui_packages(json::object(), false, "", true)
                  .find("no registry storage configured"),
              std::string::npos);
    EXPECT_NE(lake::api::web_ui_home(true).find("href=\"/ship/packages\""), std::string::npos);
}

TEST(Workspaces, EditPipelineReadsValidatesAndSavesTheWholeGraph) {
    Workspaces w;
    lake::control::InMemoryStore& store = *w.store;
    const auto principal = store.context_for(w.secret);
    ASSERT_TRUE(principal);
    auto requested = w.run(w.service->request(w.customer_id));
    ASSERT_TRUE(requested);
    const auto workspace = store.get_service(requested->service_id);
    ASSERT_TRUE(store.set_service_status(workspace->id, "Ready", "", "http://ws:4800"));
    w.deliver(*workspace, {{"kind", "zetta"}, {"token", "t"}});
    w.caller->answers["get_graph"] = {
        {"graph_id", "g-1"},
        {"name", "churn"},
        {"project", "retention"},
        {"approval", "approved"},
        {"nodes",
         json::array({{{"id", "pull"}, {"kind", "connect"}, {"name", "Pull"}},
                      {{"id", "train"}, {"kind", "model"}, {"name", "Train"}}})},
        {"edges", json::array({{{"from", "pull"}, {"to", "train"}, {"kind", "data_flow"}}})}};
    w.caller->answers["save_graph"] = {
        {"graph_id", "g-1"}, {"name", "churn"}, {"approval", "proposed"}};

    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    const auto call = [&](json input) {
        lake::api::ApiContext context;
        context.store = &store;
        context.workspaces = w.service.get();
        context.authorized = true;
        context.acting_user_id = principal->user_id;
        context.acting_identity = principal->taap_id;
        context.request_id = lake::control::new_uuid_v4();
        input["customer_id"] = w.customer_id;
        input["project"] = "retention";
        input["graph"] = "churn";
        auto completed =
            liquid::runtime::block_on(registry.call_async("edit_pipeline", input, context));
        EXPECT_TRUE(completed);
        return std::move(*completed);
    };

    const auto added = call({{"op", "add_step"}, {"kind", "serve"}, {"name", "Rank"}});
    ASSERT_TRUE(added.ok) << added.error.message;
    EXPECT_EQ(added.value["step"], "serve-1");
    ASSERT_EQ(w.caller->calls.back().first, "save_graph");
    const json saved = w.caller->calls.back().second;
    EXPECT_EQ(saved["graph"], "g-1") << "saved by id, so a rename cannot orphan it";
    EXPECT_EQ(saved["nodes"].size(), 3U);
    EXPECT_EQ(saved["edges"].size(), 1U);
    EXPECT_FALSE(saved.contains("new_name"));

    const std::size_t before = w.caller->calls.size();
    const auto cycle = call({{"op", "add_edge"}, {"from", "train"}, {"to", "pull"}});
    ASSERT_FALSE(cycle.ok);
    EXPECT_EQ(cycle.error.code, "invalid_input");
    EXPECT_NE(cycle.error.message.find("cycle"), std::string::npos);
    EXPECT_EQ(w.caller->calls.size(), before + 1) << "read, refused, nothing saved";
    EXPECT_EQ(w.caller->calls.back().first, "get_graph");

    const auto renamed = call({{"op", "rename"}, {"name", "churn v2"}});
    ASSERT_TRUE(renamed.ok) << renamed.error.message;
    EXPECT_EQ(renamed.value["name"], "churn v2");
    EXPECT_EQ(w.caller->calls.back().second["new_name"], "churn v2");
    EXPECT_TRUE(registry.find("edit_pipeline") != nullptr) << "REST, OpenAPI and MCP render it";
}

TEST(Workspaces, PipelinePageShowsTheGraphInspectorEditsGovernanceAndRuns) {
    lake::api::WorkspacePage page;
    page.section = "pipeline";
    page.customer_id = std::string(customer);
    page.project = "retention";
    page.subject = "churn";
    page.step = "train";
    page.state = {{"requested", true}, {"phase", "Ready"}, {"ready", true}};
    page.data["get_graph"] = {
        {"graph_id", "g-1"},
        {"name", "churn"},
        {"project", "retention"},
        {"approval", "proposed"},
        {"nodes",
         json::array({{{"id", "pull"}, {"kind", "connect"}, {"name", "Pull"}},
                      {{"id", "train"}, {"kind", "model"}, {"name", "Train"}}})},
        {"edges", json::array({{{"from", "pull"}, {"to", "train"}, {"kind", "data_flow"}}})}};
    page.data["list_runs"] = {{"runs",
                               json::array({{{"id", "r1"},
                                             {"graph", "churn"},
                                             {"project", "retention"},
                                             {"status", "succeeded"},
                                             {"created", "2026-09-26T10:00:00Z"}}})}};
    page.data["latest_run"] = {{"status", "succeeded"},
                               {"lineage", {{"steps", json::array({"pull", "train"})}}}};
    page.data["list_datasets"] = {
        {"datasets", json::array({{{"name", "accounts"}, {"database", "datasets"}}})}};
    page.data["gate"] = {
        {"verdict", "fail"},
        {"block_deploy", true},
        {"checks",
         json::array({{{"name", "has_source"}, {"status", "fail"}, {"reason", "no data"}}})}};
    const std::string html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("<svg class=\"pipeline-graph\""), std::string::npos);
    EXPECT_NE(html.find("step-model status-succeeded selected"), std::string::npos);
    EXPECT_NE(html.find("<h2>Step Train</h2>"), std::string::npos);
    EXPECT_NE(html.find("proposed — approve before it runs"), std::string::npos);
    EXPECT_NE(html.find("action=\"/design/pipeline/approve\""), std::string::npos);
    EXPECT_NE(html.find("Check and run</button>"), std::string::npos);
    EXPECT_NE(html.find(" disabled>Check and run"), std::string::npos) << "not approved yet";
    EXPECT_NE(html.find("<option value=\"datasets.accounts\">"), std::string::npos);
    EXPECT_NE(html.find("has_source"), std::string::npos) << "every gate check is shown";
    EXPECT_NE(html.find("no data"), std::string::npos);
    for (const char* op : {"rename_step",
                           "kind_step",
                           "remove_step",
                           "add_step",
                           "add_edge",
                           "remove_edge",
                           "rename"}) {
        EXPECT_NE(html.find(std::string("name=\"op\" value=\"") + op + "\""), std::string::npos)
            << op;
    }
    EXPECT_NE(html.find("/train/run?customer_id="), std::string::npos) << "its runs link out";

    page.section = "run";
    page.subject = "r1";
    page.data = {{"get_run",
                  {{"status", "succeeded"},
                   {"graph", "churn"},
                   {"project", "retention"},
                   {"logs", json::array({{{"level", "info"}, {"message", "trained"}}})}}},
                 {"get_run_report",
                  {{"dataset", {{"accounts", 120}, {"features", json::array({"mrr", "seats"})}}},
                   {"drivers", json::array({{{"feature", "failed_payments"}, {"weight", 26.4}}})},
                   {"at_risk", json::array({{{"account", "acme"}, {"risk", 0.91}}})}}},
                 {"list_traces",
                  {{"spans",
                    json::array({{{"name", "run"},
                                  {"duration_ms", 12.5},
                                  {"ok", true},
                                  {"attributes", {{"run_id", "r1"}}}}})}}}};
    const std::string run = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(run.find("What the model leans on"), std::string::npos);
    EXPECT_NE(run.find("failed_payments"), std::string::npos);
    EXPECT_NE(run.find("Most at risk"), std::string::npos);
    EXPECT_NE(run.find("mrr, seats"), std::string::npos);
    EXPECT_NE(run.find("<h2>Trace</h2>"), std::string::npos);
    EXPECT_NE(run.find("<h2>Log</h2>"), std::string::npos);
    EXPECT_NE(run.find("/design/pipeline?"), std::string::npos) << "a run links to its pipeline";
}

TEST(Workspaces, SearchIndexAnswersOnlyForAPublishedIndex) {
    Workspaces w;
    lake::control::InMemoryStore& store = *w.store;
    const auto principal = store.context_for(w.secret);
    auto requested = w.run(w.service->request(w.customer_id));
    const auto workspace = store.get_service(requested->service_id);
    ASSERT_TRUE(store.set_service_status(workspace->id, "Ready", "", "http://ws:4800"));
    w.deliver(*workspace, {{"kind", "zetta"}, {"token", "t"}});
    w.caller->answers["get_index"] = {{"id", "i-1"}, {"table", "docs"}, {"exposed", false}};
    w.caller->answers["search"] = {{"hits", json::array({{{"id", "doc-7"}, {"score", 2.5}}})}};
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    const auto search = [&](json extra) {
        lake::api::ApiContext context;
        context.store = &store;
        context.workspaces = w.service.get();
        context.authorized = true;
        context.acting_user_id = principal->user_id;
        context.acting_identity = principal->taap_id;
        context.request_id = lake::control::new_uuid_v4();
        json input{{"customer_id", w.customer_id}, {"index", "i-1"}, {"query", "renewal"}};
        input.update(extra);
        auto completed =
            liquid::runtime::block_on(registry.call_async("search_index", input, context));
        EXPECT_TRUE(completed);
        return std::move(*completed);
    };
    const auto refused = search(json::object());
    ASSERT_FALSE(refused.ok);
    EXPECT_NE(refused.error.message.find("not published"), std::string::npos);
    EXPECT_NE(w.caller->calls.back().first, "search") << "nothing reaches search";

    w.caller->answers["get_index"]["exposed"] = true;
    const auto found = search({{"field", "body"}, {"top_k", 3}});
    ASSERT_TRUE(found.ok) << found.error.message;
    EXPECT_EQ(found.value["hits"][0]["id"], "doc-7");
    EXPECT_EQ(w.caller->calls.back().first, "search");
    EXPECT_EQ(w.caller->calls.back().second["target"], "i-1");
    EXPECT_EQ(w.caller->calls.back().second["field"], "body");
    EXPECT_EQ(w.caller->calls.back().second["top_k"], 3);
    EXPECT_EQ(registry.find("search_index")->kind, lake::api::CapabilityKind::Read)
        << "GET /v1/search_index for outside callers";
}

TEST(Workspaces, IndexPageSearchesPublishesAndShowsHowToCallIt) {
    lake::api::WorkspacePage page;
    page.section = "index";
    page.customer_id = std::string(customer);
    page.subject = "i-1";
    page.origin = "https://lake.example";
    page.state = {{"requested", true}, {"phase", "Ready"}, {"ready", true}};
    page.data["get_index"] = {{"id", "i-1"},
                              {"table", "docs"},
                              {"kind", "search"},
                              {"fields", json::array({"title", "body"})},
                              {"exposed", false}};
    std::string html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find(">Publish</button>"), std::string::npos);
    EXPECT_NE(html.find("Publish the index first"), std::string::npos);
    EXPECT_EQ(html.find("Create a search key"), std::string::npos);
    EXPECT_NE(html.find("<option>body</option>"), std::string::npos);

    page.data["get_index"]["exposed"] = true;
    page.data["search_query"] = "renewal";
    page.data["search"] = {{"hits", json::array({{{"id", "doc-7"}, {"score", 2.5}}})},
                           {"duration_ms", 1.2}};
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find(">Unpublish</button>"), std::string::npos);
    EXPECT_NE(html.find("doc-7"), std::string::npos);
    EXPECT_NE(html.find("1 hits"), std::string::npos);
    EXPECT_NE(html.find("value=\"search_index\""), std::string::npos) << "a key scoped to search";
    EXPECT_NE(html.find("https://lake.example/v1/search_index?customer_id="), std::string::npos);
    EXPECT_NE(html.find("MCP tool <code>search_index</code>"), std::string::npos);
}

TEST(Workspaces, ASearchKeyFromAnOlderSessionLastsAsLongAsTheSession) {
    Workspaces w;
    lake::control::InMemoryStore& store = *w.store;
    const auto principal = store.context_for(w.secret);
    ASSERT_TRUE(principal);
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::ApiContext context;
    context.store = &store;
    context.authorized = true;
    context.session_credential = true;
    context.acting_user_id = principal->user_id;
    context.acting_identity = principal->taap_id;
    context.credential_session_id = principal->session_id;
    context.credential_kind = principal->kind;
    context.pool_id = principal->pool_id;
    context.scopes = {"*"};
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    context.credential_expires_at_unix = now + 10 * 24 * 60 * 60; // ten days left
    context.request_id = lake::control::new_uuid_v4();

    const auto capped =
        registry.call("create_token",
                      {{"name", "search docs"}, {"scopes", json::array({"search_index"})}},
                      context);
    ASSERT_TRUE(capped.ok) << capped.error.message;
    context.request_id = lake::control::new_uuid_v4();
    const auto too_long = registry.call("create_token",
                                        {{"name", "search docs"},
                                         {"scopes", json::array({"search_index"})},
                                         {"expires_in_seconds", 30 * 24 * 60 * 60}},
                                        context);
    EXPECT_FALSE(too_long.ok) << "an explicit lifetime past the session is still refused";
}

TEST(Workspaces, ConnectServiceFillsTheConnectionFromTheServiceItself) {
    Workspaces w;
    lake::control::InMemoryStore& store = *w.store;
    const auto principal = store.context_for(w.secret);
    auto requested = w.run(w.service->request(w.customer_id));
    const auto workspace = store.get_service(requested->service_id);
    ASSERT_TRUE(store.set_service_status(workspace->id, "Ready", "", "http://ws:4800"));
    w.deliver(*workspace, {{"kind", "zetta"}, {"token", "t"}});
    const auto crm = w.service->create_service({w.customer_id, "postgres", "crm", ""});
    ASSERT_TRUE(crm);
    const auto cache = w.service->create_service({w.customer_id, "valkey", "cache", ""});
    ASSERT_TRUE(cache);
    w.caller->answers["create_connection"] = {{"name", "crm"}};
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    const auto connect = [&](const std::string& customer_id, const std::string& service_id) {
        lake::api::ApiContext context;
        context.store = &store;
        context.workspaces = w.service.get();
        context.authorized = true;
        context.acting_user_id = principal->user_id;
        context.acting_identity = principal->taap_id;
        context.request_id = lake::control::new_uuid_v4();
        auto completed = liquid::runtime::block_on(
            registry.call_async("connect_service",
                                {{"customer_id", customer_id}, {"service_id", service_id}},
                                context));
        EXPECT_TRUE(completed);
        return std::move(*completed);
    };
    const auto early = connect(w.customer_id, crm->id);
    ASSERT_FALSE(early.ok);
    EXPECT_EQ(early.error.code, "workspace_not_ready");

    ASSERT_TRUE(store.set_service_status(
        crm->id, "Ready", "", "prod-svc-1-rw.lake.svc.cluster.local:5432"));
    w.deliver(*store.get_service(crm->id),
              {{"kind", "postgres"},
               {"host", "prod-svc-1-rw.lake.svc.cluster.local"},
               {"port", 5432},
               {"database", "app"},
               {"user", "app"},
               {"password", "s3cret"}});
    const auto connected = connect(w.customer_id, crm->id);
    ASSERT_TRUE(connected.ok) << connected.error.message;
    EXPECT_EQ(connected.value["service_name"], "crm");
    ASSERT_EQ(w.caller->calls.back().first, "create_connection");
    const json sent = w.caller->calls.back().second;
    EXPECT_EQ(sent["name"], "crm");
    EXPECT_EQ(sent["connector"]["type"], "postgres");
    EXPECT_EQ(sent["connector"]["host"], "prod-svc-1-rw.lake.svc.cluster.local");
    EXPECT_EQ(sent["connector"]["port"], "5432");
    EXPECT_EQ(sent["connector"]["token"], "s3cret") << "the password goes to Zetta, not to a form";
    EXPECT_EQ(connected.value.dump().find("s3cret"), std::string::npos) << "and never back out";

    ASSERT_TRUE(store.set_service_status(cache->id, "Ready", "", "valkey:6379"));
    w.deliver(*store.get_service(cache->id), {{"kind", "valkey"}, {"host", "valkey"}});
    const auto unsupported = connect(w.customer_id, cache->id);
    ASSERT_FALSE(unsupported.ok);
    EXPECT_NE(unsupported.error.message.find("cannot read a valkey"), std::string::npos);

    const auto foreign = connect(w.customer_id, lake::control::new_uuid_v4());
    EXPECT_FALSE(foreign.ok) << "a service that is not the customer's is not found";
}

TEST(Workspaces, DataPageOffersTheSpacesServicesAndServicesWarnBeforeDeletingAUsedOne) {
    lake::api::WorkspacePage page;
    page.section = "data";
    page.customer_id = std::string(customer);
    page.state = {{"requested", true}, {"phase", "Ready"}, {"ready", true}};
    page.data["list_connections"] = {
        {"connections",
         json::array(
             {{{"name", "crm"},
               {"connector",
                {{"type", "postgres"}, {"host", "prod-svc-1-rw"}, {"database", "app"}}}}})}};
    page.data["list_services"] = {{"services",
                                   json::array({{{"id", "a"},
                                                 {"name", "crm"},
                                                 {"kind", "postgres"},
                                                 {"phase", "Ready"},
                                                 {"desired", "present"},
                                                 {"connected", true},
                                                 {"endpoint", "prod-svc-1-rw:5432"}},
                                                {{"id", "b"},
                                                 {"name", "cache"},
                                                 {"kind", "valkey"},
                                                 {"phase", "Ready"},
                                                 {"desired", "present"},
                                                 {"connected", true}},
                                                {{"id", "c"},
                                                 {"name", "workspace"},
                                                 {"kind", "zetta"},
                                                 {"default", true},
                                                 {"phase", "Ready"},
                                                 {"desired", "present"}}})}};
    std::string html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("Connect one of your services"), std::string::npos);
    EXPECT_NE(html.find(">Refresh</button>"), std::string::npos) << "crm is already connected";
    EXPECT_NE(html.find("not supported yet: the workspace cannot read valkey"), std::string::npos);
    EXPECT_EQ(html.find(">workspace</td>"), std::string::npos)
        << "the workspace itself is not offered";
    EXPECT_NE(html.find("action=\"/train/data/connect-service\""), std::string::npos);
    EXPECT_NE(html.find("<td><a href=\"/services?customer_id="), std::string::npos)
        << "the connection shows the service it came from";

    page.section = "services";
    page.data["list_services"]["limit"] = 5;
    html = lake::api::web_ui_workspace(page, true);
    EXPECT_NE(html.find("connection <code>crm</code> uses it"), std::string::npos);
}
