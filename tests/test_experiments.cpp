#include <cstdlib>
#include <iostream>
#include <regex>

#include <gtest/gtest.h>

#include "audit_test_support.hpp"
#include "experiment_fixture.hpp"
#include "http1/client.hpp"
#include "one_thread_io.hpp"
#include "lake/api/agent_docs.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/server.hpp"
#include "lake/api/web_ui.hpp"

namespace {
using nlohmann::json;

void closed_objects(const json& schema) {
    if (schema.is_array()) {
        for (const auto& item : schema) {
            closed_objects(item);
        }
    } else if (schema.is_object()) {
        const auto type = schema.value("type", json());
        if (type == "object" || (type.is_array() && std::ranges::find(type, "object") != type.end())) {
            EXPECT_EQ(schema.value("additionalProperties", json()), false) << schema.dump();
        }
        for (const auto& [key, child] : schema.items()) {
            if (key != "enum" && key != "const") {
                closed_objects(child);
            }
        }
    }
}

void controls_registered(const std::string& html, const lake::api::Registry& registry) {
    const auto begin = html.find("<section id=\"experiments\"");
    const auto end = html.find("<script type=\"application/json\" id=\"experiment-data\"", begin);
    ASSERT_NE(begin, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    const auto content = html.substr(begin, end - begin);
    const std::regex controls("</?(?:form|a|input|button|select|textarea|summary)\\b[^>]*>");
    const std::regex mapping("data-capability=\"([^\"]+)\"");
    std::string form;
    for (auto match = std::sregex_iterator(content.begin(), content.end(), controls);
         match != std::sregex_iterator(); ++match) {
        const auto tag = match->str();
        if (tag == "</form>") {
            form.clear();
        }
        if (tag.starts_with("</")) {
            continue;
        }
        std::smatch declaration;
        const auto capability = std::regex_search(tag, declaration, mapping) ? declaration[1].str() : form;
        ASSERT_FALSE(capability.empty()) << "Unmapped experiment control: " << tag;
        EXPECT_NE(registry.find(capability), nullptr) << tag;
        if (tag.starts_with("<form ")) {
            form = capability;
        }
    }
}

class Experiments : public testing::Test {
protected:
    std::shared_ptr<lake::control::InMemoryStore> store = std::make_shared<lake::control::InMemoryStore>();
    lake::api::Registry registry;
    std::unique_ptr<lake::test::ExperimentWorkspace> workspace;
    lake::api::ApiContext context;
    std::string customer;

    void SetUp() override {
        const auto user = store->create_user({"taap:experiment-owner", "", "Experiment Owner"});
        ASSERT_TRUE(user);
        const auto created = store->create_customer({"individual", user->id, "", "experiments"});
        ASSERT_TRUE(created);
        customer = created->id;
        workspace = std::make_unique<lake::test::ExperimentWorkspace>(store, customer);
        lake::api::register_core_capabilities(registry, "test");
        context.store = store.get();
        context.workspaces = workspace->service.get();
        context.authorized = true;
        context.operator_credential = true;
        context.acting_identity = "admin";
    }

    lake::api::ApiResult call(const std::string& name, json input) {
        input["customer_id"] = customer;
        input["workspace_id"] = workspace->id;
        context.request_id = lake::control::new_uuid_v4();
        const auto result = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(result);
        return result ? *result : lake::api::ApiResult::failure("store_error", result.error().message);
    }

    std::string request(unsigned short port, const std::string& method, const std::string& path,
                        const json& body = nullptr) {
        const auto data = body.is_null() ? std::string() : body.dump();
        const auto* bytes = reinterpret_cast<const std::byte*>(data.data());
        const auto response = lake::test::http_request({
            .url = "http://127.0.0.1:" + std::to_string(port), .method = method, .suffix = path,
            .headers = {{"Authorization", "Bearer experiment-operator"}, {"Content-Type", "application/json"}},
            .body = std::vector<std::byte>(bytes, bytes + data.size())});
        EXPECT_TRUE(response);
        if (!response) {
            return "{}";
        }
        EXPECT_EQ(response->status, 200U);
        return std::string(reinterpret_cast<const char*>(response->body.data()), response->body.size());
    }
};

TEST(ExperimentsRenderer, RunBoxShowsAnEditableConfirmedAgentProposal) {
    lake::api::ExperimentsPage page;
    page.customer_id = "customer-one";
    page.customers = {{"customer-one", "Personal", true}};
    page.data = {{"studies", json::array()}, {"runs", json::array()}};
    page.experiments_agent = true;
    page.agent_task = {
        {"task_id", "task-one"},
        {"status", "succeeded"},
        {"answer",
         {{"reply", "Review the bounded study."},
          {"result",
           {{"kind", "proposal"},
            {"confirm", true},
            {"study", lake::test::experiment_study().at("spec")},
            {"explanations", {{"objective", "Use validation AUC."}}}}}}}};
    page.proposal_sha256 = std::string(64, 'a');
    const std::string html = lake::api::web_ui_experiments(page, true);
    EXPECT_NE(html.find("<h2>Run an Experiment</h2>"), std::string::npos);
    EXPECT_NE(html.find("Ask ExperimentsAgent"), std::string::npos);
    EXPECT_NE(html.find("Study proposal (JSON)"), std::string::npos);
    EXPECT_NE(html.find("held_out_auc"), std::string::npos);
    EXPECT_NE(html.find("Confirm and start study"), std::string::npos);
    EXPECT_NE(html.find(std::string(64, 'a')), std::string::npos);
    EXPECT_EQ(html.find("ExperimentsAgent is not registered"), std::string::npos);
}

TEST(ExperimentsRenderer, AQuestionHasAReplyBoxForTheSameTurn) {
    lake::api::ExperimentsPage page;
    page.customer_id = "customer-one";
    page.customers = {{"customer-one", "Personal", true}};
    page.data = {{"studies", json::array()}, {"runs", json::array()}};
    page.experiments_agent = true;
    page.agent_reply = true;
    page.agent_task = {
        {"task_id", "task-question"},
        {"status", "succeeded"},
        {"answer",
         {{"reply", "Which project should I use?"},
          {"result", {{"kind", "question"}, {"confirm", false}}}}}};
    const std::string html = lake::api::web_ui_experiments(page, true);
    EXPECT_NE(html.find("Which project should I use?"), std::string::npos);
    EXPECT_NE(html.find("<h3>Reply</h3>"), std::string::npos);
    EXPECT_NE(html.find("name=\"task_id\" value=\"task-question\""), std::string::npos);
    EXPECT_NE(html.find("name=\"reply\""), std::string::npos);
    EXPECT_EQ(html.find("name=\"goal\" value="), std::string::npos);
}

TEST_F(Experiments, SpacePolicyConfirmationAndWorkspaceAliasesCannotBypassPromotion) {
    const auto object = store->find_policy_object("customer", customer);
    ASSERT_TRUE(object);
    ASSERT_TRUE(store->create_grant({"reader", object->id, "read", "test", "admin", ""}));
    ASSERT_TRUE(store->create_grant({"writer", object->id, "write", "test", "admin", ""}));
    context.operator_credential = false;
    context.acting_identity = "reader";
    EXPECT_TRUE(call("list_experiments", json::object()).ok);
    EXPECT_FALSE(call("start_study", lake::test::experiment_start_input()).ok);
    context.acting_identity = "writer";
    EXPECT_TRUE(call("start_study", lake::test::experiment_start_input()).ok);
    EXPECT_FALSE(call("promote_trial", {{"study", "study-one"}, {"run", "trial-1"}, {"confirm", true}}).ok);
    EXPECT_FALSE(call("workspace_act", {{"capability", "promote_trial"},
        {"input", {{"study", "study-one"}, {"run", "trial-1"}, {"confirm", true}}}}).ok);
    context.acting_identity = "admin";
    context.operator_credential = true;
    auto unconfirmed = lake::test::experiment_start_input();
    unconfirmed["confirm"] = false;
    auto count = workspace->caller->calls.size();
    EXPECT_FALSE(call("start_study", unconfirmed).ok);
    EXPECT_FALSE(call("workspace_act", {{"capability", "create_study"}, {"input", unconfirmed}}).ok);
    EXPECT_EQ(workspace->caller->calls.size(), count);
    EXPECT_TRUE(call("promote_trial", {{"study", "study-one"}, {"run", "trial-1"}, {"confirm", true}}).ok);
    const auto events = lake::test::audit_events(*store, {.request_id = context.request_id});
    ASSERT_TRUE(events);
    ASSERT_EQ(events->size(), 1);
    EXPECT_EQ(events->at(0).draft.customer_id, customer);
    EXPECT_EQ(events->at(0).draft.action, "promote_trial");
    EXPECT_EQ(workspace->caller->last_token, "experiment-service-secret");
    EXPECT_EQ(workspace->caller->last_url, "http://experiments.test");
    context.session_credential = true;
    context.scopes = {"workspace_act"};
    EXPECT_FALSE(call("workspace_act", {{"capability", "promote_trial"},
        {"input", {{"study", "study-one"}, {"run", "trial-1"}, {"confirm", true}}}}).ok);
    context.scopes.clear();
    context.session_credential = false;
    context.acting_identity = "outsider";
    context.operator_credential = false;
    count = workspace->caller->calls.size();
    for (auto [name, input] : lake::test::experiment_calls(customer, workspace->id)) {
        input["workspace_id"] = workspace->id;
        EXPECT_FALSE(call(name, input).ok) << name;
    }
    EXPECT_EQ(workspace->caller->calls.size(), count);
}

TEST_F(Experiments, WorkspaceToolSchemasDescribeTheRequiredConfirmation) {
    json paths = json::object();
    for (const std::string name : {"create_study", "stop_study", "stop_trial", "promote_trial"}) {
        auto& schema = paths["/v1/" + name]["post"]["requestBody"]["content"]["application/json"]["schema"];
        schema = {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}};
    }
    const auto document = lake::api::workspace_openapi({{"paths", paths}}, "/workspaces/space/zetta");
    for (const auto& [path, methods] : document.at("paths").items()) {
        const auto schema = methods.at("post").at("requestBody").at("content")
            .at("application/json").at("schema");
        EXPECT_EQ(schema.at("properties").at("confirm").at("const"), true) << path;
        EXPECT_EQ(schema.at("required"), json::array({"confirm"})) << path;
    }
}

TEST_F(Experiments, ListsFilterTrackedRunsAndMetricsRespectPrivacyAndBounds) {
    const auto listing = call("list_experiments", json::object());
    ASSERT_TRUE(listing.ok) << listing.error.message;
    EXPECT_EQ(listing.value.at("runs").size(), 3);
    EXPECT_EQ(listing.value.at("studies").size(), 1);
    EXPECT_TRUE(call("list_experiments", {{"project", "different"}}).value.at("runs").empty());
    EXPECT_TRUE(call("list_experiments", {{"state", "failed"}}).value.at("studies").empty());
    const auto dp = call("get_run_metrics", {{"run", "dp-run"}});
    ASSERT_TRUE(dp.ok);
    EXPECT_EQ(dp.value.at("metrics_suppressed"), "differential_privacy");
    EXPECT_TRUE(dp.value.at("series").empty());
    const auto comparison = call("compare_runs", {{"runs", {"run-a", "dp-run"}}});
    ASSERT_TRUE(comparison.ok) << comparison.error.message;
    EXPECT_EQ(comparison.value.at("details").size(), 2);
    EXPECT_TRUE(comparison.value.at("details").back().at("series").empty());
    EXPECT_FALSE(call("compare_runs", {{"runs", {"run-a", "run-a"}}}).ok);
    EXPECT_FALSE(call("compare_runs", {{"runs", {"run-a"}}}).ok);
    EXPECT_FALSE(call("get_run_metrics", {{"run", "run-a"}, {"max_points", 3}}).ok);
    EXPECT_FALSE(call("get_run_metrics", {{"run", "run-a"}, {"max_points", nullptr}}).ok);
    EXPECT_FALSE(call("get_run_metrics", {{"run", "run-a"}, {"names", {"bad metric"}}}).ok);
    auto invalid = lake::test::experiment_start_input();
    invalid["objective"]["validation_split"] = "test";
    EXPECT_FALSE(call("start_study", invalid).ok);
    invalid["objective"]["direction"] = "sideways";
    EXPECT_FALSE(call("start_study", invalid).ok);
    for (const auto& [name, input] : lake::test::experiment_calls(customer, workspace->id)) {
        closed_objects(registry.find(name)->input_schema);
        closed_objects(registry.find(name)->output_schema);
    }
}

// lake#463: from_defaults lets Zetta fill the objective and budgets, and Zetta's error path is
// what the caller sees.
TEST_F(Experiments, StartStudyLeavesDefaultsToZettaAndKeepsItsErrorPath) {
    const json minimal = {{"name", "Defaults"}, {"methodology", "M-1"}, {"dataset", "accounts"},
        {"from_defaults", true}, {"confirm", true}};
    const auto started = call("start_study", minimal);
    ASSERT_TRUE(started.ok) << started.error.message;
    const auto upstream = workspace->caller->calls.back();
    EXPECT_EQ(upstream.at("name"), "create_study");
    EXPECT_FALSE(upstream.at("input").contains("objective"));
    EXPECT_FALSE(upstream.at("input").contains("budgets"));
    EXPECT_TRUE(upstream.at("input").at("from_defaults").get<bool>());

    auto partial = minimal;
    partial["objective"] = {{"metric", "held_out_log_loss"}};
    EXPECT_TRUE(call("start_study", partial).ok);
    EXPECT_EQ(workspace->caller->calls.back().at("input").at("objective"),
              (json{{"metric", "held_out_log_loss"}}));

    const auto count = workspace->caller->calls.size();
    auto without = minimal;
    without.erase("from_defaults");
    const auto refused = call("start_study", without);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "invalid_input");
    EXPECT_EQ(refused.error.path, "/objective");
    EXPECT_NE(refused.error.message.find("from_defaults"), std::string::npos);
    without["objective"] = lake::test::experiment_start_input().at("objective");
    EXPECT_EQ(call("start_study", without).error.path, "/budgets");
    EXPECT_EQ(workspace->caller->calls.size(), count);

