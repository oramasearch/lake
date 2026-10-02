#include "lake/api/capabilities.hpp"

#include <algorithm>
#include <cmath>
#include <regex>
#include <set>

#include "experiment_schemas.hpp"
#include "lake/control/agent_bus.hpp"
#include "lake/control/zetta_workspace.hpp"

namespace lake::api {
namespace {
using namespace experiments;
using Task = liquid::runtime::task<ApiResult>;

struct ApiCompletion {
    liquid::result<ApiResult> value;
};

boost::capy::io_task<ApiCompletion> complete_api(Task task) {
    auto result = co_await std::move(task);
    co_return boost::capy::io_result<ApiCompletion>{{}, {std::move(result)}};
}

ApiResult completed(std::expected<ApiResult, liquid::error> result) {
    return result ? std::move(*result) : ApiResult::failure("store_error", result.error().message);
}

std::expected<std::string, ApiError> customer_object(const json& input, ApiContext& context) {
    if (!context.store) {
        return std::unexpected(ApiError{"store_unavailable", "no control store", {}});
    }
    context.audit_customer_id = input.at("customer_id");
    context.audit_object = "experiments/" + context.audit_customer_id;
    const auto found = context.store->find_policy_object("customer", context.audit_customer_id);
    if (!found) {
        return std::unexpected(ApiError{found.error().code, found.error().message, "/customer_id"});
    }
    return found->id;
}

bool constraints(const json& schema, const json& value) {
    if (value.is_null()) {
        return false;
    }
    if (schema.contains("const") && value != schema.at("const")) {
        return false;
    }
    if (schema.contains("enum") &&
        std::ranges::find(schema.at("enum"), value) == schema.at("enum").end()) {
        return false;
    }
    if (value.is_number()) {
        const auto number = value.get<double>();
        if (!std::isfinite(number) ||
            (schema.contains("minimum") && number < schema.at("minimum").get<double>()) ||
            (schema.contains("maximum") && number > schema.at("maximum").get<double>())) {
            return false;
        }
    }
    if (value.is_string()) {
        const auto& string = value.get_ref<const std::string&>();
        if (string.size() < schema.value("minLength", std::size_t{0}) ||
            string.size() > schema.value("maxLength", std::numeric_limits<std::size_t>::max()) ||
            (schema.contains("pattern") &&
             !std::regex_match(string, std::regex(schema.at("pattern").get<std::string>())))) {
            return false;
        }
    }
    if (value.is_array()) {
        if (value.size() < schema.value("minItems", std::size_t{0}) ||
            value.size() > schema.value("maxItems", std::numeric_limits<std::size_t>::max()) ||
            (schema.value("uniqueItems", false) &&
             std::set<json>(value.begin(), value.end()).size() != value.size())) {
            return false;
        }
        for (const auto& child : value) {
            if (!constraints(schema.at("items"), child)) {
                return false;
            }
        }
    }
    if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            if (!constraints(schema.at("properties").at(key), child)) {
                return false;
            }
        }
    }
    return true;
}

ApiResult authorize_list(const json& input, ApiContext& context) {
    const auto object = customer_object(input, context);
    if (!object) {
        return ApiResult::failure(object.error().code, object.error().message, object.error().path);
    }
    if (!context.operator_credential) {
        const auto decision = control::authorize(*context.store, context.acting_identity,
            *object, "read", context.authorization_cache);
        if (!decision) {
            return ApiResult::failure(decision.error().code, decision.error().message);
        }
        if (!decision->allowed) {
            return ApiResult::failure("unauthorized", "principal cannot read this space's experiments");
        }
    }
    return ApiResult::success(json::object());
}

Task proxy(ApiContext& context, const json& input, std::string name, json args) {
    if (!context.workspaces) {
        co_return ApiResult::failure("store_unavailable", "workspaces are not configured");
    }
    const auto result = control::settled(co_await call_workspace(
        context, input.at("customer_id"), name, workspace_uses_get(name), std::move(args)));
    if (!result) {
        // Zetta names the field at fault (zetta#1217); a caller repairs that field, not a guess.
        co_return ApiResult::failure(result.error().code, result.error().message, result.error().path);
    }
    co_return ApiResult::success(*result);
}

