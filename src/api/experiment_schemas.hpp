#pragma once

#include <limits>

#include <nlohmann/json.hpp>

namespace lake::api::experiments {
using nlohmann::json;

inline json object(json properties, json required = json::array()) {
    return {{"type", "object"}, {"properties", std::move(properties)},
            {"required", std::move(required)}, {"additionalProperties", false}};
}

inline json text() {
    return {{"type", "string"}};
}

inline json identifier() {
    return {{"type", "string"}, {"minLength", 1}, {"maxLength", 512}};
}

inline json integer(std::int64_t minimum = 0,
                    std::int64_t maximum = std::numeric_limits<std::int64_t>::max()) {
    return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}

inline json number() {
    return {{"type", "number"}};
}

inline json flag() {
    return {{"type", "boolean"}};
}

inline json choice(json values) {
    return {{"type", "string"}, {"enum", std::move(values)}};
}

inline json array(json items) {
    return {{"type", "array"}, {"items", std::move(items)}};
}

inline json dictionary(json value, std::string pattern = "^[A-Za-z0-9_.-]{1,128}$") {
    return {{"type", "object"}, {"patternProperties", {{pattern, std::move(value)}}},
            {"additionalProperties", false}};
}

inline json scalar(bool nullable = false) {
    json types = {"string", "number", "integer", "boolean"};
    if (nullable) {
        types.push_back("null");
    }
    return {{"type", types}};
}

inline json objective() {
    return object({{"metric", identifier()}, {"direction", choice({"minimize", "maximize"})},
                   {"validation_split", identifier()}},
                  {"metric", "direction", "validation_split"});
}

inline json budgets() {
    return object({{"max_trials", integer(1)}, {"max_wall_time_ms", integer(0, 9007199254740991)},
                   {"max_parallel_trials", integer(1)},
                   {"cost_cap", {{"type", "number"}, {"minimum", 0}}}},
                  {"max_trials", "max_wall_time_ms", "max_parallel_trials", "cost_cap"});
}

inline json search_space() {
    auto schema = array(object({{"name", identifier()},
        {"kind", choice({"int", "float", "categorical"})}, {"minimum", number()},
        {"maximum", number()}, {"step", {{"type", "number"}, {"minimum", 0}}},
        {"log", flag()}, {"choices", array(scalar())}, {"condition_parameter", text()},
        {"condition_values", array(scalar())}}, {"name", "kind", "minimum", "maximum", "step",
        "log", "choices", "condition_parameter", "condition_values"}));
    schema["minItems"] = 1;
    return schema;
}

inline json experiment_spec() {
    auto released = array(identifier());
    released["uniqueItems"] = true;
    return object({{"name", identifier()}, {"project", text()}, {"pipeline", text()},
        {"dataset", text()}, {"methodology", identifier()}, {"objective", objective()},
        {"search_space", search_space()}, {"sampler", choice({"grid", "random", "tpe"})},
        {"pruner", choice({"none", "median", "asha"})}, {"budgets", budgets()},
        {"seed", integer(0, 9007199254740991)}, {"differential_privacy", flag()},
        {"released_metrics", released}}, {"name", "project", "pipeline", "dataset",
        "methodology", "objective", "search_space", "sampler", "pruner", "budgets", "seed",
        "differential_privacy", "released_metrics"});
}

inline json trial() {
    return object({{"number", integer()}, {"run", identifier()}, {"params", dictionary(scalar())},
        {"state", choice({"running", "pruned", "failed", "complete"})}, {"objective", number()},
        {"seed", integer(0, 9007199254740991)}, {"cost", number()}, {"reason", text()},
        {"model", text()}, {"intermediate", array(object({{"step", integer()},
        {"value", number()}}, {"step", "value"}))}}, {"number", "run", "params", "state", "seed",
        "cost", "intermediate", "reason", "model"});
}

inline json experiment() {
    return object({{"id", identifier()},
                   {"spec", experiment_spec()},
                   {"status", choice({"running", "stopped", "complete"})},
                   {"created", text()},
                   {"updated", text()},
                   {"trials", array(trial())},
                   {"spent_cost", number()},
                   {"stop_reason", text()},
                   {"promoted_model", text()}},
                  {"id",
                   "spec",
                   "status",
                   "created",
                   "updated",
                   "trials",
                   "spent_cost",
                   "stop_reason",
                   "promoted_model"});
}

inline json analysis() {
    auto best = object({{"run", text()}, {"number", integer()}, {"value", number()},
        {"params", dictionary(scalar())}}, {"run", "number", "value", "params"});
    best["type"] = {"object", "null"};
    return object({{"best_trial", best}, {"optimization_history", array(object({{"number", integer()},
        {"run", text()}, {"value", number()}, {"best", number()}},
        {"number", "run", "value", "best"}))}, {"parameter_importances", array(object({
        {"parameter", text()}, {"importance", number()}}, {"parameter", "importance"}))},
        {"parallel_coordinates", array(object({{"run", text()}, {"params", dictionary(scalar())},
        {"value", number()}}, {"run", "params", "value"}))},
        {"leaderboard", array(object({{"rank", integer()}, {"run", text()}, {"value", number()},
        {"confidence_low", number()}, {"confidence_high", number()}},
        {"rank", "run", "value", "confidence_low", "confidence_high"}))}},
        {"best_trial", "optimization_history", "parameter_importances", "parallel_coordinates",
        "leaderboard"});
}

inline json artifact_variant(std::string kind, json data) {
    return object({{"type", {{"type", "string"}, {"const", kind}}}, {"name", text()},
        {"time", text()}, {"data", std::move(data)}}, {"type", "name", "time", "data"});
}

inline json artifact() {
    const auto numbers = array(number());
    const auto curve = object({{"x", numbers}, {"y", numbers}, {"thresholds", numbers}},
                              {"x", "y", "thresholds"});
    return {{"oneOf", {
        artifact_variant("confusion_matrix", object({{"labels", array(text())},
            {"matrix", array(numbers)}}, {"labels", "matrix"})),
        artifact_variant("roc_curve", curve), artifact_variant("pr_curve", curve),
        artifact_variant("calibration", object({{"predicted", numbers}, {"observed", numbers},
            {"counts", array(integer())}}, {"predicted", "observed", "counts"})),
        artifact_variant("feature_importance", object({{"features", array(text())},
            {"values", numbers}}, {"features", "values"})),
        artifact_variant("sample_predictions", object({{"rows", array(dictionary(scalar(true),
            "^.{1,128}$"))}}, {"rows"}))}}};
}

inline json metrics() {
    const auto summary = object({{"final", number()}, {"final_step", integer()},
        {"best", number()}, {"best_step", integer()}, {"direction", choice({"min", "max"})}},
        {"final", "final_step", "best", "best_step", "direction"});
    const auto point = object({{"step", integer()}, {"value", number()}, {"time", text()}},
                              {"step", "value", "time"});
    return object({{"run", identifier()}, {"params", dictionary(scalar(true))},
        {"summary", dictionary(summary)}, {"artifacts", array(artifact())},
        {"series", array(object({{"name", text()}, {"points", array(point)}}, {"name", "points"}))},
        {"metrics_suppressed", choice({"differential_privacy"})}},
        {"run", "params", "summary", "artifacts", "series"});
}

inline json run_summary() {
    return object({{"id", identifier()}, {"project", text()}, {"graph", text()},
        {"deployment", text()}, {"status", text()}, {"created", text()}, {"updated", text()},
        {"log_count", integer()}, {"output_count", integer()},
        {"metrics_suppressed", choice({"differential_privacy"})}},
        {"id", "project", "graph", "deployment", "status", "created", "updated", "log_count",
        "output_count"});
}

inline json comparison() {
    const auto value = object({{"run", identifier()}, {"value", number()}, {"time", text()}},
                              {"run", "value", "time"});
    const auto point = object({{"step", integer()}, {"values", array(value)}}, {"step", "values"});
    return object({{"runs", array(identifier())}, {"metrics", array(object({{"name", text()},
        {"points", array(point)}}, {"name", "points"}))}, {"details", array(metrics())}},
        {"runs", "metrics", "details"});
}

inline json events() {
    const auto event = object({{"sequence", integer(1, 9007199254740991)}, {"id", text()},
        {"encoding", text()}, {"payload", text()}, {"from", text()}, {"correlation_id", text()}},
        {"sequence", "id", "encoding", "payload"});
    return object({{"events", array(event)}, {"next_sequence", integer(1, 9007199254740991)}},
                  {"events", "next_sequence"});
}
} // namespace lake::api::experiments