    auto rejected = lake::test::experiment_start_input();
    rejected["name"] = "Rejected By Zetta";
    const auto failed = call("start_study", rejected);
    EXPECT_FALSE(failed.ok);
    EXPECT_EQ(failed.error.code, "invalid_input");
    EXPECT_EQ(failed.error.path, "/objective");
    EXPECT_NE(failed.error.message.find("from_defaults"), std::string::npos);
}

TEST(ExperimentsRenderer, ObjectivesUseTheRunReportsMetricNames) {
    lake::api::ExperimentsPage page;
    page.customer_id = "customer-one";
    page.customers = {{"customer-one", "Personal", true}};
    page.can_write = true;
    auto study = lake::test::experiment_study();
    study["spec"]["objective"]["metric"] = "validation.auc";
    page.data = {{"studies", json::array({study})}, {"runs", json::array()}};
    const std::string html = lake::api::web_ui_experiments(page, true);
    EXPECT_NE(html.find("name=\"objective.metric\" value=\"held_out_auc\""), std::string::npos);
    EXPECT_NE(html.find("<option value=\"held_out_log_loss\">"), std::string::npos);
    EXPECT_NE(html.find("<td>held_out_auc</td>"), std::string::npos);
    EXPECT_EQ(html.find(">validation.auc<"), std::string::npos);
    EXPECT_NE(html.find("\"roc_auc\":\"held_out_auc\""), std::string::npos);
}

