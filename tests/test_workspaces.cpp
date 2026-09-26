#include <filesystem>
#include <fstream>
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
        co_return ZettaControlResult<json>(value);
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
