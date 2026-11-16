// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

// Workspace dataset rules lake applies on top of what the workspace Zetta answers
// (docs/api/workspaces.md, "Datasets"). Pure; no I/O.
namespace lake::api::datasets {

// True when `database` holds tables the engine writes as the output of a run (entity resolution
// pairs, churn scores, segment assignments, decisions ...) rather than data a person loaded:
// every `<methodology>_results` database, plus the engine's own `forecasts`, `feedback` and
// `system` databases.
bool engine_database(std::string_view database);

// "result" for a table in an engine database, otherwise "data".
std::string kind_of(std::string_view database);

// Why `name` cannot be a dataset (or database) name, or empty when it can. A name must be one the
// workspace's SQL can query unquoted: a letter or underscore, then letters, digits and
// underscores, at most 63 characters, and not a SQL keyword.
std::string name_problem(std::string_view name);

// Why `content` is not a CSV file the workspace loads faithfully, or empty when it is. The rules
// mirror the workspace's reader (one record per line, double-quoted fields, "" escapes a quote):
// the header must name every column once, every quoted field must close on its own line, and
// every data row must have exactly as many fields as the header. The message names the line.
std::string csv_problem(std::string_view content);

// One dataset id form for every surface: `<database>/<name>`. `parse_id` also reads
// `<database>.<name>` and a bare `<name>` (in `datasets`); it answers nothing for an empty part.
std::string id_of(std::string_view database, std::string_view name);
struct DatasetRef {
    std::string database;
    std::string name;
};
std::optional<DatasetRef> parse_id(std::string_view id);

// Adds `id` and `kind` to each entry of a list_datasets answer ({"datasets": [...]}). An entry
// that carries the name as `dataset` (the system.datasets column) also gains `name`.
void annotate_list(nlohmann::json& listed);
// One dataset as get_dataset answers it (lake#594): `format` says what the person loaded
// (`source_format`, csv for a CSV upload) and the engine's own storage format moves to
// `storage_format`; a name SQL cannot name (from before the naming rule, such as one with a
// hyphen) is marked `sql_queryable: false` with what to do. annotate_list applies the same to
// each listed dataset.
void annotate_one(nlohmann::json& dataset);
// Whether SQL can name this table: letters, digits and underscores, not starting with a digit.
bool sql_name(std::string_view name);

}  // namespace lake::api::datasets