// lake#449: analysis panels are tables, promotion is marked, and the latest trials' curves are in
// the page before any script runs.
TEST(ExperimentsRenderer, StudyPanelsAreReadableAndCarryTrialCurves) {
    lake::api::ExperimentsPage page;
    page.customer_id = "customer-one";
    page.customers = {{"customer-one", "Personal", true}};
    page.mode = "study";
    page.study = "study-one";
    page.data = lake::test::experiment_study();
    page.data["promoted_model"] = "model-one";
    lake::test::ExperimentCaller caller;
    page.analysis = caller.analysis();
    page.trial_metrics = json::array({lake::test::experiment_metrics("trial-1")});
    const std::string html = lake::api::web_ui_experiments(page, true);
    const auto history = html.find("id=\"optimization-table\"");
    ASSERT_NE(history, std::string::npos);
    const auto panels = html.substr(history, html.find("Trial Leaderboard") - history);
    EXPECT_EQ(panels.find("<pre>"), std::string::npos);
    EXPECT_NE(panels.find("<td>trial-1</td>"), std::string::npos);
    EXPECT_NE(panels.find("<th scope=\"col\">learning_rate</th>"), std::string::npos);
    EXPECT_NE(panels.find("<td>learning_rate</td><td>1.0</td>"), std::string::npos);
    EXPECT_NE(html.find("status-success\">promoted</span>"), std::string::npos);
    EXPECT_NE(html.find("trial-1 · validation.auc — Data Table"), std::string::npos);
    const auto data = html.find("id=\"experiment-data\"");
    ASSERT_NE(data, std::string::npos);
    EXPECT_NE(html.find("\"details\":[{", data), std::string::npos);
}

