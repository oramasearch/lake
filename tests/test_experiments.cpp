#include <cstdlib>
#include <iostream>

#include <gtest/gtest.h>

#include "audit_test_support.hpp"
#include "experiment_fixture.hpp"
#include "http1/client.hpp"
#include "lake/api/agent_docs.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/server.hpp"
#include "one_thread_io.hpp"

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

TEST_F(Experiments, SpacePolicyConfirmationAndWorkspaceAliasesCannotBypassPromotion) {
    const auto object = store->find_policy_object("customer", customer);
    ASSERT_TRUE(object);
    ASSERT_TRUE(store->create_grant({"reader", object->id, "read", "test", "admin", ""}));
    ASSERT_TRUE(store->create_grant({"writer", object->id, "write", "test", "admin", ""}));
    context.operator_credential = false;
    context.acting_identity = "reader";
    EXPECT_TRUE(call("list_experiments", json::object()).ok);
    EXPECT_FALSE(call("start_experiment", lake::test::experiment_start_input()).ok);
    context.acting_identity = "writer";
    EXPECT_TRUE(call("start_experiment", lake::test::experiment_start_input()).ok);
    EXPECT_FALSE(call("promote_trial",
                      {{"experiment_id", "study-one"}, {"run", "trial-1"}, {"confirm", true}})
                     .ok);
    EXPECT_FALSE(call("workspace_act", {{"capability", "promote_trial"},
        {"input", {{"study", "study-one"}, {"run", "trial-1"}, {"confirm", true}}}}).ok);
    context.acting_identity = "admin";
    context.operator_credential = true;
    auto unconfirmed = lake::test::experiment_start_input();
    unconfirmed["confirm"] = false;
    auto count = workspace->caller->calls.size();
    EXPECT_FALSE(call("start_experiment", unconfirmed).ok);
    EXPECT_FALSE(call("workspace_act", {{"capability", "create_study"}, {"input", unconfirmed}}).ok);
    EXPECT_EQ(workspace->caller->calls.size(), count);
    EXPECT_TRUE(call("promote_trial",
                     {{"experiment_id", "study-one"}, {"run", "trial-1"}, {"confirm", true}})
                    .ok);
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
    EXPECT_EQ(listing.value.at("experiments").size(), 1);
    EXPECT_TRUE(call("list_experiments", {{"project", "different"}}).value.at("runs").empty());
    EXPECT_TRUE(call("list_experiments", {{"state", "failed"}}).value.at("experiments").empty());
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
    EXPECT_FALSE(call("start_experiment", invalid).ok);
    invalid["objective"]["direction"] = "sideways";
    EXPECT_FALSE(call("start_experiment", invalid).ok);
    for (const auto& [name, input] : lake::test::experiment_calls(customer, workspace->id)) {
        closed_objects(registry.find(name)->input_schema);
        closed_objects(registry.find(name)->output_schema);
    }
}

// lake#463: from_defaults lets Zetta fill the objective and budgets, and Zetta's error path is
// what the caller sees.
TEST_F(Experiments, StartExperimentLeavesDefaultsToZettaAndKeepsItsErrorPath) {
    const json minimal = {{"name", "Defaults"}, {"methodology", "M-1"}, {"dataset", "accounts"},
        {"from_defaults", true}, {"confirm", true}};
    const auto started = call("start_experiment", minimal);
    ASSERT_TRUE(started.ok) << started.error.message;
    const auto upstream = workspace->caller->calls.back();
    EXPECT_EQ(upstream.at("name"), "create_study");
    EXPECT_FALSE(upstream.at("input").contains("objective"));
    EXPECT_FALSE(upstream.at("input").contains("budgets"));
    EXPECT_TRUE(upstream.at("input").at("from_defaults").get<bool>());

    auto partial = minimal;
    partial["objective"] = {{"metric", "held_out_log_loss"}};
    EXPECT_TRUE(call("start_experiment", partial).ok);
    EXPECT_EQ(workspace->caller->calls.back().at("input").at("objective"),
              (json{{"metric", "held_out_log_loss"}}));

    const auto count = workspace->caller->calls.size();
    auto without = minimal;
    without.erase("from_defaults");
    const auto refused = call("start_experiment", without);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "invalid_input");
    EXPECT_EQ(refused.error.path, "/objective");
    EXPECT_NE(refused.error.message.find("from_defaults"), std::string::npos);
    without["objective"] = lake::test::experiment_start_input().at("objective");
    EXPECT_EQ(call("start_experiment", without).error.path, "/budgets");
    EXPECT_EQ(workspace->caller->calls.size(), count);

    auto rejected = lake::test::experiment_start_input();
    rejected["name"] = "Rejected By Zetta";
    const auto failed = call("start_experiment", rejected);
    EXPECT_FALSE(failed.ok);
    EXPECT_EQ(failed.error.code, "invalid_input");
    EXPECT_EQ(failed.error.path, "/objective");
    EXPECT_NE(failed.error.message.find("from_defaults"), std::string::npos);
}

