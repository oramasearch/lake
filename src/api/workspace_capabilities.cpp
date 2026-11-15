#include "methodology_serving.hpp"
#include "lake/api/capabilities.hpp"

#include <algorithm>
#include <format>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <expected>
#include <map>
#include <string>
#include <string_view>

#include <optional>
#include <regex>
#include <set>
#include <span>
#include <vector>

#include "agent_task_records_api.hpp"
#include "feature_planning.hpp"
#include "lake/api/agent_stream_triggers.hpp"
#include "lake/api/datasets.hpp"
#include "lake/api/oauth.hpp"
#include "lake/api/pipeline.hpp"
#include "lake/api/request_recorder.hpp"
#include "lake/api/usage_recorder.hpp"
#include "lake/api/workspace_deletion.hpp"
#include "lake/control/agent_bus.hpp"
#include "lake/control/agent_library.hpp"
#include "lake/control/agent_revisions.hpp"
#include "lake/control/agent_runs.hpp"
#include "lake/control/resource_repairs.hpp"
#include "lake/control/ask_messages.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/opportunities.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/project_decisions.hpp"
#include "lake/control/store.hpp"
#include "lake/control/store_buckets.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "quickstart_columns.hpp"
#include "store_mounts.hpp"
#include "workspace_bus.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {

liquid::runtime::task<control::ZettaControlResult<nlohmann::json>>
call_workspace(ApiContext& context,
               std::string customer_id,
               std::string capability,
               bool read,
               nlohmann::json input) {
    if (capability == "save_graph") {
        if (std::string problem = pipeline::check_size(input); !problem.empty()) {
            co_return control::ZettaControlResult<nlohmann::json>(
                std::unexpected(control::ZettaControlError{"invalid_input", std::move(problem)}));
        }
    }
    const auto timing = context.workspace_call_timing;
    const std::size_t round = timing
        ? timing->completed_rounds.load(std::memory_order_relaxed) + 1 : 0;
    std::string named = timing ? capability : std::string();
    auto completed = co_await context.workspaces->call(
        std::move(customer_id), context.workspace_id, std::move(capability), read, std::move(input),
        [timing, round, named = std::move(named)](std::chrono::nanoseconds elapsed) {
            if (timing) {
                timing->count.fetch_add(1, std::memory_order_relaxed);
                timing->nanoseconds.fetch_add(elapsed.count(), std::memory_order_relaxed);
                timing->record(named, elapsed.count());
                auto completed = timing->completed_rounds.load(std::memory_order_relaxed);
                while (completed < round && !timing->completed_rounds.compare_exchange_weak(
                    completed, round, std::memory_order_relaxed)) {
                }
            }
        }, context.cache_workspace_definitions);
    if (!completed) {
        co_return std::unexpected(completed.error());
    }
    co_return std::move(*completed);
}

namespace {

std::string trimmed_text(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// A project's decision around the engine's project calls (lake#577,
// docs/api/workspaces.md#a-projects-decision): `create_project` writes the decision it was sent
// under the new project's id, `get_project` answers it as `decision`, and `delete_project` removes
// it. Answers a refusal only when a decision that was sent could not be kept.
liquid::runtime::task<std::optional<ApiResult>>
project_decision_step(ApiContext& context,
                      const nlohmann::json& input,
                      const std::string& capability,
                      const std::string& project_name,
                      const std::string& decision,
                      nlohmann::json& value) {
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.value("workspace_id", context.workspace_id);
    if (capability == "create_project" && decision.empty()) {
        co_return std::nullopt;
    }
    auto records = workspace_records(context, customer_id, workspace_id);
    if (!records) {
        if (capability != "create_project") {
            co_return std::nullopt;
        }
        co_return ApiResult::failure(records.error().code,
                                     "the project was created, but its decision was not kept: " +
                                         records.error().message);
    }
    if (capability == "get_project") {
        const std::string id = value.value("id", std::string());
        if (id.empty()) {
            co_return std::nullopt;
        }
        // A decision that cannot be read now leaves the project readable without it.
        auto found =
            control::settled(co_await control::get_project_decision(*records, customer_id, id));
        if (found && *found) {
            value["decision"] = (*found)->decision;
        }
        co_return std::nullopt;
    }
    if (capability == "delete_project") {
        const std::string id = value.value("id", std::string());
        if (!id.empty()) {
            static_cast<void>(control::settled(
                co_await control::remove_project_decision(*records, customer_id, id)));
        }
        co_return std::nullopt;
    }
    // create_project: the engine answers the name only, so its id is read back.
    auto project = control::settled(co_await call_workspace(context,
                                                            customer_id,
                                                            "get_project",
                                                            workspace_uses_get("get_project"),
                                                            {{"name", project_name}}));
    const std::string id =
        project && project->is_object() ? project->value("id", std::string()) : std::string();
    if (id.empty()) {
        co_return ApiResult::failure(project ? "store_error" : project.error().code,
                                     "the project was created, but its decision was not kept: " +
                                         (project ? std::string("the workspace did not name its id")
                                                  : project.error().message));
    }
    control::ProjectDecision kept{.project_id = id,
                                  .customer_id = customer_id,
                                  .project = project_name,
                                  .decision = decision,
                                  .created_by = context.acting_user_id.empty()
                                                    ? context.acting_identity
                                                    : context.acting_user_id};
    auto written =
        control::settled(co_await control::record_project_decision(*records, std::move(kept)));
    if (!written) {
        co_return ApiResult::failure(written.error().code,
                                     "the project was created, but its decision was not kept: " +
                                         written.error().message);
    }
    value["decision"] = written->decision;
    co_return std::nullopt;
}

// Keeps the definition an agent now has as one revision in its workspace (lake#653,
// docs/api/train-agents.md#definition-revisions), after the engine accepted a create_agent or an
// update_agent. The engine keeps only the latest definition, so this is the agent's history. The
// change has happened whether or not this write succeeds, so a failure is reported beside the
// agent (`definition_revision.recorded: false`), never as the call's failure.
liquid::runtime::task<nlohmann::json> agent_revision_step(ApiContext& context,
                                                          const nlohmann::json& input,
                                                          const std::string& capability,
                                                          const nlohmann::json& agent) {
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.value("workspace_id", context.workspace_id);
    const std::string agent_id = agent.value("id", std::string());
    if (agent_id.empty()) {
        co_return nlohmann::json{{"recorded", false},
                                 {"message", "the workspace did not name the agent's id"}};
    }
    auto records = workspace_records(context, customer_id, workspace_id);
    if (!records) {
        co_return nlohmann::json{{"recorded", false}, {"message", records.error().message}};
    }
    control::AgentRevision revision{
        .customer_id = customer_id,
        .agent_id = agent_id,
        .change = capability == "create_agent" ? "create" : "update",
        .definition = control::agent_definition_of(agent),
        .created_by =
            context.acting_user_id.empty() ? context.acting_identity : context.acting_user_id};
    auto written =
        control::settled(co_await control::record_agent_revision(*records, std::move(revision)));
    if (!written) {
        co_return nlohmann::json{{"recorded", false}, {"message", written.error().message}};
    }
    co_return nlohmann::json{
        {"recorded", true}, {"id", written->id}, {"created_at", written->created_at}};
}

} // namespace

nlohmann::json agent_creator_answer(const nlohmann::json& turn) {
    if (const nlohmann::json result = turn.value("result", nlohmann::json());
        result.is_object() && result.contains("kind")) {
        return result;
    }
    nlohmann::json answer =
        nlohmann::json::parse(turn.contains("reply") && turn.at("reply").is_string()
                                  ? turn.at("reply").get<std::string>()
                                  : std::string(),
                              nullptr,
                              false);
    return answer.is_object() ? answer : nlohmann::json::object();
}

std::string agent_proposal_digest(const nlohmann::json& proposal) {
    return control::identity_crypto::sha256_hex(proposal.dump());
}

namespace {

using nlohmann::json;

// The Zetta capabilities lake's pages may call on a customer's workspace (docs/api/workspaces.md).
// Reads need `read` on the customer, actions `write`. Anything else is refused, so a lake caller
// can never reach Zetta's administrative surface (sql, users, cloud provisioning) through lake.
constexpr std::array<std::string_view, 49> workspace_reads = {"server_info",
                                                              "get_run_metrics",
                                                              "compare_runs",
                                                              "get_study",
                                                              "list_studies",
                                                              "list_trials",
                                                              "get_study_analysis",
                                                              "browse_topic",
                                                              "score",
                                                              "explain",
                                                              "list_projects",
                                                              "get_project",
                                                              "get_graph",
                                                              "list_connections",
                                                              "list_datasets",
                                                              "get_dataset",
                                                              "list_runs",
                                                              "get_run",
                                                              "get_run_report",
                                                              "list_models",
                                                              "get_model",
                                                              "model_health",
                                                              "list_indexes",
                                                              "get_index",
                                                              "search",
                                                              "list_jobs",
                                                              "get_job",
                                                              "get_trace",
                                                              "list_traces",
                                                              "list_source_tables",
                                                              // A database's safe mode
                                                              // (lake#906).
                                                              "get_safe_mode",
                                                              "list_dataset_generators",
                                                              "list_agents",
                                                              "get_agent",
                                                              "agent_training_status",
                                                              // The workspace's own pub/sub
                                                              // (docs/api/agent-bus.md): only
                                                              // topics created exposed.
                                                              "list_topics",
                                                              "receive_events",
                                                              // What the engine can run
                                                              // (zetta#1061) and how to
                                                              // featurize for it (zetta#1062).
                                                              "list_methodologies",
                                                              // Study preflight (zetta#1207): the
                                                              // same checks as create_study,
                                                              // storing nothing.
                                                              "validate_study",
                                                              // A model version's weights as a
                                                              // file, as kept or as ONNX
                                                              // (lake#681).
                                                              "export_model",
                                                              "analyze_dataset",
                                                              "plan_features",
                                                              // The solutions catalogue
                                                              // (zetta#1102).
                                                              "list_solutions",
                                                              "get_solution",
                                                              // A solution project's governed
                                                              // operating state (zetta#1128).
                                                              "get_policy",
                                                              "get_experiment",
                                                              "list_ledger",
                                                              "get_monitor",
                                                              "get_roi_readout"};
constexpr std::array<std::string_view, 46> workspace_actions = {"create_project",
                                                                "create_study",
                                                                "stop_study",
                                                                "stop_trial",
                                                                "promote_trial",
                                                                "create_solution",
                                                                "run_solution",
                                                                "update_policy",
                                                                "decide",
                                                                "approve_decision",
                                                                "record_outcome",
                                                                "capture_intent",
                                                                "compile_intent",
                                                                "approve_plan",
                                                                "gate",
                                                                "run",
                                                                "create_connection",
                                                                "delete_connection",
                                                                "set_safe_mode",
                                                                "create_dataset_from_query",
                                                                "upload_dataset",
                                                                "delete_project",
                                                                "create_index",
                                                                "expose_model",
                                                                "unexpose_model",
                                                                "expose_index",
                                                                "unexpose_index",
                                                                "score",
                                                                "explain",
                                                                "save_graph",
                                                                "deploy",
                                                                "import_n8n",
                                                                "compile_n8n",
                                                                "compile_python",
                                                                "promote_dataset",
                                                                "generate_dataset",
                                                                "create_agent",
                                                                "update_agent",
                                                                "delete_agent",
                                                                "set_training_schedule",
                                                                "agent_run",
                                                                // A model made on its own, not as
                                                                // a project pipeline's step
                                                                // (lake#654).
                                                                "train_model",
                                                                "retrain_model",
                                                                "register_model",
                                                                "move_model_to_project",
                                                                // A read in Zetta, called with GET,
                                                                // but it loads a model and spends
                                                                // compute: `write` in lake.
                                                                "chat"};

template <std::size_t Size>
bool listed(const std::array<std::string_view, Size>& names, std::string_view name) {
    return std::ranges::find(names, name) != names.end();
}

template <std::size_t Size> json allowed_names(const std::array<std::string_view, Size>& names) {
    std::vector<std::string> sorted(names.begin(), names.end());
    std::ranges::sort(sorted);
    return json(sorted);
}

std::expected<std::string, ApiError> customer_object(const json& input, ApiContext& context) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const auto object = context.store->find_policy_object("customer", input.at("customer_id"));
    if (!object) {
        return std::unexpected(
            ApiError{object.error().code, object.error().message, "/customer_id"});
    }
    return object->id;
}

json customer_input_schema(json properties, json required, bool workspace = true) {
    properties["customer_id"] = {{"type", "string"}};
    required.insert(required.begin(), "customer_id");
    if (workspace) {
        properties["workspace_id"] = {{"type", "string"}};
        required.push_back("workspace_id");
    }
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

ApiResult unavailable() {
    return ApiResult::failure("store_unavailable", "workspaces are not configured on this Lake");
}

json control_input_schema(json properties, json required) {
    return customer_input_schema(std::move(properties), std::move(required), false);
}

std::optional<control::AgentTask> calling_agent_task(const ApiContext& context) {
    constexpr std::string_view prefix = "task:";
    if (context.credential_kind != "agent_task" || context.store == nullptr ||
        !context.credential_session_id.starts_with(prefix)) {
        return std::nullopt;
    }
    const auto task =
        context.store->get_agent_task(context.credential_session_id.substr(prefix.size()));
    return task ? std::optional<control::AgentTask>(*task) : std::nullopt;
}

bool has_scope(const ApiContext& context, std::string_view scope) {
    return std::ranges::find(context.scopes, scope) != context.scopes.end();
}

// The stop_reason a launch withdrawn with its agent carries (lake#524).
std::string agent_gone_reason(const std::string& agent_id, bool deleted) {
    return deleted ? "Stopped: the agent " + agent_id + " was deleted."
                   : "Stopped: the agent " + agent_id +
                         " no longer exists in this workspace, so the run cannot start.";
}

// A launch or schedule whose agent's record the workspace says is gone (lake#524): agent-runtime
// reads its agent first, and an answer of not_found means it would only exit and be restarted, so
// lake withdraws the agent's launches and schedules and the operator removes their pods. Only the
// launch's or schedule's own agent, in its own workspace; any other caller changes nothing.
void withdraw_workloads_of_missing_agent(ApiContext& context,
                                         const std::string& customer_id,
                                         const std::string& workspace_id,
                                         const std::string& agent_id) {
    if (context.store == nullptr || agent_id.empty()) {
        return;
    }
    const std::string& session = context.credential_session_id;
    std::string owner_customer, owner_workspace, owner_agent;
    if (context.credential_kind == "agent_launch" && session.starts_with("launch:")) {
        const auto launch = context.store->get_agent_launch(session.substr(7));
        if (!launch) {
            return;
        }
        owner_customer = launch->customer_id;
        owner_workspace = launch->workspace_id;
        owner_agent = launch->agent_id;
    } else if (context.credential_kind == "agent_schedule" && session.starts_with("schedule:")) {
        const auto schedule = context.store->get_agent_cron_schedule(session.substr(9));
        if (!schedule) {
            return;
        }
        owner_customer = schedule->customer_id;
        owner_workspace = schedule->workspace_id;
        owner_agent = schedule->agent_id;
    } else {
        return;
    }
    if (owner_customer != customer_id || owner_agent != agent_id ||
        (!owner_workspace.empty() && owner_workspace != workspace_id)) {
        return;
    }
    (void)context.store->withdraw_agent_workloads(
        customer_id, workspace_id, agent_id, agent_gone_reason(agent_id, false));
}

void set_audit(ApiContext& context, std::string kind, const json& input, std::string capability) {
    context.audit_kind = std::move(kind);
    context.audit_details = {
        {"customer_id", input.at("customer_id")},
        {"workspace", control::workspace_slug(input.at("customer_id").get<std::string>())}};
    if (!capability.empty()) {
        context.audit_details["capability"] = std::move(capability);
    }
}

// The graph a Project Agent compile check stores: one per project and methodology, named so it
// is never the pipeline the person is changing (lake#438).
std::string project_agent_check_graph(const json& arguments) {
    for (const char* key : {"methodology", "solution"}) {
        if (const auto found = arguments.find(key);
            found != arguments.end() && found->is_string() && !found->get<std::string>().empty()) {
            return std::string(project_agent_check_prefix) + " " + found->get<std::string>();
        }
    }
    return std::string(project_agent_check_prefix);
}

// The compile check a Project Agent change proposal runs (lake#438). Zetta's compile_intent
// compiles the project's captured intent into the methodology's engine-owned graph and stores it,
// so lake confines the call: the project is the one the person asked about, the stored graph is
// lake's own check graph (never the pipeline being changed), the arguments are only those
// compile_intent takes (the agent's free-text `intent` is not one), and a repeat check reads the
// check graph back instead of compiling again.
liquid::runtime::task<ApiResult> project_agent_compile_check(ApiContext& context,
                                                             const json& input,
                                                             control::AgentTask task) {
    // The turn's request is a record in its workspace (lake#374).
    if (auto loaded = control::settled(co_await load_agent_task_content(context, task)); !loaded) {
        co_return ApiResult::failure(loaded.error().code, loaded.error().message);
    }
    const json handed = task.body.is_object() ? task.body.value("context", json::object())
                                              : json::object();
    // A change request names its project; a build turn ("build:<project>") designs one.
    std::string project =
        handed.is_object() ? handed.value("project", std::string()) : std::string();
    if (project.empty() && task.kind.starts_with("build:")) {
        project = task.kind.substr(6);
    }
    if (project.empty()) {
        co_return ApiResult::failure(
            "unauthorized", "this Project Agent turn has no project to compile a check for");
    }
    const json asked = input.value("input", json::object());
    json arguments{{"project", project}};
    for (const char* key : {"methodology", "solution"}) {
        if (const auto found = asked.find(key);
            found != asked.end() && found->is_string() && !found->get<std::string>().empty()) {
            arguments[key] = *found;
        }
    }
    if (const auto hints = asked.find("hints"); hints != asked.end() && hints->is_object()) {
        arguments["hints"] = *hints;
    }
    const std::string graph = project_agent_check_graph(arguments);
    arguments["name"] = graph;
    const std::string customer = input.at("customer_id");
    auto compiled = co_await call_workspace(context, customer, "compile_intent", false, arguments);
    if (!compiled) {
        co_return ApiResult::failure("store_unavailable", compiled.error().message);
    }
    if (*compiled) {
        json value = std::move(**compiled);
        co_return ApiResult::success(value.is_object() ? std::move(value)
                                                       : json{{"value", std::move(value)}});
    }
    const control::ZettaControlError refused = compiled->error();
    // A collision means an earlier check already stored this methodology's graph.
    if (refused.code == "engine_error") {
        auto stored = co_await call_workspace(
            context, customer, "get_graph", true, {{"project", project}, {"graph", graph}});
        if (stored && *stored && (**stored).is_object()) {
            json value = std::move(**stored);
            value["project"] = project;
            co_return ApiResult::success(std::move(value));
        }
    }
    co_return ApiResult::failure(refused.code, refused.message);
}

// A dataset reference in any id form (`<database>/<name>`, `<database>.<name>`, `<name>`) as the
// workspace capability reads it (lake#447): get_dataset takes `dataset` plus `database`, the
// analysis reads take `database.table`.
void normalize_dataset_reference(const std::string& capability, json& arguments) {
    if (!arguments.is_object()) {
        return;
    }
    // score and explain read `dataset_path` as `<database>.<table>`; the id list_datasets
    // answers (`<database>/<table>`) names the same table (lake#450).
    if (const auto path = arguments.find("dataset_path");
        path != arguments.end() && path->is_string() &&
        path->get_ref<const std::string&>().find('/') != std::string::npos) {
        if (const auto ref = datasets::parse_id(path->get<std::string>())) {
            *path = ref->database + "." + ref->name;
        }
    }
    if (!arguments.contains("dataset") || !arguments["dataset"].is_string()) {
        return;
    }
    const std::string id = arguments["dataset"];
    if (id.find('/') == std::string::npos &&
        (capability != "get_dataset" || id.find('.') == std::string::npos)) {
        return;
    }
    const auto ref = datasets::parse_id(id);
    if (!ref) {
        return;
    }
    if (capability == "get_dataset") {
        arguments["dataset"] = ref->name;
        if (!arguments.contains("database")) {
            arguments["database"] = ref->database;
        }
    } else if (capability == "analyze_dataset" || capability == "plan_features") {
        arguments["dataset"] = ref->database + "." + ref->name;
    }
}

struct DatasetWriteCheck {
    std::optional<ApiResult> failure;
    std::string requested_table;
};

// Before a dataset lands (lake#447): the table and database names must be ones SQL can query, a
// CSV upload must parse into rows that match its header, and a name already taken is refused
// unless the caller asked for the next free name with `if_exists: "rename"`. Zetta's
// upload_dataset refuses a taken name too and takes the same `if_exists` (zetta#1221), so a
// rename is forwarded on upload_dataset; create_dataset_from_query takes no such field and never
// sees it. Lake reports a rename as `renamed_from`.
liquid::runtime::task<DatasetWriteCheck> check_dataset_write(ApiContext& context,
                                                             std::string customer_id,
                                                             std::string capability,
                                                             json& arguments) {
    DatasetWriteCheck check;
    if (!arguments.is_object()) {
        co_return check;
    }
    std::string if_exists = "fail";
    if (arguments.contains("if_exists")) {
        if (!arguments["if_exists"].is_string() ||
            (arguments["if_exists"] != "fail" && arguments["if_exists"] != "rename")) {
            check.failure = ApiResult::failure(
                "invalid_input", "if_exists is \"fail\" or \"rename\"", "/input/if_exists");
            co_return check;
        }
        if_exists = arguments["if_exists"];
        if (capability != "upload_dataset" || if_exists != "rename") {
            arguments.erase("if_exists");
        }
    }
    for (const char* field : {"table_name", "database"}) {
        if (!arguments.contains(field)) {
            continue;
        }
        const std::string problem = arguments[field].is_string()
                                        ? datasets::name_problem(arguments[field].get<std::string>())
                                        : std::string("must be a string");
        if (!problem.empty()) {
            check.failure = ApiResult::failure(
                "invalid_input", std::string(field) + ": " + problem, std::string("/input/") + field);
            co_return check;
        }
    }
    if (capability == "upload_dataset" && arguments.value("format", std::string()) == "csv" &&
        arguments.contains("content") && arguments["content"].is_string()) {
        const std::string problem = datasets::csv_problem(arguments["content"].get<std::string>());
        if (!problem.empty()) {
            check.failure = ApiResult::failure(
                "invalid_input", "the CSV file is malformed: " + problem, "/input/content");
            co_return check;
        }
    }
    if (!arguments.contains("table_name")) {
        co_return check;
    }
    check.requested_table = arguments["table_name"];
    if (if_exists == "rename") {
        co_return check;
    }
    const std::string database = arguments.value("database", std::string("datasets"));
    auto existing = co_await call_workspace(
        context, std::move(customer_id), "get_dataset", true,
        {{"dataset", check.requested_table}, {"database", database}});
    if (existing && *existing && (*existing)->is_object() &&
        (*existing)->value("name", std::string()) == check.requested_table) {
        check.failure = ApiResult::failure(
            "conflict",
            "a dataset named '" + check.requested_table + "' already exists in " + database +
                "; choose another name, or pass if_exists: \"rename\" to save it under the next "
                "free name",
            "/input/table_name");
    }
    co_return check;
}

// Zetta keeps a Python agent's `definition` as a loose object and reads only the keys it knows,
// so `files: [{path, content}]` (for `contents`) was stored as an empty file without a word
// (lake#442). Lake checks the shape before create_agent or update_agent reaches the workspace.
std::optional<ApiResult> agent_definition_refusal(const json& arguments) {
    const auto definition = arguments.find("definition");
    if (definition == arguments.end()) {
        return std::nullopt;
    }
    const std::string at = "/input/definition";
    const std::string shape =
        "definition is {source: \"zetta\", instructions, entrypoint, files: [{path, contents}]} "
        "or {source: \"lake\", repository_id, revision, entrypoint}";
    if (!definition->is_object()) {
        return ApiResult::failure("invalid_input", shape, at);
    }
    constexpr std::array<std::string_view, 6> fields = {
        "source", "instructions", "files", "entrypoint", "repository_id", "revision"};
    for (const auto& [key, value] : definition->items()) {
        if (std::ranges::find(fields, key) == fields.end()) {
            return ApiResult::failure("invalid_input",
                                      "definition has no field '" + key + "'; " + shape,
                                      at + "/" + key);
        }
        if (key != "files" && !value.is_string()) {
            return ApiResult::failure(
                "invalid_input", "definition." + key + " is a string", at + "/" + key);
        }
    }
    const auto files = definition->find("files");
    if (files == definition->end()) {
        return std::nullopt;
    }
    if (!files->is_array()) {
        return ApiResult::failure(
            "invalid_input",
            "definition.files is a list of {path, contents}, one entry per file",
            at + "/files");
    }
    for (std::size_t index = 0; index < files->size(); ++index) {
        const json& file = (*files)[index];
        const std::string here = at + "/files/" + std::to_string(index);
        if (!file.is_object()) {
            return ApiResult::failure(
                "invalid_input", "each file is {path, contents}: the path and the file's text", here);
        }
        for (const auto& [key, value] : file.items()) {
            if (key != "path" && key != "contents") {
                return ApiResult::failure(
                    "invalid_input",
                    "a file has no field '" + key +
                        "'; each file is {path, contents}, and contents holds the file's text",
                    here + "/" + key);
            }
            if (!value.is_string()) {
                return ApiResult::failure("invalid_input", key + " is a string", here + "/" + key);
            }
        }
        if (file.value("path", std::string()).empty()) {
            return ApiResult::failure("invalid_input", "each file names its path", here + "/path");
        }
        if (!file.contains("contents")) {
            return ApiResult::failure(
                "invalid_input", "each file carries its text in contents", here + "/contents");
        }
    }
    return std::nullopt;
}

// expose_model and expose_index answer the engine's own route (`/v1/models/<id>/predict`), which
// does not exist on lake. Lake answers the call that does instead (lake#450): `predict` for a
// model, `search_index` for an index, with the input filled in.
void published_call(json& value, const std::string& capability, const json& input) {
    const bool model = capability.ends_with("_model");
    const std::string id = value.value(model ? "model_id" : "index_id", std::string());
    value.erase("endpoint");
    if (!value.value("exposed", false) || id.empty()) {
        return;
    }
    json call_input{{"customer_id", input.at("customer_id")},
                    {"workspace_id", input.at("workspace_id")}};
    if (model) {
        call_input["model_id"] = id;
        call_input["rows"] = json::array({json::object()});
        value["call"] = {{"capability", "predict"},
                         {"method", "POST"},
                         {"path", "/v1/predict"},
                         {"input", std::move(call_input)},
                         {"note",
                          "each row is keyed by the model's feature columns and carries "
                          "its own key in `entity`"}};
    } else {
        call_input["index_id"] = id;
        call_input["query"] = "";
        value["call"] = {{"capability", "search_index"},
                         {"method", "GET"},
                         {"path", "/v1/search_index"},
                         {"input", std::move(call_input)}};
    }
}

// One call to a workspace engine capability through the allow-lists, the policy and the audit
// (docs/api/workspaces.md): workspace_query and workspace_act, and each named engine capability
// (docs/api/engine-capabilities.md, lake#650), run this. `input` is {customer_id, workspace_id,
// capability, input}; a refusal's path points below `/input`.
// A pipeline step that runs a workspace methodology (lake#655, zetta#1261): the engine never runs
// its Python, so its `run` answers `runner_required` with what the run needs, and Lake dispatches
// it to the methodology runner as a training run of that pipeline. Answers the queued run.
ApiResult take(std::expected<ApiResult, liquid::error> completed);

liquid::runtime::task<ApiResult> dispatch_methodology_run(ApiContext& context,
                                                          const std::string& customer_id,
                                                          const std::string& workspace_id,
                                                          const json& details) {
    if (context.registry == nullptr || !details.is_object()) {
        co_return ApiResult::failure("store_error",
                                     "the engine asked Lake's methodology runner to run this "
                                     "pipeline, and Lake could not read what to run");
    }
    json request{{"customer_id", customer_id},
                 {"workspace_id", workspace_id},
                 {"id", details.value("methodology", std::string())},
                 {"dataset", details.value("dataset_path", std::string())}};
    if (const std::string target = details.value("target", std::string()); !target.empty()) {
        request["target"] = target;
    }
    if (const json params = details.value("params", json()); params.is_object()) {
        request["params"] = params;
    }
    if (const json holdout = details.value("holdout_fraction", json());
        holdout.is_number() && holdout.get<double>() > 0 && holdout.get<double>() <= 0.5) {
        request["holdout_fraction"] = holdout;
    }
    for (const auto& [key, named] :
         {std::pair{"project", "project_name"}, std::pair{"graph", "graph_name"}}) {
        std::string value = details.value(named, std::string());
        if (value.empty()) {
            value = details.value(key, std::string());
        }
        if (!value.empty()) {
            request[key] = value;
        }
    }
    ApiContext inner = context;
    inner.audit_kind.clear();
    inner.audit_details = json();
    auto ran = co_await context.registry->call_async("run_methodology", request, inner);
    if (!ran) {
        co_return ApiResult::failure("store_error", ran.error().message);
    }
    if (!ran->ok) {
        co_return *ran;
    }
    co_return ApiResult::success({{"runner", "lake"},
                                  {"status", "queued"},
                                  {"methodology", request["id"]},
                                  {"methodology_run", ran->value.value("run", json())}});
}

liquid::runtime::task<ApiResult> workspace_call(bool read, json input, ApiContext& context) {
        if (!context.workspaces) {
            co_return unavailable();
        }
        const std::string capability = input.at("capability");
        // Name the inner capability in the audit event before any refusal, so a person's
        // activity says what a refused workspace call tried to do (lake#445).
        set_audit(context, read ? "workspace.query" : "workspace.act", input, capability);
        // A call repeated after a safe mode warning says so in the audit (lake#906); the
        // workspace Zetta audits the confirmed statements themselves.
        if (const json inner = input.value("input", json::object());
            inner.is_object() && inner.value("safe_mode_confirmation", json()).is_string()) {
            context.audit_details["safe_mode_confirmed"] = true;
        }
        if (read ? !workspace_read_allowed(capability)
                 : !workspace_action_allowed(capability)) {
            co_return ApiResult::failure("invalid_input",
                                         capability + " is not a workspace " +
                                             (read ? "read" : "action"),
                                         "/capability");
        }
        const auto agent_task = calling_agent_task(context);
        // An agent turn acts only through the workspace actions its credential names
        // (lake#438): `workspace_act` alone never opens the whole allow-list to an agent. The
        // Experiments Agent's reads are named per turn as well.
        // A turn's credential may name the capability by the engine's name or by Lake's
        // (engine_capability_name, lake#650).
        if (agent_task && (!read || agent_task->agent == "agent-experiments") &&
            !has_scope(context, capability) &&
            !has_scope(context, engine_capability_name(capability))) {
            const control::LibraryAgent* library = control::library_agent(agent_task->agent);
            co_return ApiResult::failure(
                "unauthorized",
                "this " + (library ? std::string(library->name) : agent_task->agent) +
                    " turn does not grant " + capability);
        }
        // LSR-009 (lake#918): a launch's or a schedule's credential, which the agent's own code
        // holds, runs only the engine actions a run calls directly (agent_run_direct_actions) and
        // the actions its credential names: `workspace_act` alone opens nothing else, so the code
        // never deletes or changes the workspace without a person. Every other change is a
        // proposal a person confirms with their own credential.
        const auto direct = [&](std::string_view name) {
            return std::ranges::find(control::agent_run_direct_actions, name) !=
                   control::agent_run_direct_actions.end();
        };
        if (!read &&
            (context.credential_kind == "agent_launch" ||
             context.credential_kind == "agent_schedule") &&
            !direct(capability) && !direct(engine_capability_name(capability)) &&
            !has_scope(context, capability) &&
            !has_scope(context, engine_capability_name(capability))) {
            co_return ApiResult::failure(
                "unauthorized",
                "an agent run's credential does not grant the workspace action " + capability +
                    "; a person confirms changes with their own credential");
        }
        if (!read && agent_task && agent_task->agent == "agent-project" &&
            capability == "compile_intent") {
            co_return co_await project_agent_compile_check(context, input, *agent_task);
        }
        if (capability == "create_agent" || capability == "update_agent") {
            if (auto refused = agent_definition_refusal(input.value("input", json::object()))) {
                co_return *refused;
            }
        }
        // The engine's study actions are lake's confirmed experiment capabilities (lake#561):
        // the call speaks the engine's names in and out (`study`, `run`), and lake's
        // capability takes `experiment_id` and `run_id`.
        const std::map<std::string, std::string> study_actions = {
            {"create_study", "start_experiment"},
            {"stop_study", "stop_experiment"},
            {"stop_trial", "stop_experiment_trial"},
            {"promote_trial", "promote_trial"}};
        if (!read && study_actions.contains(capability)) {
            auto arguments = input.value("input", json::object());
            if (agent_task && agent_task->agent == "agent-experiments" &&
                has_scope(context, capability)) {
                arguments["confirm"] = true;
            }
            if (capability != "create_study" && arguments.contains("study") &&
                !arguments.contains("experiment_id")) {
                arguments["experiment_id"] = std::move(arguments["study"]);
                arguments.erase("study");
            }
            arguments["customer_id"] = input.at("customer_id");
            arguments["workspace_id"] = input.at("workspace_id");
            const auto result = co_await context.registry->call_async(
                study_actions.at(capability), arguments, context);
            if (!result) {
                co_return ApiResult::failure("store_error", result.error().message);
            }
            ApiResult answer = *result;
            if (answer.ok && answer.value.is_object() &&
                answer.value.contains("experiment_id")) {
                answer.value["study"] = std::move(answer.value["experiment_id"]);
                answer.value.erase("experiment_id");
            }
            if (!answer.ok && answer.error.path.starts_with("/experiment_id")) {
                answer.error.path = "/study" + answer.error.path.substr(14);
            }
            co_return answer;
        }
        json arguments = input.value("input", json::object());
        const json sent = arguments;
        // A native agent runs lake's base model, which only lake knows (viewer no longer
        // answers its path, lake#594): an agent created without one gets it here, and so does
        // one updated to native with an empty model (Design → Agents' Orama base model, lake#653).
        if (!read && context.workspaces != nullptr &&
            arguments.value("runtime", std::string()) == "native" &&
            ((capability == "create_agent" && arguments.value("model", std::string()).empty()) ||
             (capability == "update_agent" && arguments.contains("model") &&
              arguments["model"].is_string() && arguments["model"].get<std::string>().empty()))) {
            arguments["model"] = context.workspaces->config().base_agent_model;
        }
        // The engine refuses an empty name and a name in use with one message that names
        // neither (lake#452); lake says which.
        std::string project_name;
        if (!read && capability == "create_project") {
            const auto name = arguments.find("name");
            project_name = name != arguments.end() && name->is_string()
                               ? name->get<std::string>()
                               : std::string();
            if (std::ranges::all_of(project_name, [](unsigned char character) {
                    return std::isspace(character) != 0;
                })) {
                co_return ApiResult::failure(
                    "invalid_input", "a project needs a name", "/input/name");
            }
        }
        // A project's decision is lake's to keep (lake#577): the engine's project has no
        // field for it, so it is taken out here and written to the project's workspace
        // records once the engine has created the project.
        std::string decision;
        if (!read && capability == "create_project" && arguments.contains("decision")) {
            const json& field = arguments.at("decision");
            if (!field.is_null() && !field.is_string()) {
                co_return ApiResult::failure(
                    "invalid_input", "the decision is text", "/input/decision");
            }
            if (field.is_string()) {
                decision = trimmed_text(field.get<std::string>());
            }
            if (decision.size() > control::max_project_decision_bytes) {
                co_return ApiResult::failure(
                    "invalid_input",
                    "the decision is longer than " +
                        std::to_string(control::max_project_decision_bytes) + " bytes",
                    "/input/decision");
            }
            arguments.erase("decision");
        }
        // One name per concept (lake#450): `run` or `run_id`, `model` or `model_id`, as the
        // engine capability takes it.
        const auto renamed = canonical_workspace_input(capability, arguments);
        // compare_runs reads every metric the runs share when `names` is left out, as the
        // top-level compare_runs does (lake#450).
        if (read && capability == "compare_runs" && !arguments.contains("names")) {
            json compare = arguments;
            compare["customer_id"] = input.at("customer_id");
            compare["workspace_id"] = input.at("workspace_id");
            const auto result =
                co_await context.registry->call_async("compare_runs", compare, context);
            co_return result ? *result
                             : ApiResult::failure("store_error", result.error().message);
        }
        // Dataset rules lake applies before and after the workspace answers (lake#447).
        std::string requested_table;
        if (!read && (capability == "upload_dataset" ||
                      capability == "create_dataset_from_query")) {
            auto checked = co_await check_dataset_write(
                context, input.at("customer_id"), capability, arguments);
            if (!checked) {
                co_return ApiResult::failure("store_error", checked.error().message);
            }
            if (checked->failure) {
                co_return *checked->failure;
            }
            requested_table = std::move(checked->requested_table);
        } else {
            normalize_dataset_reference(capability, arguments);
        }
        // plan_features takes a dataset alone (lake#1153): Lake chooses the methodology from the
        // analysis and reads the cutoff from the dataset's snapshot column when the caller names
        // neither, and says so in the plan.
        json feature_plan_inferred;
        if (read && capability == "plan_features") {
            auto prepared =
                co_await prepare_feature_plan(context, input.at("customer_id"), arguments);
            if (!prepared) {
                co_return ApiResult::failure("store_error", prepared.error().message);
            }
            if (!*prepared) {
                co_return std::move(prepared->error());
            }
            feature_plan_inferred = std::move(**prepared);
        }
        // The agent a delete_agent removes: its stream triggers go with it (lake#677).
        const std::string deleted_agent =
            capability == "delete_agent" ? arguments.value("id", std::string()) : std::string();
        // The agent a get_agent reads: a launch's own agent found gone stops the launch (lake#524).
        const std::string read_agent =
            capability == "get_agent" ? arguments.value("id", std::string()) : std::string();
        auto called = co_await call_workspace(context,
                                              input.at("customer_id"),
                                              capability,
                                              workspace_uses_get(capability),
                                              std::move(arguments));
        if (!called) {
            co_return ApiResult::failure("store_unavailable", called.error().message);
        }
        if (!*called) {
            if (capability == "run" && called->error().code == "runner_required") {
                co_return take(co_await dispatch_methodology_run(context,
                                                                 input.at("customer_id"),
                                                                 input.at("workspace_id"),
                                                                 called->error().details));
            }
            if (!read_agent.empty() && called->error().code == "not_found") {
                withdraw_workloads_of_missing_agent(
                    context, input.at("customer_id"), input.at("workspace_id"), read_agent);
            }
            if (capability == "create_project" &&
                called->error().message.starts_with("create_project rejected")) {
                // The name is not empty (checked above), so it is in use.
                co_return ApiResult::failure(
                    "conflict",
                    "a project named '" + project_name +
                        "' already exists in this workspace; choose another name",
                    "/input/name");
            }
            // An engine older than the capability (allow-listed here, unknown there) says so in
            // words a person can act on, not as a missing route (lake#654).
            if (called->error().code == "unknown_capability") {
                co_return ApiResult::failure(
                    "not_supported",
                    "this workspace's engine does not have " +
                        std::string(engine_capability_name(capability)) +
                        " yet; it needs a newer Zetta. Nothing was changed.");
            }
            // Point at the field the workspace names (lake#256, lake#450), in lake's codes.
            ApiResult failed = workspace_failure(called->error(), sent, "/input", renamed);
            // An engine enum refusal lists the values, as lake's own do (lake#591).
            if (failed.error.message.ends_with(" is not one of the values it accepts") &&
                failed.error.path.starts_with("/input/")) {
                auto described = co_await call_workspace(
                    context, input.at("customer_id"), "openapi.json", true, json::object());
                if (described && *described) {
                    std::string field = failed.error.path.substr(7);
                    if (const auto back = std::ranges::find_if(
                            renamed, [&](const auto& pair) { return pair.second == field; });
                        back != renamed.end()) {
                        field = back->first;
                    }
                    if (const auto values = engine_input_enum(**described, capability, field)) {
                        failed.error.message = "expected one of " + values->dump();
                    }
                }
            }
            co_return failed;
        }
        json value = std::move(**called);
        if (value.is_object() &&
            (capability == "create_project" || capability == "get_project" ||
             capability == "delete_project")) {
            auto step = co_await project_decision_step(
                context, input, capability, project_name, decision, value);
            if (!step) {
                co_return ApiResult::failure("store_error", step.error().message);
            }
            if (*step) {
                co_return std::move(**step);
            }
        }
        if (!read && value.is_object() &&
            (capability == "create_agent" || capability == "update_agent")) {
            auto kept = co_await agent_revision_step(context, input, capability, value);
            value["definition_revision"] =
                kept ? std::move(*kept)
                     : json{{"recorded", false}, {"message", kept.error().message}};
        }
        if (!deleted_agent.empty() && value.is_object()) {
            // The engine has deleted the agent, whatever happens here: a trigger that cannot be
            // removed now is reported beside it, and the stream worker removes it on the next
            // message, when its launch finds no agent.
            auto removed = co_await remove_agent_stream_triggers(
                context, input.at("customer_id"), input.at("workspace_id"), deleted_agent);
            if (removed && *removed) {
                value["streams_removed"] = **removed;
            } else {
                value["streams_removed"] = 0;
                value["streams_error"] =
                    removed ? (*removed).error().message : removed.error().message;
            }
            // Its launches and schedules stop with it, so no pod is left restarting against an
            // agent that is gone (lake#524). A failure is reported beside the deletion; the
            // launch's next read of its agent stops it then.
            auto withdrawn =
                context.store ? context.store->withdraw_agent_workloads(
                                    input.at("customer_id"),
                                    input.at("workspace_id"),
                                    deleted_agent,
                                    agent_gone_reason(deleted_agent, true))
                              : control::Result<control::AgentWorkloadsWithdrawn>(std::unexpected(
                                    control::StoreError{"store_unavailable", "no control store"}));
            if (withdrawn) {
                value["launches_stopped"] = withdrawn->launches;
                value["schedules_removed"] = withdrawn->schedules;
            } else {
                value["launches_stopped"] = 0;
                value["schedules_removed"] = 0;
                value["launches_error"] = withdrawn.error().message;
            }
        }
        if (value.is_object() &&
            (capability == "expose_model" || capability == "unexpose_model" ||
             capability == "expose_index" || capability == "unexpose_index")) {
            published_call(value, capability, input);
        }
        // A workspace methodology's model is served by its own code (lake#690): publishing it
        // starts its server, unpublishing stops it.
        if (value.is_object() && (capability == "expose_model" || capability == "unexpose_model")) {
            const std::string model_id = value.value("model_id", std::string());
            if (!model_id.empty()) {
                const ApiResult served = take(co_await serve_methodology_model(
                    context,
                    input.at("customer_id"),
                    input.at("workspace_id"),
                    model_id,
                    capability == "expose_model" && value.value("exposed", false)));
                if (!served.ok) {
                    // Not served after all: the engine's publish is undone, and the reason given.
                    if (capability == "expose_model") {
                        static_cast<void>(co_await call_workspace(context,
                                                                  input.at("customer_id"),
                                                                  "unexpose_model",
                                                                  false,
                                                                  {{"model_id", model_id}}));
                    }
                    co_return served;
                }
                if (served.value.is_object()) {
                    value["deployment"] = served.value.value("deployment", json(nullptr));
                }
            }
        }
        if (capability == "plan_features") {
            describe_feature_plan(value, feature_plan_inferred);
        }
        if (capability == "list_datasets") {
            datasets::annotate_list(value);
        } else if (capability == "get_dataset" && value.is_object() &&
                   value.contains("name") && value["name"].is_string()) {
            const std::string database = value.value("database", std::string("datasets"));
            value["id"] = datasets::id_of(database, value["name"].get<std::string>());
            value["kind"] = datasets::kind_of(database);
            datasets::annotate_one(value);
        } else if (!requested_table.empty() && value.is_object()) {
            const std::string table = value.value("table", std::string());
            if (!table.empty() && table != requested_table) {
                value["renamed_from"] = requested_table;
            }
            if (!table.empty()) {
                value["id"] = datasets::id_of(value.value("database", std::string()), table);
            }
        }
        co_return ApiResult::success(value.is_object() ? std::move(value)
                                                       : json{{"value", std::move(value)}});
}

void register_workspace_call(Registry& registry, bool read) {
    registry.add(Capability{
        .name = read ? "workspace_query" : "workspace_act",
        .title = read ? "Query a workspace" : "Act in a workspace",
        .description =
            read ? "Read from a workspace's engine (the Zetta named by `workspace_id`): its "
                   "projects, datasets, runs, models, indexes and more, by naming one allowed "
                   "read `capability` and its `input`. An entity is `run_id`, `model_id`, "
                   "`index_id` or `job_id` in `input` (the engine's `run`, `model`, `index` and "
                   "`job` are accepted too), and `compare_runs` compares every shared metric when "
                   "`names` is left out."
                 : "Change something in a workspace's engine (the Zetta named by `workspace_id`): "
                   "create projects and datasets, approve and run pipelines, publish models and "
                   "indexes, by naming one allowed action `capability` and its `input`. "
                   "expose_model and expose_index answer `call`, the Lake capability that serves "
                   "what was published. A Python agent's create_agent or update_agent definition "
                   "is `{source: zetta, instructions, entrypoint, files: [{path, contents}]}` or "
                   "`{source: lake, repository_id, revision, entrypoint}`; Lake refuses any other "
                   "field.",
        // The names `capability` takes, so OpenAPI and MCP list them (lake#650); each is also
        // its own named capability (docs/api/engine-capabilities.md).
        .input_schema = customer_input_schema(
            {{"capability",
              {{"type", "string"},
               {"enum", read ? allowed_names(workspace_reads) : allowed_names(workspace_actions)}}},
             {"input", {{"type", "object"}}}},
            json::array({"capability"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .max_body_bytes = maximum_capability_body_bytes,
        .object = "customer",
        .action = read ? "read" : "write",
        .resolve_object = customer_object,
        .async_handler = [read](const json& input, ApiContext& context) {
            return workspace_call(read, input, context);
        }});
}

// The operator and the instance administrator see the operator's own storage message on a
// workspace row; everyone else sees its short reason code (migration 0069).
bool operator_view(const ApiContext& context) {
    return context.operator_credential || context.acting_identity == "admin";
}

ApiResult store_failure(const control::StoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

// A service as the capabilities answer it, with the storage it mounts (lake#666).
json service_with_stores(ApiContext& context, const control::ManagedService& service) {
    json value = control::service_json(service);
    const auto mounts = context.store->list_store_mounts(service.customer_id, service.workspace_id);
    value["stores"] = mounts ? workload_stores_json(*mounts, "service", service.id) : json::array();
    return value;
}

void service_audit(ApiContext& context, std::string kind, const control::ManagedService& service) {
    context.audit_kind = std::move(kind);
    context.audit_details = {{"customer_id", service.customer_id},
                             {"service_id", service.id},
                             {"service_kind", service.kind},
                             {"service_name", service.name}};
}

bool valid_service_name(const std::string& name) {
    if (name.empty() || name.size() > 31 || name.front() < 'a' || name.front() > 'z') {
        return false;
    }
    return std::ranges::all_of(name, [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == '-';
    });
}

// What each workspace capability does for the person calling it (lake#450). Lake writes the
// desired state; the operator's observations (phase, endpoint) are read-only.
std::string workspace_description(const std::string& operation) {
    static const std::map<std::string, std::string> descriptions = {
        {"list_workspaces",
         "List an account's workspaces (each its own Zetta) with their environment, desired state "
         "and the operator's observed phase; the `id` is the workspace_id other calls name."},
        {"get_workspace",
         "Read one workspace: its name, environment, desired state and whether the operator "
         "reports it ready."},
        {"create_workspace",
         "Create a workspace, a Zetta of its own for one environment of an account; poll "
         "workspace_status until it is ready."},
        {"update_workspace",
         "Rename a workspace or stop and start it (`desired: stopped | running`); its data is "
         "kept."},
        {"delete_workspace",
         "Delete a workspace and everything it holds, which cannot be recovered: its Zetta and "
         "every record in it, its repositories (live and tombstoned, retention cut short), its "
         "apps, its unfinished builds and its tenant images. `confirm` must be its name. "
         "`dry_run: true` deletes nothing and lists what would be destroyed, with counts."},
    };
    const auto found = descriptions.find(operation);
    return found == descriptions.end() ? std::string() : found->second;
}

void register_service_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "list_services",
        .title = "List services",
        .description = "The customer's managed services (Zetta and Postgres clusters) with the "
                       "operator's phase and in-cluster endpoint. Never includes credentials.",
        .input_schema = control_input_schema(json::object(), json::array()),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        // Like every list_ capability it authorizes itself: read on the customer.
        .authorizes_candidates = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            const auto object = customer_object(input, context);
            if (!object) {
                co_return ApiResult::failure(
                    object.error().code, object.error().message, object.error().path);
            }
            if (context.acting_identity != "admin") {
                const auto decision = control::authorize(*context.store,
                                                         context.acting_identity,
                                                         *object,
                                                         "read",
                                                         context.authorization_cache);
                if (!decision) {
                    co_return store_failure(decision.error());
                }
                if (!decision->allowed) {
                    co_return ApiResult::failure("unauthorized",
                                                 "principal is not allowed to read the customer");
                }
            }
            if (!context.workspaces) {
                co_return unavailable();
            }
            const auto listed = context.workspaces->services(input.at("customer_id"));
            if (!listed) {
                co_return store_failure(listed.error());
            }
            // The storage each service mounts (lake#666).
            const auto mounts = context.store->list_store_mounts(input.at("customer_id"));
            if (!mounts) {
                co_return store_failure(mounts.error());
            }
            json services = json::array();
            for (const control::ManagedService& service : *listed) {
                json value = control::service_json(service);
                value["stores"] = workload_stores_json(*mounts, "service", service.id);
                // The operator reports Ready once it has written the Secret; the kubelet copies
                // it into lake's mount up to a minute later. Until then lake cannot connect.
                const bool connected = context.workspaces->connection(service).has_value();
                value["connected"] = connected;
                // The same status the Services page shows (lake#452).
                const control::ServiceCondition condition =
                    control::service_condition(service, connected);
                value["status"] = condition.status;
                if (!condition.reason.empty()) {
                    value["status_reason"] = condition.reason;
                }
                services.push_back(std::move(value));
            }
            co_return ApiResult::success(
                {{"services", std::move(services)},
                 {"limit", context.workspaces->config().max_per_customer}});
        }});
    registry.add(Capability{
        .name = "create_service",
        .title = "Create a service",
        .description = "Ask the operator for a new service from the catalog (Postgres, Zetta, "
                       "Kafka, ClickHouse, TiDB, Cassandra, Valkey as a preview). "
                       "`instances` is 1-3 (default 1), `storage_gi` 1-100 (default 10). "
                       "`stores` mounts the workspace's storage into a SQLite, DuckDB or Polars "
                       "service: a volume at `mount_path`, a bucket as environment variables.",
        .input_schema = customer_input_schema(
            {{"kind",
              {{"type", "string"},
               {"enum",
                [] {
                    json keys = json::array();
                    for (const control::ServiceKind& kind : control::service_kinds) {
                        keys.push_back(std::string(kind.key));
                    }
                    return keys;
                }()}}},
             {"name", {{"type", "string"}}},
             {"instances", {{"type", "integer"}, {"minimum", 1}, {"maximum", 3}}},
             {"storage_gi", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}},
             {"stores", store_mounts_schema()}},
            json::array({"kind", "name"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            control::NewManagedService service;
            service.customer_id = input.at("customer_id");
            service.workspace_id = input.at("workspace_id");
            service.kind = input.at("kind");
            service.name = input.at("name");
            service.instances = input.value("instances", 1);
            service.storage_gi = input.value("storage_gi", 10);
            service.created_by_taap_id = context.acting_identity;
            if (control::find_service_kind(service.kind) == nullptr) {
                co_return ApiResult::failure(
                    "invalid_input", "kind is not in the service catalog", "/kind");
            }
            if (!valid_service_name(service.name)) {
                co_return ApiResult::failure(
                    "invalid_input",
                    "name must start with a letter and use lowercase letters, digits and '-', at "
                    "most 31 characters",
                    "/name");
            }
            if ((service.kind == "sqlite" || service.kind == "duckdb") && service.instances != 1) {
                co_return ApiResult::failure(
                    "invalid_input",
                    "a " + std::string(service.kind == "sqlite" ? "SQLite" : "DuckDB") +
                        " service has exactly one instance: one process "
                        "owns its database file",
                    "/instances");
            }
            if (service.instances < 1 || service.instances > 3 || service.storage_gi < 1 ||
                service.storage_gi > 100) {
                co_return ApiResult::failure("invalid_input",
                                             "instances must be 1-3 and storage_gi 1-100");
            }
            if (input.contains("stores")) {
                auto mounts = checked_store_mounts(input.at("stores"),
                                                   service.customer_id,
                                                   service.workspace_id,
                                                   service.kind,
                                                   false,
                                                   *context.store);
                if (!mounts) {
                    co_return mounts.error();
                }
                service.stores = std::move(*mounts);
            }
            const auto created = context.workspaces->create_service(std::move(service));
            if (!created) {
                co_return store_failure(created.error());
            }
            service_audit(context, "service.create", *created);
            co_return ApiResult::success(service_with_stores(context, *created));
        }});
    registry.add(Capability{
        .name = "delete_service",
        .title = "Delete a service",
        .description = "Delete one of the customer's services. The operator removes the cluster "
                       "and its data, which cannot be recovered. `confirm` must be the service's "
                       "name.",
        .input_schema = control_input_schema(
            {{"service_id", {{"type", "string"}}}, {"confirm", {{"type", "string"}}}},
            json::array({"service_id", "confirm"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            const auto found =
                context.workspaces->service(input.at("customer_id"), input.at("service_id"));
            if (!found) {
                co_return store_failure(found.error());
            }
            if (input.at("confirm") != found->name) {
                co_return ApiResult::failure(
                    "invalid_input", "confirm must be the service name " + found->name, "/confirm");
            }
            service_audit(context, "service.delete", *found);
            const auto deleted =
                context.workspaces->delete_service(input.at("customer_id"), input.at("service_id"));
            if (!deleted) {
                co_return store_failure(deleted.error());
            }
            co_return ApiResult::success(control::service_json(*deleted));
        }});
    registry.add(Capability{
        .name = "set_service_stores",
        .title = "Set a service's storage",
        .description = "Choose the workspace storage a SQLite, DuckDB or Polars service mounts, "
                       "replacing what it mounted: a volume at an absolute `mount_path` (a volume "
                       "is mounted by one service or app), a bucket in your own account as "
                       "environment variables. The operator rolls the service's pods. [] mounts "
                       "nothing.",
        .input_schema = control_input_schema(
            {{"service_id", {{"type", "string"}}}, {"stores", store_mounts_schema()}},
            json::array({"service_id", "stores"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            const auto found =
                context.workspaces->service(input.at("customer_id"), input.at("service_id"));
            if (!found) {
                co_return store_failure(found.error());
            }
            if (found->desired != "present") {
                co_return ApiResult::failure(
                    "conflict", "the service " + found->name + " is being deleted", "/service_id");
            }
            auto mounts = checked_store_mounts(input.at("stores"),
                                               found->customer_id,
                                               found->workspace_id,
                                               found->kind,
                                               false,
                                               *context.store);
            if (!mounts) {
                co_return mounts.error();
            }
            const auto updated = context.store->set_service_stores(found->id, *mounts);
            if (!updated) {
                co_return store_failure(updated.error());
            }
            service_audit(context, "service.stores", *updated);
            context.audit_details["stores"] = [&] {
                json ids = json::array();
                for (const control::StoreMountRequest& mount : *mounts) {
                    ids.push_back(mount.store_id);
                }
                return ids;
            }();
            co_return ApiResult::success(service_with_stores(context, *updated));
        }});
    registry.add(Capability{
        .name = "service_connection",
        .title = "Service connection",
        .description = "The connection details of a Ready service, including its password: "
                       "what the operator delivered for its kind. Audited. A Zetta service's "
                       "admin token is never returned: Lake calls it on the customer's behalf "
                       "through workspace_query and workspace_act.",
        .input_schema = control_input_schema({{"service_id", {{"type", "string"}}}},
                                             json::array({"service_id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            const auto found =
                context.workspaces->service(input.at("customer_id"), input.at("service_id"));
            if (!found) {
                co_return store_failure(found.error());
            }
            service_audit(context, "service.connection", *found);
            if (found->kind == "zetta") {
                co_return ApiResult::failure("invalid_input",
                                             "a Zetta service's admin token is never handed out; "
                                             "Lake calls it for you",
                                             "/service_id");
            }
            const auto connection = context.workspaces->connection(*found);
            if (found->phase != "Ready" || !connection) {
                co_return ApiResult::failure(
                    "workspace_not_ready",
                    found->phase == "Ready"
                        ? "the service is ready and its connection is still being delivered to "
                          "Lake; try again in a minute"
                        : "the service is not ready yet");
            }
            // Everything the operator delivered for this kind (docs/control-plane/services.md,
            // The catalog), for example Kafka's bootstrap and SASL settings.
            // Zetta was refused above; for every other kind the token, where there is one (SQLite),
            // is the credential the customer asked for.
            json value = *connection;
            value.erase("kind");
            co_return ApiResult::success(std::move(value));
        }});
}

void store_audit(ApiContext& context, std::string kind, const control::ManagedStore& store) {
    context.audit_kind = std::move(kind);
    context.audit_details = {{"customer_id", store.customer_id},
                             {"workspace_id", store.workspace_id},
                             {"store_id", store.id},
                             {"store_type", store.type},
                             {"store_provider", store.provider},
                             {"store_name", store.name}};
}

// A read on the customer, checked in the handler as list_services does.
std::optional<ApiResult> refuse_unless_reader(const json& input, ApiContext& context) {
    const auto object = customer_object(input, context);
    if (!object) {
        return ApiResult::failure(object.error().code, object.error().message, object.error().path);
    }
    if (context.acting_identity != "admin") {
        const auto decision = control::authorize(
            *context.store, context.acting_identity, *object, "read", context.authorization_cache);
        if (!decision) {
            return store_failure(decision.error());
        }
        if (!decision->allowed) {
            return ApiResult::failure("unauthorized",
                                      "principal is not allowed to read the customer");
        }
    }
    return std::nullopt;
}

json store_types_enum() {
    json types = json::array();
    for (const control::StoreProvider& entry : control::store_providers) {
        if (std::find(types.begin(), types.end(), std::string(entry.type)) == types.end()) {
            types.push_back(std::string(entry.type));
        }
    }
    return types;
}

json store_providers_enum() {
    json providers = json::array();
    for (const control::StoreProvider& entry : control::store_providers) {
        if (std::find(providers.begin(), providers.end(), std::string(entry.provider)) ==
            providers.end()) {
            providers.push_back(std::string(entry.provider));
        }
    }
    return providers;
}

// Another capability as the same person (defined below).
liquid::runtime::task<ApiResult> call_capability(ApiContext& context, std::string name, json input);

ApiResult call_result(std::expected<ApiResult, liquid::error> done) {
    return done ? std::move(*done) : ApiResult::failure("store_error", done.error().message);
}

// Where a new store's contents are (lake#664), checked with the caller's own authority: a
// customer's own bucket and the workspace secret that opens it, or a `lake` store's repository
// (made now when none is named). Fills `store`; the refusal otherwise.
liquid::runtime::task<std::optional<ApiResult>>
store_location(const json& input, ApiContext& context, control::NewManagedStore& store) {
    const bool bucket = control::store_external_bucket(store.type, store.provider);
    const bool volume = store.type == "volume";
    if (volume) {
        store.size_gi = input.value("size_gi", 10);
        if (store.size_gi < 1 || store.size_gi > 100) {
            co_return ApiResult::failure("invalid_input", "size_gi must be 1-100", "/size_gi");
        }
    } else {
        store.size_gi = 0;
        if (input.contains("size_gi")) {
            co_return ApiResult::failure("invalid_input", "only a volume has a size to ask for",
                                         "/size_gi");
        }
    }
    for (const char* field : {"bucket", "endpoint", "prefix", "credential"}) {
        if (!bucket && input.contains(field)) {
            co_return ApiResult::failure("invalid_input",
                                         std::string("only your own bucket takes ") + field,
                                         std::string("/") + field);
        }
    }
    if (store.provider != "lake" && input.contains("repository_id")) {
        co_return ApiResult::failure("invalid_input", "only a lake store names a repository",
                                     "/repository_id");
    }
    if (bucket) {
        store.bucket = input.value("bucket", std::string());
        if (const std::string problem = control::store_bucket_problem(store.provider, store.bucket);
            !problem.empty()) {
            co_return ApiResult::failure("invalid_input", problem, "/bucket");
        }
        store.endpoint_url = input.value("endpoint", std::string());
        while (store.endpoint_url.ends_with('/')) {
            store.endpoint_url.pop_back();
        }
        if (store.endpoint_url.empty() &&
            control::default_store_endpoint(store.provider, store.region).empty()) {
            co_return ApiResult::failure(
                "invalid_input",
                store.provider == "r2"
                    ? "an R2 bucket needs its endpoint, https://<account id>.r2.cloudflarestorage.com"
                : store.provider == "azure"
                    ? "an Azure container needs its endpoint, https://<account>.blob.core.windows.net"
                    : "a B2 bucket needs its endpoint or its region",
                "/endpoint");
        }
        if (!store.endpoint_url.empty()) {
            if (const std::string problem = control::store_endpoint_problem(store.endpoint_url);
                !problem.empty()) {
                co_return ApiResult::failure("invalid_input", problem, "/endpoint");
            }
        }
        store.prefix = input.value("prefix", std::string());
        if (const std::string problem = control::store_prefix_problem(store.prefix);
            !problem.empty()) {
            co_return ApiResult::failure("invalid_input", problem, "/prefix");
        }
        if (!input.contains("credential")) {
            co_return ApiResult::failure(
                "invalid_input",
                "your own bucket names the workspace secret that opens it: credential {mount_id, "
                "name}",
                "/credential");
        }
        store.credential_mount_id = input.at("credential").value("mount_id", std::string());
        store.credential_name = input.at("credential").value("name", std::string());
        // The caller must be able to read the secret, in this workspace's secret store.
        ApiResult mount = call_result(co_await call_capability(
            context, "get_secret_mount", {{"mount_id", store.credential_mount_id}}));
        if (!mount.ok) {
            co_return ApiResult::failure(
                mount.error.code == "unauthorized" ? "unauthorized" : "not_found",
                "no secret mount " + store.credential_mount_id + " you can read",
                "/credential/mount_id");
        }
        if (mount.value.value("workspace_id", std::string()) != store.workspace_id) {
            co_return ApiResult::failure(
                "invalid_input",
                "the secret mount belongs to another workspace than the storage",
                "/credential/mount_id");
        }
        ApiResult secret = call_result(co_await call_capability(
            context, "read_secret",
            {{"mount_id", store.credential_mount_id}, {"name", store.credential_name}}));
        if (!secret.ok) {
            co_return ApiResult::failure("not_found",
                                         "the mount has no live secret " + store.credential_name +
                                             " you can read",
                                         "/credential/name");
        }
        const json value = secret.value.value("value", json::object());
        const auto has = [&](const char* field) {
            return value.is_object() && value.contains(field) && value.at(field).is_string() &&
                   !value.at(field).get<std::string>().empty();
        };
        if (store.provider == "azure" ? !(has("account_key") || has("sas_token"))
                                      : !(has("access_key_id") && has("secret_access_key"))) {
            co_return ApiResult::failure(
                "invalid_input",
                store.provider == "azure"
                    ? "the secret " + store.credential_name +
                          " needs an account_key or a sas_token field"
                    : "the secret " + store.credential_name +
                          " needs access_key_id and secret_access_key fields",
                "/credential/name");
        }
    }
    if (store.provider == "lake") {
        store.repository_id = input.value("repository_id", std::string());
        if (store.repository_id.empty()) {
            ApiResult made = call_result(co_await call_capability(context,
                                                      "create_repository",
                                                      {{"customer_id", store.customer_id},
                                                       {"workspace_id", store.workspace_id},
                                                       {"name", store.name},
                                                       {"visibility", "private"}}));
            if (!made.ok) {
                co_return ApiResult::failure(made.error.code,
                                             "could not make the store's repository: " +
                                                 made.error.message,
                                             "/name");
            }
            store.repository_id = made.value.value("id", std::string());
        } else {
            const auto repository = context.store->get_repository(store.repository_id);
            if (!repository || !repository->deleted_at.empty() ||
                repository->customer_id != store.customer_id ||
                repository->workspace_id != store.workspace_id) {
                co_return ApiResult::failure("not_found",
                                             "no repository " + store.repository_id +
                                                 " in this workspace",
                                             "/repository_id");
            }
            if (context.acting_identity != "admin") {
                const auto object =
                    context.store->find_policy_object("repository", store.repository_id);
                bool allowed = false;
                if (object) {
                    const auto decision =
                        control::authorize(*context.store, context.acting_identity, object->id,
                                           "write", context.authorization_cache);
                    allowed = decision && decision->allowed;
                }
                if (!allowed) {
                    co_return ApiResult::failure("unauthorized",
                                                 "you may not write to that repository",
                                                 "/repository_id");
                }
            }
        }
    }
    co_return std::nullopt;
}

// Storage (docs/control-plane/storage.md, lake#196): the provider table and the customer's stores.
void register_store_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "list_store_providers",
        .title = "List store providers",
        .description = "Every type × provider pair a store can be (`object`, `filesystem`, "
                       "`network_filesystem`, `volume` on `cluster`, `r2`, `s3`, `b2`, `azure`, "
                       "`gcs`, `lake`, `zetta`, `nfs`), whether this Lake offers it, and why not when it does not.",
        .input_schema = control_input_schema(json::object(), json::array()),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        // Like every list_ capability it authorizes itself: read on the customer.
        .authorizes_candidates = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (auto refused = refuse_unless_reader(input, context)) {
                co_return std::move(*refused);
            }
            json providers = json::array();
            for (const control::StoreProvider& entry : control::store_providers) {
                json value{{"type", std::string(entry.type)},
                           {"provider", std::string(entry.provider)},
                           {"label", std::string(entry.label)},
                           {"available", entry.available}};
                if (!entry.available) {
                    value["unavailable_reason"] = std::string(entry.unavailable_reason);
                }
                providers.push_back(std::move(value));
            }
            co_return ApiResult::success({{"providers", std::move(providers)}});
        }});
    registry.add(Capability{
        .name = "list_stores",
        .title = "List stores",
        .description = "The workspace's stores (buckets, filesystems, network filesystems and "
                       "volumes) with the operator's phase, message and endpoint, the services "
                       "and apps that mount each (`mounted_by`), and its `used_bytes` (null until "
                       "the operator has measured it). Never includes credentials.",
        .input_schema = customer_input_schema(json::object(), json::array()),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .authorizes_candidates = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (auto refused = refuse_unless_reader(input, context)) {
                co_return std::move(*refused);
            }
            if (!context.workspaces) {
                co_return unavailable();
            }
            // Only this workspace's stores: the operator places each in its workspace's namespace.
            const auto listed =
                context.workspaces->stores(input.at("customer_id"), input.at("workspace_id"));
            if (!listed) {
                co_return store_failure(listed.error());
            }
            json stores = json::array();
            for (const control::ManagedStore& store : *listed) {
                json value = control::store_json(store);
                // The operator writes the connection before Ready; the kubelet copies it into
                // lake's mount up to a minute later.
                value["connected"] = context.workspaces->store_connection(store).has_value();
                stores.push_back(std::move(value));
            }
            co_return ApiResult::success({{"stores", std::move(stores)}});
        }});
    registry.add(Capability{
        .name = "create_store",
        .title = "Create a store",
        .description =
            "Ask the operator for a new store in this workspace: a `type` and `provider` pair "
            "that list_store_providers reports available, and a `name` unique in the account. A "
            "volume takes `size_gi` 1-100 (default 10). Your own bucket (r2, s3, b2, gcs, azure) "
            "takes `bucket`, `endpoint` (https; optional for s3 and gcs), `region`, `prefix` (the "
            "folder in it the store covers) and `credential` {mount_id, name}: a secret of this "
            "workspace's secret store holding access_key_id and secret_access_key (S3 protocol) or "
            "account_key or sas_token (Azure). A `lake` store keeps its contents in a liquid "
            "repository of the workspace, `repository_id` or a new one with the store's name. "
            "Private by default.",
        .input_schema = customer_input_schema(
            {{"type", {{"type", "string"}, {"enum", store_types_enum()}}},
             {"provider", {{"type", "string"}, {"enum", store_providers_enum()}}},
             {"name", {{"type", "string"}}},
             {"size_gi", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}},
             {"region", {{"type", "string"}}},
             {"bucket", {{"type", "string"}}},
             {"endpoint", {{"type", "string"}}},
             {"prefix", {{"type", "string"}}},
             {"credential",
              {{"type", "object"},
               {"properties",
                {{"mount_id", {{"type", "string"}}}, {"name", {{"type", "string"}}}}},
               {"required", json::array({"mount_id", "name"})},
               {"additionalProperties", false}}},
             {"repository_id", {{"type", "string"}}}},
            json::array({"type", "provider", "name"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            control::NewManagedStore store;
            store.customer_id = input.at("customer_id");
            store.workspace_id = input.at("workspace_id");
            store.type = input.at("type");
            store.provider = input.at("provider");
            store.name = input.at("name");
            store.region = input.value("region", std::string());
            store.created_by_taap_id = context.acting_identity;
            if (!valid_service_name(store.name)) {
                co_return ApiResult::failure(
                    "invalid_input",
                    "name must start with a letter and use lowercase letters, digits and '-', at "
                    "most 31 characters",
                    "/name");
            }
            const control::StoreProvider* pair =
                control::find_store_provider(store.type, store.provider);
            if (pair == nullptr || !pair->available) {
                // The store's own checks name the reason.
            } else {
                auto located = co_await store_location(input, context, store);
                if (!located) {
                    co_return ApiResult::failure("store_error", located.error().message);
                }
                if (*located) {
                    co_return std::move(**located);
                }
            }
            const auto created = context.workspaces->create_store(std::move(store));
            if (!created) {
                co_return store_failure(created.error());
            }
            store_audit(context, "store.create", *created);
            co_return ApiResult::success(control::store_json(*created));
        }});
    registry.add(Capability{
        .name = "delete_store",
        .title = "Delete a store",
        .description = "Delete one of this workspace's stores; `confirm` must be the store's name. "
                       "A volume and a store Lake keeps in the workspace Zetta are deleted with "
                       "their data, which cannot be recovered. Your own bucket and a `lake` store's "
                       "repository stay, with what is in them: Lake only forgets them. A store a "
                       "service or an app mounts is refused (conflict, naming them): remove it "
                       "from them first.",
        .input_schema = customer_input_schema(
            {{"store_id", {{"type", "string"}}}, {"confirm", {{"type", "string"}}}},
            json::array({"store_id", "confirm"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            const auto found = context.workspaces->store(
                input.at("customer_id"), input.at("store_id"), input.at("workspace_id"));
            if (!found) {
                co_return store_failure(found.error());
            }
            if (input.at("confirm") != found->name) {
                co_return ApiResult::failure(
                    "invalid_input", "confirm must be the store name " + found->name, "/confirm");
            }
            // A mounted store is refused before anything is deleted (lake#666): the store's
            // withdraw refuses it too, but a `zetta` store's items would already be gone.
            if (!found->mounted_by.empty()) {
                co_return ApiResult::failure("conflict",
                                             control::mounted_by_message(found->mounted_by));
            }
            if (found->provider == "zetta" && found->desired == "present") {
                // Lake keeps these bytes itself, so Lake deletes them before the store goes.
                auto emptied = co_await empty_kept_store(context, *found);
                ApiResult result = emptied ? std::move(*emptied)
                                           : ApiResult::failure("store_error",
                                                                emptied.error().message);
                if (!result.ok) {
                    co_return result;
                }
            }
            store_audit(context, "store.delete", *found);
            const auto deleted =
                context.workspaces->delete_store(
                    input.at("customer_id"), input.at("store_id"), input.at("workspace_id"));
            if (!deleted) {
                co_return store_failure(deleted.error());
            }
            co_return ApiResult::success(control::store_json(*deleted));
        }});
    registry.add(Capability{
        .name = "store_connection",
        .title = "Store connection",
        .description = "How to reach one of this workspace's Ready stores: what the operator "
                       "delivered for its type and provider (for a cluster volume, the claim, "
                       "namespace, size and storage class). Audited.",
        .input_schema =
            customer_input_schema({{"store_id", {{"type", "string"}}}}, json::array({"store_id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            const auto found = context.workspaces->store(
                input.at("customer_id"), input.at("store_id"), input.at("workspace_id"));
            if (!found) {
                co_return store_failure(found.error());
            }
            store_audit(context, "store.connection", *found);
            const auto connection = context.workspaces->store_connection(*found);
            if (found->desired != "present" || found->phase != "Ready" || !connection) {
                co_return ApiResult::failure(
                    "workspace_not_ready",
                    found->desired == "present" && found->phase == "Ready"
                        ? "the store is ready and its connection is still being delivered to "
                          "Lake; try again in a minute"
                        : "the store is not ready");
            }
            co_return ApiResult::success(*connection);
        }});
}

// One Zetta call in the customer's workspace, as an ApiResult.
liquid::runtime::task<ApiResult>
zetta(ApiContext& context, std::string customer, std::string capability, bool read, json input) {
    const json input_copy = input;
    auto called =
        co_await call_workspace(context, customer, std::move(capability), read, std::move(input));
    if (!called) {
        co_return ApiResult::failure("store_unavailable", called.error().message);
    }
    if (!*called) {
        // In lake's words, at the field the engine names (lake#591).
        co_return workspace_failure((*called).error(), input_copy);
    }
    co_return ApiResult::success(**called);
}

ApiResult take(std::expected<ApiResult, liquid::error> completed) {
    return completed ? std::move(*completed)
                     : ApiResult::failure("store_error", completed.error().message);
}

struct RunDetailCompletion {
    liquid::result<ApiResult> value;
};

boost::capy::io_task<RunDetailCompletion>
complete_run_detail(liquid::runtime::task<ApiResult> task) {
    auto result = co_await std::move(task);
    co_return boost::capy::io_result<RunDetailCompletion>{{}, {std::move(result)}};
}

std::string first_line(std::string text) {
    const std::size_t newline = text.find_first_of("\r\n");
    if (newline != std::string::npos) {
        text.erase(newline);
    }
    return text;
}

std::string run_error_line(const json& run) {
    if (run.contains("error") && run.at("error").is_string()) {
        return first_line(run.at("error").get<std::string>());
    }
    const json logs = run.value("logs", json::array());
    for (auto entry = logs.rbegin(); entry != logs.rend(); ++entry) {
        if (entry->value("level", std::string()) == "error") {
            return first_line(entry->value("message", std::string()));
        }
    }
    return {};
}

std::string run_stage(const json& run) {
    const std::string status = run.value("status", std::string());
    if (status == "queued" || status == "pending") {
        return "Queued";
    }
    std::string stage;
    constexpr std::string_view started = "solution stage started: ";
    constexpr std::string_view succeeded = "solution stage succeeded: ";
    for (const json& entry : run.value("logs", json::array())) {
        const std::string message = entry.value("message", std::string());
        if (message.starts_with(started)) {
            stage = message.substr(started.size());
        } else if (message.starts_with(succeeded)) {
            stage = message.substr(succeeded.size());
        }
    }
    if (!stage.empty()) {
        return stage;
    }
    if (status == "succeeded") {
        return "Completed";
    }
    if (status == "failed") {
        return "Foundation";
    }
    return "Preparing foundation";
}

bool solution_run_matches(const json& run, const std::string& solution) {
    if (solution.empty()) {
        return true;
    }
    for (const char* key : {"graph", "pipeline_name"}) {
        const std::string name = run.value(key, std::string());
        if (name == solution || name.starts_with(solution + " ") ||
            name.starts_with(solution + "-") || name.starts_with(solution + ":")) {
            return true;
        }
    }
    return false;
}

} // namespace

// One validated pipeline edit (docs/design/zetta-parity.md): read the graph, apply the edit with
// lake::api::pipeline, and save the whole graph back. The page's forms, REST and MCP all edit
// through this, so no client has to build a save_graph body or can save a broken graph.
void register_pipeline_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "edit_pipeline",
        .title = "Edit a pipeline",
        .description =
            "Apply one edit to a pipeline in the customer's workspace and save it: `op` is "
            "add_step (kind, name), rename_step (step, name), kind_step (step, kind), remove_step "
            "(step), add_edge (from, to, kind data_flow|control), remove_edge (from, to), "
            "set_attribute (step, name, value; a null value removes it) or rename (name). Or pass "
            "`edits`, a list of such edits, to apply them in order as one save (all or nothing), "
            "as a Project Agent proposal is applied. The result must keep unique step ids, known "
            "kinds, edges between existing steps and no cycle. A saved pipeline is proposed again "
            "and must be re-approved to run.",
        .input_schema = customer_input_schema({{"project", {{"type", "string"}}},
                                               {"graph", {{"type", "string"}}},
                                               {"op",
                                                {{"type", "string"},
                                                 {"enum",
                                                  json::array({"add_step",
                                                               "rename_step",
                                                               "kind_step",
                                                               "remove_step",
                                                               "add_edge",
                                                               "remove_edge",
                                                               "set_attribute",
                                                               "rename"})}}},
                                               {"step", {{"type", "string"}}},
                                               {"kind", {{"type", "string"}}},
                                               {"name", {{"type", "string"}}},
                                               {"from", {{"type", "string"}}},
                                               {"to", {{"type", "string"}}},
                                               {"value", json::object()},
                                               {"edits",
                                                {{"type", "array"},
                                                 {"maxItems", pipeline::maximum_edits},
                                                 {"items", {{"type", "object"}}}}}},
                                              json::array({"project", "graph"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            const std::string customer = input.at("customer_id");
            const std::string project = input.at("project");
            const std::string graph = input.at("graph");
            set_audit(context, "pipeline.edit", input, "save_graph");
            auto read = co_await call_workspace(
                context,
                customer, "get_graph", true, {{"project", project}, {"graph", graph}});
            if (!read) {
                co_return ApiResult::failure("store_unavailable", read.error().message);
            }
            if (!*read) {
                co_return ApiResult::failure((*read).error().code, (*read).error().message);
            }
            // Own the graph and edits in the pool task: parsing, validation and serialization
            // must not hold the HTTP executor, including when a stored graph exceeds our budget.
            auto prepared = co_await blocking_call(
                [value = std::move(**read), input, project, graph]() -> ApiResult {
                    auto current = pipeline::from_json(value);
                    if (!current) {
                        return ApiResult::failure("invalid_input", current.error());
                    }
                    const std::size_t before = current->steps.size();
                    const json edits = input.value("edits", json());
                    if (edits.is_null() && !input.contains("op")) {
                        return ApiResult::failure(
                            "invalid_input", "give an op or a list of edits", "/op");
                    }
                    auto edited = edits.is_null()
                                      ? pipeline::apply(*current, pipeline::edit_from_json(input))
                                      : pipeline::apply_all(*current, edits);
                    if (!edited) {
                        return ApiResult::failure("invalid_input", edited.error());
                    }
                    json save{{"project", project},
                              {"graph", current->graph_id.empty() ? graph : current->graph_id},
                              {"nodes", pipeline::steps_json(*edited)},
                              {"edges", pipeline::edges_json(*edited)}};
                    if (edited->name != current->name) {
                        save["new_name"] = edited->name;
                    }
                    json result{{"save", std::move(save)}, {"name", edited->name}};
                    if (edited->steps.size() > before) {
                        result["step"] = edited->steps.back().id;
                    }
                    return ApiResult::success(std::move(result));
                },
                [](const std::string& message) { return ApiResult::failure("busy", message); });
            if (!prepared.ok) {
                co_return prepared;
            }
            json save = std::move(prepared.value["save"]);
            auto saved =
                co_await call_workspace(context, customer, "save_graph", false, std::move(save));
            if (!saved) {
                co_return ApiResult::failure("store_unavailable", saved.error().message);
            }
            if (!*saved) {
                co_return ApiResult::failure((*saved).error().code, (*saved).error().message);
            }
            json value = **saved;
            value["name"] = std::move(prepared.value["name"]);
            if (prepared.value.contains("step")) {
                value["step"] = std::move(prepared.value["step"]);
            }
            co_return ApiResult::success(std::move(value));
        }});
}

// Search one published index of the customer's workspace (#187). The key an outside caller holds
// is a lake API token scoped to search_index alone: it can query published indexes and nothing
// else, and never sees the workspace's admin token. An unpublished index is refused.
void register_search_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "search_index",
        .title = "Search a published index",
        .description = "Search one published search index in the customer's workspace. `query` is "
                       "the text, `field` optionally limits it to one indexed field, `top_k` caps "
                       "the hits (default 10), and `fields` picks which of the index's fields each "
                       "hit carries (default all of them; [] for ids and scores only; a field the "
                       "index does not hold is refused). Answers `hits` as {id, score, fields}, "
                       "best first: `fields` holds the matched row's values, which is what a "
                       "caller shows; `id` is the workspace's internal id of the row, not a column "
                       "of the table. An index that is not published is refused; publish it with "
                       "workspace_act expose_index.",
        .input_schema = customer_input_schema(
            {{"index_id", {{"type", "string"}}},
             {"query", {{"type", "string"}}},
             {"field", {{"type", "string"}}},
             {"fields",
              {{"type", "array"},
               {"items", {{"type", "string"}, {"minLength", 1}}},
               {"maxItems", 100}}},
             {"top_k", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}}},
            json::array({"index_id", "query"})),
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"hits",
                             {{"type", "array"},
                              {"items",
                               {{"type", "object"},
                                {"properties",
                                 {{"id", {{"type", "string"}}},
                                  {"score", {{"type", "number"}}},
                                  {"fields", {{"type", "object"}}}}},
                                {"required", json::array({"id", "score"})}}}}}}}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            const std::string customer = input.at("customer_id");
            const std::string index = input.at("index_id");
            set_audit(context, "index.search", input, "search");
            auto described = co_await call_workspace(
                context, customer, "get_index", true, {{"index_id", index}});
            if (!described) {
                co_return ApiResult::failure("store_unavailable", described.error().message);
            }
            if (!*described) {
                co_return workspace_failure(described->error(), {{"index_id", index}});
            }
            if (!(**described).value("exposed", false)) {
                co_return ApiResult::failure(
                    "invalid_input", "index " + index + " is not published", "/index_id");
            }
            json search{{"target", index},
                        {"query", input.at("query")},
                        {"top_k", input.value("top_k", 10)}};
            if (const std::string field = input.value("field", std::string()); !field.empty()) {
                search["field"] = field;
            }
            // Which of the index's fields each hit carries (zetta#1243); the engine's default is
            // all of them, and it refuses a field the index does not hold.
            if (const auto fields = input.find("fields"); fields != input.end()) {
                search["fields"] = *fields;
            }
            auto found =
                co_await call_workspace(context, customer, "search", true, std::move(search));
            if (!found) {
                co_return ApiResult::failure("store_unavailable", found.error().message);
            }
            if (!*found) {
                co_return workspace_failure(found->error(), input);
            }
            // The index's answered searches over time, for what serving it costs (lake#658),
            // added off the request path like predict's predictions.
            if (context.usage_recorder != nullptr) {
                context.usage_recorder->add(input.value("workspace_id", context.workspace_id),
                                            "searches",
                                            1,
                                            {{"object_id", index}});
            }
            co_return ApiResult::success(**found);
        },
        .input_aliases = {{"index", "index_id"}}});
}

// Whether a model groups rows into segments rather than predicting a column (lake#1243): the
// engine's model kind (`segmentation`, zetta#1042), or its weights' kind.
static bool clustering_model(const json& model) {
    const auto kind_of = [](const json& value) {
        if (!value.is_object() || !value.contains("kind") || !value["kind"].is_string()) {
            return std::string();
        }
        std::string kind = value["kind"].get<std::string>();
        std::ranges::transform(
            kind, kind.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return kind;
    };
    for (const std::string& kind :
         {kind_of(model), kind_of(model.is_object() ? model.value("feature_schema", json()) : json())}) {
        if (kind == "segmentation" || kind == "clustering" || kind == "cluster") {
            return true;
        }
    }
    return false;
}

// The engine's refusal to score or explain a model whose weights are not gradient-boosted trees
// (zetta before #1497 reads only those): "score: model 'm' weights unavailable: /: required
// property 'objective' not found in object". Its schema words are never shown (lake#1243).
static bool unscorable_weights(std::string_view message) {
    return message.contains("weights unavailable") && message.contains("'objective'");
}

static std::string clustering_predict_unsupported(const std::string& model) {
    return "Model " + model +
           " is a clustering model: it groups the rows it was trained on into segments and "
           "predicts no column, so there is nothing to score. Its segments, and the table that "
           "assigns each row to one, are on its page in Ship → Models. This workspace's engine "
           "cannot place a new row in a segment yet.";
}

static std::string unscorable_model(const std::string& model) {
    return "Model " + model +
           " cannot score rows in this workspace's engine: it is not a model that predicts a "
           "column (a clustering model groups rows into segments instead). Its page in Ship → "
           "Models shows what it made.";
}

// The engine names a bad row by its zero-based index ("score: row 2 has no 'tenure'"), but people
// count rows from one: the message names it as row 3, followed by the row's own key when it has one
// as a string or a number ("row 3 ('partial')", lake#819). The error's `path` keeps the zero-based
// index, since it points into the `rows` array.
static std::string name_row_as_people_count(const std::string& message,
                                            const std::smatch& match,
                                            const json& rows,
                                            const std::string& key) {
    const auto index = std::stoull(match[1].str());
    std::string named = "row " + std::to_string(index + 1);
    if (index < rows.size() && rows[index].is_object() && rows[index].contains(key)) {
        const json& entity = rows[index].at(key);
        if (entity.is_string()) {
            named += " ('" + entity.get<std::string>() + "')";
        } else if (entity.is_number()) {
            named += " (" + entity.dump() + ")";
        }
    }
    // The match starts at "row <n>"; everything after the number is kept.
    const auto start = static_cast<std::size_t>(match.position(0));
    const auto after = static_cast<std::size_t>(match.position(1) + match.length(1));
    return message.substr(0, start) + named + message.substr(after);
}

// Predict with one published model (lake#250): the public route a customer calls from outside lake,
// with a lake key. The model must be published (expose_model); rows are keyed by the model's own
// feature columns and answered ranked by risk with what drives it (Zetta's `score`). Each ranked
// entry names its row and that row's entity key (lake#450): the engine numbers the rows it was
// given, and lake maps each number back to the row.
// The predict call itself; register_predict_capability times it and counts it per model.
// `known` is set once the workspace has answered that the model exists, so a made-up id never
// becomes a series label.
liquid::runtime::task<ApiResult> predict_rows(const json& input, ApiContext& context, bool& known) {
    if (!context.workspaces) {
        co_return unavailable();
    }
    const std::string customer = input.at("customer_id");
    const std::string model = input.at("model_id");
    const std::string key = input.value("entity_column", std::string("entity"));
    set_audit(context, "model.predict", input, "score");
    auto described =
        co_await call_workspace(context, customer, "get_model", true, {{"model_id", model}});
    if (!described) {
        co_return ApiResult::failure("store_unavailable", described.error().message);
    }
    if (!*described) {
        co_return ApiResult::failure(
            "not_found", "no model " + model + " (use its id from Ship → Models)", "/model_id");
    }
    known = true;
    if (!(**described).value("exposed", false)) {
        co_return ApiResult::failure(
            "invalid_input", "model " + model + " is not published", "/model_id");
    }
    // A workspace methodology's model answers from its own server (lake#690).
    auto served = co_await predict_methodology_model(
        context, customer, input.value("workspace_id", context.workspace_id), **described, input);
    if (!served) {
        co_return ApiResult::failure("store_error", served.error().message);
    }
    if (*served) {
        co_return std::move(**served);
    }
    const json& rows = input.at("rows");
    // A clustering model (lake#1243) answers each row's nearest segment, from an engine that can
    // place a new row in one (zetta#1497); an older engine refuses with its weights' schema.
    const bool clustering = clustering_model(**described);
    json score{{"model", model}, {"rows", rows}};
    if (input.contains("top_k") && !clustering) {
        score["top_k"] = input["top_k"];
    }
    auto scored = co_await call_workspace(context, customer, "score", true, std::move(score));
    if (!scored) {
        co_return ApiResult::failure("store_unavailable", scored.error().message);
    }
    if (!*scored) {
        const std::string message = scored->error().message;
        if (unscorable_weights(message)) {
            co_return ApiResult::failure(
                "not_supported",
                clustering ? clustering_predict_unsupported(model) : unscorable_model(model),
                "/model_id");
        }
        // "score: row 2 has no 'tenure'" is about /rows/2/tenure, and reads "row 3" (lake#819).
        static const std::regex row_field(R"(row (\d+) has (?:no|a non-numeric) '([^']+)')");
        std::smatch match;
        if (std::regex_search(message, match, row_field)) {
            co_return ApiResult::failure("invalid_input",
                                         name_row_as_people_count(message, match, rows, key),
                                         "/rows/" + match[1].str() + "/" + match[2].str());
        }
        co_return workspace_failure(scored->error(), input);
    }
    json value = std::move(**scored);
    for (const char* list : {"ranked", "assignments"}) {
        if (!value.is_object() || !value.contains(list) || !value[list].is_array()) {
            continue;
        }
        for (json& entry : value[list]) {
            if (!entry.is_object() || !entry.contains("entity") ||
                !entry["entity"].is_number_integer()) {
                continue;
            }
            const auto index = entry["entity"].get<std::int64_t>();
            entry.erase("entity");
            entry["row"] = index;
            if (index >= 0 && static_cast<std::size_t>(index) < rows.size() &&
                rows[static_cast<std::size_t>(index)].contains(key)) {
                entry["entity"] = rows[static_cast<std::size_t>(index)].at(key);
            }
        }
    }
    value["entity_column"] = key;
    co_return ApiResult::success(std::move(value));
}

void register_predict_capability(Registry& registry) {
    registry.add(Capability{
        .name = "predict",
        .title = "Predict with a published model",
        .description =
            "Score your own records with one published model of a workspace and get them back "
            "ranked by risk, with the features that drive it. `model_id` is the model's id "
            "(Ship → Models); `rows` are the records, each keyed by the model's feature columns "
            "and carrying its own key in `entity_column` (default `entity`). Every ranked entry "
            "answers that key as `entity` and the record's position as `row`. A clustering "
            "(segmentation) model answers `assignments` instead, one per row in order: "
            "{row, entity, segment_id, distance, label}, each record's nearest segment, from a "
            "workspace engine that can place a new row in a segment; an engine that cannot is "
            "refused as not_supported. A model that is not published is refused; publish it "
            "with workspace_act expose_model.",
        .input_schema = customer_input_schema(
            {{"model_id", {{"type", "string"}}},
             {"rows", {{"type", "array"}, {"items", {{"type", "object"}}}, {"minItems", 1}}},
             {"entity_column",
              {{"type", "string"},
               {"description",
                "the field of each row that holds its key, answered as `entity` (default "
                "`entity`; a row without it is answered by `row` alone)"}}},
             {"top_k", {{"type", "integer"}, {"minimum", 0}}}},
            json::array({"model_id", "rows"})),
        .output_schema = {{"type", "object"}},
        // Rows go in a JSON body, so it is POST; it changes nothing and is authorized as a read.
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            const auto started = std::chrono::steady_clock::now();
            bool known = false;
            auto completed = co_await predict_rows(input, context, known);
            ApiResult result = completed
                                   ? std::move(*completed)
                                   : ApiResult::failure("store_error", completed.error().message);
            // The per-model series (lake#599): every answer about a model that exists, refusals
            // included, in the workspace the call named. RequestRecorder only counts here; the
            // owner loop writes.
            if (context.request_recorder != nullptr && known) {
                const ErrorCode* code = result.ok ? nullptr : find_error_code(result.error.code);
                const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                              std::chrono::steady_clock::now() - started)
                                              .count();
                context.request_recorder->add_model_call(
                    input.value("workspace_id", context.workspace_id),
                    input.value("model_id", std::string()),
                    result.ok ? 200 : (code != nullptr ? code->status : 500),
                    elapsed_ms);
            }
            // The model's answered predictions over time, for what serving it costs (lake#564).
            if (context.usage_recorder != nullptr && known && result.ok) {
                context.usage_recorder->add(
                    input.value("workspace_id", context.workspace_id),
                    "predictions",
                    1,
                    {{"object_id", input.value("model_id", std::string())}});
            }
            co_return result;
        },
        .input_aliases = {{"model", "model_id"}}});
}

// A workspace's predict traffic (lake#599): calls, errors and mean latency per model from the
// workspace Zetta's `lake_model_calls` and `lake_model_call_ms` series, and the workspace's
// `predict` requests from `lake_requests` (docs/serving/telemetry.md). A series not written yet
// reads as no traffic. Prediction drift is not measured: the engine keeps no reference
// distribution of the training data to compare against.
namespace model_traffic_detail {

constexpr std::int64_t minute_ms = RequestRecorder::resolution_ms;
constexpr std::int64_t day_ms = 24LL * 60 * 60 * 1000;

struct Steps {
    double calls = 0;
    double errors = 0;
    double ms = 0;
};

struct Totals {
    double calls = 0;
    double client_errors = 0;
    double server_errors = 0;
    double ms = 0;
    std::map<std::int64_t, Steps> steps;
};

bool is_error(const std::string& status_class) {
    return status_class == "4xx" || status_class == "5xx";
}

} // namespace model_traffic_detail

void register_model_traffic_capability(Registry& registry) {
    using namespace model_traffic_detail;
    registry.add(Capability{
        .name = "model_traffic",
        .title = "A model's prediction traffic",
        .description =
            "How the workspace's published models are being called: per model, the `predict` "
            "calls, client and server errors and mean latency over a time range, step by step "
            "(`from_ms`, `to_ms`, `step_ms`, all optional: the last 24 hours in about 48 steps), "
            "and the workspace's `predict` requests by status class. `model_id` keeps one model. "
            "Read from the workspace's own series; `drift` says prediction drift is not measured "
            "yet.",
        .input_schema =
            customer_input_schema({{"model_id", {{"type", "string"}}},
                                   {"from_ms", {{"type", "integer"}, {"minimum", 0}}},
                                   {"to_ms", {{"type", "integer"}, {"minimum", 0}}},
                                   {"step_ms", {{"type", "integer"}, {"minimum", minute_ms}}}},
                                  json::array()),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            const std::string customer = input.at("customer_id");
            const std::string model = input.value("model_id", std::string());
            const std::int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::system_clock::now().time_since_epoch())
                                            .count();
            std::int64_t to_ms = input.contains("to_ms") ? input.at("to_ms").get<std::int64_t>()
                                                         : now_ms - now_ms % minute_ms + minute_ms;
            const std::int64_t from_ms = input.contains("from_ms")
                                             ? input.at("from_ms").get<std::int64_t>()
                                             : to_ms - day_ms;
            if (from_ms >= to_ms) {
                co_return ApiResult::failure(
                    "invalid_input", "from_ms is before to_ms", "/from_ms");
            }
            if (to_ms - from_ms > 31 * day_ms) {
                co_return ApiResult::failure(
                    "invalid_input",
                    "the range is at most 31 days, as long as the series keeps",
                    "/from_ms");
            }
            std::int64_t step_ms = 0;
            if (input.contains("step_ms")) {
                step_ms = input.at("step_ms").get<std::int64_t>();
                if (step_ms % minute_ms != 0) {
                    co_return ApiResult::failure("invalid_input",
                                                 "step_ms is a whole number of minutes (60000)",
                                                 "/step_ms");
                }
            } else {
                // About 48 steps, each a whole number of minutes.
                const std::int64_t minutes = (to_ms - from_ms + minute_ms - 1) / minute_ms;
                step_ms = std::max<std::int64_t>(1, (minutes + 47) / 48) * minute_ms;
            }
            if ((to_ms - from_ms) / step_ms > 10'000) {
                co_return ApiResult::failure(
                    "invalid_input", "at most 10000 steps; make step_ms larger", "/step_ms");
            }

            // One query of a series, its `series` as the value; a series never written reads
            // as none.
            const auto query = [&](std::string metric,
                                   json labels,
                                   json group_by) -> liquid::runtime::task<ApiResult> {
                json request{{"metric", std::move(metric)},
                             {"from_ms", from_ms},
                             {"to_ms", to_ms},
                             {"step_ms", step_ms},
                             {"group_by", std::move(group_by)}};
                if (!labels.empty()) {
                    request["labels"] = std::move(labels);
                }
                auto answered = control::settled(co_await call_workspace(
                    context, customer, "query_metric", false, std::move(request)));
                if (!answered) {
                    if (answered.error().code == "not_found") {
                        co_return ApiResult::success(json::array());
                    }
                    co_return workspace_failure(answered.error(), input);
                }
                co_return ApiResult::success(answered->value("series", json::array()));
            };
            const auto settle = [](std::expected<ApiResult, liquid::error> completed) {
                return completed ? std::move(*completed)
                                 : ApiResult::failure("store_error", completed.error().message);
            };

            const json model_labels = model.empty() ? json::object() : json{{"model_id", model}};
            const json by_model = json::array({"model_id", "status_class"});
            const ApiResult calls = settle(co_await query(
                std::string(RequestRecorder::model_calls_metric), model_labels, by_model));
            if (!calls.ok) {
                co_return calls;
            }
            const ApiResult ms = settle(co_await query(
                std::string(RequestRecorder::model_call_ms_metric), model_labels, by_model));
            if (!ms.ok) {
                co_return ms;
            }
            const ApiResult requests = settle(co_await query(std::string(RequestRecorder::metric),
                                                             {{"capability", "predict"}},
                                                             json::array({"status_class"})));
            if (!requests.ok) {
                co_return requests;
            }

            std::map<std::string, Totals> models;
            const auto fold = [&](const json& series, bool duration) {
                for (const json& one : series) {
                    const json labels = one.value("labels", json::object());
                    const std::string id = labels.value("model_id", std::string());
                    const std::string status_class = labels.value("status_class", std::string());
                    if (id.empty()) {
                        continue;
                    }
                    Totals& totals = models[id];
                    const double total = one.value("total", 0.0);
                    if (duration) {
                        totals.ms += total;
                    } else {
                        totals.calls += total;
                        if (status_class == "4xx") {
                            totals.client_errors += total;
                        } else if (status_class == "5xx") {
                            totals.server_errors += total;
                        }
                    }
                    for (const json& point : one.value("points", json::array())) {
                        Steps& step = totals.steps[point.value("at_ms", std::int64_t{0})];
                        const double value = point.value("value", 0.0);
                        if (duration) {
                            step.ms += value;
                        } else {
                            step.calls += value;
                            step.errors += is_error(status_class) ? value : 0;
                        }
                    }
                }
            };
            fold(calls.value, false);
            fold(ms.value, true);

            json listed = json::array();
            for (const auto& [id, totals] : models) {
                json steps = json::array();
                for (const auto& [at_ms, step] : totals.steps) {
                    json entry{{"at_ms", at_ms}, {"calls", step.calls}, {"errors", step.errors}};
                    if (step.calls > 0) {
                        entry["mean_ms"] = step.ms / step.calls;
                    }
                    steps.push_back(std::move(entry));
                }
                json entry{{"model_id", id},
                           {"calls", totals.calls},
                           {"errors", totals.client_errors + totals.server_errors},
                           {"client_errors", totals.client_errors},
                           {"server_errors", totals.server_errors},
                           {"steps", std::move(steps)}};
                if (totals.calls > 0) {
                    entry["mean_ms"] = totals.ms / totals.calls;
                }
                listed.push_back(std::move(entry));
            }
            json by_class = json::object();
            double request_total = 0;
            for (const json& one : requests.value) {
                const std::string status_class =
                    one.value("labels", json::object()).value("status_class", std::string("other"));
                const double total = one.value("total", 0.0);
                by_class[status_class] = by_class.value(status_class, 0.0) + total;
                request_total += total;
            }
            co_return ApiResult::success(
                {{"from_ms", from_ms},
                 {"to_ms", to_ms},
                 {"step_ms", step_ms},
                 {"models", std::move(listed)},
                 {"requests", {{"total", request_total}, {"by_status_class", std::move(by_class)}}},
                 {"drift",
                  {{"measured", false},
                   {"why",
                    "the workspace keeps no reference distribution of a model's training data "
                    "to compare its predictions with"}}}});
        }});
}

// The Zetta connector for a service kind, from the service's delivered connection, or nothing when
// Zetta cannot read that kind yet. Zetta's connectors today: postgres and snowflake.
std::optional<json> zetta_connector(const control::ManagedService& service,
                                    const json& connection) {
    if (service.kind == "postgres") {
        json connector{
            {"type", "postgres"},
            {"host", connection.value("host", std::string())},
            {"port",
             connection.contains("port")
                 ? (connection.at("port").is_string() ? connection.at("port").get<std::string>()
                                                      : connection.at("port").dump())
                 : std::string("5432")},
            {"user", connection.value("user", std::string())},
            {"database", connection.value("database", std::string())},
            {"token", connection.value("password", std::string())},
            // CloudNativePG serves TLS in the cluster with its own CA.
            {"sslmode", "require"}};
        return connector;
    }
    return std::nullopt;
}

// Data → Connections → Connect one of your services (lake#188, lake#651): make a Zetta connection to one of the
// customer's own Ready services from the connection the operator delivered, so nobody types or
// sees its password. Saving under an existing name replaces it, which is how Refresh works.
void register_connect_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "connect_service",
        .title = "Connect a service to the workspace",
        .description = "Create (or refresh) a Zetta connection in the customer's workspace to one "
                       "of the customer's own Ready services, filled from the service's delivered "
                       "connection. Postgres today; other kinds answer invalid_input until Zetta "
                       "can read them. `name` defaults to the service's name.",
        .input_schema = customer_input_schema(
            {{"service_id", {{"type", "string"}}}, {"name", {{"type", "string"}}}},
            json::array({"service_id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            const std::string customer = input.at("customer_id");
            const auto found = context.workspaces->service(customer, input.at("service_id"));
            if (!found) {
                co_return store_failure(found.error());
            }
            service_audit(context, "service.connect", *found);
            const auto connection = context.workspaces->connection(*found);
            if (found->phase != "Ready" || !connection) {
                co_return ApiResult::failure("workspace_not_ready", "the service is not ready yet");
            }
            const auto connector = zetta_connector(*found, *connection);
            if (!connector) {
                co_return ApiResult::failure("invalid_input",
                                             "the workspace cannot read a " + found->kind +
                                                 " service yet",
                                             "/service_id");
            }
            const std::string name = input.value("name", found->name);
            auto created = co_await call_workspace(context,
                                                   customer,
                                                   "create_connection",
                                                   false,
                                                   {{"name", name}, {"connector", *connector}});
            if (!created) {
                co_return ApiResult::failure("store_unavailable", created.error().message);
            }
            if (!*created) {
                co_return ApiResult::failure((*created).error().code, (*created).error().message);
            }
            json value = **created;
            value["service_id"] = found->id;
            value["service_name"] = found->name;
            co_return ApiResult::success(std::move(value));
        }});
}

// What the pipeline page's forms chain together, as capabilities, so REST and MCP clients get the
// same flows (lake#189): read a pipeline with its step status and runs, gate then run it, and
// create one empty, from n8n or from Python. The catalog of service kinds is readable too.
void register_parity_capabilities(Registry& registry) {
    const json project_graph{{"project", {{"type", "string"}}}, {"graph", {{"type", "string"}}}};
    registry.add(Capability{
        .name = "list_runs",
        .title = "List runs",
        .description =
            "List the latest workspace runs, optionally filtered by project or solution. Each "
            "row includes status, the stage reached and the last error line. Detailed reads for "
            "the bounded result are fetched concurrently.",
        .input_schema = customer_input_schema(
            {{"project", {{"type", "string"}, {"minLength", 1}}},
             {"solution", {{"type", "string"}, {"minLength", 1}}},
             {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 20}}}},
            json::array()),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .authorizes_candidates = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (auto refused = refuse_unless_reader(input, context)) {
                co_return std::move(*refused);
            }
            if (!context.workspaces) {
                co_return unavailable();
            }
            const std::string customer = input.at("customer_id");
            const ApiResult listed =
                take(co_await zetta(context, customer, "list_runs", true, json::object()));
            if (!listed.ok) {
                co_return listed;
            }
            const std::string project = input.value("project", std::string());
            const std::string solution = input.value("solution", std::string());
            json runs = json::array();
            for (const json& run : listed.value.value("runs", json::array())) {
                const std::string named_project =
                    run.value("project_name", run.value("project", std::string()));
                if ((!project.empty() && named_project != project) ||
                    !solution_run_matches(run, solution)) {
                    continue;
                }
                runs.push_back(run);
            }
            std::sort(runs.begin(), runs.end(), [](const json& left, const json& right) {
                return left.value("created", std::string()) > right.value("created", std::string());
            });
            const std::size_t limit = input.value("limit", std::size_t{5});
            if (runs.size() > limit) {
                runs.erase(runs.begin() + static_cast<std::ptrdiff_t>(limit), runs.end());
            }
            if (runs.empty()) {
                co_return ApiResult::success({{"runs", json::array()}});
            }

            std::vector<ApiContext> contexts(runs.size(), context);
            std::vector<boost::capy::io_task<RunDetailCompletion>> pending;
            pending.reserve(runs.size());
            for (std::size_t index = 0; index < runs.size(); ++index) {
                pending.push_back(complete_run_detail(
                    zetta(contexts[index],
                          customer,
                          "get_run",
                          true,
                          {{"run_id", runs.at(index).value("id", std::string())}})));
            }
            auto [wait_error, details] = co_await liquid::runtime::when_all(std::move(pending));
            if (wait_error) {
                co_return ApiResult::failure("store_error", wait_error.message());
            }
            for (std::size_t index = 0; index < details.size(); ++index) {
                const ApiResult detail = take(std::move(details[index].value));
                if (!detail.ok) {
                    continue;
                }
                for (const char* key :
                     {"project", "graph", "status", "created", "updated", "outputs", "lineage"}) {
                    if (detail.value.contains(key)) {
                        runs[index][key] = detail.value.at(key);
                    }
                }
                runs[index]["stage"] = run_stage(detail.value);
                const std::string error = run_error_line(detail.value);
                if (!error.empty()) {
                    runs[index]["error"] = error;
                }
            }
            co_return ApiResult::success({{"runs", std::move(runs)}});
        }});
    registry.add(Capability{
        .name = "get_pipeline",
        .title = "Get a pipeline",
        .description =
            "One pipeline in the customer's workspace: its steps and edges, the approval "
            "state, each step's status in the latest run, and the pipeline's runs, "
            "newest first.",
        .input_schema = customer_input_schema(project_graph, json::array({"project", "graph"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            const std::string customer = input.at("customer_id");
            ApiResult graph = take(
                co_await zetta(context,
                               customer,
                               "get_graph",
                               true,
                               {{"project", input.at("project")}, {"graph", input.at("graph")}}));
            if (!graph.ok) {
                co_return graph;
            }
            json value = graph.value;
            const std::string name = value.value("name", std::string());
            const std::string id = value.value("graph_id", std::string());
            json runs = json::array();
            const ApiResult listed =
                take(co_await zetta(context, customer, "list_runs", true, json::object()));
            if (listed.ok) {
                for (const json& run : listed.value.value("runs", json::array())) {
                    const std::string of = run.value("graph", std::string());
                    if (of == name || of == id) {
                        runs.push_back(run);
                    }
                }
            }
            std::sort(runs.begin(), runs.end(), [](const json& a, const json& b) {
                return a.value("created", std::string()) > b.value("created", std::string());
            });
            json status = json::object();
            if (!runs.empty()) {
                const ApiResult latest =
                    take(co_await zetta(context,
                                        customer,
                                        "get_run",
                                        true,
                                        {{"run_id", runs.front().value("id", std::string())}}));
                if (latest.ok) {
                    for (const json& step : latest.value.value("lineage", json::object())
                                                .value("steps", json::array())) {
                        if (step.is_string()) {
                            status[step.get<std::string>()] =
                                latest.value.value("status", std::string());
                        }
                    }
                }
            }
            value["step_status"] = std::move(status);
            value["runs"] = std::move(runs);
            co_return ApiResult::success(std::move(value));
        }});
    registry.add(Capability{
        .name = "run_pipeline",
        .title = "Check and run a pipeline",
        .description =
            "Run Zetta's correctness checks on an approved pipeline and, when they pass, "
            "run it. Optional `dataset_path` (database.table), `holdout_fraction` and "
            "`model_name`. Answers {gate, run}; a failing gate answers {gate} and runs "
            "nothing.",
        .input_schema = customer_input_schema(
            {{"project", {{"type", "string"}}},
             {"graph", {{"type", "string"}}},
             {"dataset_path", {{"type", "string"}}},
             {"holdout_fraction", {{"type", "number"}, {"minimum", 0}, {"maximum", 0.9}}},
             {"model_name", {{"type", "string"}}}},
            json::array({"project", "graph"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            const std::string customer = input.at("customer_id");
            set_audit(context, "pipeline.run", input, "run");
            const json target{{"project", input.at("project")}, {"graph", input.at("graph")}};
            const ApiResult gate = take(co_await zetta(context, customer, "gate", false, target));
            if (!gate.ok) {
                co_return gate;
            }
            if (gate.value.value("block_deploy", false)) {
                co_return ApiResult::success({{"gate", gate.value}});
            }
            json run = target;
            for (const char* key : {"dataset_path", "holdout_fraction", "model_name"}) {
                if (input.contains(key)) {
                    run[key] = input.at(key);
                }
            }
            auto called = co_await call_workspace(context, customer, "run", false, run);
            if (called && !*called && called->error().code == "runner_required") {
                // A workspace methodology's pipeline runs in Lake's methodology runner (lake#655).
                const ApiResult queued = take(co_await dispatch_methodology_run(
                    context, customer, input.at("workspace_id"), called->error().details));
                if (!queued.ok) {
                    co_return queued;
                }
                co_return ApiResult::success({{"gate", gate.value}, {"run", queued.value}});
            }
            if (!called) {
                co_return ApiResult::failure("store_unavailable", called.error().message);
            }
            if (!*called) {
                co_return workspace_failure(called->error(), run);
            }
            co_return ApiResult::success({{"gate", gate.value}, {"run", **called}});
        }});
    registry.add(Capability{
        .name = "create_pipeline",
        .title = "Create a pipeline",
        .description = "Create a pipeline in a project: `from` empty (one connect step to build "
                       "on), n8n (`content` is an n8n workflow export) or python (`content` is a "
                       "simulated-Python pipeline Zetta lowers without running it). The new "
                       "pipeline is proposed and must be approved before it runs.",
        .input_schema = customer_input_schema(
            {{"project", {{"type", "string"}}},
             {"name", {{"type", "string"}}},
             {"from", {{"type", "string"}, {"enum", json::array({"empty", "n8n", "python"})}}},
             {"content", {{"type", "string"}}}},
            json::array({"project", "from"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            const std::string customer = input.at("customer_id");
            const std::string project = input.at("project");
            const std::string from = input.at("from");
            const std::string name = input.value("name", std::string());
            set_audit(context,
                      "pipeline.create",
                      input,
                      from == "empty" ? "save_graph" : "compile_" + from);
            if (from == "empty") {
                if (name.empty()) {
                    co_return ApiResult::failure(
                        "invalid_input", "an empty pipeline needs a name", "/name");
                }
                co_return take(co_await zetta(
                    context,
                    customer,
                    "save_graph",
                    false,
                    {{"project", project},
                     {"graph", name},
                     {"nodes",
                      json::array(
                          {{{"id", "connect-1"}, {"kind", "connect"}, {"name", "Source"}}})},
                     {"edges", json::array()}}));
            }
            const std::string content = input.value("content", std::string());
            if (content.empty()) {
                co_return ApiResult::failure("invalid_input", "content is required", "/content");
            }
            if (from == "n8n") {
                const ApiResult imported =
                    take(co_await zetta(context,
                                        customer,
                                        "import_n8n",
                                        false,
                                        {{"project", project}, {"content", content}}));
                if (!imported.ok) {
                    co_return imported;
                }
                json compile{{"project", project},
                             {"input_id", imported.value.value("input_id", std::string())}};
                if (!name.empty()) {
                    compile["name"] = name;
                }
                co_return take(co_await zetta(context, customer, "compile_n8n", false, compile));
            }
            json compile{{"project", project}, {"source", content}};
            if (!name.empty()) {
                compile["name"] = name;
            }
            co_return take(co_await zetta(context, customer, "compile_python", false, compile));
        }});
    registry.add(Capability{
        .name = "list_service_kinds",
        .title = "List service kinds",
        .description = "Every kind of service a customer can create: key, label, how long a new "
                       "one usually takes to become ready, and whether it can mount the "
                       "workspace's storage (`mounts_stores`).",
        .input_schema = {{"type", "object"},
                         {"properties", json::object()},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "principal",
        .action = "read",
        .resolve_object = [](const json&,
                             ApiContext& context) -> std::expected<std::string, ApiError> {
            return "principal/" + context.acting_identity;
        },
        // The catalog is the same for everyone signed in; nothing to filter per caller.
        .authorizes_candidates = true,
        .handler =
            [](const json&, ApiContext&) {
                json kinds = json::array();
                for (const control::ServiceKind& kind : control::service_kinds) {
                    kinds.push_back(
                        {{"kind", std::string(kind.key)},
                         {"label", std::string(kind.label)},
                         {"ready_seconds", kind.ready_seconds},
                         {"mounts_stores", control::service_kind_mounts_stores(kind.key)}});
                }
                return ApiResult::success({{"kinds", std::move(kinds)}});
            }});
}

// Empty when `statement` is one read-only query: a SELECT or WITH, with at most a trailing
// semicolon. Otherwise why not. A coarse guard in front of Zetta's own SQL, which also runs DDL and
// administration; the workspace is the customer's own cluster, so reading its data is fine.
std::string read_only_statement_problem(std::string_view statement) {
    std::string text(statement);
    while (!text.empty() &&
           (std::isspace(static_cast<unsigned char>(text.back())) != 0 || text.back() == ';')) {
        text.pop_back();
    }
    std::size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
    }
    std::string head;
    for (std::size_t index = start;
         index < text.size() && std::isalpha(static_cast<unsigned char>(text[index])) != 0;
         ++index) {
        head += static_cast<char>(std::tolower(static_cast<unsigned char>(text[index])));
    }
    if (head != "select" && head != "with") {
        // Name the word it starts with, so a typo (`SELEC 1`) reads as one (lake#450); the
        // workspace's own parse error, with its position, applies once the statement is a read.
        std::string first;
        for (std::size_t index = start; index < text.size() && first.size() < 32 &&
                                        std::isspace(static_cast<unsigned char>(text[index])) == 0;
             ++index) {
            first += text[index];
        }
        if (first.empty()) {
            return "the statement is empty: write one SELECT or WITH query";
        }
        return "the statement must start with SELECT or WITH (only reads run here); it starts "
               "with '" +
               first + "'";
    }
    // Read the text the way a SQL parser does, and refuse anything two parsers could split
    // differently (LSR-017, lake#926): a comment (`--`, `/*`), a dollar quote, a backslash inside
    // a string, or a string or quoted name left open. Only then is a `;` outside them a second
    // statement, refused. The engine sees exactly the text checked here.
    enum class State { plain, string, name };
    State state = State::plain;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        const char next = index + 1 < text.size() ? text[index + 1] : '\0';
        switch (state) {
        case State::plain:
            if (character == '\'') {
                state = State::string;
            } else if (character == '"') {
                state = State::name;
            } else if ((character == '-' && next == '-') || (character == '/' && next == '*')) {
                return "comments are not accepted here: send the query alone";
            } else if (character == '$' &&
                       (next == '$' || std::isalpha(static_cast<unsigned char>(next)) != 0 ||
                        next == '_')) {
                return "dollar-quoted text is not accepted here: use a single-quoted string";
            } else if (character == ';') {
                return "run one statement at a time";
            }
            break;
        case State::string:
            if (character == '\\') {
                return "a backslash inside a string is not accepted here";
            }
            if (character == '\'') {
                if (next == '\'') {
                    ++index; // '' is a quote inside the string
                } else {
                    state = State::plain;
                }
            }
            break;
        case State::name:
            if (character == '"') {
                if (next == '"') {
                    ++index;
                } else {
                    state = State::plain;
                }
            }
            break;
        }
    }
    if (state != State::plain) {
        return "a string or quoted name is not closed";
    }
    return {};
}

// Train → Data → Query (Zetta parity phase 2, lake#181): read-only SQL over the workspace's data.
void register_sql_capability(Registry& registry) {
    registry.add(Capability{
        .name = "workspace_sql",
        .title = "Query the workspace",
        .description = "Run one read-only SQL query over the customer's workspace data and answer "
                       "{columns, rows}; `columns` is always filled, even for no rows. The "
                       "workspace speaks a SQL subset: SELECT <columns or *> FROM "
                       "<database>.<table> [WHERE <column> <op> <value>] [GROUP BY <column>] "
                       "[ORDER BY <column> [DESC], ...] [LIMIT n] [OFFSET n]; one WHERE "
                       "comparison; aggregates COUNT(*), COUNT(col), SUM, AVG, MIN and MAX with "
                       "or without GROUP BY (without it the whole table is one group and the "
                       "answer is one row); GROUP BY on any column, text included; no joins, "
                       "expressions or subqueries. A column no row carries is an error naming "
                       "the table's columns, and a parse error gives the position where it "
                       "stopped. list_datasets (through workspace_query) names the tables. "
                       "Anything but a read is refused; the workspace's administrative SQL is not "
                       "reachable. A long statement can be sent as POST with a JSON body. When "
                       "the database's safe mode warns (Warn All warns on every read), the call "
                       "answers safe_mode_warning with the statement and a confirmation; the same "
                       "call with `safe_mode_confirmation` runs it.",
        .input_schema = customer_input_schema(
            {{"statement", {{"type", "string"}}},
             {"database", {{"type", "string"}}},
             {"safe_mode_confirmation", safe_mode_confirmation_schema()},
             {"intent",
              {{"type", "string"},
               {"description", "why the query runs, recorded on the engine's trace"}}}},
            json::array({"statement"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        // A statement of any length travels as a JSON body (lake#594), as the engine's own sql
        // takes it; GET with a query string still works.
        .post_alias = true,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            const std::string statement = input.at("statement");
            if (const std::string problem = read_only_statement_problem(statement);
                !problem.empty()) {
                co_return ApiResult::failure("invalid_input", problem, "/statement");
            }
            set_audit(context, "workspace.sql", input, "sql");
            // The workspace Zetta route caps what an App reads (lake#1229); a direct call has no
            // cap. Refused before it runs, or run with LIMIT cap+1 to see a larger answer.
            const std::size_t cap = context.sql_row_cap;
            std::string sent = statement;
            if (cap > 0) {
                auto capped = cap_sql_statement(statement, cap);
                if (!capped) {
                    context.audit_details["row_cap"] = cap;
                    co_return ApiResult::failure("too_many_rows", capped.error(), "/statement");
                }
                sent = std::move(*capped);
                context.audit_details["row_cap"] = cap;
            }
            json query{{"statement", sent}};
            if (const std::string database = input.value("database", std::string());
                !database.empty()) {
                query["database"] = database;
            }
            if (const std::string confirmation =
                    input.value("safe_mode_confirmation", std::string());
                !confirmation.empty()) {
                query["safe_mode_confirmation"] = confirmation;
                context.audit_details["safe_mode_confirmed"] = true;
            }
            // The engine takes every call's `intent` (app-runtime sends one on each read) and
            // records it on the call's trace.
            if (const std::string intent = input.value("intent", std::string()); !intent.empty()) {
                query["intent"] = intent;
            }
            ApiResult answered =
                take(co_await zetta(context, input.at("customer_id"), "sql", true, query));
            if (cap > 0 && answered.ok && answered.value.is_object() &&
                answered.value.value("rows", json::array()).is_array() &&
                answered.value.value("rows", json::array()).size() > cap) {
                co_return ApiResult::failure(
                    "too_many_rows", too_many_rows_message(cap), "/statement");
            }
            co_return answered;
        }});
}

// Application Agent reads the workspace catalogue as a first-class Lake tool. The ordinary
// workspace_query surface remains available to every other caller.
void register_dataset_catalog_capability(Registry& registry) {
    registry.add(Capability{
        .name = "list_datasets",
        .title = "List workspace datasets",
        .description = "List the datasets in a customer's workspace. Each carries `id` "
                       "(<database>/<name>, the form get_dataset takes) and `kind`: `data` for "
                       "data a person loaded, `result` for tables the engine wrote as a run's "
                       "output.",
        .input_schema = customer_input_schema(json::object(), json::array()),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .authorizes_candidates = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            // A list authorizes its own candidates: the account's, read as any other workspace
            // read is (lake#650 found this one answering any signed-in person).
            if (auto refused = refuse_unless_reader(input, context)) {
                co_return std::move(*refused);
            }
            if (!context.workspaces) {
                co_return unavailable();
            }
            auto called = co_await call_workspace(
                context,
                input.at("customer_id"), "list_datasets", true, json::object());
            if (!called) {
                co_return ApiResult::failure("store_unavailable", called.error().message);
            }
            if (!*called) {
                co_return ApiResult::failure(called->error().code, called->error().message);
            }
            json value = std::move(**called);
            datasets::annotate_list(value);
            set_audit(context, "workspace.query", input, "list_datasets");
            co_return ApiResult::success(std::move(value));
        }});
}

namespace {

// The workspace's answer to one Zetta call, or the failure as an ApiResult. Not std::expected:
// a liquid task already wraps its value in one, and nesting them makes every co_return ambiguous.
struct ZettaAnswer {
    json value;
    std::optional<ApiResult> failure;

    explicit operator bool() const {
        return !failure;
    }
    const json* operator->() const {
        return &value;
    }
    json& operator*() {
        return value;
    }
    ApiResult error() const {
        return *failure;
    }
};

liquid::runtime::task<ZettaAnswer>
zetta_call(ApiContext& context, std::string customer_id, std::string capability, json input) {
    auto called = co_await call_workspace(
        context,
        customer_id, capability, workspace_uses_get(capability), std::move(input));
    if (!called) {
        co_return ZettaAnswer{json(),
                              ApiResult::failure("store_unavailable", called.error().message)};
    }
    if (!*called) {
        co_return ZettaAnswer{json(),
                              ApiResult::failure(called->error().code, called->error().message)};
    }
    co_return ZettaAnswer{std::move(**called), std::nullopt};
}

ZettaAnswer answered(std::expected<ZettaAnswer, liquid::error> completed) {
    if (!completed) {
        return ZettaAnswer{json(), ApiResult::failure("store_error", completed.error().message)};
    }
    return std::move(*completed);
}

// A launch as callers see it. Until the Orama operator reports, `phase` and `message` say why the
// run is waiting, and a run it never started reads Failed with the reason (lake#442);
// `operator_phase` is what the operator itself wrote, empty until it reports.
json launch_json(const control::AgentLaunch& launch) {
    const control::AgentLaunchProgress progress = control::agent_launch_progress(
        launch, std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    return {{"id", launch.id},
            {"customer_id", launch.customer_id},
            {"workspace_id", launch.workspace_id},
            {"agent_id", launch.agent_id},
            {"agent_name", launch.agent_name},
            {"mode", launch.mode},
            {"trigger", launch.trigger},
            {"input", launch.input},
            {"slug", launch.slug},
            {"desired", launch.desired},
            {"phase", progress.phase},
            {"message", progress.message},
            {"operator_phase", launch.phase},
            {"stop_reason", launch.stop_reason},
            {"exit_code", launch.exit_code ? json(*launch.exit_code) : json(nullptr)},
            {"result", launch.result},
            {"created_at", launch.created_at},
            {"created_by_taap_id", launch.created_by_taap_id}};
}

// A background turn as callers see it (lake#269). A finished Project Agent proposal names the
// pipeline page where it is reviewed and applied.
json task_json(const control::AgentTask& task) {
    const std::string status =
        task.phase.empty() ? std::string("pending") : [&] {
            std::string lower = task.phase;
            std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return lower;
        }();
    json out{{"task_id", task.id},
             {"workspace_id", task.workspace_id},
             {"agent", task.agent},
             {"kind", task.kind},
             {"status", status},
             {"message", task.message},
             {"attempts", task.attempts},
             {"lease_until", task.lease_until},
             {"lease_seconds", task.lease_seconds},
             {"created_at", task.created_at},
             {"finished_at", task.finished_at},
             {"answer", task.result}};
    if (!task.conversation_id.empty()) {
        out["conversation_id"] = task.conversation_id;
    }
    if (task.kind.starts_with("solution:") && task.body.is_object()) {
        out["context"] = task.body.value("context", json::object());
    }
    const json proposal = task.result.is_object()
                              ? task.result.value("result", json::object()).value("proposal", json())
                              : json();
    if (task.agent == "agent-project" && proposal.is_object()) {
        out["review_url"] = "/design/pipeline?customer_id=" + task.customer_id + "&project=" +
                            oauth_url_encode(proposal.value("project", std::string())) +
                            "&graph=" + oauth_url_encode(proposal.value("graph", std::string())) +
                            "&task=" + task.id;
    }
    const json app_proposal = task.result.is_object()
                                  ? task.result.value("result", json::object())
                                  : json::object();
    if (task.agent == "agent-application" &&
        app_proposal.value("kind", std::string()) == "proposal" &&
        app_proposal.value("confirm", false)) {
        // A repair or change is reviewed on its app's page (lake#1207).
        const json proposed_app = app_proposal.value("app", json::object());
        const std::string app_id =
            proposed_app.is_object() ? proposed_app.value("id", std::string()) : std::string();
        out["review_url"] =
            (task.kind == "repair" || task.kind == "change") && !app_id.empty()
                ? "/ship/app?customer_id=" + task.customer_id + "&app_id=" +
                      oauth_url_encode(app_id) + "&task=" + task.id + "#proposal"
                : "/ship/apps?customer_id=" + task.customer_id + "&task=" + task.id + "#proposal";
        // What agent_turn's confirmation names: the proposal as lake stored it (lake#561).
        out["proposal_sha256"] = agent_proposal_digest(app_proposal);
    }
    if (task.agent == "agent-experiments" && task.kind == "experiment" &&
        app_proposal.value("kind", std::string()) == "proposal" &&
        app_proposal.value("confirm", false)) {
        out["review_url"] = "/train/experiments?customer_id=" + task.customer_id +
                            "&task=" + task.id + "#experiment-proposal";
        out["proposal_sha256"] = agent_proposal_digest(app_proposal);
    }
    return out;
}

// The turns of an agent conversation before and including `latest`, newest first, by following each
// turn's `prior_turn` (lake#1240): agent_turn records the conversation's previous turn of the same
// agent and kind in a turn's request, so the chain is the conversation, with no new index. Each
// turn is read with its content from its workspace. The walk stops at `limit` turns, at a turn of
// another person (agent_turn refuses to continue someone else's conversation, so their turns are
// not part of this one), and at a turn it cannot read. The earlier turns an Application Agent
// repair or change turn is handed, and get_app_conversation lists at most.
constexpr std::size_t application_history_turns = 20;
constexpr std::size_t app_conversation_turns = 50;

liquid::runtime::task<std::vector<control::AgentTask>>
conversation_chain(const ApiContext& context, control::AgentTask latest, std::size_t limit) {
    std::vector<control::AgentTask> turns;
    const std::string person = latest.requested_by_user_id;
    std::set<std::string> seen;
    std::optional<control::AgentTask> current = std::move(latest);
    while (current && turns.size() < limit && seen.insert(current->id).second) {
        const json prior =
            current->body.is_object() ? current->body.value("prior_turn", json()) : json();
        const std::string prior_id =
            prior.is_object() ? prior.value("task_id", std::string()) : std::string();
        const control::AgentTask turn = *current;
        turns.push_back(turn);
        current.reset();
        if (prior_id.empty() || context.store == nullptr) {
            break;
        }
        auto found = context.store->get_agent_task(prior_id);
        if (!found || found->customer_id != turn.customer_id ||
            found->workspace_id != turn.workspace_id ||
            found->conversation_id != turn.conversation_id || found->agent != turn.agent ||
            found->requested_by_user_id != person) {
            break;
        }
        if (auto loaded = control::settled(co_await load_agent_task_content(context, *found));
            !loaded || !*loaded) {
            break;
        }
        current = std::move(*found);
    }
    co_return turns;
}

// What the Application Agent is handed of a repair or change conversation's earlier turns, oldest
// first (lake#1240): each turn's instruction, the agent's reply, what it answered (a proposal, a
// question, an applied change) and the diff it proposed, cut to keep a turn's request bounded.
json application_history(const std::vector<control::AgentTask>& newest_first) {
    constexpr std::size_t diff_limit = 8000;
    json history = json::array();
    for (auto turn = newest_first.rbegin(); turn != newest_first.rend(); ++turn) {
        const json answer = turn->result.is_object() ? turn->result : json::object();
        const json result = answer.value("result", json::object());
        std::string reply = answer.value("reply", std::string());
        if (reply.empty() && result.is_object()) {
            reply = result.value("reply", std::string());
        }
        json item{
            {"task_id", turn->id},
            {"message",
             turn->body.is_object() ? turn->body.value("message", std::string()) : std::string()},
            {"reply", reply},
            {"status", turn->phase.empty() ? std::string("Pending") : turn->phase}};
        if (result.is_object()) {
            item["outcome"] = result.value("kind", std::string());
            if (const std::string diff = result.value("diff", std::string()); !diff.empty()) {
                item["diff"] = diff.size() > diff_limit ? diff.substr(0, diff_limit) + "\n…" : diff;
            }
        }
        if (turn->body.is_object() && turn->body.value("context", json::object()).is_object() &&
            turn->body.value("context", json::object()).contains("confirmed_proposal")) {
            item["confirmed"] = true;
        }
        history.push_back(std::move(item));
    }
    return history;
}

// The running turn whose task credential is calling, or nullopt (lake#451). When Ask Orama (or
// any library agent) delegates with agent_turn, this is the turn that asked.
std::optional<control::AgentTask> calling_turn(const ApiContext& context,
                                               const std::string& customer_id,
                                               const std::string& workspace_id) {
    if (context.store == nullptr || context.credential_kind != "agent_task" ||
        !context.credential_session_id.starts_with("task:")) {
        return std::nullopt;
    }
    auto task = context.store->get_agent_task(context.credential_session_id.substr(5));
    if (!task || task->phase != "Running" || task->customer_id != customer_id ||
        task->workspace_id != workspace_id) {
        return std::nullopt;
    }
    return std::move(*task);
}

// A delegated turn reported on the turn that asked for it (lake#451): the asking task's message
// becomes `message`, and the same progress event goes to the workspace's agent-events, so a page
// following the asking turn says which agent is working and what it last reported. Best effort:
// a parent that already finished, or an unavailable workspace, leaves the delegated turn as it is.
liquid::runtime::task<void> report_on_asking_turn(ApiContext& context,
                                                  const std::string& parent_id,
                                                  std::string message,
                                                  json details) {
    if (context.store == nullptr || parent_id.empty()) {
        co_return {};
    }
    const auto parent = context.store->get_agent_task(parent_id);
    if (!parent || parent->phase != "Running") {
        co_return {};
    }
    if (message.size() > 300) {
        message = message.substr(0, 297) + "...";
    }
    const auto applied = context.store->record_agent_task_event(parent->id, parent->agent, "Running");
    if (!applied || !context.workspaces) {
        co_return {};
    }
    // The report is the asking turn's message, kept in its workspace with its request (lake#375).
    static_cast<void>(control::settled(
        co_await write_agent_task_event(context, *applied, "Running", message, nullptr)));
    WorkspaceApiBus bus(context, parent->customer_id, parent->workspace_id);
    if (!control::settled(co_await bus.create_topic(std::string(control::agent_events_topic)))) {
        co_return {};
    }
    json event{{"task_id", parent->id},
               {"agent", parent->agent},
               {"type", "progress"},
               {"message", message},
               {"attempts", applied->attempts}};
    event.update(details);
    static_cast<void>(control::settled(
        co_await bus.publish(std::string(control::agent_events_topic), event, parent->id)));
    // A task<void> carries a result<void>, so its promise has return_value and no return_void:
    // flowing off the end leaves the result unset and the awaiting turn reads (and destroys)
    // uninitialized memory (lake#476).
    co_return {};
}

// A finished Discover scan's Opportunities become records, once (lake#269), in the workspace Zetta
// of the workspace the scan ran in (lake#376): what Auto-Suggest Agent suggested (new) and what it
// declined (not_yet), in its order. A task that names no workspace has nowhere to keep them.
liquid::runtime::task<ApiResult> settle_discover(ApiContext& context, control::AgentTask task) {
    if (task.kind != "discover" || task.phase != "Succeeded" || !task.result.is_object() ||
        task.workspace_id.empty()) {
        co_return ApiResult::success(json::object());
    }
    const json profile = task.result.value("result", json::object())
                             .value("project_profile", json::object());
    std::vector<control::Opportunity> rows;
    int rank = 0;
    for (const json& item : profile.value("suggested", json::array())) {
        rows.push_back({.customer_id = task.customer_id,
                        .task_id = task.id,
                        .rank = ++rank,
                        .intent = item.value("intent", std::string()),
                        .solution = item.value("solution", std::string()),
                        .methodology = item.value("methodology", std::string()),
                        .why = item.value("why_this_data_supports_it", std::string()),
                        .payload = item,
                        .status = "new"});
    }
    for (const json& item : profile.value("declined", json::array())) {
        rows.push_back({.customer_id = task.customer_id,
                        .task_id = task.id,
                        .rank = ++rank,
                        .intent = item.value("intent", std::string()),
                        .why = item.value("reason", std::string()),
                        .payload = item,
                        .status = "not_yet"});
    }
    // An item without an intent is not an Opportunity anyone can act on.
    std::erase_if(rows, [](const control::Opportunity& row) { return row.intent.empty(); });
    if (rows.empty()) {
        co_return ApiResult::success(json::object());
    }
    auto records = workspace_records(context, task.customer_id, task.workspace_id);
    if (!records) {
        co_return ApiResult::failure(records.error().code, records.error().message,
                                     records.error().path);
    }
    auto written =
        control::settled(co_await control::record_opportunities(*records, std::move(rows)));
    if (!written) {
        co_return ApiResult::failure(written.error().code, written.error().message,
                                     written.error().path);
    }
    co_return ApiResult::success({{"written", *written}});
}

// A Discover scan that has not finished: pending (not claimed yet) or running (lake#1195).
bool scan_running(const control::AgentTask& task) {
    return task.phase != "Succeeded" && task.phase != "Failed";
}

// The workspace's newest Discover scan as list_opportunities reports it (lake#1195): enough for a
// page opened on any tab or device to say "Scanning… started 2 min ago" or when the last scan
// finished, without the scan's task id. `started_at` is when the scan was asked for.
json last_scan_json(const control::AgentTask& task) {
    const json shown = task_json(task);
    return {{"task_id", task.id},
            {"status", shown.at("status")},
            {"started_at", task.created_at},
            {"finished_at", task.finished_at.empty() ? json(nullptr) : json(task.finished_at)},
            {"message", task.message}};
}

json opportunity_json(const control::Opportunity& row) {
    return {{"id", row.id},
            {"task_id", row.task_id},
            {"rank", row.rank},
            {"intent", row.intent},
            {"solution", row.solution},
            {"methodology", row.methodology},
            {"why", row.why},
            {"status", row.status},
            {"project", row.project},
            {"created_at", row.created_at},
            {"detail", row.payload}};
}

json schedule_json(const control::AgentCronSchedule& schedule) {
    // The phase and message in lake's words, never the operator's or Kubernetes's (lake#586).
    const control::AgentScheduleProgress progress = control::agent_schedule_progress(schedule);
    return {{"id", schedule.id},
            {"customer_id", schedule.customer_id},
            {"workspace_id", schedule.workspace_id},
            {"agent_id", schedule.agent_id},
            {"agent_name", schedule.agent_name},
            {"cron", schedule.cron},
            {"time_zone", schedule.time_zone},
            {"description", schedule.description},
            {"input", schedule.input},
            {"slug", schedule.slug},
            {"desired", schedule.desired},
            {"phase", progress.phase},
            {"message", progress.message},
            {"last_schedule_at", schedule.last_schedule_at},
            {"created_at", schedule.created_at},
            {"created_by_taap_id", schedule.created_by_taap_id}};
}

// Read on the customer, for the list_ capabilities, which authorize themselves.
std::optional<ApiResult> authorize_customer_read(const json& input, ApiContext& context) {
    const auto object = customer_object(input, context);
    if (!object) {
        return ApiResult::failure(object.error().code, object.error().message, object.error().path);
    }
    if (context.acting_identity == "admin") {
        return std::nullopt;
    }
    const auto decision = control::authorize(
        *context.store, context.acting_identity, *object, "read", context.authorization_cache);
    if (!decision) {
        return ApiResult::failure(decision.error().code, decision.error().message);
    }
    if (!decision->allowed) {
        return ApiResult::failure("unauthorized", "principal is not allowed to read the customer");
    }
    return std::nullopt;
}

void agent_audit(ApiContext& context,
                 std::string kind,
                 const std::string& customer_id,
                 json details) {
    context.audit_kind = std::move(kind);
    context.audit_details = std::move(details);
    context.audit_details["customer_id"] = customer_id;
}

// Another capability, called as the same person with its own authorization and audit (a copy of
// the context, so the outer call's audit record is left as it is).
liquid::runtime::task<ApiResult> call_capability(ApiContext& context, std::string name, json input) {
    if (context.registry == nullptr) {
        co_return ApiResult::failure("unavailable", "no registry to call " + name);
    }
    ApiContext inner = context;
    const auto capability = context.registry->find(name);
    if (capability && capability->input_schema.value("properties", json::object()).contains("workspace_id") &&
        !input.contains("workspace_id")) {
        input["workspace_id"] = context.workspace_id;
    }
    inner.audit_kind.clear();
    inner.audit_details = json();
    auto done = co_await context.registry->call_async(name, input, inner);
    co_return done ? std::move(*done) : ApiResult::failure("store_error", done.error().message);
}

ApiResult called(std::expected<ApiResult, liquid::error> completed) {
    return completed ? std::move(*completed)
                     : ApiResult::failure("store_error", completed.error().message);
}

// The kind of an Application Agent turn: `repair` (fix a failed app) and `change` (change an app
// from instructions) name an existing app (lake#1207); everything else builds a new one (`app`).
std::string application_turn_kind(const json& input) {
    const std::string kind = input.value("kind", std::string("app"));
    return kind == "repair" || kind == "change" ? kind : std::string("app");
}

// A confirmed proposal hands an Application Agent or Experiments Agent turn its write tools, so lake
// checks it here rather than trusting the caller's copy (lake#561): it must be the proposal in the
// answer of `proposal_task_id`, a finished turn of the same agent and kind that the caller may
// follow, and `proposal_sha256` must be that proposal's digest (get_agent_task returns it), so a
// proposal that changed since the person saw it is refused. Experiments Agent's `study` (the
// experiment) is the one part the person may edit; everything else is the stored proposal. A public
// app also needs `context.confirm_public`. Answers the proposal the turn carries.
liquid::runtime::task<ApiResult>
verified_confirmed_proposal(const json& input, ApiContext& context, std::string_view agent) {
    // A confirmation is a person's (LSR-183, lake#989): an agent's credential can never confirm
    // a proposal, however it got the proposal's id and digest.
    if (context.credential_kind.starts_with("agent_")) {
        co_return ApiResult::failure(
            "unauthorized", "a proposal is confirmed by a person, not an agent", "/context");
    }
    const bool experiments = agent == "agent-experiments";
    // An Application Agent proposal is confirmed by a turn of the kind that proposed it: app,
    // repair or change (lake#1207).
    const std::string expected_kind =
        experiments ? std::string("experiment") : application_turn_kind(input);
    const std::string who = control::agent_display_name(agent);
    const json context_input = input.value("context", json::object());
    const json offered = context_input.value("confirmed_proposal", json());
    const std::string task_id = input.value("proposal_task_id", std::string());
    if (task_id.empty()) {
        co_return ApiResult::failure("invalid_input",
                                     "proposal_task_id: name the turn whose proposal you confirm",
                                     "/proposal_task_id");
    }
    const ApiResult got =
        called(co_await call_capability(context,
                                        "get_agent_task",
                                        {{"customer_id", input.at("customer_id")},
                                         {"workspace_id", input.at("workspace_id")},
                                         {"task_id", task_id}}));
    if (!got.ok) {
        co_return got;
    }
    const json& task = got.value;
    const json answer = task.value("answer", json::object());
    json proposal = answer.is_object() ? answer.value("result", json::object()) : json::object();
    if (!proposal.is_object()) {
        proposal = json::object();
    }
    const bool shaped = experiments ? proposal.value("study", json()).is_object()
                                    : proposal.value("app", json()).is_object() &&
                                          proposal.value("files", json()).is_array();
    if (task.value("agent", std::string()) != agent ||
        task.value("kind", std::string()) != expected_kind ||
        task.value("status", std::string()) != "succeeded" ||
        proposal.value("kind", std::string()) != "proposal" || !proposal.value("confirm", false) ||
        !shaped) {
        co_return ApiResult::failure("invalid_input",
                                     "That task has no " + who + " proposal to confirm.",
                                     "/proposal_task_id");
    }
    const std::string changed = "The " + who + " proposal changed before confirmation.";
    if (input.value("proposal_sha256", std::string()) != agent_proposal_digest(proposal)) {
        co_return ApiResult::failure("conflict", changed, "/proposal_sha256");
    }
    if (experiments) {
        const json study = offered.is_object() ? offered.value("study", json()) : json();
        if (!study.is_object()) {
            co_return ApiResult::failure("invalid_input",
                                         "The edited experiment proposal must be a JSON object.",
                                         "/context/confirmed_proposal/study");
        }
        json unedited = offered;
        unedited["study"] = proposal.at("study");
        if (unedited != proposal) {
            co_return ApiResult::failure("conflict", changed, "/context");
        }
        proposal["study"] = study;
        co_return ApiResult::success(std::move(proposal));
    }
    if (offered != proposal) {
        co_return ApiResult::failure("conflict", changed, "/context");
    }
    // A repair or change keeps the app's access as it is (its turn has no set_app_auth), so only a
    // new app's proposal asks about public access.
    if (expected_kind != "app") {
        co_return ApiResult::success(std::move(proposal));
    }
    // A public app needs the person's acknowledgement that anyone with its address can open it
    // (lake#452); create_app accepts the agent's public app only with it.
    const json app = proposal.at("app");
    const auto access = app.find("auth");
    const bool public_app =
        access != app.end() && access->is_string() && (*access == "none" || *access == "public");
    if (public_app && context_input.value("confirm_public", json(false)) != json(true)) {
        co_return ApiResult::failure(
            "invalid_input",
            "This app will be public. Tick the box to confirm that anyone with its address can "
            "open it.",
            "/context/confirm_public");
    }
    co_return ApiResult::success(std::move(proposal));
}

// A repair or change turn of the Application Agent (lake#1207): the app it names must be a live app
// of this workspace; the turn is handed the app and its repair record, and reads its builds and
// logs. It writes (commit_repository_files, build_app, deploy_app, and set_app_env for the APP_DEPS a
// changed manifest needs; never create_app or set_app_auth) only with a
// confirmed proposal for that same app, or, for repair, when the person turned auto-fix on and
// attempts remain, which this records as one attempt (an audit event of its own).
liquid::runtime::task<ApiResult> prepare_app_turn(const json& input,
                                                  ApiContext& context,
                                                  const std::string& kind,
                                                  json& handed,
                                                  std::vector<std::string>& scopes,
                                                  std::string& planned_task_id) {
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const std::string app_id = handed.value("app_id", std::string());
    if (app_id.empty()) {
        co_return ApiResult::failure(
            "invalid_input", "a " + kind + " turn names its app as context.app_id", "/context/app_id");
    }
    const auto app = context.store->get_web_app(app_id);
    if (!app || app->customer_id != customer_id || app->workspace_id != workspace_id ||
        app->desired == "deleted") {
        co_return ApiResult::failure("not_found", "no such app in this workspace", "/context/app_id");
    }
    auto records = workspace_records(context, customer_id, workspace_id);
    if (!records) {
        co_return ApiResult::failure(records.error().code, records.error().message, "/workspace_id");
    }
    auto repair = control::settled(co_await control::get_resource_repair(*records, customer_id, "app", app_id));
    if (!repair) {
        co_return ApiResult::failure(repair.error().code, repair.error().message);
    }
    handed["app"] = control::web_app_json(*app);
    handed["repair"] = control::resource_repair_json(*repair);
    scopes.insert(scopes.end(),
                  {"get_auto_fix", "list_builds", "get_build", "build_logs",
                   "receive_build_events"});
    const bool confirming = handed.contains("confirmed_proposal");
    const bool auto_fix = handed.value("auto_fix", json(false)) == json(true);
    handed.erase("auto_fix");
    const auto grant_writes = [&scopes] {
        scopes.insert(scopes.end(),
                      {"commit_repository_files", "build_app", "deploy_app", "set_app_env"});
    };
    if (auto_fix && (confirming || kind != "repair")) {
        co_return ApiResult::failure(
            "invalid_input",
            "auto-fix is a repair turn without a confirmed proposal",
            "/context/auto_fix");
    }
    if (confirming) {
        const ApiResult confirmed =
            called(co_await verified_confirmed_proposal(input, context, "agent-application"));
        if (!confirmed.ok) {
            co_return confirmed;
        }
        const json proposed = confirmed.value.value("app", json::object());
        if (!proposed.is_object() || proposed.value("id", std::string()) != app_id) {
            co_return ApiResult::failure("invalid_input",
                                         "the confirmed proposal is for another app",
                                         "/context/confirmed_proposal/app/id");
        }
        handed["confirmed_proposal"] = confirmed.value;
        grant_writes();
        co_return ApiResult::success(json::object());
    }
    if (!auto_fix) {
        co_return ApiResult::success(json::object());
    }
    if (!repair->auto_fix) {
        co_return ApiResult::failure("invalid_input",
                                     "auto-fix is off for this app; turn it on with "
                                     "set_auto_fix",
                                     "/context/auto_fix");
    }
    if (repair->auto_fix_stopped || repair->attempts >= repair->max_attempts) {
        co_return ApiResult::failure(
            "conflict",
            "auto-fix has stopped for this app after " + std::to_string(repair->attempts) +
                " attempts; turn it on again to reset them",
            "/context/auto_fix");
    }
    planned_task_id = control::new_uuid_v4();
    const std::string task_id = planned_task_id;
    const std::int64_t generation = app->generation;
    auto counted = control::settled(co_await control::change_resource_repair(
        *records, customer_id, "app", app_id, [&](control::ResourceRepair& value) {
            value.attempts += 1;
            value.attempted_generations.push_back(generation);
            value.auto_fix_task_id = task_id;
            value.auto_fix_generation = generation;
        }));
    if (!counted) {
        co_return ApiResult::failure(counted.error().code, counted.error().message);
    }
    handed["auto_fix"] = {{"attempt", counted->attempts}, {"max_attempts", counted->max_attempts}};
    handed["repair"] = control::resource_repair_json(*counted);
    agent_audit(context,
                "app.auto_fix.attempt",
                customer_id,
                {{"agent", "agent-application"},
                 {"app_id", app_id},
                 {"generation", generation},
                 {"attempt", counted->attempts},
                 {"max_attempts", counted->max_attempts},
                 {"task_id", task_id}});
    context.audit_object = "app/" + app_id;
    grant_writes();
    co_return ApiResult::success(json::object());
}

std::string base64(std::string_view bytes) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const unsigned n = (static_cast<unsigned char>(bytes[i]) << 16) |
                           (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                           static_cast<unsigned char>(bytes[i + 2]);
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += alphabet[(n >> 6) & 63];
        out += alphabet[n & 63];
    }
    if (const std::size_t rest = bytes.size() - i; rest > 0) {
        unsigned n = static_cast<unsigned char>(bytes[i]) << 16;
        if (rest == 2) {
            n |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        }
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += rest == 2 ? alphabet[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

// A name for something made from free text: lower case, words joined by '-', at most `limit`.
std::string slug_of(std::string_view text, std::size_t limit) {
    std::string out;
    for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!out.empty() && out.back() != '-') {
            out += '-';
        }
        if (out.size() >= limit) {
            break;
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out;
}

// The files an Agent Creator Agent proposal carries, with instructions.md written from the agent when it
// is missing.
json proposal_files(const json& proposal) {
    const json agent = proposal.value("agent", json::object());
    json files = json::array();
    bool instructions = false;
    for (const json& file : proposal.value("files", json::array())) {
        instructions = instructions || file.value("path", std::string()) == "instructions.md";
        files.push_back({{"path", file.value("path", std::string())},
                         {"contents", file.value("contents", std::string())}});
    }
    if (!instructions) {
        std::string text = "# " + agent.value("name", std::string()) + "\n\n" +
                           agent.value("purpose", std::string()) + "\n";
        if (!agent.value("manifesto", std::string()).empty()) {
            text += "\n## How you work\n\n" + agent.value("manifesto", std::string()) + "\n";
        }
        if (!agent.value("goal", std::string()).empty()) {
            text += "\n## Your goal\n\n" + agent.value("goal", std::string()) + "\n";
        }
        files.push_back({{"path", "instructions.md"}, {"contents", text}});
    }
    return files;
}

} // namespace

// The agent capabilities that were page-only (UX review 2026-09-27): creating an agent from an
// approved Agent Creator Agent proposal, turning chosen Opportunities into projects, and reading an Ask
// Orama conversation back. The pages call these, so REST, MCP and the site do the same thing.
namespace {

// A design field as text: the model writes null (or a number) as readily as a string.
std::string design_text_field(const json& design, const char* key) {
    const auto found = design.find(key);
    return found != design.end() && found->is_string() ? found->get<std::string>() : std::string();
}

// The one table a Project Agent design trains on: `dataset`, else the first of
// `training_sources` or `datasets` (the agent's build output names it either way).
std::string design_dataset(const json& design) {
    if (const std::string dataset = design_text_field(design, "dataset"); !dataset.empty()) {
        return dataset;
    }
    for (const char* key : {"training_sources", "datasets", "sources"}) {
        const auto found = design.find(key);
        const json list = found != design.end() ? *found : json();
        if (list.is_array() && !list.empty() && list.front().is_string()) {
            return list.front().get<std::string>();
        }
    }
    return {};
}

// A design in words for Zetta's compiler: what it predicts, from which data, with which model.
std::string design_text(const std::string& brief, const json& design) {
    std::string text = brief;
    const std::string label = design_text_field(design, "label");
    if (!label.empty()) {
        text += " Predict " + label;
        if (const std::string dataset = design_dataset(design); !dataset.empty()) {
            text += " from " + dataset;
        }
        std::string features;
        const auto features_found = design.find("features");
        for (const json& feature : features_found != design.end() && features_found->is_array()
                                       ? *features_found
                                       : json::array()) {
            if (feature.is_string()) {
                features += (features.empty() ? "" : ", ") + feature.get<std::string>();
            }
        }
        if (!features.empty()) {
            text += " using " + features;
        }
        text += ".";
    }
    if (const std::string model = design_text_field(design, "model_kind"); !model.empty()) {
        text += " Train a " + model + " model";
        if (design.contains("holdout_fraction") && design["holdout_fraction"].is_number()) {
            text += " with a " +
                    std::to_string(static_cast<int>(design["holdout_fraction"].get<double>() * 100)) +
                    "% hold-out";
        }
        text += ".";
    }
    return text;
}

} // namespace

liquid::runtime::task<json> workspace_methodologies(ApiContext& context,
                                                    std::string customer_id) {
    const ApiResult listed = called(co_await call_capability(
        context,
        "workspace_query",
        {{"customer_id", customer_id}, {"capability", "list_methodologies"}, {"input", json::object()}}));
    if (!listed.ok || !listed.value.contains("methodologies") ||
        !listed.value["methodologies"].is_array()) {
        co_return json(nullptr);
    }
    co_return listed.value["methodologies"];
}

json find_methodology(const json& methodologies,
                      std::string_view methodology,
                      std::string_view solution) {
    // An id written anywhere in the text ("M-3", "M-3 Segmentation"), else the catalogue
    // solution ("S-5") the registry lists the methodology as serving.
    const auto token = [](std::string_view text, char prefix) {
        for (std::size_t at = 0; at + 2 < text.size(); ++at) {
            if (text[at] != prefix || text[at + 1] != '-') continue;
            std::size_t end = at + 2;
            while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) ++end;
            if (end > at + 2) return std::string(text.substr(at, end - at));
        }
        return std::string();
    };
    if (!methodologies.is_array()) return nullptr;
    // A workspace's own methodology (lake#655) is named by its whole id, W-<8 hex>.
    for (std::size_t at = methodology.find("W-"); at != std::string_view::npos;
         at = methodology.find("W-", at + 2)) {
        const std::string id(methodology.substr(at, 10));
        for (const json& entry : methodologies) {
            if (id.size() == 10 && entry.value("id", std::string()) == id)
                return entry;
        }
    }
    if (const std::string id = token(methodology, 'M'); !id.empty()) {
        for (const json& entry : methodologies) {
            if (entry.value("id", std::string()) == id) return entry;
        }
    }
    if (const std::string id = token(solution, 'S'); !id.empty()) {
        for (const json& entry : methodologies) {
            const json solutions = entry.value("solutions", json::array());
            if (solutions.is_array() &&
                std::ranges::find(solutions, json(id)) != solutions.end()) {
                return entry;
            }
        }
    }
    return nullptr;
}

std::string methodology_waits(const json& entry) {
    std::string text = entry.value("name", std::string("This methodology")) + " (" +
                       entry.value("id", std::string()) + ") does not run in this workspace's "
                       "engine yet";
    if (const std::string issue = entry.value("issue", std::string()); !issue.empty()) {
        text += " (" + issue + ")";
    }
    return text + ".";
}

liquid::runtime::task<ApiResult> point_pipeline(ApiContext& context,
                                                std::string customer_id,
                                                std::string project,
                                                std::string graph,
                                                std::string dataset,
                                                json model) {
    const ApiResult read = called(co_await call_capability(
        context,
        "workspace_query",
        {{"customer_id", customer_id},
         {"capability", "get_graph"},
         {"input", {{"project", project}, {"graph", graph}}}}));
    if (!read.ok) {
        co_return read;
    }
    const std::size_t dot = dataset.find('.');
    const std::string source = dot == std::string::npos ? std::string("datasets") : dataset.substr(0, dot);
    const std::string table = dot == std::string::npos ? dataset : dataset.substr(dot + 1);
    json edits = json::array();
    bool pointed = false, modelled = false;
    for (const json& node : read.value.value("nodes", json::array())) {
        const std::string id = node.value("id", std::string());
        const std::string kind = node.value("kind", std::string());
        if (kind == "connect" && !dataset.empty()) {
            if (!pointed) {
                // An engine given the dataset's hints (zetta#1246) already reads it and names the
                // step from the table; only an older engine's step is pointed and renamed here.
                const json attributes = node.value("attributes", json::object());
                const bool reads = attributes.is_object() &&
                                   attributes.value("source", json()) == json(source) &&
                                   attributes.value("table", json()) == json(table);
                if (!reads) {
                    edits.push_back({{"op", "set_attribute"},
                                     {"step", id},
                                     {"name", "source"},
                                     {"value", source}});
                    edits.push_back({{"op", "set_attribute"},
                                     {"step", id},
                                     {"name", "table"},
                                     {"value", table}});
                    edits.push_back(
                        {{"op", "rename_step"}, {"step", id}, {"name", "Load " + dataset}});
                }
                pointed = true;
            } else {
                edits.push_back({{"op", "remove_step"}, {"step", id}});
            }
        } else if (kind == "model" && !modelled && model.is_object()) {
            for (const auto& [name, value] : model.items()) {
                if (!value.is_null()) {
                    edits.push_back({{"op", "set_attribute"}, {"step", id}, {"name", name},
                                     {"value", value}});
                }
            }
            modelled = true;
        }
    }
    if (edits.empty()) {
        co_return ApiResult::success({{"graph", graph}});
    }
    const ApiResult edited = called(co_await call_capability(
        context,
        "edit_pipeline",
        {{"customer_id", customer_id}, {"project", project}, {"graph", graph}, {"edits", edits}}));
    if (!edited.ok) {
        co_return edited;
    }
    co_return ApiResult::success({{"graph", graph}});
}

// What the engine's compiler fits the template to (zetta#1246): the dataset's `source` and
// `table`, and the label and entity columns when they are known (`label_column`, else the model's
// `target`). Empty when nothing is known, so the template keeps its own names.
static json pipeline_hints(const std::string& dataset, const json& model, const json& columns) {
    json hints = json::object();
    if (!dataset.empty()) {
        const std::size_t dot = dataset.find('.');
        hints["source"] =
            dot == std::string::npos ? std::string("datasets") : dataset.substr(0, dot);
        hints["table"] = dot == std::string::npos ? dataset : dataset.substr(dot + 1);
    }
    const auto text_of = [](const json& object, const char* key) {
        const auto found = object.is_object() ? object.find(key) : object.end();
        return found != object.end() && found->is_string() ? found->get<std::string>()
                                                           : std::string();
    };
    std::string label = text_of(columns, "label_column");
    if (label.empty()) {
        label = text_of(model, "target");
    }
    if (!label.empty()) {
        hints["label_column"] = label;
    }
    if (const std::string entity = text_of(columns, "entity_column"); !entity.empty()) {
        hints["entity_column"] = entity;
    }
    return hints;
}

liquid::runtime::task<ApiResult> compile_pipeline(ApiContext& context,
                                                  std::string customer_id,
                                                  std::string project,
                                                  std::string intent,
                                                  std::string dataset,
                                                  json model,
                                                  std::string methodology,
                                                  json columns,
                                                  std::string name,
                                                  bool create_project) {
    const auto act = [&](std::string capability, json input) {
        return call_capability(context,
                               "workspace_act",
                               {{"customer_id", customer_id},
                                {"capability", std::move(capability)},
                                {"input", std::move(input)}});
    };
    // A new project's pipeline in one call (lake#1193): the project is created first, described by
    // the intent. A name already in use is refused before anything is written, so a confirmed
    // proposal never adds a pipeline to a project the person did not mean.
    if (create_project) {
        const ApiResult existing = called(co_await call_capability(context,
                                                                   "workspace_query",
                                                                   {{"customer_id", customer_id},
                                                                    {"capability", "get_project"},
                                                                    {"input", {{"name", project}}}}));
        if (existing.ok) {
            co_return ApiResult::failure("conflict",
                                         "a project named '" + project +
                                             "' already exists; leave out create_project to add "
                                             "the pipeline to it",
                                         "/create_project");
        }
        if (existing.error.code != "not_found") {
            co_return existing;
        }
        const ApiResult created = called(co_await act(
            "create_project", {{"name", project}, {"description", intent}}));
        if (!created.ok) {
            ApiResult refused = created;
            if (refused.error.path.starts_with("/input/")) {
                refused.error.path.erase(0, 6);
            }
            co_return refused;
        }
    }
    ApiResult result = called(co_await act("capture_intent", {{"project", project}, {"text", intent}}));
    if (result.ok) {
        // A short title rather than the engine's, which is the description's words with the
        // punctuation dropped (lake#816), numbered past a pipeline of the project that has it, as
        // a name the caller chose is. The description itself stays the project's captured intent.
        // A chosen name is trimmed; one that is only spaces is no name.
        const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
        while (!name.empty() && space(name.back())) {
            name.pop_back();
        }
        name.erase(0, std::ranges::find_if_not(name, space) - name.begin());
        const std::string title = name.empty() ? pipeline_title(intent) : name;
        name = title;
        const ApiResult read = called(co_await call_capability(context,
                                                               "workspace_query",
                                                               {{"customer_id", customer_id},
                                                                {"capability", "get_project"},
                                                                {"input", {{"name", project}}}}));
        const json graphs = read.ok ? read.value.value("graphs", json::array()) : json::array();
        const auto taken = [&graphs](const std::string& candidate) {
            return graphs.is_array() && std::ranges::any_of(graphs, [&](const json& graph) {
                       return graph.is_object() && graph.value("name", std::string()) == candidate;
                   });
        };
        for (int copy = 2; taken(name); ++copy) {
            name = title + " (" + std::to_string(copy) + ")";
        }
    }
    if (result.ok) {
        json input{{"project", project}, {"name", name}};
        if (const std::string input_id = result.value.value("input_id", std::string());
            !input_id.empty()) {
            input["input_id"] = input_id;
        }
        // The methodology's own pipeline (zetta#1061); an engine that cannot run it refuses
        // with not_supported, naming the issue, rather than compiling a skeleton.
        if (!methodology.empty()) {
            input["methodology"] = methodology;
        }
        // The chosen dataset's hints, so the steps are named from the data (zetta#1246). An
        // older engine ignores them and point_pipeline names the connect step below.
        if (json hints = pipeline_hints(dataset, model, columns); !hints.empty()) {
            input["hints"] = std::move(hints);
        }
        result = called(co_await act("compile_intent", std::move(input)));
    }
    if (!result.ok) {
        // The steps are workspace_act calls, but the caller sent `project` at the top level: the
        // path names that field, and a missing project says how to make one (lake#591).
        if (result.error.path.starts_with("/input/")) {
            result.error.path.erase(0, 6);
        }
        if (create_project) {
            result.error.message = "the project '" + project + "' was created, but its pipeline " +
                                   "was not: " + result.error.message;
        } else if (result.error.code == "not_found" && result.error.path == "/project") {
            result.error.message +=
                "; create the project first with workspace_act create_project {name, intent}, "
                "or call compile_pipeline with create_project";
        }
        co_return result;
    }
    const std::string graph = result.value.value("name", result.value.value("graph", std::string()));
    // The template its steps started from (zetta#1238), said so a person knows the step names
    // are the template's until they edit them. An older engine names none.
    const std::string template_name = result.value.value("template", std::string());
    const auto with_template = [&](ApiResult answer) {
        if (answer.ok && !template_name.empty()) {
            answer.value["template"] = template_name;
        }
        if (answer.ok && create_project) {
            answer.value["project_created"] = true;
        }
        return answer;
    };
    if (graph.empty() || (dataset.empty() && !model.is_object())) {
        co_return with_template(ApiResult::success({{"graph", graph}}));
    }
    co_return with_template(
        called(co_await point_pipeline(context, customer_id, project, graph, dataset, model)));
}

liquid::runtime::task<ApiResult> settle_build(ApiContext& context, control::AgentTask task) {
    if (!task.kind.starts_with("build:") || task.phase != "Succeeded" || !task.result.is_object()) {
        co_return ApiResult::success(json::object());
    }
    const std::string project = task.kind.substr(6);
    const json answer = task.result.value("result", json());
    const json design = answer.is_object() && answer.contains("pipeline") &&
                                answer["pipeline"].is_object()
                            ? answer["pipeline"]
                            : json::object();
    const std::string dataset = design_dataset(design);
    json model = json::object();
    if (const std::string label = design_text_field(design, "label"); !label.empty()) {
        model["target"] = label;
    }
    if (const std::string algo = design_text_field(design, "model_kind"); !algo.empty()) {
        model["algo"] = algo;
    }
    // The design's entity column, a hint to the compiler with the label (zetta#1246).
    const json design_columns{{"entity_column", design_text_field(design, "entity_column")}};
    // The methodology the design names, else the one of the Opportunity the build came from
    // (lake#289). An engine with the registry (zetta#1061) builds that methodology's own
    // pipeline; one it cannot run yet is refused here, never replaced by a skeleton.
    const json context_body =
        task.body.is_object() ? task.body.value("context", json::object()) : json::object();
    const json opportunity = context_body.is_object()
                                 ? context_body.value("opportunity", json::object())
                                 : json::object();
    // The pipeline is titled from what the project is for (lake#816): the Opportunity's intent,
    // else the brief's words after "Design the pipeline for the project <name>: ", never the
    // brief's own "Design the pipeline" words.
    std::string title;
    if (const std::string intent =
            opportunity.is_object() ? design_text_field(opportunity, "intent") : std::string();
        !intent.empty()) {
        title = pipeline_title(intent);
    } else if (const std::string brief = design_text_field(task.body, "message"),
               lead = "Design the pipeline for the project " + project + ": ";
               brief.starts_with(lead) && brief.size() > lead.size()) {
        title = pipeline_title(std::string_view(brief).substr(lead.size()));
    } else if (brief.starts_with("Design the pipeline")) {
        // A brief that says only "Design the pipeline ..." (an agent_turn a client started with
        // no description after it) is titled with the project's own name.
        title = pipeline_title(project);
    }
    std::string methodology;
    auto listed = co_await workspace_methodologies(context, task.customer_id);
    if (const json registry = listed ? *listed : json(nullptr); registry.is_array()) {
        json entry = find_methodology(registry, design_text_field(design, "methodology"),
                                      design_text_field(design, "solution"));
        if (entry.is_null() && opportunity.is_object()) {
            entry = find_methodology(registry, design_text_field(opportunity, "methodology"),
                                     design_text_field(opportunity, "solution"));
        }
        if (entry.is_object()) {
            if (!entry.value("runnable", false)) {
                co_return ApiResult::failure("not_supported", methodology_waits(entry));
            }
            methodology = entry.value("id", std::string());
        }
    }
    const ApiResult read = called(co_await call_capability(
        context,
        "workspace_query",
        {{"customer_id", task.customer_id},
         {"capability", "get_project"},
         {"input", {{"name", project}}}}));
    if (!read.ok) {
        co_return read;
    }
    // The Project Agent's compile check graphs are never the project's pipeline (lake#438).
    const json graphs = without_project_agent_checks(read.value.value("graphs", json::array()));
    if (graphs.empty()) {
        co_return called(
            co_await compile_pipeline(context,
                                      task.customer_id,
                                      project,
                                      design_text(design_text_field(task.body, "message"), design),
                                      dataset,
                                      model,
                                      methodology,
                                      design_columns,
                                      title));
    }
    // The project's own pipeline (Zetta compiles one from its intent when it is created):
    // the first one not yet approved. Applied once: a connect step that already reads the
    // design's table means it was.
    const auto open = std::ranges::find_if(graphs, [](const json& graph) {
        return design_text_field(graph, "approval") != "approved";
    });
    if (open == graphs.end() || dataset.empty()) {
        co_return ApiResult::success(json::object());
    }
    const std::string graph = design_text_field(*open, "name");
    // A pipeline of another methodology's shape (the churn skeleton a segmentation project got
    // before zetta#1061) is not repointed: the design's methodology gets its own pipeline.
    if (!methodology.empty() && design_text_field(*open, "methodology") != methodology) {
        co_return called(
            co_await compile_pipeline(context,
                                      task.customer_id,
                                      project,
                                      design_text(design_text_field(task.body, "message"), design),
                                      dataset,
                                      model,
                                      methodology,
                                      design_columns,
                                      title));
    }
    const ApiResult current = called(co_await call_capability(
        context,
        "workspace_query",
        {{"customer_id", task.customer_id},
         {"capability", "get_graph"},
         {"input", {{"project", project}, {"graph", graph}}}}));
    const std::string table = dataset.substr(dataset.find('.') == std::string::npos ? 0 : dataset.find('.') + 1);
    for (const json& node : current.ok ? current.value.value("nodes", json::array()) : json::array()) {
        if (design_text_field(node, "kind") == "connect" && node.contains("attributes") &&
            node["attributes"].is_object() && design_text_field(node["attributes"], "table") == table) {
            co_return ApiResult::success({{"graph", graph}});
        }
    }
    co_return called(co_await point_pipeline(context, task.customer_id, project, graph, dataset, model));
}

namespace {

std::pair<std::string, std::string> dataset_parts(std::string dataset) {
    const std::size_t dot = dataset.find('.');
    if (dot == std::string::npos) {
        return {"datasets", std::move(dataset)};
    }
    return {dataset.substr(0, dot), dataset.substr(dot + 1)};
}

json solution_answer(const control::AgentTask& task) {
    if (!task.result.is_object()) {
        return json::object();
    }
    const json answer = task.result.value("result", json::object());
    return answer.is_object() ? answer : json::object();
}

json solution_proposal(const control::AgentTask& task) {
    const json answer = solution_answer(task);
    const json proposed = answer.value("foundation", json::object());
    return proposed.is_object() ? proposed : json::object();
}

json task_solution_context(const control::AgentTask& task) {
    if (!task.body.is_object()) {
        return json::object();
    }
    const json value = task.body.value("context", json::object());
    return value.is_object() ? value : json::object();
}

std::expected<std::string, ApiError> task_customer_object(const json& input, ApiContext& context) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const auto task = context.store->get_agent_task(input.at("task_id"));
    if (!task) {
        return std::unexpected(ApiError{"not_found", "no such solution task", "/task_id"});
    }
    const auto object = context.store->find_policy_object("customer", task->customer_id);
    if (!object) {
        return std::unexpected(ApiError{object.error().code, object.error().message, "/task_id"});
    }
    return object->id;
}

json missing_solution_predicates(const std::string& solution, const json& proposal) {
    json missing = proposal.value("missing_predicates", json::array());
    if (!missing.is_array()) {
        missing = json::array();
    }
    if (solution != "S-1") {
        return missing;
    }
    const json predicates = proposal.value("predicates", json::object());
    for (const char* name : {"in_collections", "open_complaint", "vulnerable"}) {
        const bool named = predicates.is_object() && predicates.contains(name) &&
                           predicates.at(name).is_string() &&
                           !predicates.at(name).get_ref<const std::string&>().empty();
        if (!named && std::ranges::find(missing, json(name)) == missing.end()) {
            missing.push_back(name);
        }
    }
    return missing;
}

} // namespace

liquid::runtime::task<ApiResult>
settle_solution(ApiContext& context, control::AgentTask task, json edits) {
    if (!task.kind.starts_with("solution:")) {
        co_return ApiResult::failure("invalid_input", "this is not a solution proposal task");
    }
    if (task.phase != "Succeeded") {
        co_return ApiResult::failure("invalid_input", "the Project Agent has not finished");
    }
    json proposal = solution_proposal(task);
    if (proposal.empty()) {
        co_return ApiResult::failure("invalid_input",
                                     "the Project Agent returned no foundation proposal");
    }
    const json start = task_solution_context(task);
    const std::string project = task.kind.substr(9);
    const std::string solution = start.value("solution", std::string());
    const int level = start.value("level", 0);
    if (project.empty() || solution.empty() || level < 1 || level > 4) {
        co_return ApiResult::failure("invalid_input", "the solution task has invalid start inputs");
    }

    // Delivery and economics came from the person, not the agent. They form the proposal's base;
    // an explicit confirmation edit may change them.
    proposal["delivery"] = start.value("delivery", json::object());
    proposal["roi_model"] = start.value("roi_model", json::object());
    const auto [database, table] = dataset_parts(start.value("dataset", std::string()));
    json sources =
        proposal.value("sources", solution_answer(task).value("sources", json::object()));
    if (!sources.is_object()) {
        sources = json::object();
    }
    sources["source"] = database;
    sources["table"] = table;
    proposal["sources"] = std::move(sources);
    if (!edits.is_null()) {
        if (!edits.is_object()) {
            co_return ApiResult::failure("invalid_input", "edits must be an object", "/edits");
        }
        proposal.merge_patch(edits);
    }

    json source_roles = proposal.value("sources", json::object());
    if (!source_roles.is_object() || source_roles.value("source", std::string()).empty() ||
        source_roles.value("table", std::string()).empty()) {
        co_return ApiResult::failure("invalid_input",
                                     "the proposal must name its source database and table");
    }
    const json missing_predicates = missing_solution_predicates(solution, proposal);
    source_roles["policy_predicates"] = proposal.value("predicates", json::object());
    source_roles["missing_policy_predicates"] = missing_predicates;
    json foundation = json::object();
    for (const char* name : {"snapshot", "dataset", "delivery", "roi_model", "decision_source"}) {
        if (proposal.contains(name)) {
            foundation[name] = proposal.at(name);
        }
    }
    for (const char* required : {"snapshot", "dataset", "delivery", "roi_model"}) {
        if (!foundation.contains(required) || !foundation.at(required).is_object()) {
            co_return ApiResult::failure("invalid_input",
                                         "the proposal is missing " + std::string(required),
                                         "/edits/" + std::string(required));
        }
    }

    // Confirmation is idempotent. Zetta's default graph name is stable for a solution and level.
    const std::string graph_name = solution + " CL-" + std::to_string(level);
    const ApiResult project_state =
        called(co_await call_capability(context,
                                        "workspace_query",
                                        {{"customer_id", task.customer_id},
                                         {"capability", "get_project"},
                                         {"input", {{"name", project}}}}));
    if (project_state.ok) {
        const json graphs = project_state.value.value("graphs", json::array());
        const auto existing = std::ranges::find_if(graphs, [&](const json& graph) {
            return graph.value("name", std::string()) == graph_name;
        });
        if (existing != graphs.end()) {
            co_return ApiResult::success({{"project", project},
                                          {"solution", solution},
                                          {"level", level},
                                          {"graph", graph_name},
                                          {"already_confirmed", true},
                                          {"missing_predicates", missing_predicates},
                                          {"actions_blocked", !missing_predicates.empty()}});
        }
    }

    ApiResult created =
        called(co_await call_capability(context,
                                        "workspace_act",
                                        {{"customer_id", task.customer_id},
                                         {"capability", "create_solution"},
                                         {"input",
                                          {{"project", project},
                                           {"solution", solution},
                                           {"level", level},
                                           {"sources", source_roles},
                                           {"foundation", std::move(foundation)}}}}));
    if (!created.ok) {
        co_return created;
    }
    created.value["task_id"] = task.id;
    created.value["missing_predicates"] = missing_predicates;
    created.value["actions_blocked"] = !missing_predicates.empty();
    co_return created;
}

namespace {

// Defined with the conversation capabilities below; get_ask_conversation answers it (lake#942).
json ask_proposal_calls(const ApiContext& context,
                        const json& payload,
                        const std::string& customer_id,
                        const std::string& workspace_id);

} // namespace

void register_agent_flow_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "start_solution",
        .title = "Start a solution project",
        .description =
            "Create the named project and ask the Project Agent to inspect one chosen dataset and "
            "propose the complete solution foundation. Delivery and ROI inputs are the caller's "
            "own values and are never defaulted. The solution is not created until "
            "confirm_solution.",
        .input_schema = customer_input_schema(
            {{"project", {{"type", "string"}, {"minLength", 1}}},
             {"solution", {{"type", "string"}, {"minLength", 1}}},
             {"level", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4}}},
             {"dataset", {{"type", "string"}, {"minLength", 1}}},
             {"delivery",
              {{"type", "object"},
               {"required", {"kind", "endpoint", "owner"}},
               {"additionalProperties", false},
               {"properties",
                {{"kind", {{"type", "string"}, {"enum", {"webhook", "salesforce", "hubspot"}}}},
                 {"endpoint", {{"type", "string"}, {"minLength", 1}}},
                 {"owner", {{"type", "string"}, {"minLength", 1}}}}}}},
             {"roi_model",
              {{"type", "object"},
               {"required", {"value_per_outcome", "investment", "currency"}},
               {"additionalProperties", false},
               {"properties",
                {{"value_per_outcome", {{"type", "number"}}},
                 {"investment", {{"type", "number"}, {"minimum", 0}}},
                 {"currency", {{"type", "string"}, {"minLength", 1}}}}}}}},
            json::array({"project", "solution", "level", "dataset", "delivery", "roi_model"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces || !context.store || !context.agent_bus ||
                !context.agent_caller ||
                !context.workspaces->config().agent_endpoints.contains("agent-project")) {
                co_return ApiResult::failure("store_unavailable",
                                             "the Project Agent is not running on this Lake");
            }
            if (context.acting_user_id.empty()) {
                co_return ApiResult::failure("unauthorized",
                                             "starting a solution needs a signed-in person");
            }
            const std::string customer_id = input.at("customer_id");
            const std::string project = input.at("project");
            const std::string solution = input.at("solution");
            const int level = input.at("level");
            const std::string dataset = input.at("dataset");
            const ApiResult created = called(
                co_await call_capability(context,
                                         "workspace_act",
                                         {{"customer_id", customer_id},
                                          {"capability", "create_project"},
                                          {"input",
                                           {{"name", project},
                                            {"description",
                                             "Solution " + solution + " over " + dataset +
                                                 ". Foundation awaiting owner confirmation."}}}}));
            if (!created.ok) {
                co_return created;
            }
            const json solution_context{{"project", project},
                                        {"solution", solution},
                                        {"level", level},
                                        {"dataset", dataset},
                                        {"delivery", input.at("delivery")},
                                        {"roi_model", input.at("roi_model")}};
            const std::string message =
                "Propose the executable foundation for solution " + solution + " at level " +
                std::to_string(level) + " in project " + project + " using dataset " + dataset +
                ". Read get_solution, analyze_dataset, get_dataset and plan_features. Return "
                "result.foundation with sources (including every selected methodology's roles), "
                "snapshot, dataset roles, decision_source when required, predicates, delivery, "
                "roi_model and missing_predicates. Preserve the supplied delivery and ROI values. "
                "Never fill a missing exclusion predicate with false and do not call "
                "create_solution.";
            ApiResult started = called(co_await call_capability(context,
                                                                "agent_turn",
                                                                {{"customer_id", customer_id},
                                                                 {"agent", "agent-project"},
                                                                 {"kind", "solution:" + project},
                                                                 {"message", message},
                                                                 {"context", solution_context}}));
            if (!started.ok) {
                co_return started;
            }
            started.value["project"] = project;
            started.value["solution"] = solution;
            co_return started;
        }});
    registry.add(Capability{
        .name = "confirm_solution",
        .title = "Confirm a solution foundation",
        .description =
            "Confirm one finished solution:<project> Project Agent task. Optional edits are a "
            "JSON merge patch over result.foundation. This is the only step that calls the "
            "engine's create_solution (not Lake's create_solution, which writes a workspace's "
            "own solution).",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"task_id", {{"type", "string"}, {"minLength", 1}}},
                           {"workspace_id", {{"type", "string"}}},
                           {"edits", {{"type", "object"}}}}},
                         {"required", json::array({"task_id", "workspace_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = task_customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return unavailable();
            }
            auto task = context.store->get_agent_task(input.at("task_id"));
            if (!task || task->workspace_id != input.at("workspace_id").get<std::string>() ||
                (task->requested_by_user_id != context.acting_user_id &&
                          context.acting_identity != "admin")) {
                co_return ApiResult::failure("not_found", "no such solution task");
            }
            if (auto loaded = control::settled(co_await load_agent_task_content(context, *task));
                !loaded) {
                co_return ApiResult::failure(loaded.error().code, loaded.error().message);
            }
            co_return co_await settle_solution(
                context, *task, input.value("edits", json::object()));
        }});
    registry.add(Capability{
        .name = "create_agent_from_proposal",
        .title = "Create an agent from an Agent Creator Agent proposal",
        .description =
            "Create the agent an Agent Creator Agent proposal describes (agent_turn with agent-creator "
            "answers `proposal`): a native agent, or a Python agent whose code goes into a new "
            "private Lake repository (`storage: lake`) or into the workspace (`storage: zetta`). A "
            "Python agent with a trigger is scheduled, and its first check run starts. Refused "
            "when "
            "the proposal lists defects.",
        .input_schema = customer_input_schema(
            {{"proposal", {{"type", "object"}}}, {"defects", {{"type", "array"}}}},
            json::array({"proposal"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            const std::string customer_id = input.at("customer_id");
            const json proposal = input.at("proposal");
            if (!input.value("defects", json::array()).empty()) {
                co_return ApiResult::failure("invalid_input", "This proposal still has defects.");
            }
            const json agent = proposal.value("agent", json::object());
            if (agent.value("name", std::string()).empty()) {
                co_return ApiResult::failure(
                    "invalid_input", "the proposal names no agent", "/proposal/agent/name");
            }
            agent_audit(context,
                        "agent.factory.approve",
                        customer_id,
                        {{"name", agent.value("name", std::string())}});
            const control::WorkspaceConfig& config = context.workspaces->config();
            const bool python = proposal.value("type", std::string()) == "python";
            json record{{"name", agent.value("name", std::string())},
                        {"purpose", agent.value("purpose", std::string())},
                        {"description", agent.value("purpose", std::string())},
                        {"work_area", agent.value("work_area", json::object())},
                        {"manifesto", agent.value("manifesto", std::string())},
                        {"goal", agent.value("goal", std::string())},
                        {"skills", agent.value("skills", json::array())},
                        {"trigger", agent.value("trigger", std::string())}};
            if (!proposal.value("pipelines", json::array()).empty()) {
                json grants = json::array();
                for (const json& grant : proposal["pipelines"]) {
                    grants.push_back({{"project", grant.value("project", std::string())},
                                      {"graph", grant.value("graph", std::string())}});
                }
                record["pipelines"] = std::move(grants);
            }
            json made = json::object();
            if (!python) {
                record["runtime"] = "native";
                record["model"] = config.base_agent_model;
                record["model_name"] = "Orama base model";
                record["system_prompt"] = agent.value("manifesto", std::string());
            } else {
                record["runtime"] = "python";
                record["model_name"] = config.frontier_model;
                const json files = proposal_files(proposal);
                std::string entrypoint;
                for (const json& file : files) {
                    if (file.value("path", std::string()) == "agent.toml") {
                        const std::string toml = file.value("contents", std::string());
                        const auto at = toml.find("entrypoint");
                        const auto open = toml.find('"', at);
                        const auto close = toml.find('"', open + 1);
                        if (at != std::string::npos && open != std::string::npos &&
                            close != std::string::npos) {
                            entrypoint = toml.substr(open + 1, close - open - 1);
                        }
                    }
                }
                if (proposal.value("storage", std::string()) == "lake") {
                    // The code goes into a private lake repository; the agent pins its commit.
                    const std::string slug = slug_of(agent.value("name", std::string()), 40);
                    const ApiResult repository = called(co_await call_capability(
                        context,
                        "create_repository",
                        {{"customer_id", customer_id},
                         // The agent's code belongs to the workspace that holds the agent.
                         {"workspace_id", input.at("workspace_id")},
                         {"name", "agent-" + (slug.empty() ? std::string("agent") : slug) + "-" +
                                      control::new_uuid_v4().substr(0, 4)},
                         {"description",
                          "The code of the agent " + agent.value("name", std::string())},
                         {"visibility", "private"}}));
                    if (!repository.ok) {
                        co_return repository;
                    }
                    json operations = json::array();
                    for (const json& file : files) {
                        operations.push_back(
                            {{"type", "file"},
                             {"path", file.value("path", std::string())},
                             {"content", base64(file.value("contents", std::string()))}});
                    }
                    const std::string repository_id = repository.value.value("id", std::string());
                    const ApiResult committed = called(co_await call_capability(
                        context,
                        "commit_repository_files",
                        {{"id", repository_id},
                         {"message",
                          "The agent " + agent.value("name", std::string()) +
                              ", as approved in the Agent Factory"},
                         {"operations", std::move(operations)}}));
                    if (!committed.ok) {
                        co_return committed;
                    }
                    const std::string revision = committed.value.value("commitOid", std::string());
                    record["definition"] = {{"source", "lake"},
                                            {"repository_id", repository_id},
                                            {"revision", revision},
                                            {"entrypoint", entrypoint}};
                    made["repository_id"] = repository_id;
                    made["revision"] = revision;
                } else {
                    std::string instructions;
                    json inline_files = json::array();
                    for (const json& file : files) {
                        if (file.value("path", std::string()) == "instructions.md") {
                            instructions = file.value("contents", std::string());
                        } else {
                            inline_files.push_back(file);
                        }
                    }
                    record["definition"] = {{"source", "zetta"},
                                            {"instructions", instructions},
                                            {"files", std::move(inline_files)},
                                            {"entrypoint", entrypoint}};
                }
            }
            const ApiResult created = called(co_await call_capability(
                context,
                "workspace_act",
                {{"customer_id", customer_id}, {"capability", "create_agent"}, {"input", record}}));
            if (!created.ok) {
                co_return created;
            }
            const std::string agent_id = created.value.value("id", std::string());
            made["agent_id"] = agent_id;
            made["name"] = agent.value("name", std::string());
            made["runtime"] = python ? "python" : "native";
            if (python && !agent.value("trigger", std::string()).empty()) {
                const ApiResult scheduled = called(co_await call_capability(
                    context,
                    "schedule_agent",
                    {{"customer_id", customer_id},
                     {"agent_id", agent_id},
                     {"trigger", agent["trigger"]}}));
                made["scheduled"] = scheduled.ok;
                if (!scheduled.ok) {
                    made["schedule_error"] = scheduled.error.message;
                }
            }
            if (python) {
                // The first place a new agent runs: a short check job the person can read.
                const ApiResult checked = called(co_await call_capability(
                    context,
                    "launch_agent",
                    {{"customer_id", customer_id},
                     {"agent_id", agent_id},
                     {"mode", "job"},
                     {"input",
                      "A first check: say in two sentences what you are for and what you can "
                      "reach."}}));
                if (checked.ok) {
                    made["check_launch_id"] = checked.value.value("id", std::string());
                }
            }
            co_return ApiResult::success(std::move(made));
        }});
    registry.add(Capability{
        .name = "create_projects_from_opportunities",
        .title = "Create projects from Opportunities",
        .description =
            "Create one draft project per chosen Opportunity (agent_turn with agent-autosuggest "
            "answers them): named from its intent, with the Opportunity it came from in its "
            "description. The Project Agent then designs each project's pipeline in the "
            "background (builds: {project, task_id}); the project's page compiles the design "
            "when it is done. A project gets its Opportunity's methodology's own pipeline "
            "(zetta#1061); one whose methodology the engine cannot run yet gets no pipeline and is "
            "listed in `waiting` with the issue that tracks it. Answers the projects' names, the "
            "builds and `waiting`.",
        .input_schema = customer_input_schema(
            {{"opportunities",
              {{"type", "array"}, {"items", {{"type", "object"}}}, {"minItems", 1}}},
             {"opportunity_ids", {{"type", "array"}, {"items", {{"type", "string"}}}}}},
            json::array()),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            const std::string customer_id = input.at("customer_id");
            // Saved Opportunities by id (Design → Opportunities), or Opportunities as given.
            json items = input.value("opportunities", json::array());
            std::vector<std::string> ids;
            std::optional<control::WorkspaceRecords> records;
            if (const json chosen = input.value("opportunity_ids", json::array());
                !chosen.empty() && context.store) {
                // Saved Opportunities live in this workspace's Zetta (lake#376).
                auto opened = workspace_records(context, customer_id, input.at("workspace_id"));
                if (!opened) {
                    co_return ApiResult::failure(opened.error().code, opened.error().message,
                                                 opened.error().path);
                }
                records.emplace(std::move(*opened));
                for (const json& id : chosen) {
                    const std::string wanted = id.get<std::string>();
                    auto row = control::settled(
                        co_await control::get_opportunity(*records, customer_id, wanted));
                    if (!row) {
                        co_return ApiResult::failure(row.error().code, row.error().message,
                                                     row.error().path);
                    }
                    if (!*row) {
                        co_return ApiResult::failure("not_found", "no such Opportunity " + wanted);
                    }
                    json item = (*row)->payload.is_object() ? (*row)->payload : json::object();
                    item["intent"] = (*row)->intent;
                    if (!(*row)->solution.empty()) item["solution"] = (*row)->solution;
                    if (!(*row)->methodology.empty()) item["methodology"] = (*row)->methodology;
                    items.push_back(std::move(item));
                    ids.push_back((*row)->id);
                }
            }
            if (items.empty()) {
                co_return ApiResult::failure("invalid_input", "choose at least one Opportunity");
            }
            agent_audit(context, "discover.create", customer_id, {{"count", items.size()}});
            json names = json::array();
            json builds = json::array();
            json waiting = json::array();
            // What the workspace's engine can run (zetta#1061); nullopt on an engine that predates
            // the registry, which keeps compiling from the intent alone.
            auto listed = co_await workspace_methodologies(context, customer_id);
            const json registry = listed ? *listed : json(nullptr);
            const bool builds_pipelines =
                context.workspaces && context.agent_bus &&
                context.workspaces->config().agent_endpoints.contains("agent-project");
            std::size_t position = 0;
            for (const json& item : items) {
                const std::string intent = item.value("intent", std::string());
                if (intent.empty()) {
                    co_return ApiResult::failure("invalid_input", "every Opportunity needs an intent");
                }
                const std::string slug = slug_of(intent, 40);
                const std::string name = (slug.empty() ? std::string("opportunity") : slug) + "-" +
                                         control::new_uuid_v4().substr(0, 4);
                const std::string description =
                    intent + "\n\nSource Opportunity (Auto-Suggest Agent): " +
                    item.value("solution", std::string()) + ", " +
                    item.value("methodology", std::string()) + ". " +
                    item.value("why_this_data_supports_it", std::string());
                // The methodology the Opportunity needs (lake#289): the project gets that
                // methodology's own pipeline, or none, with the reason, when it cannot run yet.
                const json entry =
                    find_methodology(registry, item.value("methodology", std::string()),
                                     item.value("solution", std::string()));
                const bool known = entry.is_object();
                const bool runnable = known && entry.value("runnable", false);
                json project_input{{"name", name}, {"description", description}};
                if (!known) {
                    project_input["intent"] = intent;  // the engine compiles from the words
                } else if (!runnable) {
                    project_input["description"] = description + "\n\n" + methodology_waits(entry);
                }
                const ApiResult created = called(co_await call_capability(
                    context,
                    "workspace_act",
                    {{"customer_id", customer_id},
                     {"capability", "create_project"},
                     {"input", std::move(project_input)}}));
                if (!created.ok) {
                    co_return created;
                }
                names.push_back(name);
                if (known && !runnable) {
                    waiting.push_back({{"project", name},
                                       {"methodology", entry.value("id", std::string())},
                                       {"name", entry.value("name", std::string())},
                                       {"issue", entry.value("issue", std::string())},
                                       {"message", methodology_waits(entry)}});
                } else if (runnable) {
                    const ApiResult compiled = called(co_await compile_pipeline(
                        context, customer_id, name, intent, "", nullptr,
                        entry.value("id", std::string())));
                    if (!compiled.ok) {
                        co_return compiled;
                    }
                }
                if (position < ids.size() && records) {
                    static_cast<void>(control::settled(co_await control::set_opportunity_status(
                        *records, customer_id, ids[position], "project", name)));
                }
                ++position;
                // The Project Agent designs its pipeline in the background; the project page
                // compiles that design when it is done (kind "build:<project>").
                if (builds_pipelines && !(known && !runnable)) {
                    std::string brief = "Design the pipeline for the project " + name + ": " +
                                        intent + ".";
                    for (const auto& [field, label] :
                         {std::pair{"business_action", " What it changes: "},
                          std::pair{"why_this_data_supports_it", " Why the data supports it: "},
                          std::pair{"solution", " Catalogue solution: "},
                          std::pair{"methodology", " Methodology: "}}) {
                        if (const std::string value = item.value(field, std::string());
                            !value.empty()) {
                            brief += label + value;
                        }
                    }
                    const ApiResult started = called(co_await call_capability(
                        context,
                        "agent_turn",
                        {{"customer_id", customer_id},
                         {"agent", "agent-project"},
                         {"kind", "build:" + name},
                         {"message", brief},
                         {"context", {{"opportunity", item}}}}));
                    if (started.ok) {
                        builds.push_back(
                            {{"project", name},
                             {"task_id", started.value.value("task_id", std::string())}});
                    }
                }
            }
            co_return ApiResult::success({{"projects", std::move(names)},
                                          {"builds", std::move(builds)},
                                          {"waiting", std::move(waiting)}});
        }});
    registry.add(Capability{
        .name = "get_ask_conversation",
        .title = "Read an Ask Orama conversation",
        .description = "The messages of one of your conversations with Ask Orama or the Agent "
                       "Factory in one workspace, oldest first, with each answer's reads, "
                       "proposals and delegations. A conversation lives in the workspace Zetta of "
                       "the workspace it was held in; only the person who had it can read it, and "
                       "another workspace of the same account never shows it.",
        .input_schema = customer_input_schema(
            {{"conversation_id", {{"type", "string"}}},
             {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}}},
            json::array({"conversation_id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store || context.acting_user_id.empty()) {
                co_return ApiResult::failure("unauthorized", "reading a conversation needs a person");
            }
            auto records = workspace_records(context, input.at("customer_id"),
                                             input.at("workspace_id"));
            if (!records) {
                co_return ApiResult::failure(records.error().code, records.error().message,
                                             records.error().path);
            }
            control::AskMessages conversation(*records);
            auto messages = control::settled(co_await conversation.list(
                control::ask_conversation_lookup(input.at("conversation_id"),
                                                 context.acting_user_id),
                static_cast<std::size_t>(input.value("limit", 200))));
            if (!messages) {
                co_return ApiResult::failure(messages.error().code, messages.error().message,
                                             messages.error().path);
            }
            json out = json::array();
            for (const control::AskMessage& message : *messages) {
                json item{{"id", message.id},
                          {"role", message.role},
                          {"content", message.content},
                          {"payload", message.payload},
                          {"created_at", message.created_at}};
                // Lake's own statement of what each proposal runs, beside the model's payload.
                if (message.payload.is_object() &&
                    message.payload.value("proposals", json()).is_array()) {
                    item["proposal_calls"] = ask_proposal_calls(context,
                                                                message.payload,
                                                                input.at("customer_id"),
                                                                input.at("workspace_id"));
                }
                out.push_back(std::move(item));
            }
            co_return ApiResult::success({{"messages", std::move(out)}});
        }});
}

namespace {

// ---- Ask Orama and Agent Factory conversations, as capabilities (lake#561) ----
// docs/api/ask-orama.md. The pages' forms and the web app both go through these, so the browser
// never writes a message itself and only a proposal lake stored can be run.

using AskMessagesResult = control::ZettaControlResult<std::vector<control::AskMessage>>;
using AskMessageResult = control::ZettaControlResult<control::AskMessage>;

ApiResult zetta_failure(const control::ZettaControlError& error) {
    return ApiResult::failure(error.code, error.message, error.path);
}

// The caller's conversation in the workspace the call names (oldest first).
liquid::runtime::task<AskMessagesResult> conversation_messages(ApiContext& context,
                                                               std::string customer_id,
                                                               std::string workspace_id,
                                                               std::string conversation_id) {
    auto records = workspace_records(context, std::move(customer_id), std::move(workspace_id));
    if (!records) {
        co_return AskMessagesResult(std::unexpected(records.error()));
    }
    control::AskMessages messages(*records);
    co_return control::settled(co_await messages.list(
        control::ask_conversation_lookup(conversation_id, context.acting_user_id), 500));
}

// Appends one message as the caller, for the space that owns the workspace. `id` empty: a new one;
// set: written only if absent (a second write of the same id is `conflict`).
liquid::runtime::task<AskMessageResult> append_message(ApiContext& context,
                                                       std::string customer_id,
                                                       std::string workspace_id,
                                                       std::string conversation_id,
                                                       std::string role,
                                                       std::string content,
                                                       json payload,
                                                       std::string id = {}) {
    control::AskMessage message{"",
                                std::move(conversation_id),
                                customer_id,
                                context.acting_user_id,
                                std::move(role),
                                std::move(content),
                                std::move(payload),
                                ""};
    if (const auto problem = control::ask_message_problem(message)) {
        co_return AskMessageResult(
            std::unexpected(control::ZettaControlError{"invalid_input", *problem}));
    }
    auto records = workspace_records(context, std::move(customer_id), std::move(workspace_id));
    if (!records) {
        co_return AskMessageResult(std::unexpected(records.error()));
    }
    control::AskMessages messages(*records);
    co_return control::settled(
        co_await messages.append(std::move(message), control::RecordWrite{.id = std::move(id)}));
}

// A UUID-shaped id derived from `seed`, so one task's answer is written under one id only.
std::string derived_record_id(std::string_view seed) {
    std::string hex = control::identity_crypto::sha256_hex(seed).substr(0, 32);
    hex[12] = '5';
    hex[16] = "89ab"[static_cast<unsigned char>(hex[16]) % 4];
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" +
           hex.substr(16, 4) + "-" + hex.substr(20);
}

std::string error_text(const json& error) {
    return error.is_string() ? error.get<std::string>() : error.dump();
}

// A string field of JSON an agent or a model wrote, or `fallback` when it is absent or of another
// type. nlohmann's value() throws when the key holds another type, and a reply must never be lost
// because a model wrote `null` or a number where lake expects text (lake#794).
std::string text_of(const json& object, const char* key, std::string fallback = {}) {
    if (object.is_object()) {
        if (const auto found = object.find(key); found != object.end() && found->is_string()) {
            return found->get<std::string>();
        }
    }
    return fallback;
}

// The assistant message an agent's finished answer becomes, the same text and payload the pages
// stored: Ask Orama's reply with its proposals, reads and delegations; Agent Creator Agent's question,
// proposal or reply; a failure as "That did not finish: …". `task_id` is empty for an answer the
// agent gave at once.
std::pair<std::string, json> answer_message(bool factory,
                                            bool succeeded,
                                            const json& answer,
                                            const std::string& agent,
                                            const std::string& task_id,
                                            const std::string& failure) {
    json payload = json::object();
    if (!task_id.empty()) {
        payload["task_id"] = task_id;
    }
    payload["agent"] = agent;
    // Every field below is the agent's (a model's) writing, read by type: a `reply` that is null
    // or a number is no reply, never an exception that loses the whole turn (lake#794).
    const bool ok = succeeded && answer.is_object() && answer.value("ok", json(true)) != false;
    if (!ok) {
        return {"That did not finish: " + (answer.is_object() && answer.contains("error")
                                               ? error_text(answer["error"])
                                               : failure),
                std::move(payload)};
    }
    if (factory) {
        const json creator = agent_creator_answer(answer);
        const std::string kind = text_of(creator, "kind");
        std::string text = kind == "question" ? text_of(creator, "question")
                           : kind == "proposal"
                               ? std::string("Here is the agent I propose. Review it below.")
                               : text_of(answer, "reply", "Agent Creator Agent did not answer.");
        // The digest approve_agent_proposal checks, so a browser sends back exactly this one.
        if (const json proposal = creator.value("proposal", json()); proposal.is_object()) {
            payload["proposal_sha256"] = agent_proposal_digest(proposal);
        }
        payload["answer"] = creator;
        return {std::move(text), std::move(payload)};
    }
    payload["proposals"] = answer.value("proposals", json::array());
    payload["reads"] = answer.value("reads", json::array());
    payload["delegations"] = answer.value("delegations", json::array());
    payload["trace_id"] = text_of(answer, "trace_id");
    return {text_of(answer, "reply"), std::move(payload)};
}

// Whether confirming the same stored proposal again could work after it failed with `code`
// (lake#794): the refusals that say nothing about the input, a dependency not reachable or not
// ready, a busy resource or a full queue, or a server fault (docs/api/errors.md). Every other code
// (invalid_input, conflict, not_found, unauthorized, engine_error, ...) refuses the same input the
// same way every time, so Try again is not offered for it.
bool confirm_retry_helps(const std::string& code) {
    static constexpr std::array<std::string_view, 7> transient = {"store_unavailable",
                                                                  "workspace_not_ready",
                                                                  "busy",
                                                                  "rate_limited",
                                                                  "locked",
                                                                  "store_error",
                                                                  "io"};
    return std::ranges::find(transient, code) != transient.end();
}

// The input a stored Ask Orama proposal runs with when the person confirms it: the conversation's
// account and workspace fill `customer_id` and `workspace_id` when the input names none and the
// capability takes them (an MCP connection's call_mcp_tool from Ask Orama, lake#652). A model that
// writes `null` or "" for one names none, so it is filled too, never asked of the person.
json proposal_input(const Capability* target,
                    const json& proposal,
                    const std::string& customer_id,
                    const std::string& workspace_id) {
    json proposed = proposal.value("input", json::object());
    if (!proposed.is_object() || target == nullptr) {
        return proposed;
    }
    const json properties = target->input_schema.value("properties", json::object());
    const auto fill = [&](const char* key, const std::string& value) {
        const auto found = proposed.find(key);
        const bool named = found != proposed.end() && !found->is_null() &&
                           !(found->is_string() && found->get_ref<const std::string&>().empty());
        if (!named && properties.is_object() && properties.contains(key)) {
            proposed[key] = value;
        }
    };
    fill("customer_id", customer_id);
    fill("workspace_id", workspace_id);
    return proposed;
}

// The first clause of a top-level field's schema description: "the source table to index (load it
// first with ...)" reads as "the source table to index". Empty when the schema says nothing.
std::string field_description(const Capability* target, const std::string& field) {
    if (target == nullptr) {
        return {};
    }
    const json properties = target->input_schema.value("properties", json::object());
    const auto schema = properties.is_object() ? properties.find(field) : properties.end();
    if (schema == properties.end()) {
        return {};
    }
    const std::string description = text_of(*schema, "description");
    return description.substr(0, description.find(" ("));
}

// The call a stored Ask Orama proposal makes when the person confirms it (lake#942): the
// capability as Lake's registry titles it and the exact input confirm_ask_proposal runs, after
// proposal_input fills the conversation's account and workspace and canonical_input renames
// deprecated fields, as Registry::call would. `fields` is that input field by field with the
// schema's own description; `input_sha256` binds the capability and input, and confirm runs only
// the call whose digest the person sends back. The model's `impact` is never part of it. Empty
// when Lake cannot say what would run: no such capability, or input that is not an object or that
// canonical_input refuses. Such a proposal cannot be confirmed.
std::optional<json> proposal_call(const ApiContext& context,
                                  const json& proposal,
                                  const std::string& customer_id,
                                  const std::string& workspace_id) {
    const std::string capability = text_of(proposal, "capability");
    const auto target = !capability.empty() && context.registry != nullptr
                            ? context.registry->find(capability)
                            : nullptr;
    if (!target) {
        return std::nullopt;
    }
    const json proposed = proposal_input(target.get(), proposal, customer_id, workspace_id);
    if (!proposed.is_object()) {
        return std::nullopt;
    }
    auto canonical = canonical_input(*target, proposed);
    if (!canonical) {
        return std::nullopt;
    }
    json fields = json::array();
    for (const auto& [name, value] : canonical->items()) {
        fields.push_back({{"name", name},
                          {"description", field_description(target.get(), name)},
                          {"value", value}});
    }
    const std::string digest = control::identity_crypto::sha256_hex(
        json{{"capability", capability}, {"input", *canonical}}.dump());
    return json{{"capability", capability},
                {"title", target->title},
                {"kind", std::string(to_string(target->kind))},
                {"input", std::move(*canonical)},
                {"fields", std::move(fields)},
                {"input_sha256", digest}};
}

// One entry per proposal of a stored answer's payload, in order: its proposal_call, or null when
// it has none.
json ask_proposal_calls(const ApiContext& context,
                        const json& payload,
                        const std::string& customer_id,
                        const std::string& workspace_id) {
    json calls = json::array();
    for (const json& proposal : payload.value("proposals", json::array())) {
        const auto call = proposal_call(context, proposal, customer_id, workspace_id);
        calls.push_back(call ? *call : json());
    }
    return calls;
}

// Every required top-level field `input` leaves out, in the schema's order, by the rule
// validate_input applies (absent or null). validate_input stops at the first; the reply asks for
// all of them at once (lake#794).
std::vector<std::string> missing_required(const Capability* target, const json& input) {
    std::vector<std::string> missing;
    if (target == nullptr || !input.is_object()) {
        return missing;
    }
    const json required = target->input_schema.value("required", json::array());
    if (!required.is_array()) {
        return missing;
    }
    for (const json& key : required) {
        if (!key.is_string()) {
            continue;
        }
        const auto found = input.find(key.get<std::string>());
        if (found == input.end() || found->is_null()) {
            missing.push_back(key.get<std::string>());
        }
    }
    return missing;
}

// "create_stream needs `topic` (the topic name) and `zetta` (the workspace Zetta that holds it),
// which this proposal does not name."
std::string missing_fields_text(const Capability* target,
                                const std::string& capability,
                                const std::vector<std::string>& missing) {
    std::string named;
    for (std::size_t at = 0; at < missing.size(); ++at) {
        if (at > 0) {
            named += at + 1 == missing.size() ? " and " : ", ";
        }
        const std::string description = field_description(target, missing[at]);
        named += "`" + missing[at] + "`" + (description.empty() ? "" : " (" + description + ")");
    }
    return capability + " needs " + named + ", which this proposal does not name.";
}

// What is wrong with a proposal's input, said plainly for the person (lake#794): "create_index
// needs `table` (the source table to index), which this proposal does not name." The error is
// the registry's own (validate_input), so the words follow the rule Registry::call enforces. A
// missing required field names every required field `input` leaves out, not only the first.
std::string proposal_problem_text(const Capability* target,
                                  const std::string& capability,
                                  const ApiError& error,
                                  const json& input) {
    std::string field;
    if (error.path.size() > 1) {
        const std::string pointer = error.path.substr(error.path.rfind('/') + 1);
        for (std::size_t at = 0; at < pointer.size(); ++at) {
            if (pointer[at] == '~' && at + 1 < pointer.size()) {
                field += pointer[at + 1] == '1' ? '/' : '~';
                ++at;
            } else {
                field += pointer[at];
            }
        }
    }
    if (field.empty()) {
        return capability + " cannot run with this input: " + error.message + ".";
    }
    const bool top_level = error.path.find('/', 1) == std::string::npos;
    if (error.message == field + " is required") {
        std::vector<std::string> missing =
            top_level ? missing_required(target, input) : std::vector<std::string>{};
        if (std::ranges::find(missing, field) == missing.end()) {
            missing = {field};
        }
        return missing_fields_text(top_level ? target : nullptr, capability, missing);
    }
    if (error.message == "unexpected field " + field) {
        return capability + " does not take `" + field + "`.";
    }
    const std::string description = top_level ? field_description(target, field) : "";
    const std::string about = description.empty() ? "" : " (" + description + ")";
    return "`" + field + "`" + about + " is not valid for " + capability + ": " + error.message +
           ".";
}

// Why a proposal cannot run as stored, checked by the registry's own rules before it is offered
// (lake#794): an unknown capability, or input its schema refuses. Nothing is called.
struct ProposalProblem {
    ApiError error;
    // What the person is told: plain words naming what is missing or wrong.
    std::string text;
    // The required top-level fields the input leaves out, every one (empty for another problem).
    std::vector<std::string> missing;
};

// Empty when the proposal can be offered. Reads the model-written proposal by type only, so a
// `capability` that is null, a number or absent is a problem to report, never an exception.
std::optional<ProposalProblem> proposal_problem(const ApiContext& context,
                                                const json& proposal,
                                                const std::string& customer_id,
                                                const std::string& workspace_id) {
    const std::string capability = text_of(proposal, "capability");
    if (capability.empty()) {
        return ProposalProblem{
            {"invalid_input", "the proposal names no capability", "/capability"},
            "a proposed change does not say which capability it runs.",
            {}};
    }
    const auto target = context.registry != nullptr ? context.registry->find(capability) : nullptr;
    if (!target) {
        ApiError error{"unknown_capability", "Lake has no capability named " + capability, ""};
        return ProposalProblem{error, error.message + ".", {}};
    }
    const json proposed = proposal_input(target.get(), proposal, customer_id, workspace_id);
    const auto canonical = canonical_input(*target, proposed);
    const json& checked_input = canonical ? *canonical : proposed;
    std::optional<ApiError> error;
    if (!canonical) {
        error = canonical.error();
    } else if (ApiResult checked = validate_input(*target, *canonical); !checked.ok) {
        error = checked.error;
    }
    if (!error) {
        return std::nullopt;
    }
    std::vector<std::string> missing;
    if (error->message.ends_with(" is required") && error->path.find('/', 1) == std::string::npos) {
        missing = missing_required(target.get(), checked_input);
    }
    return ProposalProblem{*error,
                           proposal_problem_text(target.get(), capability, *error, checked_input),
                           std::move(missing)};
}

// An answer's proposals, keeping only those that can run (lake#794). A proposal whose input the
// capability's schema refuses is never offered: it moves to `withheld_proposals` with the reason,
// and the reply says, in plain words, what is missing and asks the person for it.
void withhold_invalid_proposals(const ApiContext& context,
                                const std::string& customer_id,
                                const std::string& workspace_id,
                                std::string& content,
                                json& payload) {
    if (!payload.is_object() || !payload.value("proposals", json()).is_array()) {
        return;
    }
    json offered = json::array();
    json withheld = json::array();
    std::string said;
    for (const json& proposal : payload.at("proposals")) {
        const auto problem = proposal_problem(context, proposal, customer_id, workspace_id);
        if (!problem) {
            offered.push_back(proposal);
            continue;
        }
        json entry = proposal.is_object() ? proposal : json::object();
        entry["error"] = {{"code", problem->error.code},
                          {"message", problem->text},
                          {"path", problem->error.path}};
        if (!problem->missing.empty()) {
            entry["error"]["missing"] = problem->missing;
        }
        withheld.push_back(std::move(entry));
        said += "\n\nI could not offer this change yet: " + problem->text +
                " Tell me what to use and I will propose it again.";
    }
    if (withheld.empty()) {
        return;
    }
    payload["proposals"] = std::move(offered);
    payload["withheld_proposals"] = std::move(withheld);
    content += said;
}

// The capabilities whose `project` names a project that must already exist (lake#1193), by Lake's
// name and, for a proposal sent through workspace_act by an older agent, by the engine's.
// compile_pipeline is here too, unless it is asked to create the project (`create_project`).
bool names_existing_project(const std::string& capability) {
    static constexpr std::array<std::string_view, 19> owning = {
        "compile_intent", "capture_intent", "compile_n8n",      "compile_python",
        "import_n8n",     "save_pipeline",  "check_pipeline",   "start_run",
        "deploy_pipeline", "approve_pipeline", "compile_pipeline", "run_pipeline",
        "edit_pipeline",  "create_pipeline", "save_graph",       "gate",
        "run",            "deploy",         "approve_plan"};
    return std::ranges::find(owning, capability) != owning.end();
}

// The project a proposal names and whether it must exist (true) or must not (false, a
// compile_pipeline that creates it). Empty when the proposal names no project to check.
std::optional<std::pair<std::string, bool>> proposal_project(const json& proposal) {
    std::string capability = text_of(proposal, "capability");
    json input = proposal.is_object() ? proposal.value("input", json::object()) : json();
    if (!input.is_object()) {
        return std::nullopt;
    }
    if (capability == "workspace_act") {
        capability = text_of(input, "capability");
        input = input.value("input", json::object());
        if (!input.is_object()) {
            return std::nullopt;
        }
    }
    if (!names_existing_project(capability)) {
        return std::nullopt;
    }
    const std::string project = text_of(input, "project");
    if (project.empty()) {
        return std::nullopt;
    }
    const bool creates = capability == "compile_pipeline" && input.value("create_project", json(false)) == true;
    return std::pair{project, !creates};
}

// Proposals that name a project the workspace does not have, or create one it already has, are
// never offered (lake#1193): confirming one could only fail. Each named project is read once with
// the engine's get_project, as the person; a project the read cannot settle (the workspace not
// reachable) leaves the proposal offered, and confirm still checks. A withheld proposal says what
// is wrong and how to go on: a new project's pipeline is one compile_pipeline with
// `create_project`.
liquid::runtime::task<void> withhold_proposals_naming_missing_projects(ApiContext& context,
                                                                       std::string customer_id,
                                                                       std::string workspace_id,
                                                                       std::string& content,
                                                                       json& payload) {
    if (!payload.is_object() || !payload.value("proposals", json()).is_array() ||
        payload.at("proposals").empty()) {
        co_return {};
    }
    std::map<std::string, std::optional<bool>> exists;
    json offered = json::array();
    json withheld = payload.value("withheld_proposals", json::array());
    std::string said;
    for (const json& proposal : payload.at("proposals")) {
        const auto named = proposal_project(proposal);
        if (!named) {
            offered.push_back(proposal);
            continue;
        }
        const auto& [project, must_exist] = *named;
        if (!exists.contains(project)) {
            const ApiResult read = called(co_await call_capability(
                context,
                "workspace_query",
                {{"customer_id", customer_id},
                 {"workspace_id", workspace_id},
                 {"capability", "get_project"},
                 {"input", {{"name", project}}}}));
            exists[project] = read.ok                          ? std::optional<bool>(true)
                              : read.error.code == "not_found" ? std::optional<bool>(false)
                                                               : std::nullopt;
        }
        const std::optional<bool> found = exists[project];
        if (!found || *found == must_exist) {
            offered.push_back(proposal);
            continue;
        }
        const std::string capability = text_of(proposal, "capability");
        json entry = proposal;
        std::string text;
        if (must_exist) {
            text = "there is no project named \"" + project + "\" in this workspace, so " +
                   capability +
                   " could not run. To build it in a new project, I can propose "
                   "compile_pipeline with `create_project`, which creates the project and its "
                   "pipeline in one confirmation.";
            entry["error"] = {{"code", "not_found"}, {"message", text}, {"path", "/project"}};
        } else {
            text = "a project named \"" + project +
                   "\" already exists, so compile_pipeline cannot create it. I can propose the "
                   "pipeline in that project instead, without `create_project`.";
            entry["error"] = {{"code", "conflict"}, {"message", text}, {"path", "/create_project"}};
        }
        withheld.push_back(std::move(entry));
        said += "\n\nI could not offer this change: " + text +
                " Tell me how to go on and I will propose it again.";
    }
    if (said.empty()) {
        co_return {};
    }
    payload["proposals"] = std::move(offered);
    payload["withheld_proposals"] = std::move(withheld);
    content += said;
    co_return {};
}

// What became of each change Ask Orama proposed in this conversation (lake#1194), oldest first,
// as Lake recorded it: `pending` (offered, not confirmed), `confirmed`, `failed` with the stored
// error and whether trying again could work, or `withheld` (never offered, with the reason). Ask
// Orama receives the last few with each turn, so after a failure it proposes a corrected change
// instead of pointing at one that can no longer run.
json ask_proposal_states(const std::vector<control::AskMessage>& messages, std::size_t keep = 10) {
    json states = json::array();
    std::map<std::string, std::size_t> at;
    for (const control::AskMessage& message : messages) {
        if (message.role != "assistant" || !message.payload.is_object()) {
            continue;
        }
        const json& payload = message.payload;
        if (const json proposals = payload.value("proposals", json()); proposals.is_array()) {
            for (std::size_t index = 0; index < proposals.size(); ++index) {
                const json& proposal = proposals[index];
                json state{{"message_id", message.id},
                           {"index", index},
                           {"capability", text_of(proposal, "capability")},
                           {"input", proposal.is_object() ? proposal.value("input", json::object())
                                                          : json::object()},
                           {"state", "pending"}};
                at[message.id + "/" + std::to_string(index)] = states.size();
                states.push_back(std::move(state));
            }
        }
        if (const json withheld = payload.value("withheld_proposals", json());
            withheld.is_array()) {
            for (const json& proposal : withheld) {
                const json error = proposal.is_object() ? proposal.value("error", json::object())
                                                        : json::object();
                states.push_back({{"message_id", message.id},
                                  {"capability", text_of(proposal, "capability")},
                                  {"input", proposal.is_object()
                                                ? proposal.value("input", json::object())
                                                : json::object()},
                                  {"state", "withheld"},
                                  {"error", text_of(error, "message")}});
            }
        }
        if (const json mark = payload.value("confirmed", json()); mark.is_object()) {
            const json index = mark.value("index", json());
            const std::string key =
                text_of(mark, "message_id") + "/" +
                (index.is_string() ? index.get<std::string>()
                 : index.is_number_unsigned() ? std::to_string(index.get<std::size_t>())
                                              : std::string());
            const auto found = at.find(key);
            if (found == at.end()) {
                continue;
            }
            json& state = states[found->second];
            if (payload.value("ok", json(true)) == false) {
                const json error = payload.value("error", json::object());
                state["state"] = "failed";
                state["error"] = error.is_object() ? text_of(error, "message") : std::string();
                state["code"] = error.is_object() ? text_of(error, "code") : std::string();
                state["retry"] = payload.value("retry", json(false)) == true;
            } else {
                state["state"] = "confirmed";
                state.erase("error");
                state.erase("code");
                state.erase("retry");
            }
        }
    }
    if (states.size() > keep) {
        states.erase(states.begin(), states.begin() + static_cast<std::ptrdiff_t>(states.size() - keep));
    }
    return states;
}

json conversation_kind_schema() {
    return {{"type", "string"}, {"enum", {"ask", "factory"}}};
}

json agent_task_value_schema() {
    return {{"type", "object"},
            {"properties",
             {{"task_id", {{"type", "string"}}},
              {"workspace_id", {{"type", "string"}}},
              {"agent", {{"type", "string"}}},
              {"kind", {{"type", "string"}}},
              {"status", {{"type", "string"}}},
              {"message", {{"type", "string"}}},
              {"attempts", {{"type", "integer"}}},
              {"lease_until", {{"type", "string"}}},
              {"lease_seconds", {{"type", "integer"}}},
              {"created_at", {{"type", "string"}}},
              {"finished_at", {{"type", "string"}}},
              {"answer", json::object()},
              {"conversation_id", {{"type", "string"}}},
              {"context", {{"type", "object"}}},
              {"review_url", {{"type", "string"}}},
              {"build_error", {{"type", "string"}}}}},
            {"required",
             json::array({"task_id",
                          "workspace_id",
                          "agent",
                          "kind",
                          "status",
                          "message",
                          "attempts",
                          "created_at",
                          "finished_at",
                          "answer"})},
            {"additionalProperties", false}};
}

liquid::runtime::task<ApiResult> send_ask_message(const json& input, ApiContext& context) {
    if (!context.workspaces || !context.store) {
        co_return unavailable();
    }
    if (context.acting_user_id.empty()) {
        co_return ApiResult::failure("unauthorized", "a conversation needs a person");
    }
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const bool factory = input.value("kind", std::string("ask")) == "factory";
    std::string agent = input.value("agent", std::string());
    if (factory && !agent.empty() && agent != "agent-creator") {
        co_return ApiResult::failure(
            "invalid_input", "agent: the Agent Factory talks to agent-creator", "/agent");
    }
    if (agent.empty()) {
        agent = factory ? "agent-creator" : "agent-ask-orama";
    }
    std::string conversation_id = input.value("conversation_id", std::string());
    if (conversation_id.empty()) {
        conversation_id = control::new_uuid_v4();
    }
    const std::string text = input.at("message");
    // A message the workspace did not store is an error, never a silent loss: nothing is asked.
    auto stored = control::settled(co_await append_message(
        context, customer_id, workspace_id, conversation_id, "user", text, nullptr));
    if (!stored) {
        co_return ApiResult::failure(
            stored.error().code, "The conversation could not be saved: " + stored.error().message);
    }
    json turn{{"customer_id", customer_id},
              {"agent", agent},
              {"message", text},
              {"conversation_id", conversation_id},
              {"kind", factory ? "factory" : "ask"}};
    if (!factory) {
        turn["page"] = input.value("page", json::object());
    }
    // What became of Ask Orama's earlier proposals in this conversation (lake#1194): a failed one
    // is never pointed at again; Ask Orama proposes a corrected change instead. A conversation
    // that cannot be read now sends none, and the turn still runs.
    if (agent == "agent-ask-orama") {
        auto earlier = control::settled(
            co_await conversation_messages(context, customer_id, workspace_id, conversation_id));
        if (earlier) {
            if (json states = ask_proposal_states(*earlier); !states.empty()) {
                turn["context"] = {{"proposals", std::move(states)}};
            }
        }
    }
    const ApiResult answered = called(co_await call_capability(context, "agent_turn", turn));
    if (!answered.ok) {
        co_return answered;
    }
    json out{{"conversation_id", conversation_id}, {"message_id", stored->id}};
    if (const std::string task_id = answered.value.value("task_id", std::string());
        !task_id.empty()) {
        // Running in the background (lake#269): settle_ask_turn stores the answer once.
        out["task_id"] = task_id;
        co_return ApiResult::success(std::move(out));
    }
    auto [content, payload] = answer_message(factory, true, answered.value, agent, "", "");
    withhold_invalid_proposals(context, customer_id, workspace_id, content, payload);
    static_cast<void>(co_await withhold_proposals_naming_missing_projects(
        context, customer_id, workspace_id, content, payload));
    auto reply = control::settled(co_await append_message(context,
                                                          customer_id,
                                                          workspace_id,
                                                          conversation_id,
                                                          "assistant",
                                                          std::move(content),
                                                          std::move(payload)));
    if (!reply) {
        co_return ApiResult::failure(
            reply.error().code, "The conversation could not be saved: " + reply.error().message);
    }
    out["reply_id"] = reply->id;
    co_return ApiResult::success(std::move(out));
}

liquid::runtime::task<ApiResult> settle_ask_turn(const json& input, ApiContext& context) {
    if (!context.workspaces || !context.store) {
        co_return unavailable();
    }
    if (context.acting_user_id.empty()) {
        co_return ApiResult::failure("unauthorized", "a conversation needs a person");
    }
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const std::string conversation_id = input.at("conversation_id");
    const std::string task_id = input.at("task_id");
    const bool factory = input.at("kind") == "factory";
    const ApiResult got = called(co_await call_capability(
        context, "get_agent_task", {{"customer_id", customer_id}, {"task_id", task_id}}));
    if (!got.ok) {
        co_return got;
    }
    const json& task = got.value;
    if (const std::string of = task.value("conversation_id", std::string());
        !of.empty() && of != conversation_id) {
        co_return ApiResult::failure(
            "invalid_input", "task_id: that turn belongs to another conversation", "/task_id");
    }
    const std::string status = task.value("status", std::string());
    if (status == "pending" || status == "running") {
        co_return ApiResult::success({{"settled", false}, {"task", task}});
    }
    // Once: an answer already in the conversation (written by this or by an earlier page) is
    // left as it is, and the answer is written under an id derived from the task.
    auto messages = control::settled(
        co_await conversation_messages(context, customer_id, workspace_id, conversation_id));
    if (!messages) {
        co_return zetta_failure(messages.error());
    }
    for (const control::AskMessage& message : *messages) {
        if (message.payload.is_object() &&
            message.payload.value("task_id", std::string()) == task_id) {
            co_return ApiResult::success({{"settled", true}, {"task", task}});
        }
    }
    auto [content, payload] =
        answer_message(factory,
                       status == "succeeded",
                       task.value("answer", json::object()),
                       text_of(task, "agent"),
                       task_id,
                       text_of(task, "message", "the agent did not answer"));
    withhold_invalid_proposals(context, customer_id, workspace_id, content, payload);
    static_cast<void>(co_await withhold_proposals_naming_missing_projects(
        context, customer_id, workspace_id, content, payload));
    auto written =
        control::settled(co_await append_message(context,
                                                 customer_id,
                                                 workspace_id,
                                                 conversation_id,
                                                 "assistant",
                                                 std::move(content),
                                                 std::move(payload),
                                                 derived_record_id("ask-turn:" + task_id)));
    if (!written && written.error().code != "conflict") {
        co_return zetta_failure(written.error());
    }
    co_return ApiResult::success({{"settled", true}, {"task", task}});
}

liquid::runtime::task<ApiResult> confirm_ask_proposal(const json& input, ApiContext& context) {
    if (!context.workspaces || !context.store) {
        co_return unavailable();
    }
    if (context.acting_user_id.empty()) {
        co_return ApiResult::failure("unauthorized", "a conversation needs a person");
    }
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const std::string conversation_id = input.at("conversation_id");
    const std::string message_id = input.at("message_id");
    const auto index = input.at("index").get<std::size_t>();
    auto messages = control::settled(
        co_await conversation_messages(context, customer_id, workspace_id, conversation_id));
    if (!messages) {
        co_return zetta_failure(messages.error());
    }
    // Only a proposal lake stored for this person's conversation can be run.
    json proposal;
    for (const control::AskMessage& message : *messages) {
        const json proposals = message.payload.is_object()
                                   ? message.payload.value("proposals", json::array())
                                   : json::array();
        if (message.id == message_id && proposals.is_array() && index < proposals.size()) {
            proposal = proposals[index];
        }
    }
    if (!proposal.is_object() || !proposal.value("capability", json()).is_string()) {
        co_return ApiResult::failure(
            "not_found", "That proposal is not in this conversation.", "/message_id");
    }
    const std::string capability = proposal.value("capability", std::string());
    // Only the exact call the person was shown runs (lake#942): the capability and input Lake
    // derived from the stored proposal, named by the digest get_ask_conversation answered with it,
    // never the model's `impact`. A proposal Lake cannot state as a call cannot be confirmed.
    const auto call = proposal_call(context, proposal, customer_id, workspace_id);
    if (!call) {
        co_return ApiResult::failure("invalid_input",
                                     "Lake cannot show exactly what this proposal runs, so it "
                                     "cannot be confirmed. Ask Orama again.",
                                     "/index");
    }
    if (input.at("input_sha256") != call->at("input_sha256")) {
        co_return ApiResult::failure(
            "conflict",
            "This is not the call you were shown; read the conversation again and review it.",
            "/input_sha256");
    }
    context.audit_details = {{"ask_confirm", true},
                             {"conversation_id", conversation_id},
                             {"capability", capability},
                             {"input_sha256", call->at("input_sha256")}};
    // A proposal runs in the conversation's account and workspace when its input names none (an
    // MCP connection's call_mcp_tool from Ask Orama, lake#652); proposal_call filled them.
    const auto target = context.registry->find(capability);
    const json& proposed = call->at("input");
    ApiResult done = called(co_await call_capability(context, capability, proposed));
    // A Quickstart model is said as what started and what comes next (lake#1204); a training the
    // engine refused is a failure, in plain words, so Ask Orama proposes the corrected change.
    std::optional<quickstart_columns::Outcome> quickstart;
    if (capability == "train_quickstart" && done.ok) {
        quickstart = quickstart_columns::confirm_outcome(done.value);
        if (quickstart && !quickstart->ok) {
            done = ApiResult::failure("training_failed", quickstart->text);
        }
    }
    context.audit_details["ok"] = done.ok;
    // Input the capability refused is said plainly, naming what is missing (lake#794). The stored
    // outcome keeps the error's code and whether pressing Try again could help: only a refusal
    // that may pass later with the same input (confirm_retry_helps). Any other refusal fails the
    // same way every time, so the person is told to ask Ask Orama again instead.
    const bool retry = !done.ok && confirm_retry_helps(done.error.code);
    std::string why;
    if (!done.ok) {
        if (done.error.code == "invalid_input" && !done.error.path.empty()) {
            why = proposal_problem_text(target.get(), capability, done.error, proposed);
        } else {
            why = done.error.message;
            if (!why.empty() && why.back() != '.') {
                why += '.';
            }
        }
        if (quickstart) {
            // The Quickstart's own words already say what to do next.
        } else if (!retry) {
            why += " Ask Orama again with what to use and it will propose the change again.";
        }
    }
    json recorded{{"agent", "agent-ask-orama"},
                  {"confirmed", {{"message_id", message_id}, {"index", std::to_string(index)}}},
                  {"ok", done.ok}};
    if (!done.ok) {
        recorded["error"] = {{"code", done.error.code}, {"message", why}};
        recorded["retry"] = retry;
    }
    auto outcome = control::settled(co_await append_message(context,
                                                            customer_id,
                                                            workspace_id,
                                                            conversation_id,
                                                            "assistant",
                                                            done.ok && quickstart
                                                                ? quickstart->text
                                                            : done.ok
                                                                ? "Done: " + capability + "."
                                                                : "That did not work: " + why,
                                                            std::move(recorded)));
    if (!outcome) {
        co_return ApiResult::failure(outcome.error().code,
                                     "The conversation could not be saved: " +
                                         outcome.error().message);
    }
    json out{{"ok", done.ok}, {"message_id", outcome->id}, {"capability", capability}};
    if (!done.ok) {
        out["error"] = why;
        out["code"] = done.error.code;
        out["retry"] = retry;
    }
    co_return ApiResult::success(std::move(out));
}

liquid::runtime::task<ApiResult> approve_agent_proposal(const json& input, ApiContext& context) {
    if (!context.workspaces || !context.store) {
        co_return unavailable();
    }
    if (context.acting_user_id.empty()) {
        co_return ApiResult::failure("unauthorized", "a conversation needs a person");
    }
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const std::string conversation_id = input.at("conversation_id");
    const std::string message_id = input.at("message_id");
    auto messages = control::settled(
        co_await conversation_messages(context, customer_id, workspace_id, conversation_id));
    if (!messages) {
        co_return zetta_failure(messages.error());
    }
    // Only the exact proposal the person was shown, without defects.
    json answer;
    for (const control::AskMessage& message : *messages) {
        if (message.id == message_id && message.payload.is_object()) {
            answer = message.payload.value("answer", json::object());
        }
    }
    const json proposal = answer.is_object() ? answer.value("proposal", json()) : json();
    if (!proposal.is_object()) {
        co_return ApiResult::failure(
            "not_found", "That proposal is not in this conversation.", "/message_id");
    }
    if (agent_proposal_digest(proposal) != input.at("proposal_sha256").get<std::string>()) {
        co_return ApiResult::failure("conflict",
                                     "The proposal changed since you saw it; review it again.",
                                     "/proposal_sha256");
    }
    const json defects = answer.value("defects", json::array());
    if (!defects.empty()) {
        co_return ApiResult::failure("invalid_input", "This proposal still has defects.");
    }
    // The same capability REST and MCP call.
    const ApiResult created = called(co_await call_capability(
        context,
        "create_agent_from_proposal",
        {{"customer_id", customer_id}, {"proposal", proposal}, {"defects", defects}}));
    if (!created.ok) {
        static_cast<void>(control::settled(
            co_await append_message(context,
                                    customer_id,
                                    workspace_id,
                                    conversation_id,
                                    "assistant",
                                    "I could not create it: " + created.error.message,
                                    {{"created", false}})));
        co_return created;
    }
    const std::string agent_id = created.value.value("agent_id", std::string());
    const std::string name = proposal.value("agent", json::object()).value("name", std::string());
    std::string note = "Created " + name + ".";
    json out{{"agent_id", agent_id}, {"name", name}};
    if (created.value.contains("scheduled")) {
        if (created.value.value("scheduled", false)) {
            note += " Its trigger is scheduled.";
        } else {
            const std::string why = created.value.value("schedule_error", std::string());
            note += " Its trigger was not scheduled: " + why;
            out["schedule_error"] = why;
        }
    }
    if (created.value.contains("check_launch_id")) {
        note += " A first check run has started.";
        out["check_launch_id"] = created.value.value("check_launch_id", std::string());
    }
    auto stored =
        control::settled(co_await append_message(context,
                                                 customer_id,
                                                 workspace_id,
                                                 conversation_id,
                                                 "assistant",
                                                 std::move(note),
                                                 {{"created", true}, {"agent_id", agent_id}}));
    if (!stored) {
        co_return ApiResult::failure(stored.error().code,
                                     "The agent was created, but the conversation could not be "
                                     "saved: " +
                                         stored.error().message);
    }
    co_return ApiResult::success(std::move(out));
}

// ---- The project page's own reads and its compile, as capabilities (lake#561) ----

liquid::runtime::task<ApiResult> latest_agent_task(const json& input, ApiContext& context) {
    if (!context.workspaces || !context.store) {
        co_return unavailable();
    }
    const std::string subject = input.at("subject");
    if (!subject.starts_with("build:") && !subject.starts_with("solution:")) {
        co_return ApiResult::failure(
            "invalid_input", "subject must be build:<project> or solution:<project>", "/subject");
    }
    const auto row = context.store->latest_agent_task(
        input.at("customer_id"), subject, input.at("workspace_id"));
    if (!row) {
        if (row.error().code == "not_found") {
            co_return ApiResult::success({{"task", nullptr}});
        }
        co_return ApiResult::failure(row.error().code, row.error().message);
    }
    // Read as get_agent_task reads it: a finished build is applied to the project once.
    const ApiResult followed = called(
        co_await call_capability(context,
                                 "get_agent_task",
                                 {{"customer_id", input.at("customer_id")}, {"task_id", row->id}}));
    if (!followed.ok) {
        // Another member's task is theirs to follow.
        if (followed.error.code == "not_found") {
            co_return ApiResult::success({{"task", nullptr}});
        }
        co_return followed;
    }
    co_return ApiResult::success({{"task", followed.value}});
}

// The pipeline drawn as the pipeline page draws it: the latest run of this pipeline colours the
// steps it ran, and every step links to the page with that step selected.
liquid::runtime::task<ApiResult> render_pipeline(const json& input, ApiContext& context) {
    if (!context.workspaces || !context.store) {
        co_return unavailable();
    }
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const std::string project = input.at("project");
    const std::string graph_name = input.at("graph");
    ApiResult read = called(
        co_await call_capability(context,
                                 "workspace_query",
                                 {{"customer_id", customer_id},
                                  {"capability", "get_graph"},
                                  {"input", {{"project", project}, {"graph", graph_name}}}}));
    if (!read.ok) {
        co_return read;
    }
    auto parsed = co_await run_blocking(
        [value = std::move(read.value)] { return pipeline::from_json(value); });
    if (!parsed) {
        co_return ApiResult::failure("busy", parsed.error().message);
    }
    auto graph = std::move(*parsed);
    if (!graph) {
        co_return ApiResult::failure("invalid_input",
                                     "the pipeline cannot be drawn: " + graph.error());
    }
    std::map<std::string, std::string> status;
    const ApiResult runs = called(co_await call_capability(
        context,
        "workspace_query",
        {{"customer_id", customer_id}, {"capability", "list_runs"}, {"input", json::object()}}));
    if (runs.ok) {
        json latest;
        for (const json& run : runs.value.value("runs", json::array())) {
            const std::string of = run.value("graph", std::string());
            if ((of == graph_name || of == graph->graph_id) &&
                (!latest.is_object() ||
                 run.value("created", std::string()) >= latest.value("created", std::string()))) {
                latest = run;
            }
        }
        if (latest.is_object()) {
            for (const json& step :
                 latest.value("lineage", json::object()).value("steps", json::array())) {
                if (step.is_string()) {
                    status[step.get<std::string>()] = latest.value("status", std::string());
                }
            }
        }
    }
    const std::string base = "/w/" + oauth_url_encode(workspace_id) +
                             "/design/pipeline?customer_id=" + oauth_url_encode(customer_id) +
                             "&project=" + oauth_url_encode(project) +
                             "&graph=" + oauth_url_encode(graph->name) + "&step=";
    co_return co_await blocking_call(
        [graph = std::move(*graph),
         status = std::move(status),
         selected = input.value("step", std::string()),
         base] {
            return ApiResult::success(
                {{"svg", pipeline::render_svg(graph, status, selected, base)}});
        },
        [](const std::string& message) { return ApiResult::failure("busy", message); });
}

} // namespace

void register_conversation_capabilities(Registry& registry) {
    const json page_schema = {{"type", "object"},
                              {"properties", {{"path", {{"type", "string"}}}}},
                              {"additionalProperties", {{"type", "string"}}}};
    registry.add(Capability{
        .name = "send_ask_message",
        .title = "Send a message to Ask Orama",
        .description =
            "Say something in one of your conversations with Ask Orama (`kind` ask, the "
            "default; `agent` picks another chat agent) or the Agent Factory (`kind` factory, "
            "Agent Creator Agent): Lake stores your message in the workspace's conversation, then asks "
            "the agent. A library agent answers in the background: follow `task_id` with "
            "get_agent_task, then call settle_ask_turn {conversation_id, task_id, kind} once it "
            "finished, because the answer is saved in the conversation only by that call (the "
            "web app makes it; an API or MCP client must too). An agent of the account "
            "answers at once, stored as `reply_id`. Without `conversation_id` a new conversation "
            "starts. `page` names the page it was asked from, by its path and query values.",
        .input_schema = customer_input_schema({{"conversation_id", {{"type", "string"}}},
                                               {"kind", conversation_kind_schema()},
                                               {"agent", {{"type", "string"}}},
                                               {"message", {{"type", "string"}, {"minLength", 1}}},
                                               {"page", page_schema}},
                                              json::array({"message"})),
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"conversation_id", {{"type", "string"}}},
                            {"task_id", {{"type", "string"}}},
                            {"message_id", {{"type", "string"}}},
                            {"reply_id", {{"type", "string"}}}}},
                          {"required", json::array({"conversation_id", "message_id"})},
                          {"additionalProperties", false}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = send_ask_message});
    registry.add(Capability{
        .name = "settle_ask_turn",
        .title = "Store a finished agent turn",
        .description =
            "Once the background turn `task_id` of your conversation finished, store the "
            "agent's answer in the conversation, exactly once (a second call changes nothing). "
            "While the turn is pending or running nothing is written and `settled` is false. "
            "`task` is the turn as get_agent_task reads it.",
        .input_schema = customer_input_schema({{"conversation_id", {{"type", "string"}}},
                                               {"task_id", {{"type", "string"}}},
                                               {"kind", conversation_kind_schema()}},
                                              json::array({"conversation_id", "task_id", "kind"})),
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"settled", {{"type", "boolean"}}},
                            {"task", agent_task_value_schema()}}},
                          {"required", json::array({"settled", "task"})},
                          {"additionalProperties", false}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = settle_ask_turn});
    registry.add(Capability{
        .name = "confirm_ask_proposal",
        .title = "Confirm an Ask Orama proposal",
        .description =
            "Run proposal `index` of the Ask Orama message `message_id` in your conversation, as "
            "you: only a proposal Lake stored there can be run, and only as the exact call "
            "get_ask_conversation answered for it in `proposal_calls` (capability and input), "
            "named by its `input_sha256`; another digest is refused with `conflict` and nothing "
            "runs. The outcome is stored in the conversation (`message_id` is that outcome "
            "message) and answered with `ok`; a capability that refused is `ok: false` with its "
            "`error`.",
        .input_schema = customer_input_schema(
            {{"conversation_id", {{"type", "string"}}},
             {"message_id", {{"type", "string"}}},
             {"index", {{"type", "integer"}, {"minimum", 0}}},
             {"input_sha256",
              {{"type", "string"},
               {"description",
                "the input_sha256 of the proposal's entry in get_ask_conversation's "
                "proposal_calls: the exact call you reviewed"}}}},
            json::array({"conversation_id", "message_id", "index", "input_sha256"})),
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"ok", {{"type", "boolean"}}},
                            {"message_id", {{"type", "string"}}},
                            {"capability", {{"type", "string"}}},
                            {"error", {{"type", "string"}}}}},
                          {"required", json::array({"ok", "message_id", "capability"})},
                          {"additionalProperties", false}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = confirm_ask_proposal});
    registry.add(Capability{
        .name = "approve_agent_proposal",
        .title = "Approve an Agent Factory proposal",
        .description =
            "Create the agent Agent Creator Agent proposed in message `message_id` of your Agent "
            "Factory conversation, only when `proposal_sha256` is the digest stored with that "
            "proposal (the one you reviewed) and it has no defects, through "
            "create_agent_from_proposal. The outcome is stored in the conversation.",
        .input_schema = customer_input_schema(
            {{"conversation_id", {{"type", "string"}}},
             {"message_id", {{"type", "string"}}},
             {"proposal_sha256", {{"type", "string"}}}},
            json::array({"conversation_id", "message_id", "proposal_sha256"})),
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"agent_id", {{"type", "string"}}},
                            {"name", {{"type", "string"}}},
                            {"schedule_error", {{"type", "string"}}},
                            {"check_launch_id", {{"type", "string"}}}}},
                          {"required", json::array({"agent_id", "name"})},
                          {"additionalProperties", false}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = approve_agent_proposal});
    registry.add(Capability{
        .name = "latest_agent_task",
        .title = "A project's latest background task",
        .description =
            "The newest background task of `subject` in this workspace: `build:<project>` is the "
            "Project Agent's pipeline design for a project, `solution:<project>` its solution "
            "foundation proposal. Read as get_agent_task reads it (a finished design is applied "
            "to the project once); `task` is null when there is none you can follow.",
        .input_schema =
            customer_input_schema({{"subject", {{"type", "string"}}}}, json::array({"subject"})),
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"task",
                             [] {
                                 json schema = agent_task_value_schema();
                                 schema["type"] = json::array({"object", "null"});
                                 return schema;
                             }()}}},
                          {"required", json::array({"task"})},
                          {"additionalProperties", false}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = latest_agent_task});
    registry.add(Capability{
        .name = "compile_pipeline",
        .title = "Build a pipeline from a description",
        .description =
            "The project page's Build a Pipeline: capture `intent` as the project's intent, "
            "compile it (with `methodology`, that methodology's own pipeline), and, with "
            "`dataset` (\"<database>.<table>\"), point the new pipeline's first connect step at "
            "it. The dataset's source and table, and the optional `label_column` (what it "
            "predicts) and `entity_column` (what each row is about), are passed to the compiler "
            "as hints, so the steps are named from the data (\"Load subscriptions\", \"Train "
            "downgraded model\"). The pipeline is named `name`, else a short title from the "
            "intent (\"Predict which customers will churn, with the reasons\" is \"Customers "
            "churn\"), numbered \" (2)\" when the project has one by that name; the intent stays "
            "the project's description. Answers the new pipeline's name as `graph` and, from an "
            "engine that says so, the `template` its steps started from. With `create_project` it "
            "creates the project first (its description the intent), so a new project's "
            "pipeline is one call: a project that already has the name is refused as a "
            "conflict, and the answer carries `project_created`.",
        .input_schema = customer_input_schema({{"project", {{"type", "string"}, {"minLength", 1}}},
                                               {"intent", {{"type", "string"}, {"minLength", 1}}},
                                               {"dataset", {{"type", "string"}}},
                                               {"label_column", {{"type", "string"}}},
                                               {"entity_column", {{"type", "string"}}},
                                               {"methodology", {{"type", "string"}}},
                                               {"name", {{"type", "string"}}},
                                               {"create_project",
                                                {{"type", "boolean"},
                                                 {"description",
                                                  "create the project first, described by the "
                                                  "intent; refused when a project has this name"}}}},
                                              json::array({"project", "intent"})),
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"graph", {{"type", "string"}}},
                            {"template", {{"type", "string"}}},
                            {"project_created", {{"type", "boolean"}}}}},
                          {"required", json::array({"graph"})},
                          {"additionalProperties", false}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces || !context.store) {
                co_return unavailable();
            }
            co_return called(co_await compile_pipeline(
                context,
                input.at("customer_id"),
                input.at("project"),
                input.at("intent"),
                input.value("dataset", std::string()),
                nullptr,
                input.value("methodology", std::string()),
                {{"label_column", input.value("label_column", std::string())},
                 {"entity_column", input.value("entity_column", std::string())}},
                input.value("name", std::string()),
                input.value("create_project", false)));
        }});
    registry.add(Capability{
        .name = "render_pipeline",
        .title = "Draw a pipeline",
        .description =
            "A pipeline graph as inline SVG, drawn as the pipeline page draws it: one column per "
            "stage, data-flow arrows solid and control arrows dashed, the steps the latest run "
            "of this pipeline ran coloured by its outcome, `step` outlined. Every step links to "
            "the pipeline page with that step selected. The markup is escaped by Lake.",
        .input_schema = customer_input_schema({{"project", {{"type", "string"}}},
                                               {"graph", {{"type", "string"}}},
                                               {"step", {{"type", "string"}}}},
                                              json::array({"project", "graph"})),
        .output_schema = {{"type", "object"},
                          {"properties", {{"svg", {{"type", "string"}}}}},
                          {"required", json::array({"svg"})},
                          {"additionalProperties", false}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = render_pipeline});
}

void register_agent_run_capabilities(Registry& registry) {
    const auto customer_agent_schema = [](json properties, json required) {
        return customer_input_schema(std::move(properties), std::move(required));
    };
    registry.add(Capability{
        .name = "launch_agent",
        .title = "Run an agent",
        .description = "Run a user-created Python agent on agent-runtime: `job` runs it once "
                       "(unattended), `service` keeps it up for conversations. The operator runs "
                       "the launch; list_agent_launches follows it.",
        .input_schema =
            customer_agent_schema({{"agent_id", {{"type", "string"}}},
                                   {"mode", {{"type", "string"}, {"enum", {"job", "service"}}}},
                                   {"input", {{"description", "what a job is asked to do"}}}},
                                  json::array({"agent_id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces || !context.store) {
                co_return unavailable();
            }
            const std::string customer_id = input.at("customer_id");
            const std::string agent_id = input.at("agent_id");
            auto agent = answered(
                co_await zetta_call(context, customer_id, "get_agent", {{"id", agent_id}}));
            if (!agent) {
                co_return agent.error();
            }
            if (agent->value("runtime", std::string("native")) != "python") {
                co_return ApiResult::failure(
                    "invalid_input",
                    "only Python agents run on agent-runtime; a native agent runs in the "
                    "workspace (talk to it with agent_turn)");
            }
            const std::string mode = input.value("mode", std::string("job"));
            agent_audit(
                context, "agent.launch", customer_id, {{"agent_id", agent_id}, {"mode", mode}});
            auto launched = context.store->create_agent_launch(
                control::NewAgentLaunch{.customer_id = customer_id,
                                        .agent_id = agent_id,
                                        .agent_name = agent->value("name", std::string()),
                                        .mode = mode,
                                        // A run a stream message started says so (lake#677).
                                        .trigger = context.authentication_method ==
                                                               "agent_stream_trigger" ||
                                                           context.authentication_method ==
                                                               "mcp_connection_stream"
                                                       ? "stream"
                                                       : "manual",
                                        .input = input.value("input", json(nullptr)),
                                        .scopes = control::agent_launch_scopes_for(*agent),
                                        .created_by_user_id = context.acting_user_id,
                                        .created_by_taap_id = context.acting_identity,
                                        .workspace_id = input.at("workspace_id")});
            if (!launched) {
                co_return ApiResult::failure(launched.error().code, launched.error().message);
            }
            co_return ApiResult::success(launch_json(*launched));
        }});
    registry.add(Capability{
        .name = "list_agent_launches",
        .title = "List agent runs",
        .description = "The runs of the account's agents (or one agent's), newest first, with the "
                       "operator's phase and each finished job's result.",
        .input_schema = customer_agent_schema(
            {{"agent_id", {{"type", "string"}}}, {"limit", {{"type", "integer"}}}}, json::array()),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        // Like every list_ capability it authorizes itself: read on the customer.
        .authorizes_candidates = true,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return unavailable();
            }
            if (auto refused = authorize_customer_read(input, context)) {
                return *refused;
            }
            const std::string customer_id = input.at("customer_id");
            const std::string workspace_id = input.at("workspace_id");
            const std::string agent_id = input.value("agent_id", std::string());
            const int limit = input.value("limit", 50);
            // Only this workspace's runs: another workspace's agents are not in this Zetta.
            auto listed =
                context.store->list_agent_launches(customer_id, agent_id, limit, workspace_id);
            if (!listed) {
                return ApiResult::failure(listed.error().code, listed.error().message);
            }
            // The runs the schedules' CronJobs started sit beside the launches, newest first.
            auto scheduled =
                context.store->list_agent_cron_runs(customer_id, agent_id, limit, workspace_id);
            if (!scheduled) {
                return ApiResult::failure(scheduled.error().code, scheduled.error().message);
            }
            std::vector<control::AgentLaunch> runs = std::move(*listed);
            runs.insert(runs.end(), scheduled->begin(), scheduled->end());
            std::ranges::stable_sort(runs, [](const auto& left, const auto& right) {
                return left.created_at > right.created_at;
            });
            if (limit > 0 && static_cast<int>(runs.size()) > limit) {
                runs.resize(static_cast<std::size_t>(limit));
            }
            json launches = json::array();
            for (const control::AgentLaunch& launch : runs) {
                launches.push_back(launch_json(launch));
            }
            return ApiResult::success({{"launches", std::move(launches)}});
        }});
    registry.add(Capability{
        .name = "stop_agent_launch",
        .title = "Stop an agent run",
        .description = "Stop a run: the operator removes its Job or Deployment. A finished job "
                       "is left as it is.",
        .input_schema = customer_agent_schema({{"launch_id", {{"type", "string"}}}},
                                              json::array({"launch_id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return unavailable();
            }
            const std::string customer_id = input.at("customer_id");
            auto launch = context.store->get_agent_launch(input.at("launch_id"));
            if (!launch || launch->customer_id != customer_id ||
                launch->workspace_id != input.at("workspace_id").get<std::string>()) {
                return ApiResult::failure("not_found", "no such run in this workspace");
            }
            agent_audit(context,
                        "agent.stop",
                        customer_id,
                        {{"launch_id", launch->id}, {"agent_id", launch->agent_id}});
            auto stopped = context.store->withdraw_agent_launch(launch->id);
            if (!stopped) {
                return ApiResult::failure(stopped.error().code, stopped.error().message);
            }
            return ApiResult::success(launch_json(*stopped));
        }});
    registry.add(Capability{
        .name = "schedule_agent",
        .title = "Schedule an agent",
        .description =
            "Add a cron schedule to a Python agent: it runs as a job on every tick, as whoever "
            "set the schedule. An agent may have several; each is its own Kubernetes CronJob, "
            "which the Orama operator runs. Give `cron` (five fields, or @hourly, @daily, "
            "@weekly, @monthly) or `trigger` in plain words (every N minutes, hours or days, "
            "hourly, daily, weekly, every day|weekday|<weekday> at HH:MM). At most once every "
            "five minutes; `time_zone` is an IANA zone (such as America/Toronto), Etc/UTC by "
            "default. An unknown zone or cron expression is refused before anything is stored.",
        .input_schema =
            customer_agent_schema({{"agent_id", {{"type", "string"}}},
                                   {"cron", {{"type", "string"}}},
                                   {"trigger", {{"type", "string"}}},
                                   {"time_zone", {{"type", "string"}}},
                                   {"input", {{"description", "what each run is asked to do"}}}},
                                  json::array({"agent_id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces || !context.store) {
                co_return unavailable();
            }
            const std::string customer_id = input.at("customer_id");
            const std::string agent_id = input.at("agent_id");
            const std::string trigger = input.value("trigger", std::string());
            std::string cron = input.value("cron", std::string());
            if (cron.empty() == trigger.empty()) {
                co_return ApiResult::failure(
                    "invalid_input", "give either cron or trigger", cron.empty() ? "" : "/cron");
            }
            if (cron.empty()) {
                const auto translated = control::trigger_cron(trigger);
                if (!translated) {
                    co_return ApiResult::failure("invalid_input", translated.error(), "/trigger");
                }
                cron = *translated;
            }
            const auto normalized = control::normalize_cron(cron);
            if (!normalized) {
                co_return ApiResult::failure("invalid_input", normalized.error(), "/cron");
            }
            // A CronJob takes only IANA names; refuse any other before a schedule exists
            // (lake#586), rather than let Kubernetes refuse it later.
            const auto zone =
                control::checked_time_zone(input.value("time_zone", std::string("Etc/UTC")));
            if (!zone) {
                co_return ApiResult::failure("invalid_input", zone.error(), "/time_zone");
            }
            const std::string& time_zone = *zone;
            auto agent = answered(
                co_await zetta_call(context, customer_id, "get_agent", {{"id", agent_id}}));
            if (!agent) {
                co_return agent.error();
            }
            if (agent->value("runtime", std::string("native")) != "python") {
                co_return ApiResult::failure("invalid_input",
                                             "only Python agents run unattended on a schedule");
            }
            json run_input = input.value("input", json(nullptr));
            if (run_input.is_null()) {
                run_input = {{"trigger", trigger.empty() ? *normalized : trigger}};
            }
            agent_audit(context,
                        "agent.schedule",
                        customer_id,
                        {{"agent_id", agent_id}, {"cron", *normalized}, {"time_zone", time_zone}});
            auto stored = context.store->create_agent_cron_schedule(
                control::NewAgentCronSchedule{.customer_id = customer_id,
                                              .agent_id = agent_id,
                                              .agent_name = agent->value("name", std::string()),
                                              .cron = *normalized,
                                              .time_zone = time_zone,
                                              .description = trigger,
                                              .input = std::move(run_input),
                                              .scopes = control::agent_launch_scopes_for(*agent),
                                              .created_by_user_id = context.acting_user_id,
                                              .created_by_taap_id = context.acting_identity,
                                              .workspace_id = input.at("workspace_id")});
            if (!stored) {
                co_return ApiResult::failure(stored.error().code, stored.error().message);
            }
            co_return ApiResult::success(schedule_json(*stored));
        }});
    registry.add(Capability{
        .name = "list_agent_schedules",
        .title = "List agent schedules",
        .description = "The cron schedules of the workspace's agents (or one agent's), oldest "
                       "first, each with its phase (Pending, Ready, Failed, Stopped), a message "
                       "that says why and what to do, and when it last fired.",
        .input_schema = customer_agent_schema({{"agent_id", {{"type", "string"}}}}, json::array()),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .authorizes_candidates = true,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return unavailable();
            }
            if (auto refused = authorize_customer_read(input, context)) {
                return *refused;
            }
            auto listed = context.store->list_agent_cron_schedules(
                input.at("customer_id"),
                input.value("agent_id", std::string()),
                input.at("workspace_id").get<std::string>());
            if (!listed) {
                return ApiResult::failure(listed.error().code, listed.error().message);
            }
            json schedules = json::array();
            for (const control::AgentCronSchedule& schedule : *listed) {
                schedules.push_back(schedule_json(schedule));
            }
            return ApiResult::success({{"schedules", std::move(schedules)}});
        }});
    registry.add(Capability{
        .name = "unschedule_agent",
        .title = "Remove an agent schedule",
        .description = "Remove one cron schedule: the operator deletes its CronJob, which stops "
                       "a run under way. Results already written stay in list_agent_launches.",
        .input_schema = customer_agent_schema({{"schedule_id", {{"type", "string"}}}},
                                              json::array({"schedule_id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return unavailable();
            }
            const std::string customer_id = input.at("customer_id");
            auto schedule = context.store->get_agent_cron_schedule(input.at("schedule_id"));
            if (!schedule || schedule->customer_id != customer_id ||
                schedule->workspace_id != input.at("workspace_id").get<std::string>()) {
                return ApiResult::failure("not_found", "no such schedule in this workspace");
            }
            agent_audit(context,
                        "agent.unschedule",
                        customer_id,
                        {{"schedule_id", schedule->id}, {"agent_id", schedule->agent_id}});
            auto removed = context.store->withdraw_agent_cron_schedule(schedule->id);
            if (!removed) {
                return ApiResult::failure(removed.error().code, removed.error().message);
            }
            return ApiResult::success(schedule_json(*removed));
        }});
    registry.add(Capability{
        .name = "get_agent_task",
        .title = "Follow an agent turn",
        .description = "One background agent turn (from agent_turn): its status (pending, "
                       "running, succeeded, failed) and, once it finished, the agent's answer.",
        .input_schema = customer_agent_schema({{"task_id", {{"type", "string"}}}},
                                              json::array({"task_id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return unavailable();
            }
            auto task = context.store->get_agent_task(input.at("task_id"));
            if (!task || task->customer_id != input.at("customer_id").get<std::string>() ||
                task->workspace_id != input.at("workspace_id").get<std::string>() ||
                (task->requested_by_user_id != context.acting_user_id &&
                 context.acting_identity != "admin")) {
                co_return ApiResult::failure("not_found", "no such task in this account");
            }
            // Its request, message and answer are a record in its workspace (lake#374, #375).
            if (auto loaded = control::settled(co_await load_agent_task_content(context, *task));
                !loaded) {
                co_return ApiResult::failure(loaded.error().code, loaded.error().message);
            }
            // The scan's Opportunities are kept the first time it is read finished; a workspace
            // that cannot take them now is retried on the next read.
            static_cast<void>(called(co_await settle_discover(context, *task)));
            json out = task_json(*task);
            // A project's pipeline design is applied once it is done, wherever it is read.
            if (const ApiResult built = called(co_await settle_build(context, *task)); !built.ok) {
                out["build_error"] = built.error.message;
            }
            co_return ApiResult::success(std::move(out));
        }});
    registry.add(Capability{
        .name = "get_app_conversation",
        .title = "Your conversation about an app",
        .description =
            "Your change conversation with the Application Agent about one app (lake#1240), "
            "oldest turn first: each turn as get_agent_task answers it (status, answer, a proposal "
            "and its proposal_sha256 to confirm with agent_turn) and `request`, what you wrote. "
            "Each person has their own conversation per app; change_app continues yours. "
            "`conversation_id` is empty and `turns` empty before your first change.",
        .input_schema =
            customer_agent_schema({{"app_id", {{"type", "string"}}}}, json::array({"app_id"})),
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"app_id", {{"type", "string"}}},
                            {"conversation_id", {{"type", "string"}}},
                            {"turns", {{"type", "array"}, {"items", {{"type", "object"}}}}},
                            {"truncated", {{"type", "boolean"}}}}}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store || !context.workspaces) {
                co_return unavailable();
            }
            const std::string customer_id = input.at("customer_id");
            const std::string workspace_id = input.at("workspace_id");
            const std::string app_id = input.at("app_id");
            const auto app = context.store->get_web_app(app_id);
            if (!app || app->customer_id != customer_id || app->workspace_id != workspace_id ||
                app->desired == "deleted") {
                co_return ApiResult::failure(
                    "not_found", "no such app in this workspace", "/app_id");
            }
            auto records = workspace_records(context, customer_id, workspace_id);
            if (!records) {
                co_return ApiResult::failure(
                    records.error().code, records.error().message, "/workspace_id");
            }
            auto repair = control::settled(
                co_await control::get_resource_repair(*records, customer_id, "app", app_id));
            if (!repair) {
                co_return ApiResult::failure(repair.error().code, repair.error().message);
            }
            const std::string user = context.acting_user_id;
            const auto latest_of = [&](const std::string& conversation) {
                return context.store->latest_agent_conversation_task(
                    customer_id, conversation, "agent-application", "change", workspace_id);
            };
            const std::string conversation = control::person_change_conversation(
                *repair, user, [&](const std::string& candidate) {
                    const auto latest = latest_of(candidate);
                    return latest && !user.empty() && latest->requested_by_user_id == user;
                });
            json out{
                {"app_id", app_id}, {"conversation_id", conversation}, {"turns", json::array()}};
            if (conversation.empty()) {
                co_return ApiResult::success(std::move(out));
            }
            auto latest = latest_of(conversation);
            if (!latest) {
                if (latest.error().code == "not_found") {
                    co_return ApiResult::success(std::move(out));
                }
                co_return ApiResult::failure(latest.error().code, latest.error().message);
            }
            if (latest->requested_by_user_id != user && context.acting_identity != "admin") {
                co_return ApiResult::success(std::move(out));
            }
            if (auto loaded = control::settled(co_await load_agent_task_content(context, *latest));
                !loaded) {
                co_return ApiResult::failure(loaded.error().code, loaded.error().message);
            }
            auto chained =
                co_await conversation_chain(context, std::move(*latest), app_conversation_turns);
            if (!chained) {
                co_return ApiResult::failure("store_error", chained.error().message);
            }
            const std::vector<control::AgentTask>& turns = *chained;
            for (auto turn = turns.rbegin(); turn != turns.rend(); ++turn) {
                json item = task_json(*turn);
                item["request"] =
                    turn->body.is_object() ? turn->body.value("message", std::string()) : "";
                const json handed =
                    turn->body.is_object() ? turn->body.value("context", json::object()) : json();
                item["confirmation"] = handed.is_object() && handed.contains("confirmed_proposal");
                out["turns"].push_back(std::move(item));
            }
            out["truncated"] = turns.size() >= app_conversation_turns;
            co_return ApiResult::success(std::move(out));
        }});
    registry.add(Capability{
        .name = "list_opportunities",
        .title = "List Opportunities",
        .description =
            "What Auto-Suggest Agent recommended for the account's data in this workspace, kept "
            "across scans, newest scan first: each Opportunity's intent, catalogue "
            "solution, methodology, why the data supports it, and its status (new, "
            "project, dismissed, or not_yet when Auto-Suggest Agent declined it). "
            "Opportunities live in the workspace Zetta of the workspace their scan ran "
            "in; another workspace of the account never lists them. `last_scan` is the "
            "workspace's newest scan, whoever started it ({task_id, status pending, "
            "running, succeeded or failed, started_at, finished_at, message}; null "
            "before the first): a page follows a running scan with it, and a finished "
            "scan's Opportunities are listed without its task id. `task_id` (optional) "
            "also settles that scan of this workspace.",
        .input_schema = customer_agent_schema({{"task_id", {{"type", "string"}}}}, json::array()),
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"opportunities", {{"type", "array"}, {"items", {{"type", "object"}}}}},
                            {"last_scan",
                             {{"type", json::array({"object", "null"})},
                              {"properties",
                               {{"task_id", {{"type", "string"}}},
                                {"status", {{"type", "string"}}},
                                {"started_at", {{"type", "string"}}},
                                {"finished_at", {{"type", json::array({"string", "null"})}}},
                                {"message", {{"type", "string"}}}}}}}}},
                          {"required", json::array({"opportunities", "last_scan"})}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .authorizes_candidates = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return unavailable();
            }
            if (auto refused = authorize_customer_read(input, context)) {
                co_return *refused;
            }
            const std::string customer_id = input.at("customer_id");
            const std::string workspace_id = input.at("workspace_id");
            auto records = workspace_records(context, customer_id, workspace_id);
            if (!records) {
                co_return ApiResult::failure(records.error().code, records.error().message,
                                             records.error().path);
            }
            auto rows =
                control::settled(co_await control::list_opportunities(*records, customer_id));
            if (!rows) {
                co_return ApiResult::failure(rows.error().code, rows.error().message,
                                             rows.error().path);
            }
            // A scan of this workspace that finished and is not saved yet is saved now, once.
            if (const std::string id = input.value("task_id", std::string());
                !id.empty() && std::ranges::none_of(*rows, [&](const control::Opportunity& row) {
                    return row.task_id == id;
                })) {
                if (auto task = context.store->get_agent_task(id);
                    task && task->customer_id == customer_id &&
                    task->workspace_id == workspace_id &&
                    control::settled(co_await load_agent_task_content(context, *task))) {
                    const ApiResult settled = called(co_await settle_discover(context, *task));
                    if (settled.ok && settled.value.value("written", 0) > 0) {
                        rows = control::settled(
                            co_await control::list_opportunities(*records, customer_id));
                        if (!rows) {
                            co_return ApiResult::failure(rows.error().code, rows.error().message,
                                                         rows.error().path);
                        }
                    }
                }
            }
            // The workspace's newest scan, whoever started it (lake#1195): the page learns a
            // running or finished scan without its task id, and a finished scan that was not
            // saved when it settled (its workspace was unavailable then) is saved now, once.
            json last_scan = nullptr;
            auto latest = context.store->latest_agent_task(customer_id, "discover", workspace_id);
            if (!latest && latest.error().code != "not_found") {
                co_return ApiResult::failure(latest.error().code, latest.error().message);
            }
            if (latest && latest->agent == "agent-autosuggest") {
                // Its message is in its workspace (lake#375); without it the scan is still shown.
                const bool loaded =
                    control::settled(co_await load_agent_task_content(context, *latest))
                        .value_or(false);
                if (loaded && latest->phase == "Succeeded" &&
                    std::ranges::none_of(*rows, [&](const control::Opportunity& row) {
                        return row.task_id == latest->id;
                    })) {
                    const ApiResult settled = called(co_await settle_discover(context, *latest));
                    if (settled.ok && settled.value.value("written", 0) > 0) {
                        rows = control::settled(
                            co_await control::list_opportunities(*records, customer_id));
                        if (!rows) {
                            co_return ApiResult::failure(
                                rows.error().code, rows.error().message, rows.error().path);
                        }
                    }
                }
                last_scan = last_scan_json(*latest);
            }
            json out = json::array();
            for (const control::Opportunity& row : *rows) {
                out.push_back(opportunity_json(row));
            }
            co_return ApiResult::success(
                {{"opportunities", std::move(out)}, {"last_scan", std::move(last_scan)}});
        }});
    registry.add(Capability{
        .name = "dismiss_opportunity",
        .title = "Dismiss an Opportunity",
        .description = "Mark one Opportunity of this workspace as not wanted; it stays listed as "
                       "dismissed.",
        .input_schema = customer_agent_schema({{"opportunity_id", {{"type", "string"}}}},
                                              json::array({"opportunity_id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return unavailable();
            }
            const std::string customer_id = input.at("customer_id");
            agent_audit(context, "opportunity.dismiss", customer_id,
                        {{"opportunity_id", input.at("opportunity_id")},
                         {"workspace_id", input.at("workspace_id")}});
            auto records = workspace_records(context, customer_id, input.at("workspace_id"));
            if (!records) {
                co_return ApiResult::failure(records.error().code, records.error().message,
                                             records.error().path);
            }
            auto row = control::settled(co_await control::set_opportunity_status(
                *records, customer_id, input.at("opportunity_id"), "dismissed", ""));
            if (!row) {
                if (row.error().code == "not_found") {
                    co_return ApiResult::failure("not_found",
                                                 "no such Opportunity in this workspace");
                }
                co_return ApiResult::failure(row.error().code, row.error().message,
                                             row.error().path);
            }
            co_return ApiResult::success(opportunity_json(*row));
        }});
    registry.add(Capability{
        .name = "agent_turn",
        .title = "Talk to an agent",
        .description =
            "Send one message to any agent: a specialized agent of the Agent Library by its key "
            "(agent-ask-orama, agent-project, agent-application, agent-experiments, "
            "agent-autosuggest, agent-creator, agent-eval-generator for an eval, "
            "agent-autosuggest-corpus or agent-project-corpus for a training corpus) or an agent "
            "of this account by its id. A library agent's turn runs in the background: the answer "
            "is {task_id, status} at once, and get_agent_task follows it to its answer. A "
            "native agent answers from the workspace; a Python agent from its running service "
            "(started on first use). A workspace scan (agent-autosuggest, kind discover) while one is "
            "pending or running joins it: the answer is that scan's task with joined: true.",
        .input_schema = customer_agent_schema({{"agent", {{"type", "string"}}},
                                               {"message", {{"type", "string"}}},
                                               {"conversation_id", {{"type", "string"}}},
                                               {"context", {{"type", "object"}}},
                                               {"page", {{"type", "object"}}},
                                               {"kind", {{"type", "string"}}},
                                               {"proposal_task_id", {{"type", "string"}}},
                                               {"proposal_sha256", {{"type", "string"}}}},
                                              json::array({"agent", "message"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [&registry](const json& input,
                                     ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces || !context.store) {
                co_return unavailable();
            }
            const std::string customer_id = input.at("customer_id");
            const std::string agent = input.at("agent");
            const std::string message = input.at("message");
            std::string conversation_id = input.value("conversation_id", std::string());
            if (conversation_id.empty()) {
                conversation_id = control::new_uuid_v4();
            }
            const control::WorkspaceConfig& config = context.workspaces->config();
            agent_audit(context, "agent.turn", customer_id, {{"agent", agent}});
            json body{{"conversation_id", conversation_id},
                      {"workspace_id", input.at("workspace_id")},
                      {"message", message},
                      {"customer_id", customer_id},
                      {"page", input.value("page", json::object())}};

            if (const control::LibraryAgent* library = control::library_agent(agent)) {
                if (library->runs_as == "offline") {
                    co_return ApiResult::failure(
                        "invalid_input", std::string(library->name) + " is an offline tool");
                }
                const auto endpoint = config.agent_endpoints.find(agent);
                if (endpoint == config.agent_endpoints.end() || context.agent_caller == nullptr) {
                    co_return ApiResult::failure("store_unavailable",
                                                 std::string(library->name) + " is not running on "
                                                                              "this Lake");
                }
                std::vector<std::string> scopes{"workspace_query", "workspace_openapi"};
                std::string task_kind = input.value("kind", std::string("api"));
                if (agent == "agent-application") {
                    task_kind = application_turn_kind(input);
                }
                // A turn whose id lake must know before it is recorded (an auto-fix attempt).
                std::string planned_task_id;
                std::string lake_url = config.internal_url;
                auto prior = context.store->latest_agent_conversation_task(
                    customer_id, conversation_id, agent, task_kind, input.at("workspace_id"));
                if (prior) {
                    // What the earlier turn asked and answered is in its workspace (lake#374).
                    if (auto loaded =
                            control::settled(co_await load_agent_task_content(context, *prior));
                        !loaded) {
                        co_return ApiResult::failure(loaded.error().code, loaded.error().message);
                    }
                    if (!context.operator_credential &&
                        prior->requested_by_user_id != context.acting_user_id) {
                        co_return ApiResult::failure(
                            "not_found", "no earlier turn in this conversation");
                    }
                    body["prior_turn"] = {{"task_id", prior->id},
                                          {"message", prior->body.value("message", std::string())},
                                          {"answer", prior->result}};
                    if (agent == "agent-application" &&
                        (task_kind == "change" || task_kind == "repair")) {
                        // The whole thread, so the agent continues the conversation (lake#1240).
                        auto chained =
                            co_await conversation_chain(context, *prior, application_history_turns);
                        if (!chained) {
                            co_return ApiResult::failure("store_error", chained.error().message);
                        }
                        body["history"] = application_history(*chained);
                    }
                } else if (prior.error().code != "not_found") {
                    co_return ApiResult::failure(prior.error().code, prior.error().message);
                }
                if (agent == "agent-ask-orama") {
                    // Ask Orama reads everything the person may read and changes nothing itself:
                    // its key carries every read capability and agent_turn, never an action.
                    scopes = {"agent_turn", "workspace_openapi"};
                    for (const auto& capability : registry.list()) {
                        // Every read, including actions that only read (workspace_query).
                        if (capability->kind == CapabilityKind::Read ||
                            capability->action == "read") {
                            scopes.push_back(capability->name);
                        }
                    }
                    scopes.push_back("workspace_query");
                    // New Methodology's Ask Orama (lake#688): a first draft of a workspace
                    // methodology's files from the person's description (the message), with the
                    // name and target kind they chose. Nothing is written; draft_methodology
                    // {task_id} checks the files before the drawer offers them.
                    if (task_kind == "methodology_draft") {
                        body["kind"] = task_kind;
                        body["context"] = input.value("context", json::object());
                        if (!body["context"].is_object()) {
                            co_return ApiResult::failure(
                                "invalid_input", "context is an object", "/context");
                        }
                    } else if (const json sent = input.value("context", json());
                               sent.is_object() && !sent.empty()) {
                        // send_ask_message's `proposals`: what became of Ask Orama's earlier
                        // proposals in the conversation (lake#1194). Information for the turn,
                        // never authority: Ask Orama's key still carries no action.
                        body["context"] = sent;
                    }
                } else if (agent == "agent-application") {
                    task_kind = application_turn_kind(input);
                    scopes = {"workspace_query",
                              "workspace_openapi",
                              "workspace_sql",
                              "list_datasets",
                              "get_dataset",
                              "list_services",
                              "list_repositories",
                              "get_repository",
                              "repository_tree",
                              "repository_file",
                              "list_apps",
                              "get_app",
                              "app_logs",
                              "receive_app_events"};
                    body["context"] = input.value("context", json::object());
                    if (!body["context"].is_object()) {
                        co_return ApiResult::failure(
                            "invalid_input", "context is an object", "/context");
                    }
                    if (task_kind != "app") {
                        // Repair or change one existing app (lake#1207): the turn is handed the
                        // app and its repair record, and may write only that app's source and
                        // deployment, after the person confirms the diff (or, for repair, when
                        // the person turned auto-fix on and attempts remain).
                        auto prepared = called(co_await prepare_app_turn(
                            input, context, task_kind, body["context"], scopes, planned_task_id));
                        if (!prepared.ok) {
                            co_return prepared;
                        }
                        body["kind"] = task_kind;
                    } else if (body["context"].contains("confirmed_proposal")) {
                        const ApiResult confirmed =
                            called(co_await verified_confirmed_proposal(input, context, agent));
                        if (!confirmed.ok) {
                            co_return confirmed;
                        }
                        body["context"]["confirmed_proposal"] = confirmed.value;
                        scopes.push_back("create_repository");
                        scopes.push_back("commit_repository_files");
                        scopes.push_back("create_app");
                        // Apps whose dependencies are outside app-runtime's baked set are built
                        // first (build_app), then deployed with the built image.
                        scopes.push_back("build_app");
                        scopes.push_back("get_build");
                        scopes.push_back("receive_build_events");
                        scopes.push_back("build_logs");
                        scopes.push_back("deploy_app");
                    }
                } else if (agent == "agent-experiments") {
                    task_kind = input.value("kind", std::string("experiment"));
                    if (task_kind != "experiment" && task_kind != "experiment_status" &&
                        task_kind != "experiment_explain") {
                        co_return ApiResult::failure(
                            "invalid_input",
                            "an Experiments Agent turn kind is experiment, experiment_status or "
                            "experiment_explain",
                            "/kind");
                    }
                    body["kind"] = task_kind;
                    body["context"] = input.value("context", json::object());
                    // A lake caller names the experiment `experiment_id`; Experiments Agent reads
                    // the engine's `study` (lake#561).
                    if (body["context"].is_object() && body["context"].contains("experiment_id") &&
                        !body["context"].contains("study")) {
                        body["context"]["study"] = body["context"]["experiment_id"];
                    }
                    lake_url = config.internal_url + "/w/" + input.at("workspace_id").get<std::string>() +
                               "/workspaces/" + customer_id + "/zetta";
                    if (task_kind == "experiment") {
                        scopes.insert(scopes.end(),
                                      {"analyze_dataset",
                                       "plan_features",
                                       "list_methodologies",
                                       "validate_study",
                                       "get_solution"});
                        if (body["context"].contains("confirmed_proposal")) {
                            const ApiResult confirmed =
                                called(co_await verified_confirmed_proposal(input, context, agent));
                            if (!confirmed.ok) {
                                co_return confirmed;
                            }
                            body["context"]["confirmed_proposal"] = confirmed.value;
                            scopes.push_back("workspace_act");
                            scopes.push_back("create_study");
                            scopes.push_back("start_experiment");
                        }
                    } else if (task_kind == "experiment_status") {
                        scopes.insert(scopes.end(),
                                      {"get_study",
                                       "list_trials",
                                       "get_run_metrics",
                                       "receive_events",
                                       "workspace_act",
                                       "stop_trial",
                                       "stop_experiment_trial"});
                    } else {
                        scopes.insert(scopes.end(), {"get_study", "get_study_analysis"});
                        const json confirmed =
                            body["context"].value("confirmed_promotion", json());
                        // Only a person's turn carries a promotion confirmation (lake#989).
                        if (!context.credential_kind.starts_with("agent_") &&
                            confirmed.is_object() && confirmed.value("confirm", false) &&
                            confirmed.value("promotion", json()).is_object()) {
                            scopes.push_back("workspace_act");
                            scopes.push_back("promote_trial");
                        }
                    }
                }
                // The turn runs in the background (lake#269): lake records it, publishes it on
                // the agent bus and answers at once. The agent claims it through lake, which
                // replaces "{{credential}}" with a credential it hands over once and never stores
                // (docs/api/agent-bus.md).
                scopes.push_back("get_agent_task");
                body["lake_token"] = "{{credential}}";
                body["lake_url"] = lake_url;
                // The agents that read the person's workspace get lake's read-only path to it.
                if (agent == "agent-project" || agent == "agent-autosuggest" ||
                    agent == "agent-creator" || agent == "agent-project-corpus") {
                    body["zetta_base_url"] =
                        config.internal_url + "/w/" + input.at("workspace_id").get<std::string>() +
                        "/workspaces/" + customer_id + "/zetta";
                    body["zetta_token"] = "{{credential}}";
                }
                if (agent == "agent-project") {
                    scopes.push_back("agent_turn");
                    body["request"] = message;
                    if (prior) {
                        std::string prior_reply;
                        if (prior->result.is_object()) {
                            prior_reply = prior->result.value("reply", std::string());
                            const json prior_result =
                                prior->result.value("result", json::object());
                            if (prior_reply.empty() && prior_result.is_object()) {
                                prior_reply = prior_result.value("reply", std::string());
                            }
                        }
                        if (!prior_reply.empty()) {
                            body["request"] = "The Project Agent previously replied:\n" +
                                              prior_reply + "\n\nThe person's reply:\n" + message;
                        }
                    }
                    body["context"] = input.value("context", json::object());
                    // A request about an existing pipeline is a change request (lake#249): the
                    // agent is handed the graph as it is and answers with a proposal.
                    json& handed = body["context"];
                    if (!handed.is_object()) {
                        handed = json::object();
                    }
                    if (handed.value("project", std::string()) != "" &&
                        handed.value("graph", std::string()) != "" &&
                        !handed.contains("pipeline")) {
                        auto graph = answered(co_await zetta_call(
                            context,
                            customer_id,
                            "get_graph",
                            {{"project", handed["project"]}, {"graph", handed["graph"]}}));
                        if (!graph) {
                            co_return graph.error();
                        }
                        handed["pipeline"] = std::move(*graph);
                    }
                    // A change proposal is checked against the methodology's compiled graph
                    // (lake#438). The turn may run that one action, confined by
                    // project_agent_compile_check to lake's check graph on this project; every
                    // other workspace action stays refused, and the edits themselves are applied
                    // only by the person.
                    // A build turn ("build:<project>") gets the same confined check: the graph
                    // it stores is skipped by settle_build's pick of the project's pipeline.
                    if ((handed.value("project", std::string()) != "" &&
                         handed.value("graph", std::string()) != "") ||
                        task_kind.starts_with("build:")) {
                        scopes.push_back("workspace_act");
                        scopes.push_back("compile_intent");
                    }
                } else if (agent == "agent-eval-generator" || agent == "agent-autosuggest-corpus" ||
                           agent == "agent-project-corpus") {
                    body["request"] = message;
                } else if (agent == "agent-autosuggest") {
                    // Auto-Suggest Agent builds the workspace profile itself, with the task's
                    // read-only credential; lake reads nothing inside the request. Before it
                    // suggests, it also reads the account's Ship Apps and image builds, so an
                    // existing App can inform a proposal (agent-autosuggest's apps.py, lake#795):
                    // those reads, and none of the App or build actions it may only propose.
                    scopes.insert(scopes.end(),
                                  {"list_apps",
                                   "get_app",
                                   "receive_app_events",
                                   "list_builds",
                                   "get_build",
                                   "receive_build_events"});
                    if (const json profile =
                            input.value("context", json::object()).value("profile", json());
                        profile.is_object()) {
                        body["profile"] = profile;
                    }
                    body["request"] = message;
                }
                if (context.acting_user_id.empty()) {
                    co_return ApiResult::failure(
                        "unauthorized", "talking to " + agent + " needs a signed-in person");
                }
                if (context.agent_bus == nullptr) {
                    co_return ApiResult::failure("store_unavailable",
                                                 "the agent bus is not configured on this Lake");
                }
                const auto ready = control::settled(co_await context.workspaces->state(
                    customer_id, input.at("workspace_id")));
                if (!ready || !ready->ready()) {
                    co_return ApiResult::failure(ready ? "workspace_not_ready" : ready.error().code,
                        ready ? "workspace_id needs a Ready workspace Zetta" : ready.error().message,
                        "/workspace_id");
                }
                // A turn another agent asked for names the asking turn (lake#451), so its
                // progress is reported there too.
                // One scan of a workspace at a time (lake#1195): a second Scan while one is
                // pending or running joins it, so every tab and member follows the same scan.
                if (agent == "agent-autosuggest" && task_kind == "discover") {
                    auto running = context.store->latest_agent_task(
                        customer_id, "discover", input.at("workspace_id"));
                    if (!running && running.error().code != "not_found") {
                        co_return ApiResult::failure(running.error().code, running.error().message);
                    }
                    if (running && running->agent == agent && scan_running(*running)) {
                        json joined = task_json(*running);
                        joined["joined"] = true;
                        co_return ApiResult::success(std::move(joined));
                    }
                }
                const auto asking = calling_turn(context, customer_id, input.at("workspace_id"));
                if (asking) {
                    body["parent_task_id"] = asking->id;
                }
                // A turn an agent asks for never holds more than the asking agent does (LSR-013,
                // lake#922): delegation attenuates. Its scopes are the ones the caller's own
                // credential carries, plus reading its own task, which every turn needs.
                if (context.credential_kind.starts_with("agent_")) {
                    std::erase_if(scopes, [&](const std::string& scope) {
                        return scope != "get_agent_task" &&
                               std::ranges::find(context.scopes, scope) == context.scopes.end();
                    });
                }
                control::AgentTask asked{.id = planned_task_id.empty() ? control::new_uuid_v4()
                                                                       : planned_task_id,
                                         .customer_id = customer_id,
                                         .agent = agent,
                                         .kind = std::move(task_kind),
                                         .conversation_id = conversation_id,
                                         .workspace_id = input.at("workspace_id"),
                                         .parent_task_id = asking ? asking->id : std::string(),
                                         .body = std::move(body),
                                         .scopes = std::move(scopes),
                                         .requested_by_user_id = context.acting_user_id,
                                         .requested_by_taap_id = context.acting_identity};
                // The request goes to the task's workspace first (lake#374), so the dispatch row
                // never names a task whose request is missing; a row that then fails leaves an
                // unreachable record behind, never a task without its request.
                if (auto written =
                        control::settled(co_await write_agent_task_request(context, asked));
                    !written) {
                    co_return ApiResult::failure(written.error().code, written.error().message,
                                                 "/workspace_id");
                }
                auto task = context.store->create_agent_task(asked);
                if (!task) {
                    co_return ApiResult::failure(task.error().code, task.error().message);
                }
                task->body = std::move(asked.body);
                // A publish that fails is not the caller's failure: the task is recorded, and
                // lake's bus loop publishes an unclaimed task again.
                static_cast<void>(co_await context.agent_bus->publish_task(*task));
                if (asking) {
                    co_await report_on_asking_turn(
                        context,
                        asking->id,
                        control::agent_display_name(asking->agent) + " asked " +
                            std::string(library->name) + ": " + message,
                        {{"delegated_task_id", task->id}, {"delegated_agent", agent}});
                }
                co_return ApiResult::success(task_json(*task));
            }

            auto record =
                answered(co_await zetta_call(context, customer_id, "get_agent", {{"id", agent}}));
            if (!record) {
                co_return record.error();
            }
            const std::string runtime = record->value("runtime", std::string("native"));
            if (runtime == "native") {
                auto chatted = answered(co_await zetta_call(
                    context, customer_id, "chat", {{"agent", agent}, {"message", message}}));
                if (!chatted) {
                    co_return chatted.error();
                }
                json answer = std::move(*chatted);
                answer["agent"] = agent;
                co_return ApiResult::success(std::move(answer));
            }
            if (runtime != "python") {
                co_return ApiResult::failure("invalid_input", "this agent is not talked to here");
            }
            // A service launch acts with its creator's credential (lal.), so a person talks only
            // to a launch they started and never drives another's with that one's authority
            // (lake#932); without a person there is no launch to choose.
            if (context.acting_user_id.empty()) {
                co_return ApiResult::failure("unauthorized",
                                             "talking to " + agent + " needs a signed-in person");
            }
            auto launches = context.store->list_agent_launches(
                customer_id, agent, 20, input.at("workspace_id").get<std::string>());
            if (!launches) {
                co_return ApiResult::failure(launches.error().code, launches.error().message);
            }
            const auto live = std::ranges::find_if(*launches, [&](const control::AgentLaunch& l) {
                return l.mode == "service" && l.desired == "present" &&
                       l.created_by_user_id == context.acting_user_id;
            });
            if (live == launches->end()) {
                auto started = context.store->create_agent_launch(
                    control::NewAgentLaunch{.customer_id = customer_id,
                                            .agent_id = agent,
                                            .agent_name = record->value("name", std::string()),
                                            .mode = "service",
                                            .trigger = "conversation",
                                            .scopes = control::agent_launch_scopes_for(*record),
                                            .created_by_user_id = context.acting_user_id,
                                            .created_by_taap_id = context.acting_identity,
                                            .workspace_id = input.at("workspace_id")});
                if (!started) {
                    co_return ApiResult::failure(started.error().code, started.error().message);
                }
                co_return ApiResult::success(
                    {{"agent", agent},
                     {"status", "starting"},
                     {"launch_id", started->id},
                     {"reply", "The agent is starting. Send your message again in a moment."}});
            }
            if (live->phase != "Ready" || live->endpoint.empty() ||
                context.agent_caller == nullptr) {
                co_return ApiResult::success(
                    {{"agent", agent},
                     {"status", "starting"},
                     {"launch_id", live->id},
                     {"reply",
                      "The agent is still starting (" +
                          (live->phase.empty() ? std::string("pending") : live->phase) +
                          "). Send your message again in a moment."}});
            }
            // The launch's own token, never the Lake's (agent-runtime#14).
            auto reply = co_await context.agent_caller->turn(
                live->endpoint,
                control::agent_launch_service_token(config.agent_service_token, live->id),
                {{"conversation_id", body["conversation_id"]}, {"message", message}});
            if (!reply) {
                co_return ApiResult::failure("store_error", reply.error().message);
            }
            if (!*reply) {
                co_return ApiResult::failure(reply->error().code, reply->error().message);
            }
            json answer = std::move(**reply);
            if (!answer.is_object()) {
                co_return ApiResult::failure("engine_error", "the agent's answer is not an object");
            }
            answer["agent"] = agent;
            // The agent's own writing, read by type (lake#794): an `ok` or `error` of another type
            // is a failed turn with its text, never an exception.
            if (answer.value("ok", json(false)) == true) {
                co_return ApiResult::success(std::move(answer));
            }
            co_return ApiResult::failure(
                "engine_error",
                answer.contains("error") && !answer.at("error").is_null()
                    ? error_text(answer.at("error"))
                    : std::string("the agent failed"));
        }});
}

bool is_project_agent_check_graph(const nlohmann::json& graph) {
    if (!graph.is_object()) {
        return false;
    }
    const auto name = graph.find("name");
    return name != graph.end() && name->is_string() &&
           name->get<std::string>().starts_with(project_agent_check_prefix);
}

nlohmann::json without_project_agent_checks(const nlohmann::json& graphs) {
    if (!graphs.is_array()) {
        return graphs;
    }
    nlohmann::json kept = nlohmann::json::array();
    for (const nlohmann::json& graph : graphs) {
        if (!is_project_agent_check_graph(graph)) {
            kept.push_back(graph);
        }
    }
    return kept;
}

std::map<std::string, std::string> canonical_workspace_input(std::string_view capability,
                                                             json& input) {
    // The field each engine capability takes for the entity it addresses. Every other capability
    // is passed through as sent.
    static const std::map<std::string_view, std::string_view> takes = {
        {"get_run", "run_id"},
        {"get_run_report", "run_id"},
        {"get_run_metrics", "run"},
        {"get_model", "model_id"},
        {"model_health", "model_id"},
        {"expose_model", "model"},
        {"unexpose_model", "model"},
        {"score", "model"},
        {"explain", "model"},
        {"get_index", "index_id"},
        {"expose_index", "index"},
        {"unexpose_index", "index"},
        {"get_job", "job_id"},
    };
    std::map<std::string, std::string> renamed;
    const auto found = takes.find(capability);
    if (found == takes.end() || !input.is_object()) {
        return renamed;
    }
    const std::string field(found->second);
    const std::string other =
        field.ends_with("_id") ? field.substr(0, field.size() - 3) : field + "_id";
    if (!input.contains(field) && input.contains(other)) {
        input[field] = std::move(input[other]);
        input.erase(other);
        renamed[field] = other;
    }
    return renamed;
}

namespace {

// The first code point of a UTF-8 string and its length in bytes, or nullopt when the string
// does not start with a well-formed one.
std::optional<std::pair<char32_t, std::size_t>> first_code_point(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto lead = static_cast<unsigned char>(text[0]);
    const std::size_t size = lead < 0x80                ? 1
                             : (lead & 0xE0) == 0xC0 ? 2
                             : (lead & 0xF0) == 0xE0 ? 3
                             : (lead & 0xF8) == 0xF0 ? 4
                                                     : 0;
    if (size == 0 || text.size() < size) {
        return std::nullopt;
    }
    char32_t value = size == 1   ? lead
                     : size == 2 ? (lead & 0x1F)
                     : size == 3 ? (lead & 0x0F)
                                 : (lead & 0x07);
    for (std::size_t at = 1; at < size; ++at) {
        const auto next = static_cast<unsigned char>(text[at]);
        if ((next & 0xC0) != 0x80) {
            return std::nullopt;
        }
        value = (value << 6) | (next & 0x3F);
    }
    return std::pair{value, size};
}

std::string utf8_of(char32_t value) {
    std::string out;
    if (value < 0x80) {
        out += static_cast<char>(value);
    } else if (value < 0x800) {
        out += static_cast<char>(0xC0 | (value >> 6));
        out += static_cast<char>(0x80 | (value & 0x3F));
    } else if (value < 0x10000) {
        out += static_cast<char>(0xE0 | (value >> 12));
        out += static_cast<char>(0x80 | ((value >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (value & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (value >> 18));
        out += static_cast<char>(0x80 | ((value >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((value >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (value & 0x3F));
    }
    return out;
}

// The capital of a lowercase letter of Latin-1, Latin Extended-A, Greek or Cyrillic; any other
// code point as it is. The web app's `upperFirst` maps the same letters.
char32_t title_capital(char32_t c) {
    if (c >= U'a' && c <= U'z') {
        return c - 0x20;
    }
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) {
        return c - 0x20;
    }
    if (c == 0xFF) {
        return 0x178;
    }
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) {
        return c % 2 == 1 ? c - 1 : c;
    }
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) {
        return c % 2 == 0 ? c - 1 : c;
    }
    if (c >= 0x3B1 && c <= 0x3C9) {
        return c == 0x3C2 ? 0x3A3 : c - 0x20;
    }
    if (c >= 0x430 && c <= 0x44F) {
        return c - 0x20;
    }
    if (c >= 0x450 && c <= 0x45F) {
        return c - 0x50;
    }
    return c;
}

// How many characters (code points) a UTF-8 string holds: the bytes that do not continue one.
std::size_t characters_in(std::string_view text) {
    return static_cast<std::size_t>(std::ranges::count_if(
        text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

// The first `count` characters of a UTF-8 string.
std::string first_characters(std::string_view text, std::size_t count) {
    std::size_t seen = 0;
    for (std::size_t at = 0; at < text.size(); ++at) {
        if ((static_cast<unsigned char>(text[at]) & 0xC0) != 0x80 && seen++ == count) {
            return std::string(text.substr(0, at));
        }
    }
    return std::string(text);
}

}  // namespace

std::string pipeline_title(std::string_view description) {
    // Typographic punctuation is read as its plain form first: a dash ends the clause, curly
    // quotes are quotes (an apostrophe inside a word stays one), an ellipsis is three dots and a
    // no-break space is a space. Every other non-ASCII character is part of a word.
    std::string text(description);
    for (const auto& [from, to] : {std::pair<std::string_view, std::string_view>{"—", ","},
                                   {"–", ","},
                                   {"‘", "'"},
                                   {"’", "'"},
                                   {"‚", "'"},
                                   {"‛", "'"},
                                   {"“", "\""},
                                   {"”", "\""},
                                   {"„", "\""},
                                   {"‟", "\""},
                                   {"«", "\""},
                                   {"»", "\""},
                                   {"…", "..."},
                                   {" ", " "}}) {
        for (std::size_t at = text.find(from); at != std::string::npos;
             at = text.find(from, at + to.size())) {
            text.replace(at, from.size(), to);
        }
    }
    const auto lower = [](std::string value) {
        for (char& c : value) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return value;
    };
    const auto one_of = [](const std::string& value, const auto& list) {
        return std::ranges::find(list, std::string_view(value)) != std::ranges::end(list);
    };
    // A request that leads the description, and the words that only lead into what it asks.
    static constexpr std::string_view request[] = {
        "predict", "forecast", "estimate", "classify",   "detect",  "find",      "identify",
        "flag",    "score",    "rank",     "recommend",  "answer",  "determine", "tell",
        "show",    "spot",     "list",     "segment",    "cluster", "group",     "measure",
        "track",   "explain",  "know",     "understand", "see",     "figure",    "work",
        "build",   "create",   "make",     "help",       "out",     "me",        "us",
        "all",     "any",      "each",     "every"};
    // Words that carry nothing in a title, wherever they are.
    static constexpr std::string_view helper[] = {
        "which", "what", "whether", "who",   "whom",   "how",   "that",   "the",      "a",
        "an",    "will", "would",   "could", "should", "can",   "may",    "might",    "is",
        "are",   "was",  "were",    "be",    "been",   "being", "likely", "unlikely", "going",
        "to",    "do",   "does",    "did",   "has",    "have",  "had",    "most",     "more",
        "least", "our",  "my",      "their", "its",    "your"};
    // A preposition or conjunction after the subject ends the title ("in the next 90 days").
    static constexpr std::string_view ending[] = {
        "in",    "within", "over",   "during", "for",   "by",      "from",  "with",  "without",
        "using", "based",  "before", "after",  "and",   "or",      "but",   "so",    "because",
        "since", "per",    "at",     "on",     "as",    "than",    "given", "when",  "where",
        "if",    "via",    "into",   "across", "among", "between", "until", "while", "about"};
    // A word the title keeps: not a request, a helper or a leading "of".
    const auto carries = [&](const std::string& each) {
        const std::string folded = lower(each);
        return !one_of(folded, request) && !one_of(folded, helper) && folded != "of";
    };
    // The words of the first clause: a bracket, a comma, a semicolon, a colon, a sentence's end
    // or a dash between words ends it, so punctuation never lands inside the title. It ends only
    // once a word the title keeps is read, so "Predict: customer churn" reads past the colon.
    const auto word_byte = [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '_' || c >= 0x80;
    };
    std::vector<std::string> words;
    std::string word;
    for (std::size_t at = 0; at < text.size(); ++at) {
        const auto c = static_cast<unsigned char>(text[at]);
        const bool inside = !word.empty() && at + 1 < text.size() &&
                            word_byte(static_cast<unsigned char>(text[at + 1]));
        if (word_byte(c) || ((c == '-' || c == '\'') && inside)) {
            word += static_cast<char>(c);
            continue;
        }
        if (!word.empty()) {
            words.push_back(std::move(word));
            word.clear();
        }
        const bool sentence_end =
            (c == '.' || c == '!' || c == '?') &&
            (at + 1 == text.size() || !word_byte(static_cast<unsigned char>(text[at + 1])));
        if (std::string_view("([{,;:\n").contains(static_cast<char>(c)) || sentence_end ||
            (c == '-' && !inside)) {
            if (std::ranges::any_of(words, carries)) {
                break;
            }
        }
    }
    if (!word.empty()) {
        words.push_back(std::move(word));
    }
    std::vector<std::string> kept;
    bool leading = true;
    for (const std::string& each : words) {
        const std::string folded = lower(each);
        if (leading && one_of(folded, request)) {
            continue;
        }
        if (one_of(folded, helper)) {
            continue;
        }
        if (one_of(folded, ending) && !kept.empty()) {
            break;
        }
        if (folded == "of" && kept.empty()) {
            continue;
        }
        leading = false;
        kept.push_back(each);
        if (kept.size() == 4) {
            break;
        }
    }
    while (!kept.empty() && lower(kept.back()) == "of") {
        kept.pop_back();
    }
    // Nothing left of the words (a description such as "Predict which"): its first words as
    // written.
    if (kept.empty()) {
        for (const std::string& each : words) {
            kept.push_back(each);
            if (kept.size() == 4) {
                break;
            }
        }
    }
    // At most 48 characters (code points, not bytes): whole words, or the first word cut.
    std::string title;
    for (const std::string& each : kept) {
        if (title.empty()) {
            title = first_characters(each, 48);
            continue;
        }
        if (characters_in(title) + 1 + characters_in(each) > 48) {
            break;
        }
        title += " " + each;
    }
    if (title.empty()) {
        return "Pipeline";
    }
    if (const auto first = first_code_point(title)) {
        title.replace(0, first->second, utf8_of(title_capital(first->first)));
    }
    return title;
}

std::string engine_words_in_lake_terms(std::string text) {
    const auto replace_all = [&text](std::string_view from, std::string_view to) {
        for (std::size_t at = text.find(from); at != std::string::npos;
             at = text.find(from, at + to.size())) {
            text.replace(at, from.size(), to);
        }
    };
    // Capabilities lake does not serve, named as the call a lake caller makes (lake#591).
    replace_all("(load the table first with add_dataset)",
                "(load the table first with workspace_act upload_dataset)");
    replace_all("load the table first with add_dataset",
                "load the table first with workspace_act upload_dataset");
    replace_all("(approve_plan)", "(workspace_act with capability approve_plan)");
    // A native agent whose checkpoint is not in the workspace (lake#246, lake#799): the engine's
    // tokenizer error, said as what is missing and how the base model gets there.
    constexpr std::string_view no_tokenizer = "bpe: no tokenizer.json or vocab.json+merges.txt in ";
    if (const std::size_t at = text.find(no_tokenizer); at != std::string::npos) {
        const std::size_t begin = at + no_tokenizer.size();
        std::size_t end = begin;
        while (end < text.size() && std::isspace(static_cast<unsigned char>(text[end])) == 0) {
            ++end;
        }
        const std::string directory = text.substr(begin, end - begin);
        text.replace(at,
                     end - at,
                     "the agent's model checkpoint is not in this workspace: " + directory +
                         " has no tokenizer.json (or vocab.json and merges.txt). The Orama base "
                         "model is copied to the workspace's volume when its Zetta starts; until "
                         "it is there, an agent on it cannot chat or run");
    }
    return text;
}

namespace {
// The engine's JSON Schema validator speaks in its own terms; lake's own refusals say "unexpected
// field x" and "expected one of [...]" (lake#591). `field` is the field the message is about.
std::string schema_words_in_lake_terms(std::string message, const std::string& field) {
    if (message.contains("instance invalid as per false-schema") && !field.empty()) {
        return "unexpected field " + field;
    }
    if (message.contains("instance not found in required enum")) {
        return (field.empty() ? std::string("the value") : field) +
               " is not one of the values it accepts";
    }
    return message;
}
} // namespace

std::optional<json>
engine_input_enum(const json& openapi, std::string_view capability, std::string_view field) {
    if (!openapi.is_object() || field.empty() || field.contains('/')) {
        return std::nullopt;
    }
    const auto paths = openapi.find("paths");
    if (paths == openapi.end() || !paths->is_object()) {
        return std::nullopt;
    }
    const auto operations = paths->find("/v1/" + std::string(capability));
    if (operations == paths->end() || !operations->is_object()) {
        return std::nullopt;
    }
    for (const auto& [method, operation] : operations->items()) {
        if (!operation.is_object()) {
            continue;
        }
        const json body = operation.value(
            json::json_pointer("/requestBody/content/application~1json/schema"), json::object());
        if (const json values = body.value(
                json::json_pointer("/properties/" + std::string(field) + "/enum"), json());
            values.is_array()) {
            return std::optional<json>(std::in_place, values);
        }
        for (const json& parameter : operation.value("parameters", json::array())) {
            if (parameter.is_object() && parameter.value("name", std::string()) == field) {
                if (const json values = parameter.value(json::json_pointer("/schema/enum"), json());
                    values.is_array()) {
                    return std::optional<json>(std::in_place, values);
                }
            }
        }
    }
    return std::nullopt;
}

json safe_mode_confirmation_schema() {
    return {{"type", "string"},
            {"minLength", 1},
            {"description",
             "the `confirmation` a safe_mode_warning answered: repeat the same call with it to run "
             "the statements the warning showed"}};
}

ApiResult workspace_failure(const control::ZettaControlError& error,
                            const json& input,
                            std::string_view prefix,
                            const std::map<std::string, std::string>& renamed) {
    // The database's safe mode warned (lake#906): the refusal reaches the caller unchanged, its
    // details included, so a person sees the statements and can repeat the call with
    // `safe_mode_confirmation`. No field is at fault.
    if (error.code == "safe_mode_warning") {
        ApiResult warned = ApiResult::failure(
            error.code,
            error.message.empty() ? "the database's safe mode asks to confirm this call first"
                                  : error.message);
        warned.error.details = error.details.is_object() ? error.details : json::object();
        return warned;
    }
    // score or explain on a model that is not gradient-boosted trees, such as a clustering model
    // (lake#1243): said in words, never as the weights' missing `objective` property.
    if (unscorable_weights(error.message)) {
        std::string model;
        if (input.is_object()) {
            for (const char* key : {"model_id", "model"}) {
                if (input.contains(key) && input[key].is_string()) {
                    model = input[key].get<std::string>();
                    break;
                }
            }
        }
        if (model.empty()) {
            static const std::regex quoted(R"(model '([^']+)')");
            std::smatch match;
            if (std::regex_search(error.message, match, quoted)) {
                model = match[1].str();
            }
        }
        return ApiResult::failure("not_supported", unscorable_model(model));
    }
    std::string message = engine_words_in_lake_terms(error.message);
    std::string field;
    // The engine's schema check leads with a pointer: "/format: instance not found in required
    // enum". The pointer belongs in `path`, the field's name in the message.
    if (message.starts_with("/")) {
        if (const auto colon = message.find(": "); colon != std::string::npos) {
            field = message.substr(1, colon - 1);
            message = field + message.substr(colon);
        }
    }
    if (error.path.starts_with("/") && error.path.size() > 1) {
        // The engine's own pointer, when it names the field (zetta#1217), wins.
        field = error.path.substr(1);
    } else if (const auto at = message.find("property '");
               field.empty() && at != std::string::npos) {
        // "required property 'x' not found in object" is about x (lake#256).
        const auto end = message.find('\'', at + 10);
        if (end != std::string::npos) {
            field = message.substr(at + 10, end - at - 10);
        }
    }
    // A refusal that quotes a value the caller sent ("no model named 'm-1'") is about that field.
    if (field.empty() && input.is_object() && error.code != "store_unavailable") {
        for (const auto& [key, value] : input.items()) {
            if (!value.is_string() || value.get_ref<const std::string&>().empty()) {
                continue;
            }
            const std::string& text = value.get_ref<const std::string&>();
            if (message.contains("'" + text + "'") || message.contains("\"" + text + "\"")) {
                field = key;
                break;
            }
        }
    }
    if (const auto back = renamed.find(field.substr(0, field.find('/'))); back != renamed.end()) {
        field = back->second + field.substr(back->first.size());
    }
    std::string code = error.code;
    // The engine names the input field, so the input is at fault, whatever it called it.
    if (!field.empty() && (code == "engine_error" || code == "invalid_argument")) {
        code = "invalid_input";
    }
    if (const std::size_t colon = message.find(": ");
        colon != std::string::npos && !field.empty() && message.starts_with(field + ": ") &&
        message.contains("instance not found in required enum")) {
        message.erase(0, colon + 2);
    }
    message = schema_words_in_lake_terms(std::move(message), field);
    return ApiResult::failure(std::move(code),
                              std::move(message),
                              field.empty() ? std::string() : std::string(prefix) + "/" + field);
}

std::span<const std::string_view> workspace_read_names() {
    return workspace_reads;
}

std::span<const std::string_view> workspace_action_names() {
    return workspace_actions;
}

liquid::runtime::task<ApiResult>
call_workspace_capability(bool read, nlohmann::json input, ApiContext& context) {
    return workspace_call(read, std::move(input), context);
}

bool workspace_read_allowed(std::string_view capability) {
    return listed(workspace_reads, capability);
}

bool workspace_action_allowed(std::string_view capability) {
    return listed(workspace_actions, capability);
}

bool workspace_uses_get(std::string_view capability) {
    return workspace_read_allowed(capability) || capability == "chat";
}

namespace {

// 5000 as "5,000", for the too_many_rows message.
std::string grouped(std::size_t value) {
    std::string digits = std::to_string(value);
    for (std::size_t at = digits.size(); at > 3; at -= 3) {
        digits.insert(at - 3, ",");
    }
    return digits;
}

} // namespace

std::string too_many_rows_message(std::size_t cap) {
    return "the query returns more than " + grouped(cap) +
           " rows, the most an App reads in one query: aggregate in SQL (GROUP BY with COUNT, "
           "SUM, AVG, MIN or MAX) or page with LIMIT and OFFSET (at most " +
           grouped(cap) + " rows a page)";
}

std::expected<std::string, std::string> cap_sql_statement(std::string_view statement,
                                                          std::size_t cap) {
    std::string text(statement);
    while (!text.empty() &&
           (std::isspace(static_cast<unsigned char>(text.back())) != 0 || text.back() == ';')) {
        text.pop_back();
    }
    // The words outside quotes and parentheses, lower-cased, with where each starts and ends.
    struct Word {
        std::string text;
        std::size_t begin;
        std::size_t end;
    };
    std::vector<Word> words;
    int depth = 0;
    char quote = 0;
    for (std::size_t index = 0; index < text.size();) {
        const char character = text[index];
        if (quote != 0) {
            if (character == quote) {
                quote = 0;
            }
            ++index;
        } else if (character == '\'' || character == '"' || character == '`') {
            quote = character;
            ++index;
        } else if (character == '(') {
            ++depth;
            ++index;
        } else if (character == ')') {
            --depth;
            ++index;
        } else if (std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_') {
            const std::size_t begin = index;
            std::string word;
            while (index < text.size() &&
                   (std::isalnum(static_cast<unsigned char>(text[index])) != 0 ||
                    text[index] == '_' || text[index] == '.')) {
                word += static_cast<char>(std::tolower(static_cast<unsigned char>(text[index])));
                ++index;
            }
            if (depth == 0) {
                words.push_back({std::move(word), begin, index});
            }
        } else {
            ++index;
        }
    }
    std::optional<std::size_t> limit;
    std::optional<std::size_t> offset;
    for (std::size_t index = 0; index < words.size(); ++index) {
        if (words[index].text == "limit") {
            limit = index;
        } else if (words[index].text == "offset") {
            offset = index;
        }
    }
    if (limit) {
        // Not a number: the workspace's own parse error says so.
        if (*limit + 1 < words.size()) {
            const std::string& count = words[*limit + 1].text;
            if (!count.empty() && std::ranges::all_of(count, [](unsigned char character) {
                    return std::isdigit(character) != 0;
                })) {
                const bool over = count.size() > 18 || std::stoull(count) > cap;
                if (over) {
                    return std::unexpected("LIMIT " + count + " is more than " + grouped(cap) +
                                           " rows, the most an App reads in one query: "
                                           "aggregate in SQL (GROUP BY with COUNT, SUM, AVG, MIN "
                                           "or MAX) or page with LIMIT and OFFSET (at most " +
                                           grouped(cap) + " rows a page)");
                }
            }
        }
        return std::string(statement);
    }
    const std::string clause = "LIMIT " + std::to_string(cap + 1);
    if (offset) {
        text.insert(words[*offset].begin, clause + " ");
        return text;
    }
    return text + " " + clause;
}

// A workspace capability's description as a customer reads it (lake#257): the engine's issue
// references ("(#1008)") and permission notes ("Requires orchestration:add_dataset.") removed.
static std::string customer_description(std::string text) {
    text = engine_words_in_lake_terms(std::move(text));
    for (std::size_t at = text.find("(#"); at != std::string::npos; at = text.find("(#", at)) {
        const std::size_t close = text.find(')', at);
        if (close == std::string::npos) {
            break;
        }
        std::size_t start = at;
        while (start > 0 && text[start - 1] == ' ') {
            --start;
        }
        text.erase(start, close + 1 - start);
        at = start;
    }
    for (std::size_t at = text.find("Requires "); at != std::string::npos;
         at = text.find("Requires ", at)) {
        const std::size_t end = text.find('.', at);
        const std::string_view clause(text.data() + at, (end == std::string::npos ? text.size() : end) - at);
        if (clause.find(':') == std::string_view::npos) {
            at += 9;
            continue;
        }
        std::size_t start = at;
        while (start > 0 && text[start - 1] == ' ') {
            --start;
        }
        text.erase(start, (end == std::string::npos ? text.size() : end + 1) - start);
        at = start;
    }
    return text;
}

json workspace_openapi(const json& document, std::string_view base) {
    json filtered = document.is_object() ? document : json::object();
    json paths = json::object();
    const json source = filtered.value("paths", json::object());
    for (const auto& [path, operations] : source.items()) {
        if (!path.starts_with("/v1/") || !operations.is_object()) {
            continue;
        }
        const std::string name = path.substr(4);
        const bool read = workspace_read_allowed(name);
        const bool action = workspace_action_allowed(name);
        json kept = json::object();
        for (const auto& [method, operation] : operations.items()) {
            if ((method == "get" && read) || (method == "post" && action)) {
                json shown = operation;
                if (method == "post" && (name == "create_study" || name == "stop_study" ||
                    name == "stop_trial" || name == "promote_trial")) {
                    auto& schema = shown["requestBody"]["content"]["application/json"]["schema"];
                    schema["properties"]["confirm"] = {{"type", "boolean"}, {"const", true}};
                    auto required = schema.value("required", json::array());
                    if (std::ranges::find(required, "confirm") == required.end()) {
                        required.push_back("confirm");
                    }
                    schema["required"] = std::move(required);
                }
                for (const char* key : {"description", "summary"}) {
                    if (shown.is_object() && shown.contains(key) && shown[key].is_string()) {
                        shown[key] = customer_description(shown[key].get<std::string>());
                    }
                }
                kept[method] = std::move(shown);
            }
        }
        if (!kept.empty()) {
            paths[path] = std::move(kept);
        }
    }
    filtered["paths"] = std::move(paths);
    filtered["servers"] = json::array({{{"url", std::string(base)}}});
    json info = filtered.value("info", json::object());
    info["description"] =
        "A Lake workspace's Zetta, reduced to the capabilities Lake allows. Reads are GET "
        "/v1/<name> with the input in the query string; actions are POST /v1/<name> with a JSON "
        "body. Authenticate with a Lake token or agent key; every call is authorized on the "
        "customer and audited.";
    filtered["info"] = std::move(info);
    return filtered;
}

// Agent traffic over Zetta pub/sub (docs/api/agent-bus.md, lake#269). The first three are the
// library agents' side, each agent authenticated by its own bus credential; receive_agent_events
// is for anyone who may read the task.
namespace {
// True when the caller is that library agent, by its own bus credential.
bool agent_service(const ApiContext& context, const json& input) {
    return context.credential_kind == "agent_service" && !context.agent_service.empty() &&
           input.value("agent", std::string()) == context.agent_service;
}

// The bus's agent side is authorized by the service token, not by policy; this names the
// object for the audit trail.
std::expected<std::string, ApiError> agent_bus_object(const json&, ApiContext&) {
    return std::string("agent_bus");
}

ApiResult not_agent_service() {
    return ApiResult::failure("unauthorized",
                              "only that library agent, with its own bus credential, may call this");
}

// Replaces every "{{credential}}" string in a task body.
void fill_credential(json& value, const std::string& credential) {
    if (value.is_string() && value.get<std::string>() == "{{credential}}") {
        value = credential;
    } else if (value.is_structured()) {
        for (json& item : value) {
            fill_credential(item, credential);
        }
    }
}

json bus_wait_schema() {
    return {{"type", "integer"},
            {"minimum", 0},
            {"maximum", 60000},
            {"description", "long poll: wait up to this many milliseconds for an event"}};
}
} // namespace

void register_agent_bus_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "receive_agent_tasks",
        .title = "Receive agent tasks",
        .description =
            "A library agent's inbox (topic agent-tasks.<agent> on Lake's Zetta pub/sub): the "
            "tasks published for it from `from_sequence` on, oldest first, without those "
            "already claimed. Long-polls up to `wait_ms`. Pass the returned `next_sequence` to "
            "continue. Only that agent may call it, with its own bus credential.",
        .input_schema = {{"type", "object"},
                         {"required", json::array({"agent"})},
                         {"additionalProperties", false},
                         {"properties",
                          {{"agent", {{"type", "string"}}},
                           {"workspace_id", {{"type", "string"}}},
                           {"from_sequence", {{"type", "integer"}, {"minimum", 1}}},
                           {"wait_ms", bus_wait_schema()}}}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .long_poll = true,
        .requires_auth = false,
        .object = "agent_bus",
        .action = "read",
        .resolve_object = agent_bus_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!agent_service(context, input)) {
                co_return not_agent_service();
            }
            if (context.agent_bus == nullptr || context.store == nullptr) {
                co_return ApiResult::failure("store_unavailable",
                                             "the agent bus is not configured on this Lake");
            }
            const std::string agent = input.at("agent");
            if (!std::ranges::contains(context.agent_bus->agents(), agent)) {
                co_return ApiResult::failure("not_found", "no library agent " + agent, "/agent");
            }
            const std::uint64_t from = input.value("from_sequence", std::uint64_t{1});
            auto batch = control::settled(co_await context.agent_bus->bus().receive(
                control::agent_tasks_topic(agent),
                from,
                100,
                std::chrono::milliseconds(input.value("wait_ms", 0))));
            if (!batch) {
                co_return ApiResult::failure("store_unavailable", batch.error().message);
            }
            json tasks = json::array();
            for (const control::BusEvent& event : batch->events) {
                auto task = context.store->get_agent_task(event.correlation_id);
                if (!task || !task->phase.empty() || task->desired != "present") {
                    continue;
                }
                if (input.contains("workspace_id") &&
                    task->workspace_id != input.at("workspace_id").get<std::string>()) {
                    continue;
                }
                tasks.push_back({{"sequence", event.sequence},
                                 {"task_id", task->id},
                                 {"customer_id", task->customer_id},
                                 {"workspace_id", task->workspace_id},
                                 {"kind", task->kind}});
            }
            co_return ApiResult::success(
                {{"tasks", std::move(tasks)}, {"next_sequence", batch->next_sequence}});
        }});
    registry.add(Capability{
        .name = "claim_agent_task",
        .title = "Claim an agent task",
        .description =
            "A library agent takes one task it received: exactly one claim succeeds (another is "
            "a conflict). Answers the task's body with the task's own credential in it, handed "
            "over this once; Lake keeps only its SHA-256. Only that agent may call it, with "
            "its own bus credential.",
        .input_schema = {{"type", "object"},
                         {"required", json::array({"task_id", "agent"})},
                         {"additionalProperties", false},
                         {"properties",
                          {{"task_id", {{"type", "string"}}},
                           {"workspace_id", {{"type", "string"}}},
                           {"agent", {{"type", "string"}}}}}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "agent_bus",
        .action = "write",
        .resolve_object = agent_bus_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!agent_service(context, input)) {
                co_return not_agent_service();
            }
            if (context.store == nullptr) {
                co_return unavailable();
            }
            const std::string task_id = input.at("task_id");
            const std::string agent = input.at("agent");
            const auto current = context.store->get_agent_task(task_id);
            if (current && input.contains("workspace_id") &&
                current->workspace_id != input.at("workspace_id").get<std::string>()) {
                co_return ApiResult::failure(
                    "conflict", "workspace_id does not match the task", "/workspace_id");
            }
            // The request the agent receives is a record in the task's workspace (lake#374). It
            // is read before the claim, so a workspace that cannot answer leaves the task
            // waiting for another claim instead of claimed without its request. A task that is
            // not waiting for this agent reads nothing: the claim below refuses it.
            json request = json::object();
            if (current && current->agent == agent && current->phase.empty() &&
                current->desired == "present" && !current->retry_notice_pending) {
                control::AgentTask content = *current;
                auto loaded =
                    control::settled(co_await load_agent_task_content(context, content));
                if (!loaded) {
                    co_return ApiResult::failure(loaded.error().code, loaded.error().message,
                                                 "/task_id");
                }
                if (!*loaded || !content.body.is_object() || content.body.empty()) {
                    // A task from before the move whose request is not copied yet: never hand
                    // an agent an empty request.
                    co_return ApiResult::failure(
                        "not_found", "the task's request is not in its workspace", "/task_id");
                }
                request = std::move(content.body);
            }
            const std::string credential = std::string(control::agent_task_credential_prefix) +
                                           task_id + "." +
                                           control::identity_crypto::random_secret();
            // Where its events will start on agent-events, so following it never reads the
            // topic's whole history (lake#275).
            int lease_seconds = 600;
            if (context.workspaces) {
                const auto& leases = context.workspaces->config().agent_lease_seconds;
                const auto configured = leases.find(agent);
                if (configured != leases.end()) {
                    lease_seconds = configured->second;
                }
            }
            // The claim and its lease are one control-store statement: exactly one claim per
            // attempt succeeds, with no workspace call inside it.
            auto task =
                context.store->claim_agent_task(task_id,
                                                agent,
                                                control::identity_crypto::sha256_hex(credential),
                                                1,
                                                lease_seconds);
            if (!task) {
                co_return ApiResult::failure(task.error().code, task.error().message, "/task_id");
            }
            json body = std::move(request);
            body["workspace_id"] = task->workspace_id;
            fill_credential(body, credential);
            context.audit_customer_id = task->customer_id;
            context.audit_object = "agent_task/" + task->id;
            co_return ApiResult::success({{"task_id", task->id},
                                          {"workspace_id", task->workspace_id},
                                          {"customer_id", task->customer_id},
                                          {"agent", task->agent},
                                          {"kind", task->kind},
                                          {"deadline_seconds", task->deadline_seconds},
                                          {"lease_seconds", task->lease_seconds},
                                          {"lease_until", task->lease_until},
                                          {"attempts", task->attempts},
                                          {"body", std::move(body)}});
        }});
    registry.add(Capability{
        .name = "publish_agent_event",
        .title = "Publish an agent event",
        .description =
            "A library agent reports on a task it claimed, on topic agent-events: `progress` "
            "(a message while it works), `result` (its answer, which finishes the task) or "
            "`failed`. Lake applies it to the task. Only that agent may call it, with its own "
            "bus credential, and only for a task it claimed that has not finished.",
        .input_schema = {{"type", "object"},
                         {"required", json::array({"task_id", "agent", "type"})},
                         {"additionalProperties", false},
                         {"properties",
                          {{"task_id", {{"type", "string"}}},
                           {"workspace_id", {{"type", "string"}}},
                           {"agent", {{"type", "string"}}},
                           {"type",
                            {{"type", "string"},
                             {"enum", json::array({"progress", "result", "failed"})}}},
                           {"message", {{"type", "string"}}},
                           {"result", {{"type", "object"}}}}}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "agent_bus",
        .action = "write",
        .resolve_object = agent_bus_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!agent_service(context, input)) {
                co_return not_agent_service();
            }
            if (context.agent_bus == nullptr || context.store == nullptr) {
                co_return ApiResult::failure("store_unavailable",
                                             "the agent bus is not configured on this Lake");
            }
            const std::string task_id = input.at("task_id");
            auto task = context.store->get_agent_task(task_id);
            if (!task || task->agent != input.at("agent").get<std::string>() ||
                task->phase != "Running") {
                co_return ApiResult::failure(
                    "conflict", "the task is not running on this agent", "/task_id");
            }
            if (input.contains("workspace_id") &&
                task->workspace_id != input.at("workspace_id").get<std::string>()) {
                co_return ApiResult::failure(
                    "conflict", "workspace_id does not match the task", "/workspace_id");
            }
            json event = input;
            WorkspaceApiBus bus(context, task->customer_id, task->workspace_id);
            const auto created = control::settled(co_await bus.create_topic("agent-events"));
            if (!created) {
                co_return ApiResult::failure(created.error().code, created.error().message);
            }
            // Serialize terminal writes in the store before publishing. A losing result must
            // never appear in the event stream as an accepted answer.
            const auto applied = control::AgentTaskBus::apply(*context.store, event);
            if (!applied) {
                co_return ApiResult::failure(applied.error().code, applied.error().message);
            }
            // What the agent said is kept in the task's workspace (lake#375), written
            // conditionally on the revision read; a finished answer there is never replaced.
            const std::string type = input.at("type");
            const auto recorded = control::settled(co_await write_agent_task_event(
                context, *applied, applied->phase, input.value("message", std::string()),
                type == "result" ? input.value("result", json()) : json()));
            event["attempts"] = applied->attempts;
            auto published = control::settled(co_await bus.publish("agent-events", event, task_id));
            if (!recorded) {
                co_return ApiResult::failure(recorded.error().code,
                                             "the event was accepted, but its workspace could not "
                                             "keep it: " + recorded.error().message);
            }
            if (!published) {
                co_return ApiResult::failure("store_unavailable", published.error().message);
            }
            // A finished Discover scan's Opportunities are saved now, whether or not anyone is
            // reading (lake#1195), by the same function every reader settles with. A workspace
            // that cannot take them now is retried by the next list_opportunities.
            if (type == "result" && applied->phase == "Succeeded" && applied->kind == "discover") {
                control::AgentTask finished = *applied;
                finished.result = input.value("result", json());
                static_cast<void>(called(co_await settle_discover(context, std::move(finished))));
            }
            // A delegated turn's progress is the asking turn's progress too (lake#451).
            if (const std::string parent = task->parent_task_id; !parent.empty()) {
                const std::string name = control::agent_display_name(task->agent);
                const std::string said = input.value("message", std::string());
                std::string message =
                    type == "result"   ? name + " finished; writing the answer."
                    : type == "failed" ? name + " did not finish" + (said.empty() ? "." : ": " + said)
                    : said.empty() || said == "Working on it" ? name + " is working."
                                                              : name + ": " + said;
                co_await report_on_asking_turn(
                    context,
                    parent,
                    std::move(message),
                    {{"delegated_task_id", task_id}, {"delegated_agent", task->agent}});
            }
            co_return ApiResult::success({{"event_id", *published}});
        }});
    registry.add(Capability{
        .name = "receive_agent_events",
        .title = "Follow an agent turn live",
        .description =
            "What the agent of one background turn has published (topic agent-events on Lake's "
            "Zetta pub/sub): progress messages and its result, from `from_sequence` on, "
            "long-polling up to `wait_ms`. Always answers the task as it stands (as "
            "get_agent_task); pass `next_sequence` to continue.",
        .input_schema =
            customer_input_schema({{"task_id", {{"type", "string"}}},
                                   {"from_sequence", {{"type", "integer"}, {"minimum", 1}}},
                                   {"wait_ms", bus_wait_schema()}},
                                  json::array({"task_id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .long_poll = true,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (context.store == nullptr) {
                co_return unavailable();
            }
            const std::string task_id = input.at("task_id");
            const auto owned = [&](const control::Result<control::AgentTask>& task) {
                return task && task->customer_id == input.at("customer_id").get<std::string>() &&
                       task->workspace_id == input.at("workspace_id").get<std::string>() &&
                       (task->requested_by_user_id == context.acting_user_id ||
                        context.acting_identity == "admin");
            };
            if (!owned(context.store->get_agent_task(task_id))) {
                co_return ApiResult::failure("not_found", "no such task in this account");
            }
            if (context.agent_bus == nullptr) {
                co_return ApiResult::failure("store_unavailable",
                                             "the agent bus is not configured on this Lake");
            }
            // Never before the task's own first event (lake#275): the topic only grows.
            std::uint64_t next = std::max(input.value("from_sequence", std::uint64_t{1}),
                                          context.store->get_agent_task(task_id)->events_from);
            const auto until = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(input.value("wait_ms", 0));
            json events = json::array();
            WorkspaceApiBus bus(context, input.at("customer_id"), input.at("workspace_id"));
            const auto created = control::settled(co_await bus.create_topic("agent-events"));
            if (!created) {
                co_return ApiResult::failure(created.error().code, created.error().message);
            }
            // Other tasks' events share the topic: read past them until one of this task's
            // arrives or the wait is over.
            for (int page = 0; page < 50; ++page) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    until - std::chrono::steady_clock::now());
                auto batch = control::settled(co_await bus.receive(
                    std::string(control::agent_events_topic),
                    next,
                    1000,
                    std::max(left, std::chrono::milliseconds(0))));
                if (!batch) {
                    co_return ApiResult::failure("store_unavailable", batch.error().message);
                }
                for (const control::BusEvent& event : batch->events) {
                    if (event.correlation_id != task_id || !event.payload.is_object()) {
                        continue;
                    }
                    json row{{"sequence", event.sequence},
                             {"type", event.payload.value("type", std::string())},
                             {"message", event.payload.value("message", std::string())}};
                    if (event.payload.contains("result")) {
                        row["result"] = event.payload["result"];
                    }
                    for (const char* field : {"attempts", "reclaimed"}) {
                        if (event.payload.contains(field)) {
                            row[field] = event.payload[field];
                        }
                    }
                    events.push_back(std::move(row));
                }
                const bool moved = batch->next_sequence > next;
                next = std::max(next, batch->next_sequence);
                if (!events.empty() || (!moved && std::chrono::steady_clock::now() >= until)) {
                    break;
                }
            }
            auto task = control::settled(co_await agent_task_with_content(context, task_id));
            if (!task) {
                co_return ApiResult::failure(task.error().code, task.error().message);
            }
            static_cast<void>(called(co_await settle_discover(context, *task)));
            static_cast<void>(co_await settle_build(context, *task));
            co_return ApiResult::success({{"events", std::move(events)},
                                          {"next_sequence", next},
                                          {"task", task_json(*task)}});
        }});
}

// delete_workspace (lake#514, docs/api/workspaces.md#deletion): the dry run lists what would be
// destroyed; otherwise one store transaction deletes the workspace and what it holds, and
// finish_workspace_deletion removes what lives outside PostgreSQL. A repeated call for a deleted
// workspace resumes an unfinished deletion and answers the recorded one.
static ApiResult delete_workspace_cascading(const json& input, ApiContext& context,
                                            const control::Workspace& row) {
    const bool dry_run = input.value("dry_run", false);
    const bool confirmed = input.contains("confirm") && input.at("confirm") == row.name;
    if (!confirmed && (!dry_run || input.contains("confirm"))) {
        return ApiResult::failure("invalid_input", "confirm must match the workspace name",
                                  "/confirm");
    }
    context.audit_kind = dry_run ? "workspace.delete.plan" : "workspace.delete";
    context.audit_object = "workspace/" + row.id;
    context.audit_details = {{"workspace_id", row.id}, {"workspace_name", row.name}};
    if (dry_run) {
        if (row.desired == "deleted") {
            const auto recorded = context.store->get_workspace_deletion(row.id);
            if (!recorded) {
                return ApiResult::failure("conflict", "the workspace is deleted");
            }
            json value = workspace_deletion_json(*recorded);
            value["dry_run"] = true;
            return ApiResult::success(std::move(value));
        }
        auto plan = workspace_deletion_plan(context, row);
        if (!plan) {
            return ApiResult::failure(plan.error().code, plan.error().message);
        }
        (*plan)["dry_run"] = true;
        context.audit_details["counts"] = plan->at("counts");
        return ApiResult::success(std::move(*plan));
    }
    control::WorkspaceDeletionRequest request{.workspace_id = row.id,
                                              .requested_by = context.acting_identity};
    if (row.desired != "deleted" && context.workspace_content &&
        context.workspace_content->image_tags) {
        for (const std::string& image : workspace_image_candidates(*context.store, row)) {
            if (!context.workspace_content->image_tags(image).empty()) {
                request.images.push_back(image);
            }
        }
    }
    const auto deletion = context.store->delete_workspace_cascade(request);
    if (!deletion) {
        return store_failure(deletion.error());
    }
    // What lives outside PostgreSQL. A failure leaves the deletion pending for the owner loop; the
    // workspace is deleted either way, so the call answers success with `completed: false`.
    const auto finished = finish_workspace_deletion(context, *deletion);
    const auto current = context.store->get_workspace_deletion(row.id);
    const auto updated = context.store->get_workspace(row.id);
    json value = control::workspace_json(updated ? *updated : row, operator_view(context));
    value["deletion"] = workspace_deletion_json(current ? *current : *deletion);
    if (!finished) {
        value["deletion"]["pending"] = finished.error();
    }
    context.audit_details["counts"] = value["deletion"]["counts"];
    context.audit_details["completed"] = value["deletion"]["completed"];
    return ApiResult::success(std::move(value));
}

void register_workspace_capabilities(Registry& registry) {
    for (const std::string operation : {"list_workspaces", "get_workspace", "create_workspace",
                                        "update_workspace", "delete_workspace"}) {
        const bool listing = operation == "list_workspaces";
        const bool creating = operation == "create_workspace";
        const bool updating = operation == "update_workspace";
        const bool deleting = operation == "delete_workspace";
        const bool read = listing || operation == "get_workspace";
        json properties = json::object();
        json required = json::array();
        if (creating || updating) {
            properties["name"] = {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
            // A label, not an enum (lake#594): it names the workspace's namespace
            // (`<environment>-ws-<id>`) and its `orama.cloud/environment` label, and a team may
            // keep its own (`qa`, `eu-prod`). The usual values are listed for a newcomer.
            properties["environment"] = {
                {"type", "string"},
                {"minLength", 1},
                {"maxLength", 47},
                {"pattern", "^[a-z0-9]([a-z0-9-]*[a-z0-9])?$"},
                {"examples", {"dev", "staging", "prod"}},
                {"description",
                 "what the workspace is for: usually dev, staging or prod, or any lowercase DNS "
                 "label of up to 47 characters (letters, digits and inner hyphens). It names the "
                 "workspace's Kubernetes namespace and changes nothing else."}};
        }
        if (creating) {
            required = json::array({"name", "environment"});
        }
        if (updating) {
            properties["desired"] = {{"type", "string"}, {"enum", {"running", "stopped"}}};
        }
        if (deleting) {
            properties["confirm"] = {{"type", "string"}};
            properties["dry_run"] = {{"type", "boolean"}};
        }
        // A human title for each (lake#594); the raw id was shown as the title before.
        const std::string title = listing                        ? "List workspaces"
                                  : operation == "get_workspace" ? "Get a workspace"
                                  : creating                     ? "Create a workspace"
                                  : updating                     ? "Update a workspace"
                                                                 : "Delete a workspace";
        registry.add(Capability{
            .name = operation,
            .title = title,
            .description = workspace_description(operation),
            .input_schema = listing || creating ? control_input_schema(properties, required)
                                                : customer_input_schema(properties, required),
            .output_schema = {{"type", "object"}},
            .kind = read ? CapabilityKind::Read : CapabilityKind::Action,
            .object = "customer",
            .action = deleting ? "admin"
                      : read   ? "read"
                               : "write",
            .resolve_object = customer_object,
            .authorizes_candidates = listing,
            .handler = [operation, listing, creating, deleting](const json& input,
                                                                ApiContext& context) {
                if (listing) {
                    const auto object = customer_object(input, context);
                    if (!object) {
                        return ApiResult::failure(object.error().code, object.error().message);
                    }
                    if (context.acting_identity != "admin") {
                        const auto decision = control::authorize(*context.store, context.acting_identity,
                            *object, "read", context.authorization_cache);
                        if (!decision || !decision->allowed) {
                            return ApiResult::failure("unauthorized", "principal cannot read this account");
                        }
                    }
                    const auto rows = context.store->list_workspaces(input.at("customer_id"));
                    if (!rows) {
                        return store_failure(rows.error());
                    }
                    json result = json::array();
                    for (const auto& row : *rows) {
                        result.push_back(control::workspace_json(row, operator_view(context)));
                    }
                    return ApiResult::success({{"workspaces", result}});
                }
                if (creating) {
                    // lake#987: the account's workspace and service caps, counted by the store.
                    control::NewWorkspace request{input.at("customer_id"),
                                                  input.at("name"),
                                                  input.at("environment"),
                                                  context.acting_identity};
                    if (context.workspaces != nullptr) {
                        request.max_workspaces =
                            context.workspaces->config().max_workspaces_per_customer;
                        request.max_services = context.workspaces->config().max_per_customer;
                    }
                    const auto row = context.store->create_workspace(request);
                    return row ? ApiResult::success(control::workspace_json(*row, operator_view(context))) : store_failure(row.error());
                }
                const auto row = context.store->get_workspace(input.at("workspace_id"));
                if (!row || row->customer_id != input.at("customer_id").get<std::string>()) {
                    return ApiResult::failure("invalid_input", "workspace_id is not in this account", "/workspace_id");
                }
                if (operation == "get_workspace") {
                    return ApiResult::success(control::workspace_json(*row, operator_view(context)));
                }
                if (deleting) {
                    return delete_workspace_cascading(input, context, *row);
                }
                control::WorkspacePatch patch;
                if (input.contains("name")) {
                    patch.name = input.at("name").get<std::string>();
                }
                if (input.contains("environment")) {
                    patch.environment = input.at("environment").get<std::string>();
                }
                if (input.contains("desired")) {
                    patch.desired = input.at("desired").get<std::string>();
                }
                const auto updated = context.store->update_workspace(row->id, patch);
                return updated ? ApiResult::success(control::workspace_json(*updated, operator_view(context))) : store_failure(updated.error());
            }});
    }
    register_agent_run_capabilities(registry);
    register_agent_flow_capabilities(registry);
    register_conversation_capabilities(registry);
    register_agent_bus_capabilities(registry);
    register_sql_capability(registry);
    register_dataset_catalog_capability(registry);
    register_pipeline_capabilities(registry);
    register_parity_capabilities(registry);
    register_connect_capabilities(registry);
    register_search_capabilities(registry);
    register_predict_capability(registry);
    register_model_traffic_capability(registry);
    registry.add(Capability{
        .name = "workspace_status",
        .title = "Workspace status",
        .description = "Whether the workspace named by `workspace_id` was requested, its "
                       "operator phase, and whether Lake can call it.",
        .input_schema = customer_input_schema(json::object(), json::array()),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            auto state = co_await context.workspaces->state(input.at("customer_id"), input.at("workspace_id"));
            if (!state) {
                co_return ApiResult::failure("store_unavailable", state.error().message);
            }
            if (!*state) {
                co_return ApiResult::failure(state->error().code, state->error().message);
            }
            co_return ApiResult::success((*state)->to_json());
        }});
    registry.add(Capability{
        .name = "start_workspace",
        .title = "Start a workspace",
        .description = "Make sure the account has its first workspace (named after the account's "
                       "handle; one created earlier as `workspace` keeps that name), asking "
                       "the operator for it if absent, and answer its state with its "
                       "`workspace_id`. Repeating it is harmless. An account can hold many "
                       "workspaces: `create_workspace` adds another and `list_workspaces` lists "
                       "them. Every workspace-scoped call names one with `workspace_id`.",
        .input_schema = control_input_schema(json::object(), json::array()),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            set_audit(context, "workspace.start", input, "");
            auto state = co_await context.workspaces->request(input.at("customer_id"),
                                                              context.acting_identity);
            if (!state) {
                co_return ApiResult::failure("store_unavailable", state.error().message);
            }
            if (!*state) {
                co_return ApiResult::failure(state->error().code, state->error().message);
            }
            co_return ApiResult::success((*state)->to_json());
        }});
    registry.add(Capability{
        .name = "workspace_openapi",
        .title = "A workspace's OpenAPI",
        .description =
            "The customer's Zetta OpenAPI document reduced to what Lake allows, served under "
            "/workspaces/<customer_id>/zetta/v1/. Agents derive their Zetta tools from it.",
        .input_schema = customer_input_schema(json::object(), json::array()),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            const std::string customer_id = input.at("customer_id");
            auto called = co_await call_workspace(
                context,
                customer_id, "openapi.json", true, json::object());
            if (!called) {
                co_return ApiResult::failure("store_unavailable", called.error().message);
            }
            if (!*called) {
                co_return ApiResult::failure(called->error().code, called->error().message);
            }
            co_return ApiResult::success(
                workspace_openapi(**called, "/w/" + input.at("workspace_id").get<std::string>() +
                                            "/workspaces/" + customer_id + "/zetta"));
        }});
    registry.add(Capability{
        .name = "list_agent_library",
        .title = "The Agent Library",
        .description =
            "Every specialized agent Orama ships for you to use (Ask Orama, the Project Agent, "
            "the Application Agent, the Auto-Suggest Agent, the Agent Creator Agent, ...): what "
            "each does, where you meet it, and whether this Lake runs it. All nine are listed, "
            "including the Eval Generator Agent, which creates evals, and the two corpus agents, "
            "which build training corpora.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"include_internal",
                            {{"type", "boolean"},
                             {"description",
                              "accepted for compatibility and ignored: every agent is listed "
                              "(lake#788)"}}}}},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "principal",
        .action = "read",
        .resolve_object = [](const json&,
                             ApiContext& context) -> std::expected<std::string, ApiError> {
            return "principal/" + context.acting_identity;
        },
        // The library is the same for everyone signed in.
        .authorizes_candidates = true,
        .handler =
            [](const json&, ApiContext& context) {
                // Every agent, whatever `include_internal` says: none is internal (lake#788).
                const std::map<std::string, std::string> none;
                return ApiResult::success(control::agent_library_json(
                    context.workspaces ? context.workspaces->config().agent_endpoints : none));
            }});
    registry.add(Capability{
        .name = "list_agent_revisions",
        .title = "List an agent's revisions",
        .description =
            "The history of an agent's definition (its purpose, prompt, skills, model, pipelines, "
            "work area and trigger): one revision for each create_agent and update_agent Lake "
            "made, newest first, each with its number, who made it, when, and the definition "
            "it set. Restore one by calling update_agent with its `definition`, which writes a "
            "new revision. These are not the agent's trained versions, which "
            "agent_training_status lists.",
        .input_schema = customer_input_schema(
            {{"agent_id", {{"type", "string"}, {"minLength", 1}, {"description", "the agent's id"}}},
             {"limit",
              {{"type", "integer"},
               {"minimum", 1},
               {"maximum", 200},
               {"description", "the newest this many (default 50)"}}}},
            json::array({"agent_id"})),
        .output_schema =
            {{"type", "object"},
             {"properties",
              {{"revisions",
                {{"type", "array"},
                 {"items",
                  {{"type", "object"},
                   {"properties",
                    {{"id", {{"type", "string"}}},
                     {"revision", {{"type", "integer"}}},
                     {"change", {{"type", "string"}, {"enum", {"create", "update"}}}},
                     {"created_at", {{"type", "string"}}},
                     {"created_by", {{"type", "string"}}},
                     {"created_by_name", {{"type", json::array({"string", "null"})}}},
                     {"definition", {{"type", "object"}}}}}}}}},
               {"total", {{"type", "integer"}}}}},
             {"required", json::array({"revisions", "total"})}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        // A list authorizes itself, as every list_ read does.
        .authorizes_candidates = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            const std::string customer_id = input.at("customer_id");
            const std::string workspace_id = input.at("workspace_id");
            if (auto refused = authorize_customer_read(input, context)) {
                co_return *refused;
            }
            const std::string agent_id = input.at("agent_id");
            auto records = workspace_records(context, customer_id, workspace_id);
            if (!records) {
                co_return ApiResult::failure(records.error().code, records.error().message);
            }
            auto listed = control::settled(
                co_await control::list_agent_revisions(*records, customer_id, agent_id));
            if (!listed) {
                co_return ApiResult::failure(
                    listed.error().code, listed.error().message, listed.error().path);
            }
            const std::size_t total = listed->size();
            const std::size_t limit = input.value("limit", std::size_t{50});
            std::map<std::string, json> names;
            json revisions = json::array();
            for (std::size_t index = total; index > 0 && revisions.size() < limit; --index) {
                const control::AgentRevision& revision = (*listed)[index - 1];
                auto known = names.find(revision.created_by);
                if (known == names.end()) {
                    json name = nullptr;
                    if (context.store && !revision.created_by.empty()) {
                        if (const auto user = context.store->get_user(revision.created_by); user) {
                            if (!user->display_name.empty()) {
                                name = user->display_name;
                            } else if (!user->email.empty()) {
                                name = user->email;
                            }
                        }
                    }
                    known = names.emplace(revision.created_by, std::move(name)).first;
                }
                revisions.push_back({{"id", revision.id},
                                     {"revision", index},
                                     {"change", revision.change},
                                     {"created_at", revision.created_at},
                                     {"created_by", revision.created_by},
                                     {"created_by_name", known->second},
                                     {"definition", revision.definition}});
            }
            co_return ApiResult::success(
                {{"revisions", std::move(revisions)}, {"total", total}});
        }});
    register_workspace_call(registry, true);
    register_workspace_call(registry, false);
    register_engine_capabilities(registry);
    register_service_capabilities(registry);
    register_store_capabilities(registry);
}

} // namespace lake::api
