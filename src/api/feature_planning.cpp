// Feature planning for a dataset in a workspace (lake#1153, docs/api/feature-planning.md).
#include "feature_planning.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lake/control/agent_bus.hpp"

namespace lake::api {

namespace {

using json = nlohmann::json;

// Names that say a column is the date its row was observed at. A date column named otherwise
// (`signup_date`, `last_login`) is a feature, not the cutoff, so it is never taken for one.
constexpr auto snapshot_names = std::to_array<std::string_view>({
    "as_of",          "asof",          "as_of_date",       "as_of_time",   "snapshot",
    "snapshot_date",  "snapshot_at",   "snapshot_time",    "snapshot_ts",  "observed_at",
    "observed_on",    "observed_date", "observation_date", "observation_time",
    "cutoff",         "cutoff_date",   "cutoff_at",        "prediction_date",
    "scored_at",      "score_date",    "extracted_at",     "extract_date",
});

// How many of the newest values are read to find the latest date: the engine's SQL has no
// IS NOT NULL, so a few nulls or blanks sorting first are stepped over.
constexpr int latest_values_read = 16;

std::string lowered(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

// Only plain names go into the statement Lake writes.
bool sql_identifier(std::string_view name) {
    if (name.empty() || name.size() > 128 ||
        !(std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_')) {
        return false;
    }
    return std::ranges::all_of(name, [](unsigned char character) {
        return std::isalnum(character) || character == '_';
    });
}

// An ISO date (`2026-09-27`, with or without a time) or an epoch number: the values that order
// as dates when sorted, so the largest is the latest.
std::optional<std::string> date_value(const json& value) {
    if (value.is_number() && value.get<double>() > 0) {
        return value.dump();
    }
    if (!value.is_string()) {
        return std::nullopt;
    }
    const std::string& text = value.get_ref<const std::string&>();
    const auto digit = [&](std::size_t at) {
        return at < text.size() && std::isdigit(static_cast<unsigned char>(text[at])) != 0;
    };
    if (text.size() >= 10 && digit(0) && digit(1) && digit(2) && digit(3) && text[4] == '-' &&
        digit(5) && digit(6) && text[7] == '-' && digit(8) && digit(9)) {
        return text;
    }
    return std::nullopt;
}

std::string message_of(const control::ZettaControlError& error) {
    return error.message.empty() ? error.code : error.message;
}

using AsOfResult = std::expected<DatasetAsOf, std::string>;

AsOfResult no_as_of(std::string reason) {
    return AsOfResult(std::unexpect, std::move(reason));
}

// The dataset reference plan_features was given, as get_dataset and analyze_dataset take it.
struct Reference {
    std::string database;
    std::string table;
};

Reference reference_of(const json& input) {
    std::string named = input.value("dataset_path", std::string());
    if (named.empty()) {
        named = input.value("dataset", std::string());
    }
    if (const auto dot = named.find('.'); dot != std::string::npos) {
        return {named.substr(0, dot), named.substr(dot + 1)};
    }
    return {input.value("database", std::string()), named};
}

json analysis_input(const json& arguments) {
    json input = json::object();
    for (const char* field :
         {"dataset", "database", "dataset_path", "label", "sample_rows", "time_column"}) {
        if (arguments.contains(field)) {
            input[field] = arguments.at(field);
        }
    }
    return input;
}

std::string label_of(const json& analysis, const json& arguments) {
    if (const std::string label = arguments.value("label", std::string()); !label.empty()) {
        return label;
    }
    for (const json& column : analysis.value("columns", json::array())) {
        const json roles = column.value("role_candidates", json::array());
        if (std::ranges::find(roles, json("label")) != roles.end()) {
            return column.value("name", std::string());
        }
    }
    return {};
}

json step(std::string capability, std::string does, json input = nullptr) {
    json entry{{"capability", std::move(capability)}, {"does", std::move(does)}};
    if (!input.is_null()) {
        entry["input"] = std::move(input);
    }
    return entry;
}

// What in Lake performs each preparation the engine names (zetta's featurize_plan.cpp). The
// engine's SQL is a proposal over the workspace's own table; Lake runs a read-only SELECT with
// workspace_sql, and a new dataset is loaded with upload_dataset.
json performed_by(const json& transform, const json& plan) {
    const std::string name = transform.value("name", std::string());
    const std::string dataset = plan.value("dataset", std::string());
    if (name == "add_snapshot_date") {
        return json::array(
            {step("plan_features",
                  "declare the date these rows were observed at as as_of (and the column that "
                  "holds each row's snapshot date as time_column, if there is one); nothing in "
                  "the data changes",
                  {{"dataset", dataset}, {"as_of", "<the date the rows were observed at>"}}),
             step("upload_dataset",
                  "or load the table again with an as_of column holding each row's snapshot "
                  "date; Lake then reads the cutoff from it")});
    }
    if (name == "aggregate_events_to_entity") {
        json steps = json::array();
        if (const std::string sql =
                transform.value("proposed_create_dataset_from_query_sql", std::string());
            !sql.empty()) {
            steps.push_back(step("workspace_sql",
                                 "run the proposed read-only SELECT for one row per entity",
                                 {{"statement", sql}}));
        }
        steps.push_back(step("upload_dataset",
                             "load the per-entity rows as a new dataset, then plan that one"));
        return steps;
    }
    if (name == "join_label_table") {
        return json::array({step("upload_dataset",
                                 "load the table joined to its label on the reported key as a "
                                 "new dataset (the workspace SQL has no JOIN)")});
    }
    if (name == "add_time_column" || name == "add_entity_key" || name == "capture_label" ||
        name == "add_ranking_group" || name == "add_treatment" || name == "pivot_long_to_wide") {
        return json::array({step("upload_dataset",
                                 "load the table again with the missing field added, then plan "
                                 "the new dataset")});
    }
    return json::array({step("upload_dataset",
                             "load the prepared table as a new dataset, then plan that one")});
}

} // namespace

bool is_snapshot_column(std::string_view name) {
    const std::string wanted = lowered(name);
    return std::ranges::contains(snapshot_names, std::string_view(wanted));
}

liquid::runtime::task<std::expected<DatasetAsOf, std::string>>
dataset_as_of(ApiContext& context,
              std::string customer_id,
              const json& reference,
              std::string time_column) {
    Reference table = reference_of(reference);
    if (table.table.empty()) {
        co_return no_as_of("no dataset is named");
    }
    json lookup{{"dataset", table.table}};
    if (!table.database.empty()) {
        lookup["database"] = table.database;
    }
    auto described = control::settled(co_await call_workspace(context, customer_id, "get_dataset", true, lookup));
    if (!described) {
        co_return no_as_of("get_dataset: " + message_of(described.error()));
    }
    if (table.database.empty()) {
        table.database = described->value("database", std::string("datasets"));
    }
    std::vector<std::string> columns;
    for (const json& column : described->value("columns", json::array())) {
        if (column.is_object() && column.contains("name") && column["name"].is_string()) {
            columns.push_back(column["name"].get<std::string>());
        }
    }
    std::string column;
    if (!time_column.empty()) {
        if (!std::ranges::contains(columns, time_column)) {
            co_return no_as_of("time_column " + time_column + " is not a column of " +
                                      table.database + "." + table.table);
        }
        column = time_column;
    } else if (const auto found = std::ranges::find_if(columns, is_snapshot_column);
               found != columns.end()) {
        column = *found;
    } else {
        co_return no_as_of(
            "no column of " + table.database + "." + table.table +
            " is named as each row's snapshot date (as_of, snapshot_date, observed_at, ...)");
    }
    if (!sql_identifier(column) || !sql_identifier(table.database) ||
        !sql_identifier(table.table)) {
        co_return no_as_of("the snapshot column " + column +
                                  " cannot be read by name; pass as_of");
    }
    const std::string statement = "SELECT " + column + " FROM " + table.database + "." +
                                  table.table + " ORDER BY " + column + " DESC LIMIT " +
                                  std::to_string(latest_values_read);
    auto rows = control::settled(
        co_await call_workspace(context, customer_id, "sql", true, {{"statement", statement}}));
    if (!rows) {
        co_return no_as_of("reading the latest " + column + ": " +
                                  message_of(rows.error()));
    }
    for (const json& row : rows->value("rows", json::array())) {
        if (!row.is_object() || !row.contains(column)) {
            continue;
        }
        if (auto value = date_value(row.at(column))) {
            co_return AsOfResult(DatasetAsOf{column, std::move(*value)});
        }
    }
    co_return no_as_of(column + " holds no ISO dates or epoch values to take the latest "
                                       "of; pass as_of");
}

liquid::runtime::task<std::expected<json, ApiResult>>
prepare_feature_plan(ApiContext& context, std::string customer_id, json& arguments) {
    json inferred = json::object();
    if (!arguments.is_object()) {
        co_return std::expected<json, ApiResult>(std::move(inferred));
    }
    const bool named = arguments.contains("dataset") || arguments.contains("dataset_path");
    if (!named) {
        // The engine says which reference is missing.
        co_return std::expected<json, ApiResult>(std::move(inferred));
    }
    if (!arguments.contains("methodology")) {
        auto analysis =
            control::settled(co_await call_workspace(
                context, customer_id, "analyze_dataset", true, analysis_input(arguments)));
        if (!analysis) {
            co_return std::expected<json, ApiResult>(std::unexpect, ApiResult::failure(
                analysis.error().code == "invalid_input" ? "invalid_input" : "store_error",
                "plan_features needs a methodology, and the dataset could not be analyzed to "
                "choose one: " +
                    message_of(analysis.error()),
                "/input/dataset"));
        }
        const std::string label = label_of(*analysis, arguments);
        const std::string methodology = label.empty() ? "M-3" : "M-1";
        arguments["methodology"] = methodology;
        inferred["methodology"] = {
            {"id", methodology},
            {"reason",
             label.empty()
                 ? std::string("no methodology was named and the dataset has no label column, "
                               "so it is planned for segmentation (M-3)")
                 : "no methodology was named and " + label +
                       " is a label, so it is planned for classification (M-1)"}};
    }
    if (!arguments.contains("as_of")) {
        auto awaited = co_await dataset_as_of(
            context, customer_id, arguments, arguments.value("time_column", std::string()));
        AsOfResult found = awaited ? std::move(*awaited)
                                   : no_as_of("reading the cutoff: " + awaited.error().message);
        if (found) {
            arguments["as_of"] = found->value;
            if (!arguments.contains("time_column")) {
                arguments["time_column"] = found->column;
            }
            inferred["as_of"] = {
                {"column", found->column},
                {"value", found->value},
                {"reason", "no as_of was named; the latest " + found->column +
                               " in the dataset is the cutoff its features are computed at"}};
        } else {
            inferred["as_of_unavailable"] = found.error();
        }
    }
    co_return std::expected<json, ApiResult>(std::move(inferred));
}

void describe_feature_plan(json& plan, const json& inferred) {
    if (!plan.is_object()) {
        return;
    }
    json shown = inferred.is_object() ? inferred : json::object();
    std::string unavailable;
    if (shown.contains("as_of_unavailable")) {
        unavailable = shown["as_of_unavailable"].get<std::string>();
        shown.erase("as_of_unavailable");
    }
    if (!shown.empty()) {
        plan["inferred"] = std::move(shown);
    }
    const auto transform = plan.find("transform_required");
    if (transform == plan.end() || !transform->is_object()) {
        return;
    }
    (*transform)["performed_by"] = performed_by(*transform, plan);
    if (!unavailable.empty() && transform->value("name", std::string()) == "add_snapshot_date") {
        (*transform)["why_no_as_of"] = unavailable;
    }
}

} // namespace lake::api
