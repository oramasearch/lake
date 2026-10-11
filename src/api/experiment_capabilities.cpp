#include "lake/api/capabilities.hpp"

#include <algorithm>
#include <cmath>
#include <map>
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

// On a violation `at` is the JSON pointer of the value outside its bounds (lake#450).
bool constraints(const json& schema,
                 const json& value,
                 std::string& at,
                 const std::string& here = {}) {
    if (value.is_null()) {
        at = at.empty() ? here : at;
        return false;
    }
    if (schema.contains("const") && value != schema.at("const")) {
        at = at.empty() ? here : at;
        return false;
    }
    if (schema.contains("enum") &&
        std::ranges::find(schema.at("enum"), value) == schema.at("enum").end()) {
        at = at.empty() ? here : at;
        return false;
    }
    if (value.is_number()) {
        const auto number = value.get<double>();
        if (!std::isfinite(number) ||
            (schema.contains("minimum") && number < schema.at("minimum").get<double>()) ||
            (schema.contains("maximum") && number > schema.at("maximum").get<double>())) {
            at = at.empty() ? here : at;
            return false;
        }
    }
    if (value.is_string()) {
        const auto& string = value.get_ref<const std::string&>();
        if (string.size() < schema.value("minLength", std::size_t{0}) ||
            string.size() > schema.value("maxLength", std::numeric_limits<std::size_t>::max()) ||
            (schema.contains("pattern") &&
             !std::regex_match(string, std::regex(schema.at("pattern").get<std::string>())))) {
            at = at.empty() ? here : at;
            return false;
        }
    }
    if (value.is_array()) {
        if (value.size() < schema.value("minItems", std::size_t{0}) ||
            value.size() > schema.value("maxItems", std::numeric_limits<std::size_t>::max()) ||
            (schema.value("uniqueItems", false) &&
             std::set<json>(value.begin(), value.end()).size() != value.size())) {
            at = at.empty() ? here : at;
            return false;
        }
        for (std::size_t index = 0; index < value.size(); ++index) {
            if (!constraints(
                    schema.at("items"), value[index], at, here + "/" + std::to_string(index))) {
                return false;
            }
        }
    }
    if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            if (!constraints(schema.at("properties").at(key), child, at, here + "/" + key)) {
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

// An engine message says "study"; the person reads "experiment" (lake#561).
std::string experiment_words(const std::string& message) {
    static const std::regex word(R"(\b([Ss])tud(y|ies)\b)");
    std::string out;
    auto from = message.cbegin();
    for (std::sregex_iterator it(message.begin(), message.end(), word), end; it != end; ++it) {
        const auto& match = *it;
        out.append(from, match[0].first);
        out += match[1].str() == "S" ? "Experiment" : "experiment";
        if (match[2].str() == "ies") {
            out += 's';
        }
        from = match[0].second;
    }
    out.append(from, message.cend());
    return out;
}

Task proxy(ApiContext& context, const json& input, std::string name, json args) {
    if (!context.workspaces) {
        co_return ApiResult::failure("store_unavailable", "workspaces are not configured");
    }
    const json sent = args;
    const auto result = control::settled(co_await call_workspace(
        context, input.at("customer_id"), name, workspace_uses_get(name), std::move(args)));
    if (!result) {
        // Zetta names the field at fault (zetta#1217); workspace_failure keeps that path, in the
        // caller's names (lake#450), and lake's word for the engine's study (lake#561).
        auto failure = workspace_failure(
            result.error(), sent, "", {{"run", "run_id"}, {"study", "experiment_id"}});
        failure.error.message = experiment_words(failure.error.message);
        co_return failure;
    }
    co_return ApiResult::success(*result);
}

// The engine names an experiment a study and its id `study` (zetta docs/ml/studies.md); lake says
// experiment and `experiment_id` to every person and client (lake#561). Answers are renamed here,
// once, on the way out.
json experiment_answer(json value) {
    if (value.is_object() && value.contains("study") && value.at("study").is_string()) {
        value["experiment_id"] = std::move(value["study"]);
        value.erase("study");
    }
    return value;
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
    json answer = {{"experiments", json::array()}, {"runs", json::array()}};
    std::set<std::string> trial_runs;
    for (const auto& item : studies.value.at("studies")) {
        for (const auto& row : item.at("trials")) {
            trial_runs.insert(row.at("run"));
        }
        if (selected(input, item.at("spec").at("project"), item.at("status"))) {
            answer["experiments"].push_back(item);
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
    // An experiment's events are the engine's study-events.<id> topic.
    const bool is_experiment = input.at("kind") == "experiment";
    const std::string id = input.at("id");
    if (id.find_first_of(" /\\\t\r\n:*?") != std::string::npos) {
        co_return ApiResult::failure(
            "invalid_input", "a concrete experiment or run id is required", "/id");
    }
    const auto target = completed(
        co_await proxy(context,
                       input,
                       is_experiment ? "get_study" : "get_run_metrics",
                       is_experiment ? json{{"study", id}} : json{{"run", id}, {"max_points", 4}}));
    if (!target.ok) {
        co_return target;
    }
    const auto from = input.value("from_sequence", std::uint64_t{1});
    if (!is_experiment && target.value.contains("metrics_suppressed")) {
        co_return ApiResult::success({{"events", json::array()}, {"next_sequence", from}});
    }
    const auto result = completed(
        co_await proxy(context,
                       input,
                       "receive_events",
                       {{"topic", (is_experiment ? "study-events." : "run-metrics.") + id},
                        {"from_sequence", from},
                        {"max", input.value("max_events", 100)},
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
    // Lake names a run `run_id` (lake#450) and an experiment `experiment_id` (lake#561); the
    // engine's calls take `run` and `study`.
    for (const auto& [lake_name, engine_name] :
         {std::pair{"run_id", "run"}, std::pair{"experiment_id", "study"}}) {
        if (args.contains(lake_name)) {
            args[engine_name] = std::move(args[lake_name]);
            args.erase(lake_name);
        }
    }
    if (name == "start_experiment") {
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
    if (result.ok) {
        result.value = experiment_answer(std::move(result.value));
    }
    co_return result;
}

// What each experiment capability does for the person calling it (lake#450).
std::string experiment_description(const std::string& name) {
    static const std::map<std::string, std::string> descriptions = {
        {"list_experiments",
         "List the training experiments and stand-alone runs in a workspace, optionally only one "
         "project's or those in one state, to see what has been tried."},
        {"get_experiment",
         "Read one training experiment: its search space, budgets, every trial with its score, "
         "and the model it promoted."},
        {"list_experiment_trials",
         "List one experiment's trials with their parameters, state and objective, to see which "
         "settings did best."},
        {"get_experiment_analysis",
         "Read what an experiment learned: the best trial, the optimization history, which "
         "parameters mattered, and a leaderboard with confidence bounds."},
        {"get_run_metrics",
         "Read one run's metric curves, parameters and summary to judge how it trained; the "
         "summary holds its held-out scores (held_out_auc and the like) once training recorded "
         "them. `artifacts` lists the evaluation charts a trainer logged: a GBDT classifier run "
         "with a held-out split logs a confusion matrix, an ROC curve and calibration, each "
         "named held_out; a regressor, or a run without a held-out split, logs none."},
        {"compare_runs",
         "Compare two to eight runs metric by metric on aligned steps, held-out scores "
         "included; leave `names` out to compare every metric they share."},
        {"start_experiment",
         "Start a training experiment that tries settings within your budgets, one trial run "
         "each, and keeps the best model; needs `confirm: true` from the person."},
        {"stop_experiment",
         "Stop a running experiment, keeping the trials it finished; needs `confirm: true` from "
         "the person."},
        {"stop_experiment_trial",
         "Stop one running trial of an experiment, marking it failed; needs `confirm: true` from "
         "the person."},
        {"promote_trial",
         "Make one trial's model the experiment's result, published to the project's models; "
         "needs `admin` on the account and `confirm: true`."},
        {"tail_experiment_events",
         "Wait for and read the live progress events of an experiment or run, from a sequence "
         "number, without consuming them."},
    };
    const auto found = descriptions.find(name);
    return found == descriptions.end() ? std::string() : found->second;
}

// A human title for each operation (lake#594): OpenAPI's summary and the MCP tool's title.
std::string experiment_title(std::string_view name) {
    static const std::map<std::string_view, std::string_view> titles = {
        {"list_experiments", "List experiments"},
        {"get_experiment", "Get an experiment"},
        {"list_experiment_trials", "List an experiment's trials"},
        {"get_experiment_analysis", "Analyze an experiment"},
        {"get_run_metrics", "Get a run's metrics"},
        {"compare_runs", "Compare runs"},
        {"start_experiment", "Start an experiment"},
        {"stop_experiment", "Stop an experiment"},
        {"stop_experiment_trial", "Stop a trial"},
        {"promote_trial", "Promote a trial"},
        {"tail_experiment_events", "Follow an experiment's events"},
    };
    const auto found = titles.find(name);
    return found == titles.end() ? std::string(name) : std::string(found->second);
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
    const bool names_run = properties.contains("run_id");
    const auto schema = object(std::move(properties), std::move(required));
    registry.add(Capability{
        .name = name,
        .title = experiment_title(name),
        .description = experiment_description(name),
        .input_schema = schema,
        .output_schema = std::move(output),
        .kind = read ? CapabilityKind::Read : CapabilityKind::Action,
        .long_poll = name == "tail_experiment_events",
        .default_wait_ms = name == "tail_experiment_events" ? 10000 : 0,
        .post_alias = read,
        .object = "customer",
        .action = std::move(permission),
        .resolve_object = customer_object,
        .authorizes_candidates = name.starts_with("list_"),
        .resolves_operator_audit_target = true,
        .async_handler = [name, upstream, schema](const json& input, ApiContext& context) -> Task {
            if (std::string at; !constraints(schema, input, at)) {
                co_return ApiResult::failure("invalid_input",
                                             (at.empty() ? std::string("input") : at.substr(1)) +
                                                 " is outside its bounds",
                                             at);
            }
            if (name.starts_with("list_")) {
                const auto authorized = authorize_list(input, context);
                if (!authorized.ok) {
                    co_return authorized;
                }
            }
            context.audit_kind = "experiment." + name;
            context.audit_details = {{"customer_id", input.at("customer_id")}};
            for (const auto* key :
                 {"experiment_id", "run_id", "runs", "kind", "id", "confirm", "name"}) {
                if (input.contains(key)) {
                    context.audit_details[key] = input.at(key);
                }
            }
            co_return completed(co_await dispatch(context, input, name, upstream));
        },
        .input_aliases = names_run
                             ? std::vector<std::pair<std::string, std::string>>{{"run", "run_id"}}
                             : std::vector<std::pair<std::string, std::string>>{}});
}
} // namespace

void register_experiment_capabilities(Registry& registry) {
    // Lake's names say experiment; each forwards to the engine's study operation named second
    // (lake#561, docs/api/experiments.md#names).
    add(registry,
        "list_experiments",
        "",
        {{"project", text()},
         {"state",
          choice({"", "pending", "running", "succeeded", "failed", "stopped", "complete"})}},
        json::array(),
        object({{"experiments", array(experiment())}, {"runs", array(run_summary())}},
               {"experiments", "runs"}));
    add(registry,
        "get_experiment",
        "get_study",
        {{"experiment_id", identifier()}},
        {"experiment_id"},
        experiment());
    add(registry,
        "list_experiment_trials",
        "list_trials",
        {{"experiment_id", identifier()},
         {"state", choice({"running", "pruned", "failed", "complete"})}},
        {"experiment_id"},
        object({{"experiment_id", identifier()}, {"trials", array(trial())}},
               {"experiment_id", "trials"}));
    add(registry,
        "get_experiment_analysis",
        "get_study_analysis",
        {{"experiment_id", identifier()}},
        {"experiment_id"},
        analysis());
    auto names = array({{"type", "string"}, {"pattern", "^[A-Za-z0-9_.-]{1,128}$"}});
    names["uniqueItems"] = true;
    add(registry,
        "get_run_metrics",
        "get_run_metrics",
        {{"run_id", identifier()},
         {"names", names},
         {"from_step", integer(0, 9007199254740991)},
         {"max_points", integer(4, 10000)}},
        {"run_id"},
        metrics());
    auto runs = array(identifier());
    runs.update({{"minItems", 2}, {"maxItems", 8}, {"uniqueItems", true}});
    names["minItems"] = 1;
    add(registry, "compare_runs", "compare_runs", {{"runs", runs}, {"names", names}},
        {"runs"}, comparison());
    auto properties = experiment_spec().at("properties");
    properties["from_defaults"] = flag();
    // Zetta fills objective and budgets field by field when from_defaults is set, so each of them
    // and each of their fields is optional here; the handler requires both without it.
    for (const char* field : {"objective", "budgets"}) {
        properties[field]["required"] = json::array();
    }
    add(registry,
        "start_experiment",
        "create_study",
        properties,
        {"name", "methodology"},
        experiment(),
        "write");
    add(registry,
        "stop_experiment",
        "stop_study",
        {{"experiment_id", identifier()}},
        {"experiment_id"},
        object(
            {{"experiment_id", identifier()}, {"status", choice({"stopped"})}, {"reason", text()}},
            {"experiment_id", "status", "reason"}),
        "write");
    add(registry,
        "stop_experiment_trial",
        "stop_trial",
        {{"experiment_id", identifier()}, {"run_id", identifier()}},
        {"experiment_id", "run_id"},
        object(
            {{"experiment_id", identifier()}, {"run", identifier()}, {"state", choice({"failed"})}},
            {"experiment_id", "run", "state"}),
        "write");
    add(registry,
        "promote_trial",
        "promote_trial",
        {{"experiment_id", identifier()}, {"run_id", identifier()}},
        {"experiment_id", "run_id"},
        object({{"experiment_id", identifier()},
                {"run", identifier()},
                {"model", identifier()},
                {"project", text()}},
               {"experiment_id", "run", "model", "project"}),
        "admin");
    add(registry,
        "tail_experiment_events",
        "receive_events",
        {{"kind", choice({"experiment", "run"})},
         {"id", identifier()},
         {"from_sequence", integer(1, 9007199254740991)},
         {"max_events", integer(1, 1000)},
         {"wait_ms", integer(0, 30000)}},
        {"kind", "id"},
        events());
}
} // namespace lake::api