TEST_F(Experiments, StudyPageReadsTheLatestTrialCurves) {
    lake::api::ApiServer server({.port = 0, .store = store,
        .workspaces = workspace->service, .admin_token = "experiment-operator"});
    ASSERT_TRUE(server.start()) << server.reason();
    const auto html = request(server.port(), "GET", "/w/" + workspace->id +
        "/train/experiment?study=study-one&customer_id=" + customer);
    EXPECT_NE(html.find("trial-1 · validation.auc — Data Table"), std::string::npos);
    EXPECT_NE(html.find("trial-2 · train.loss — Data Table"), std::string::npos);
}

TEST_F(Experiments, BoundedLiveReadsResolveOnlyTheSelectedSpacesStudyOrRun) {
    const json input = {{"kind", "run"}, {"id", "run-a"}, {"from_sequence", 42},
        {"wait_ms", 30000}, {"max_events", 2}};
    const auto result = call("tail_experiment_events", input);
    ASSERT_TRUE(result.ok) << result.error.message;
    const auto upstream = workspace->caller->calls.back();
    EXPECT_EQ(upstream.at("name"), "receive_events");
    EXPECT_TRUE(upstream.at("read").get<bool>());
    EXPECT_EQ(upstream.at("input"), (json{{"topic", "run-metrics.run-a"},
        {"from_sequence", 42}, {"wait_ms", 30000}, {"max", 2}}));
    auto invalid = input;
    invalid["wait_ms"] = 30001;
    EXPECT_FALSE(call("tail_experiment_events", invalid).ok);
    invalid = input;
    invalid["max_events"] = 1001;
    EXPECT_FALSE(call("tail_experiment_events", invalid).ok);
    invalid = input;
    invalid["id"] = "foreign";
    EXPECT_FALSE(call("tail_experiment_events", invalid).ok);
    EXPECT_NE(workspace->caller->calls.back().at("name"), "receive_events");
    invalid["id"] = "*";
    EXPECT_FALSE(call("tail_experiment_events", invalid).ok);
    invalid["id"] = "dp-run";
    EXPECT_TRUE(call("tail_experiment_events", invalid).value.at("events").empty());
    EXPECT_NE(workspace->caller->calls.back().at("name"), "receive_events");
}