TEST_F(Experiments, BoundedLiveReadsResolveOnlyTheSelectedWorkspacesExperimentOrRun) {
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

// lake#561: lake says experiment and `experiment_id`; the engine's study names stay on the engine
// side of the call. workspace_act speaks the engine's names in and out.
TEST_F(Experiments, LakeSaysExperimentAndTheEngineKeepsItsStudyNames) {
    for (const std::string old : {"start_study", "stop_study", "get_experiment_study"}) {
        EXPECT_EQ(registry.find(old), nullptr) << old;
    }
    for (const auto& capability : registry.list()) {
        const auto schemas = capability->input_schema.dump() + capability->output_schema.dump();
        if (capability->name.find("experiment") != std::string::npos ||
            capability->name == "promote_trial") {
            EXPECT_EQ(schemas.find("\"study"), std::string::npos) << capability->name;
            EXPECT_EQ(capability->description.find("stud"), std::string::npos) << capability->name;
        }
    }
    const auto read = call("get_experiment", {{"experiment_id", "study-one"}});
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(workspace->caller->calls.back().at("name"), "get_study");
    EXPECT_EQ(workspace->caller->calls.back().at("input"), (json{{"study", "study-one"}}));

    const auto trials = call("list_experiment_trials", {{"experiment_id", "study-one"}});
    ASSERT_TRUE(trials.ok) << trials.error.message;
    EXPECT_EQ(trials.value.at("experiment_id"), "study-one");
    EXPECT_FALSE(trials.value.contains("study"));

    const auto missing = call("get_experiment", {{"experiment_id", "other"}});
    EXPECT_FALSE(missing.ok);
    EXPECT_EQ(missing.error.code, "not_found");
    EXPECT_EQ(missing.error.message, "no experiment named 'other'");
    EXPECT_EQ(missing.error.path, "/experiment_id");

    const auto stopped =
        call("stop_experiment_trial",
             {{"experiment_id", "study-one"}, {"run_id", "trial-2"}, {"confirm", true}});
    ASSERT_TRUE(stopped.ok) << stopped.error.message;
    EXPECT_EQ(stopped.value.at("experiment_id"), "study-one");
    EXPECT_EQ(workspace->caller->calls.back().at("input"),
              (json{{"study", "study-one"}, {"run", "trial-2"}}));

    const auto acted =
        call("workspace_act",
             {{"capability", "stop_trial"},
              {"input", {{"study", "study-one"}, {"run", "trial-2"}, {"confirm", true}}}});
    ASSERT_TRUE(acted.ok) << acted.error.message;
    EXPECT_EQ(acted.value.at("study"), "study-one");
    EXPECT_FALSE(acted.value.contains("experiment_id"));

    const auto live = call("tail_experiment_events",
                           {{"kind", "experiment"}, {"id", "study-one"}, {"wait_ms", 0}});
    ASSERT_TRUE(live.ok) << live.error.message;
    EXPECT_EQ(workspace->caller->calls.back().at("input").at("topic"), "study-events.study-one");
    EXPECT_FALSE(call("tail_experiment_events", {{"kind", "study"}, {"id", "study-one"}}).ok);

    const auto stop = call("stop_experiment", {{"experiment_id", "study-one"}, {"confirm", true}});
    ASSERT_TRUE(stop.ok) << stop.error.message;
    EXPECT_EQ(stop.value.at("experiment_id"), "study-one");
    EXPECT_EQ(workspace->caller->calls.back().at("name"), "stop_study");
}

// The web app's Experiments pages call only these capabilities (docs/ui/experiments.md); the
// agents' guide names the live read and the confirmation a person gives.
TEST_F(Experiments, AgentDocsNameTheLiveReadAndTheConfirmation) {
    lake::api::ApiServer server({.port = 0, .store = store,
        .workspaces = workspace->service, .admin_token = "experiment-operator"});
    ASSERT_TRUE(server.start()) << server.reason();
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
