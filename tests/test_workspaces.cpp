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

class FakeControl final : public lake::control::ZettaControl {
public:
    std::vector<json> rows;
    std::vector<std::string> statements;

    liquid::runtime::task<ZettaControlResult<lake::control::ZettaRows>>
    sql(std::string statement, std::string database) override {
        statements.push_back(statement);
        if (database != lake::control::operator_control_database) {
            co_return ZettaControlResult<lake::control::ZettaRows>(std::unexpected(
                lake::control::ZettaControlError{"invalid_input", "wrong database"}));
        }
        co_return ZettaControlResult<lake::control::ZettaRows>(rows);
    }
    liquid::runtime::task<ZettaControlResult<void>> append(std::string table, json row) override {
        EXPECT_EQ(table, lake::control::workspaces_table);
        rows.push_back(row);
        co_return ZettaControlResult<void>{};
    }
};

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

struct Workspaces {
    std::filesystem::path tokens =
        lake::test::fixture_path("workspace-tokens-" + lake::control::new_uuid_v4());
    std::shared_ptr<FakeControl> control = std::make_shared<FakeControl>();
    std::shared_ptr<FakeCaller> caller = std::make_shared<FakeCaller>();
    std::shared_ptr<lake::control::WorkspaceService> service;

    Workspaces() {
        std::filesystem::create_directories(tokens);
        service = std::make_shared<lake::control::WorkspaceService>(
            lake::control::WorkspaceConfig{
                "prod",
                "http://control.test:4800",
                "control-token",
                tokens.string(),
                "http://prod-{workspace}-zetta-client.lake.svc.cluster.local:4800"},
            control,
            caller);
    }
    void status(std::string phase, std::string at) {
        control->rows.push_back({{"workspace_id", "ws-0f1e2d3c4b5a"},
                                 {"record_type", "status"},
                                 {"phase", std::move(phase)},
                                 {"message", "note"},
                                 {"updated_at", std::move(at)}});
    }
    void deliver(std::string_view slug) {
        std::ofstream(tokens / std::string(slug)) << "workspace-admin-token\n";
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

TEST(Workspaces, RequestAppendsOnceAndStateFollowsTheLatestStatus) {
    Workspaces w;
    auto state = w.run(w.service->state(std::string(customer)));
    ASSERT_TRUE(state);
    EXPECT_FALSE(state->requested);
    EXPECT_FALSE(state->ready());
    EXPECT_NE(w.control->statements.back().find("WHERE lake = 'prod'"), std::string::npos);
    EXPECT_EQ(w.control->statements.back().find(" AND "), std::string::npos)
        << "Zetta's SQL takes one predicate per WHERE clause";

    auto requested = w.run(w.service->request(std::string(customer)));
    ASSERT_TRUE(requested);
    EXPECT_TRUE(requested->requested);
    ASSERT_EQ(w.control->rows.size(), 1U);
    EXPECT_EQ(w.control->rows[0]["record_type"], "desired");
    EXPECT_EQ(w.control->rows[0]["workspace_id"], "ws-0f1e2d3c4b5a");
    EXPECT_EQ(w.control->rows[0]["customer_id"], customer);
    EXPECT_EQ(w.control->rows[0]["lake"], "prod");
    ASSERT_TRUE(w.run(w.service->request(std::string(customer))));
    EXPECT_EQ(w.control->rows.size(), 1U) << "a second request appends nothing";

    w.status("Provisioning", "2026-09-25T10:00:00Z");
    w.status("Ready", "2026-09-25T10:02:00Z");
    state = w.run(w.service->state(std::string(customer)));
    ASSERT_TRUE(state);
    EXPECT_EQ(state->phase, "Ready");
    EXPECT_FALSE(state->ready()) << "no credential delivered yet";
    w.deliver("ws-0f1e2d3c4b5a");
    state = w.run(w.service->state(std::string(customer)));
    ASSERT_TRUE(state);
    EXPECT_TRUE(state->ready());
    EXPECT_EQ(state->to_json()["ready"], true);
}

TEST(Workspaces, WithdrawalEndsTheWorkspaceAndARestartIgnoresItsOldStatus) {
    Workspaces w;
    auto refused = w.run(w.service->withdraw(std::string(customer)));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "not_found") << "nothing to delete before a request";

    w.control->rows.push_back({{"workspace_id", "ws-0f1e2d3c4b5a"},
                               {"record_type", "desired"},
                               {"updated_at", "2026-09-25T10:00:00Z"}});
    w.status("Ready", "2026-09-25T10:02:00Z");
    w.deliver("ws-0f1e2d3c4b5a");
    auto withdrawn = w.run(w.service->withdraw(std::string(customer)));
    ASSERT_TRUE(withdrawn);
    EXPECT_FALSE(withdrawn->requested);
    EXPECT_EQ(w.control->rows.back()["record_type"], "withdrawn");
    EXPECT_EQ(w.control->rows.back()["customer_id"], customer);
    EXPECT_EQ(w.control->rows.back().size(), 8U) << "every workspaces column";
    w.control->rows.back()["updated_at"] = "2026-09-25T11:00:00Z";

    auto state = w.run(w.service->state(std::string(customer)));
    ASSERT_TRUE(state);
    EXPECT_FALSE(state->requested);
    EXPECT_EQ(state->phase, "") << "the Ready status predates the withdrawal";
    EXPECT_FALSE(state->ready()) << "a leftover token does not make it callable";
    w.status("Deleted", "2026-09-25T11:01:00Z");
    state = w.run(w.service->state(std::string(customer)));
    ASSERT_TRUE(state);
    EXPECT_EQ(state->phase, "Deleted");

    // Starting again: only statuses after the new request count.
    w.control->rows.push_back({{"workspace_id", "ws-0f1e2d3c4b5a"},
                               {"record_type", "desired"},
                               {"updated_at", "2026-09-25T12:00:00Z"}});
    state = w.run(w.service->state(std::string(customer)));
    ASSERT_TRUE(state);
    EXPECT_TRUE(state->requested);
    EXPECT_EQ(state->phase, "");
    EXPECT_FALSE(state->ready());
    w.status("Ready", "2026-09-25T12:03:00Z");
    state = w.run(w.service->state(std::string(customer)));
    ASSERT_TRUE(state);
    EXPECT_TRUE(state->ready());

    // A withdrawal in the same second as the request wins, as the operator decides.
    w.control->rows.push_back({{"workspace_id", "ws-0f1e2d3c4b5a"},
                               {"record_type", "withdrawn"},
                               {"updated_at", "2026-09-25T12:00:00Z"}});
    state = w.run(w.service->state(std::string(customer)));
    ASSERT_TRUE(state);
    EXPECT_FALSE(state->requested);
}

TEST(Workspaces, CallsReachOnlyTheCustomersOwnCluster) {
    Workspaces w;
    auto refused = w.run(w.service->call(std::string(customer), "list_projects", true, {}));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "workspace_not_ready");