TEST_F(Experiments, RestAndMcpExposeMatchingSchemasAndCallEveryOperation) {
    lake::api::ApiServer server({.port = 0, .store = store,
        .workspaces = workspace->service, .admin_token = "experiment-operator"});
    ASSERT_TRUE(server.start()) << server.reason();
    const auto openapi = json::parse(request(server.port(), "GET", "/v1/openapi.json"));
    const auto tools = json::parse(request(server.port(), "POST", "/mcp", {
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}})).at("result").at("tools");
    for (const auto& [name, input] : lake::test::experiment_calls(customer, workspace->id)) {
        const auto tool = std::ranges::find_if(tools, [&](const auto& item) {
            return item.at("name") == name;
        });
        ASSERT_NE(tool, tools.end()) << name;
        const auto operation = openapi.at("paths").at("/v1/" + name).at("post");
        EXPECT_EQ(tool->at("inputSchema"), operation.at("requestBody").at("content")
            .at("application/json").at("schema"));
        EXPECT_EQ(tool->at("outputSchema"), operation.at("responses").at("200").at("content")
            .at("application/json").at("schema"));
        EXPECT_EQ(tool->at("annotations").at("readOnlyHint"),
            registry.find(name)->kind == lake::api::CapabilityKind::Read);
        const auto rest = json::parse(request(server.port(), "POST", "/v1/" + name, input));
        EXPECT_TRUE(rest.at("ok").get<bool>()) << name << rest;
        const auto mcp = json::parse(request(server.port(), "POST", "/mcp", {
            {"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
            {"params", {{"name", name}, {"arguments", input}}}}))
            .at("result").at("structuredContent");
        EXPECT_TRUE(mcp.at("ok").get<bool>()) << name << mcp;
    }
}