bool selected(const json& input, const std::string& project, const std::string& state) {
    return (input.value("project", "").empty() || input.at("project") == project) &&
           (input.value("state", "").empty() || input.at("state") == state);
}

Task list(ApiContext& context, const json& input) {
    ApiContext studies_context = context;
    ApiContext runs_context = context;
    auto [wait_error, studies_completed, runs_completed] = co_await liquid::runtime::when_all(
        liquid::runtime::as_io(proxy(studies_context, input, "list_studies", json::object())),
        liquid::runtime::as_io(proxy(runs_context, input, "list_runs", json::object())));
    if (wait_error) {
        co_return ApiResult::failure("store_error", wait_error.message());
    }
    const auto studies = completed(std::move(studies_completed));
    if (!studies.ok) {
        co_return studies;
    }
    const auto runs = completed(std::move(runs_completed));
    if (!runs.ok) {
        co_return runs;
    }
    json answer = {{"studies", json::array()}, {"runs", json::array()}};
    std::set<std::string> trial_runs;
    for (const auto& item : studies.value.at("studies")) {
        for (const auto& row : item.at("trials")) {
            trial_runs.insert(row.at("run"));
        }
        if (selected(input, item.at("spec").at("project"), item.at("status"))) {
            answer["studies"].push_back(item);
        }
    }
    for (auto item : runs.value.at("runs")) {
        const std::string project = item.value("project_name", item.value("project", ""));
        if (trial_runs.contains(item.at("id")) ||
            !item.value("study_id", std::string()).empty() ||
            !selected(input, project, item.at("status"))) {
            continue;
        }
        answer["runs"].push_back(std::move(item));
    }
    co_return ApiResult::success(std::move(answer));
}

Task compare(ApiContext& context, const json& input) {
    json details = json::array();
    std::set<std::string> names;
    std::set<std::string> suppressed;
    std::vector<ApiContext> contexts;
    contexts.reserve(input.at("runs").size());
    std::vector<boost::capy::io_task<ApiCompletion>> pending;
    pending.reserve(input.at("runs").size());
    for (const auto& run : input.at("runs")) {
        json args = {{"run", run}};
        if (input.contains("names")) {
            args["names"] = input.at("names");
        }
        contexts.push_back(context);
        pending.push_back(
            complete_api(proxy(contexts.back(), input, "get_run_metrics", std::move(args))));
    }
    auto [wait_error, completed_metrics] =
        co_await liquid::runtime::when_all(std::move(pending));
    if (wait_error) {
        co_return ApiResult::failure("store_error", wait_error.message());
    }
    for (std::size_t index = 0; index < completed_metrics.size(); ++index) {
        const std::string run = input.at("runs").at(index);
        const auto result = completed(std::move(completed_metrics[index].value));
        if (!result.ok) {
            co_return result;
        }
        auto data = result.value;
        if (data.contains("metrics_suppressed")) {
            suppressed.insert(run);
            data["series"] = json::array();
        }
        for (const auto& series : data.at("series")) {
            names.insert(series.at("name"));
        }
        details.push_back(std::move(data));
    }
    json answer = {{"runs", input.at("runs")}, {"metrics", json::array()}};
    if (!names.empty()) {
        const auto result = completed(co_await proxy(context, input, "compare_runs",
            {{"runs", input.at("runs")}, {"names", names}}));
        if (!result.ok) {
            co_return result;
        }
        answer = result.value;
        for (auto& metric : answer.at("metrics")) {
            for (auto& point : metric.at("points")) {
                auto& values = point.at("values");
                values.erase(std::remove_if(values.begin(), values.end(), [&](const auto& value) {
                    return suppressed.contains(value.at("run").template get<std::string>());
                }), values.end());
            }
        }
    }
    answer["details"] = std::move(details);
    co_return ApiResult::success(std::move(answer));
}

