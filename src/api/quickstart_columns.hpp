#pragma once

// How the quickstart reads a dataset's columns and prepares them for its first model (lake#1181,
// docs/api/quickstart.md#columns): pure rules over the workspace engine's own column profile
// (`analyze_dataset`) and the rows, so the tests run them without an engine.
//
// - detect_columns: each column's kind (numeric, categorical, boolean, free text, date or time,
//   identifier, empty, constant, unsupported), whether the default methodology can use it as an
//   input and whether it can be predicted, each with the reason in plain words;
// - suggest: a sensible column to predict (and the value to look for) and a column that names a
//   row;
// - prepare_rows: the training copy's rows, when the dataset cannot be trained on as it is: only
//   the usable inputs, the column that names a row (or a row number), and the column to predict
//   as 0/1;
// - refusal / explain_refusal / explain_run_failure: the engine's `transform_required` answers and
//   failed runs, in plain words with what to choose.

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lake::api::quickstart_columns {

using json = nlohmann::json;

// The methodology the quickstart prepares columns for: Train → Models' default, gradient-boosted
// trees answering a yes-or-no question, which read numbers and categories (zetta
// docs/data/featurization.md).
inline constexpr std::string_view prepared_methodology = "M-1";
// A category with more values than this is not offered as a column to predict.
inline constexpr std::size_t max_target_values = 20;
// The engine one-hot encodes a text category with at most this many values (zetta#1203).
inline constexpr std::size_t max_category_values = 32;
// The rows analyze_dataset samples for the quickstart (its maximum).
inline constexpr int analysis_sample_rows = 10000;

// One column per entry, in the file's order: {name, type, kind, title, distinct_count, null_rate,
// values, feature: {use, reason}, predict: {can, reason, values?, positive?}, identifier}.
// `analysis` is analyze_dataset's answer; `stored` is the dataset's columns as get_dataset answers
// them ({name, type}), whose order and storage types are kept. A column the analysis does not
// name keeps kind null.
json detect_columns(const json& analysis, const json& stored);

// True when every column carries a detected kind.
bool detected(const json& columns);

// {target, target_value, entity_column}: a column whose name says it is an outcome, else the last
// two-valued column, else a category of a few values with its most common value; and the first
// identifier for the row's name. Each null when there is none.
json suggest(const json& columns);

// The column named `name`, or nullptr.
const json* find(const json& columns, std::string_view name);

// What a person chose for training.
struct Choice {
    std::string target;
    std::string target_value;  // the value the model looks for; empty for a two-valued column
    std::string entity_column; // empty: Lake numbers the rows
};

// Why a choice cannot be trained, in plain words (empty when it can), and the input it names.
struct Problem {
    std::string message;
    std::string path;
};
Problem check_choice(const json& columns, const Choice& choice);

// The value a two-valued column's model looks for: a yes-like value (true, yes, y, 1), else the
// less common of the two, else the later in order.
std::string positive_value(const json& column);

// A value as text, the way the quickstart compares values: strings as they are, numbers and
// booleans as JSON writes them.
std::string value_text(const json& value);

// How a refusal is fixed by the training copy: drop these columns too, look for dates in the
// values themselves, number the rows.
struct Fixes {
    std::set<std::string> drop;
    bool dates_from_values = false;
    bool number_rows = false;
};

// The training copy.
struct Prepared {
    bool copy = false;               // false: the dataset trains as it is
    std::string content;             // JSON Lines, one object per row (empty when !copy)
    std::string entity;              // the column that names a row in what is trained on
    std::vector<std::string> header; // the copy's columns in order: the row's name, inputs, target
    json used = json::array();       // the input columns kept
    json dropped = json::array();    // [{name, reason}]
    json label = nullptr;            // {column, positive, values: {"1": "...", "0": "..."}}
    std::size_t rows = 0;
    std::size_t skipped = 0; // rows with no value to predict
    std::string note;        // why the rows were numbered when a name was chosen
};

// The column a row number is written to: `row_number`, or the next free `row_number_<n>`.
std::string row_number_column(const json& columns);

// Whether the dataset can be trained on as it is, without reading its rows: every input is usable
// or the engine leaves it out itself, the column to predict is already 0/1, and a row is named.
bool trains_as_is(const json& columns, const Choice& choice, const Fixes& fixes);

// The training copy of `rows` (objects, as `SELECT *` answers them). Refused with a plain message
// when no row has a value to predict, or the value to look for appears in none.
struct Prepare {
    std::optional<Prepared> prepared;
    std::string problem;
};
Prepare prepare_rows(const json& columns,
                     const std::vector<json>& rows,
                     const Choice& choice,
                     const Fixes& fixes);

// The most upload_dataset takes in one call (the engine's limit).
inline constexpr std::size_t max_upload_bytes = 16 * 1024 * 1024;

// The training copy as CSV (a header row, then one line per row), for a copy whose JSON Lines
// would be larger than upload_dataset takes.
std::string csv_content(const Prepared& prepared);

// The columns `rows` hold as dates or times (at least 95% of non-empty values are ISO dates or
// timestamps), the engine's own rule.
std::set<std::string> date_columns_in(const std::vector<json>& rows);

// The engine's `transform_required: <name>: <description>` refusal, or nullopt.
struct Refusal {
    std::string name;
    std::string description;
};
std::optional<Refusal> refusal(std::string_view message);

// Whether Lake fixes this refusal itself (by the training copy), and how.
std::optional<Fixes> fix_for(const Refusal& refused, const json& columns, const Choice& choice);

// A refusal Lake could not fix, in plain words with what to choose; never the engine's code.
std::string explain_refusal(const Refusal& refused, const json& columns, const Choice& choice);

// A failed training run's reason (get_run's `error`) in plain words.
std::string explain_run_failure(std::string_view reason, const Choice& choice);

// A snapshot date as typed (2026-10-24, or an ISO-8601 timestamp): the same text when it is one,
// empty otherwise.
std::string snapshot_date(std::string_view typed);

} // namespace lake::api::quickstart_columns