TEST_F(Experiments, EveryPageControlUsesACapabilityAndNavigationOwnsAllExperimentRoutes) {
    lake::api::ApiServer server({.port = 0, .store = store,
        .workspaces = workspace->service, .admin_token = "experiment-operator"});
    ASSERT_TRUE(server.start()) << server.reason();
    const std::vector<std::string> paths = {"/train/experiments?", "/train/experiment?study=study-one&",
        "/train/experiment/run?run=run-a&", "/train/experiments/compare?runs=run-a,run-b&"};
    for (const auto& path : paths) {
        const auto html = request(server.port(), "GET", "/w/" + workspace->id + path + "customer_id=" + customer);
        controls_registered(html, registry);
        const auto navigation = html.find("href=\"/w/" + workspace->id + "/train/experiments\" aria-current=\"page\"");
        ASSERT_NE(navigation, std::string::npos);
        const auto link = html.substr(navigation, html.find("</a>", navigation) - navigation);
        EXPECT_NE(link.find("<svg class=\"icon\""), std::string::npos);
        EXPECT_EQ(html.find("href=\"/train/runs\" aria-current=\"page\""), std::string::npos);
        EXPECT_EQ(html.find("experiment-service-secret"), std::string::npos);
        EXPECT_EQ(html.find("data-action=\"delete"), std::string::npos);
        EXPECT_EQ(html.find(">Delete"), std::string::npos);
        EXPECT_EQ(html.find("<script>alert(1)</script>"), std::string::npos);
        for (const auto name : lake::api::web_ui_experiment_asset_names()) {
            const std::string asset(lake::api::web_ui_experiment_asset_url(name));
            EXPECT_NE(html.find(asset), std::string::npos);
            EXPECT_EQ(lake::api::web_ui_asset_cache_control(asset), "public, max-age=31536000, immutable");
            EXPECT_EQ(request(server.port(), "GET", asset), lake::api::web_ui_experiment_asset(name));
        }
    }
    const auto dp = request(server.port(), "GET", "/w/" + workspace->id + "/train/experiment/run?run=dp-run&customer_id=" + customer);
    EXPECT_NE(dp.find("curves suppressed for differential privacy"), std::string::npos);
    const auto agent_docs = lake::api::agent_docs::llms_full_txt(registry, "http://127.0.0.1");
    EXPECT_NE(agent_docs.find("tail_experiment_events"), std::string::npos);
    EXPECT_NE(agent_docs.find("person's confirmation"), std::string::npos);
    if (std::getenv("LAKE_EXPERIMENT_BROWSER")) {
        workspace->caller->live = true;
        std::cout << "Experiment browser fixture: http://127.0.0.1:" << server.port()
                  << "/train/experiments?customer_id=" << customer
                  << "\nOperator token: experiment-operator\nPress Enter to finish.\n" << std::flush;
        std::string line;
        std::getline(std::cin, line);
    }
}
} // namespace