Task tail(ApiContext& context, const json& input) {
    const bool is_study = input.at("kind") == "study";
    const std::string id = input.at("id");
    if (id.find_first_of(" /\\\t\r\n:*?") != std::string::npos) {
        co_return ApiResult::failure("invalid_input", "a concrete study or run id is required", "/id");
    }
    const auto target = completed(co_await proxy(context, input,
        is_study ? "get_study" : "get_run_metrics",
        is_study ? json{{"study", id}} : json{{"run", id}, {"max_points", 4}}));
    if (!target.ok) {
        co_return target;
    }
    const auto from = input.value("from_sequence", std::uint64_t{1});
    if (!is_study && target.value.contains("metrics_suppressed")) {
        co_return ApiResult::success({{"events", json::array()}, {"next_sequence", from}});
    }
    const auto result = completed(co_await proxy(context, input, "receive_events",
        {{"topic", (is_study ? "study-events." : "run-metrics.") + id},
         {"from_sequence", from}, {"max", input.value("max_events", 100)},
         {"wait_ms", input.value("wait_ms", 10000)}}));
    if (!result.ok && result.error.code == "not_found") {
        // A valid Run may not have emitted its first batch yet, or its ephemeral topic expired.
        co_return ApiResult::success({{"events", json::array()}, {"next_sequence", from}});
    }
    co_return result;
}

Task dispatch(ApiContext& context, const json& input, const std::string& name,
              const std::string& upstream) {
    if (name == "list_experiments") {
        co_return completed(co_await list(context, input));
    }
    if (name == "compare_runs") {
        co_return completed(co_await compare(context, input));
    }
    if (name == "tail_experiment_events") {
        co_return completed(co_await tail(context, input));
    }
    auto args = input;
    args.erase("customer_id");
    args.erase("workspace_id");
    args.erase("confirm");
    if (name == "start_study") {
        // from_defaults lets Zetta fill a missing objective (held_out_auc, maximize, the
        // validation split) and budgets field by field (zetta#1217). Without it both are required.
        if (!args.value("from_defaults", false)) {
            for (const char* field : {"objective", "budgets"}) {
                if (!args.contains(field)) {
                    co_return ApiResult::failure("invalid_input",
                        std::string(field) + " is required; set from_defaults: true to fill a "
                        "missing objective (held_out_auc, maximize, validation split) and budgets",
                        std::string("/") + field);
                }
            }
        }
        if (args.contains("objective") &&
            args.at("objective").value("validation_split", "") == "test") {
            co_return ApiResult::failure("invalid_input",
                                        "model selection requires a validation split",
                                        "/objective/validation_split");
        }
    }
    auto result = completed(co_await proxy(context, input, upstream, std::move(args)));
    if (name == "get_run_metrics" && result.ok && result.value.contains("metrics_suppressed")) {
        result.value["series"] = json::array();
    }
    co_return result;
}

void add(Registry& registry, std::string name, std::string upstream, json properties,
         json required, json output, std::string permission = "read") {
    properties["customer_id"] = identifier();
    required.push_back("customer_id");
    properties["workspace_id"] = identifier();
    required.push_back("workspace_id");
    const bool read = permission == "read";
    if (!read) {
        properties["confirm"] = {{"type", "boolean"}, {"const", true}};
        required.push_back("confirm");
    }
    const auto schema = object(std::move(properties), std::move(required));
    registry.add(Capability{
        .name = name,
        .title = name,
        .description = read ? "Read training experiments in one space; distinct from solution holdouts."
                            : "Change a training study after explicit confirmation in this space.",
        .input_schema = schema,
        .output_schema = std::move(output),
        .kind = read ? CapabilityKind::Read : CapabilityKind::Action,
        .post_alias = read,
        .object = "customer",
        .action = std::move(permission),
        .resolve_object = customer_object,
        .authorizes_candidates = name.starts_with("list_"),
        .resolves_operator_audit_target = true,
        .async_handler = [name, upstream, schema](const json& input, ApiContext& context) -> Task {
            if (!constraints(schema, input)) {
                co_return ApiResult::failure("invalid_input", "experiment arguments are outside their bounds");
            }
            if (name.starts_with("list_")) {
                const auto authorized = authorize_list(input, context);
                if (!authorized.ok) {
                    co_return authorized;
                }
            }
            context.audit_kind = "experiment." + name;
            context.audit_details = {{"customer_id", input.at("customer_id")}};
            for (const auto* key : {"study", "run", "runs", "kind", "id", "confirm", "name"}) {
                if (input.contains(key)) {
                    context.audit_details[key] = input.at(key);
                }
            }
            co_return completed(co_await dispatch(context, input, name, upstream));
        }});
}
} // namespace