    w.control->rows.push_back({{"workspace_id", "ws-0f1e2d3c4b5a"}, {"record_type", "desired"}});
    // Another workspace's rows on the same Lake are ignored.
    w.control->rows.push_back({{"workspace_id", "ws-aaaaaaaaaaaa"},
                               {"record_type", "status"},
                               {"phase", "Failed"},
                               {"updated_at", "2026-09-26T00:00:00Z"}});
    w.status("Ready", "2026-09-25T10:02:00Z");
    w.deliver("ws-0f1e2d3c4b5a");
    w.caller->value = {{"projects", json::array({"retention"})}};
    auto called =
        w.run(w.service->call(std::string(customer), "get_project", true, {{"name", "retention"}}));
    ASSERT_TRUE(called);
    EXPECT_EQ((*called)["projects"][0], "retention");
    EXPECT_EQ(w.caller->url,
              "http://prod-ws-0f1e2d3c4b5a-zetta-client.lake.svc.cluster.local:4800");
    EXPECT_EQ(w.caller->token, "workspace-admin-token");
    EXPECT_EQ(w.caller->capability, "get_project");
    EXPECT_TRUE(w.caller->read);
    EXPECT_EQ(w.caller->input["name"], "retention");
}

TEST(Workspaces, CapabilitiesAreAllowListedAndAuthorizedOnTheCustomer) {
    Workspaces w;
    lake::control::InMemoryStore store;
    const auto minted = [](std::string subject) {
        const auto identity = lake::audit::mint_custodial_identity("workspace-test-kek");
        return lake::control::ClerkIdentity{subject,
                                            subject + "@example.test",
                                            "",
                                            "browser",
                                            identity->taap_id,
                                            identity->public_key,
                                            identity->wrapped_seed};
    };
    const auto owner = store.sign_in_clerk(minted("owner"));
    const auto other = store.sign_in_clerk(minted("other"));
    ASSERT_TRUE(owner);
    ASSERT_TRUE(other);
    const auto owner_principal = store.context_for(owner->secret);
    const auto other_principal = store.context_for(other->secret);
    ASSERT_TRUE(owner_principal);
    ASSERT_TRUE(other_principal);
    const auto customers = store.list_customers(10, owner_principal->user_id);
    ASSERT_TRUE(customers);
    ASSERT_EQ(customers->size(), 1U);
    const std::string owner_customer = customers->front().id;

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
    for (const char* action :
         {"create_connection", "delete_connection", "create_dataset_from_query"}) {
        const auto allowed = call(*owner_principal,
                                  "workspace_act",
                                  {{"customer_id", owner_customer}, {"capability", action}});
        EXPECT_EQ(allowed.error.code, "workspace_not_ready") << action << " is an allowed action";
    }

    const std::string slug = lake::control::workspace_slug(owner_customer);
    const auto foreign_delete = call(
        *other_principal, "delete_workspace", {{"customer_id", owner_customer}, {"confirm", slug}});
    EXPECT_FALSE(foreign_delete.ok) << "another customer cannot delete the workspace";
    const auto unconfirmed = call(*owner_principal,
                                  "delete_workspace",
                                  {{"customer_id", owner_customer}, {"confirm", "yes"}});
    ASSERT_FALSE(unconfirmed.ok);
    EXPECT_EQ(unconfirmed.error.code, "invalid_input");
    const auto rows_before = w.control->rows.size();
    const auto deleted = call(
        *owner_principal, "delete_workspace", {{"customer_id", owner_customer}, {"confirm", slug}});
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(deleted.value["requested"], false);
    ASSERT_EQ(w.control->rows.size(), rows_before + 1);
    EXPECT_EQ(w.control->rows.back()["record_type"], "withdrawn");
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
                                         "href=\"/deployments\""}) {
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