void register_experiment_capabilities(Registry& registry) {
    add(registry, "list_experiments", "", {{"project", text()}, {"state", choice({"", "pending",
        "running", "succeeded", "failed", "stopped", "complete"})}}, json::array(),
        object({{"studies", array(study())}, {"runs", array(run_summary())}}, {"studies", "runs"}));
    add(registry, "get_experiment_study", "get_study", {{"study", identifier()}}, {"study"}, study());
    add(registry, "list_experiment_trials", "list_trials", {{"study", identifier()},
        {"state", choice({"running", "pruned", "failed", "complete"})}}, {"study"},
        object({{"study", identifier()}, {"trials", array(trial())}}, {"study", "trials"}));
    add(registry, "get_experiment_analysis", "get_study_analysis", {{"study", identifier()}},
        {"study"}, analysis());
    auto names = array({{"type", "string"}, {"pattern", "^[A-Za-z0-9_.-]{1,128}$"}});
    names["uniqueItems"] = true;
    add(registry, "get_run_metrics", "get_run_metrics", {{"run", identifier()}, {"names", names},
        {"from_step", integer(0, 9007199254740991)}, {"max_points", integer(4, 10000)}},
        {"run"}, metrics());
    auto runs = array(identifier());
    runs.update({{"minItems", 2}, {"maxItems", 8}, {"uniqueItems", true}});
    names["minItems"] = 1;
    add(registry, "compare_runs", "compare_runs", {{"runs", runs}, {"names", names}},
        {"runs"}, comparison());
    auto properties = study_spec().at("properties");
    properties["from_defaults"] = flag();
    // Zetta fills objective and budgets field by field when from_defaults is set, so each of them
    // and each of their fields is optional here; the handler requires both without it.
    for (const char* field : {"objective", "budgets"}) {
        properties[field]["required"] = json::array();
    }
    add(registry, "start_study", "create_study", properties, {"name", "methodology"}, study(),
        "write");
    add(registry, "stop_study", "stop_study", {{"study", identifier()}}, {"study"},
        object({{"study", identifier()}, {"status", choice({"stopped"})}, {"reason", text()}},
        {"study", "status", "reason"}), "write");
    add(registry, "stop_experiment_trial", "stop_trial", {{"study", identifier()},
        {"run", identifier()}}, {"study", "run"}, object({{"study", identifier()},
        {"run", identifier()}, {"state", choice({"failed"})}}, {"study", "run", "state"}), "write");
    add(registry, "promote_trial", "promote_trial", {{"study", identifier()}, {"run", identifier()}},
        {"study", "run"}, object({{"study", identifier()}, {"run", identifier()},
        {"model", identifier()}, {"project", text()}}, {"study", "run", "model", "project"}), "admin");
    add(registry, "tail_experiment_events", "receive_events", {{"kind", choice({"study", "run"})},
        {"id", identifier()}, {"from_sequence", integer(1, 9007199254740991)},
        {"max_events", integer(1, 1000)}, {"wait_ms", integer(0, 30000)}}, {"kind", "id"}, events());
}
} // namespace lake::api
