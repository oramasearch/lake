// How the quickstart reads a dataset's columns and prepares them for its first model (lake#1181,
// docs/api/quickstart.md#columns). Pure rules; quickstart_capabilities.cpp calls the engine.

#include "quickstart_columns.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace lake::api::quickstart_columns {
namespace {

std::string text_of(const json& value, const char* key) {
    if (!value.is_object()) {
        return {};
    }
    const auto found = value.find(key);
    return found != value.end() && found->is_string() ? found->get<std::string>() : std::string();
}

std::string lowered(std::string_view text) {
    std::string out(text);
    std::ranges::transform(
        out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::size_t count_of(const json& value, const char* key) {
    if (!value.is_object() || !value.contains(key) || !value.at(key).is_number()) {
        return 0;
    }
    const double number = value.at(key).get<double>();
    return number > 0 ? static_cast<std::size_t>(number) : 0;
}

std::string plural(std::size_t count, std::string_view one, std::string_view many) {
    return std::to_string(count) + " " + std::string(count == 1 ? one : many);
}

// A value with nothing in it: missing, null or an empty string.
bool empty_value(const json& value) {
    return value.is_null() || (value.is_string() && value.get<std::string>().empty());
}

bool digits(std::string_view text, std::size_t from, std::size_t length, int& out) {
    if (from + length > text.size()) {
        return false;
    }
    const char* first = text.data() + from;
    const auto result = std::from_chars(first, first + length, out);
    return result.ec == std::errc() && result.ptr == first + length;
}

// YYYY-MM-DD, a real day.
bool calendar_date(std::string_view text) {
    int year = 0;
    int month = 0;
    int day = 0;
    if (text.size() < 10 || text[4] != '-' || text[7] != '-' || !digits(text, 0, 4, year) ||
        !digits(text, 5, 2, month) || !digits(text, 8, 2, day)) {
        return false;
    }
    using namespace std::chrono;
    return year_month_day{std::chrono::year{year},
                          std::chrono::month{static_cast<unsigned>(month)},
                          std::chrono::day{static_cast<unsigned>(day)}}
        .ok();
}

// An ISO-8601 date or timestamp: YYYY-MM-DD, then optionally T or a space, HH:MM, :SS, a
// fraction, and Z or an offset.
bool iso_instant(std::string_view text) {
    if (!calendar_date(text)) {
        return false;
    }
    if (text.size() == 10) {
        return true;
    }
    if (text[10] != 'T' && text[10] != ' ') {
        return false;
    }
    int hour = 0;
    int minute = 0;
    if (!digits(text, 11, 2, hour) || text.size() < 16 || text[13] != ':' ||
        !digits(text, 14, 2, minute) || hour > 23 || minute > 59) {
        return false;
    }
    std::size_t at = 16;
    if (at < text.size() && text[at] == ':') {
        int second = 0;
        if (!digits(text, at + 1, 2, second) || second > 60) {
            return false;
        }
        at += 3;
        if (at < text.size() && text[at] == '.') {
            ++at;
            const std::size_t fraction = at;
            while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at])) != 0) {
                ++at;
            }
            if (at == fraction) {
                return false;
            }
        }
    }
    if (at == text.size()) {
        return true;
    }
    if (text[at] == 'Z') {
        return at + 1 == text.size();
    }
    if (text[at] == '+' || text[at] == '-') {
        int offset_hour = 0;
        int offset_minute = 0;
        return text.size() == at + 6 && digits(text, at + 1, 2, offset_hour) &&
               text[at + 3] == ':' && digits(text, at + 4, 2, offset_minute) && offset_hour <= 23 &&
               offset_minute <= 59;
    }
    return false;
}

bool yes_like(std::string_view value) {
    const std::string word = lowered(value);
    return word == "true" || word == "yes" || word == "y" || word == "1" || word == "t";
}

// A column name in lower-case words: letters and digits kept, everything else one space
// ("Customer Id" and "customer_id" both read "customer id"; "Unnamed: 0" reads "unnamed 0").
std::string name_words(std::string_view name) {
    std::string out;
    bool space = false;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) != 0) {
            if (space && !out.empty()) {
                out += ' ';
            }
            space = false;
            out += static_cast<char>(std::tolower(u));
        } else {
            space = true;
        }
    }
    return out;
}

// Whether a column's name says it numbers or names the row (lake#1242): a file's row number
// (`Index`, `Row`, `#`, pandas' `Unnamed: 0`), an id (`id`, `Customer Id`, `customer_id`,
// `CustomerId`, `customerID`), a key or a UUID. Measures that merely end in "index" (`price
// index`) or "number" (`number of orders`) are not.
bool row_naming_name(std::string_view name) {
    const std::string words = name_words(name);
    for (const char* exact : {"",           "index",      "idx",        "row",       "rows",
                              "row number", "row num",    "rownum",     "row no",    "row id",
                              "rowid",      "row index",  "no",         "id",        "key",
                              "uuid",       "guid",       "serial",     "seq",       "sequence",
                              "unnamed 0",  "record id",  "record number", "record no", "level 0"}) {
        if (words == exact) {
            return true;
        }
    }
    for (const char* suffix : {" id", " key", " uuid", " guid"}) {
        if (words.ends_with(suffix)) {
            return true;
        }
    }
    // camelCase: CustomerId, customerID (a lower-case letter, then Id or ID at the end).
    if (name.size() > 2) {
        const std::string_view last = name.substr(name.size() - 2);
        const auto before = static_cast<unsigned char>(name[name.size() - 3]);
        if ((last == "Id" || last == "ID") && std::islower(before) != 0) {
            return true;
        }
    }
    return false;
}

// Whether a column's name says it holds a person's name or contact details, which describe who a
// row is, never what to group rows by (lake#1242).
bool personal_name(std::string_view name) {
    const std::string words = name_words(name);
    for (const char* exact : {"name",       "first name", "last name",   "full name",
                              "surname",    "given name", "family name", "middle name",
                              "firstname",  "lastname",   "fullname",    "contact name",
                              "email",      "e mail",     "phone",       "telephone",
                              "mobile",     "fax",        "website",     "url",
                              "web site",   "homepage",   "address",     "street",
                              "street address"}) {
        if (words == exact) {
            return true;
        }
    }
    for (const char* part : {"email", "e mail", "phone", "website", "url", "first name",
                             "last name", "full name", "address"}) {
        const std::string needle(part);
        if (words.starts_with(needle + " ") || words.ends_with(" " + needle) ||
            words.find(" " + needle + " ") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// An email address, a web address or a phone number.
bool contact_value(std::string_view value) {
    const auto at = value.find('@');
    if (at != std::string_view::npos && at > 0 && value.find('.', at) != std::string_view::npos &&
        value.find(' ') == std::string_view::npos) {
        return true;
    }
    const std::string word = lowered(value.substr(0, std::min<std::size_t>(value.size(), 8)));
    if (word.starts_with("http://") || word.starts_with("https://") || word.starts_with("www.")) {
        return true;
    }
    std::size_t numerals = 0;
    for (const char c : value) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isdigit(u) != 0) {
            ++numerals;
        } else if (c != ' ' && c != '+' && c != '-' && c != '(' && c != ')' && c != '.' &&
                   c != 'x' && c != 'X') {
            return false;
        }
    }
    // A phone number has punctuation or a plus; plain digits are a number.
    return numerals >= 7 && numerals < value.size();
}

// What a person reads for a kind.
std::string title_of(std::string_view kind, std::size_t distinct) {
    if (kind == "numeric") {
        return "number";
    }
    if (kind == "categorical") {
        return "category, " + plural(distinct, "value", "values");
    }
    if (kind == "boolean") {
        return "yes or no";
    }
    if (kind == "free_text") {
        return "free text";
    }
    if (kind == "datetime") {
        return "date or time";
    }
    if (kind == "identifier") {
        return "identifier";
    }
    if (kind == "empty") {
        return "empty";
    }
    if (kind == "constant") {
        return "the same value in every row";
    }
    return "not readable";
}

// The engine leaves these out of the default methodology's inputs itself, and the run's feature
// report says so, so they do not need a training copy.
bool engine_leaves_out(const json& column) {
    const std::string kind = text_of(column, "kind");
    // An identifier only Lake recognised (a row number the engine reads as a measure, lake#1242)
    // is still an input to the engine, so the dataset cannot train as it is.
    if (kind == "identifier" && column.contains("engine_kind") &&
        text_of(column, "engine_kind") != "identifier") {
        return false;
    }
    return kind == "identifier" || kind == "free_text" ||
           (kind == "categorical" && count_of(column, "distinct_count") > max_category_values);
}

bool numeric_type(std::string_view type) {
    const std::string word = lowered(type);
    return word == "integer" || word == "int" || word == "int64" || word == "number" ||
           word == "float" || word == "double" || word == "float64" || word == "numeric" ||
           word == "bool" || word == "boolean";
}

json describe(const json& analyzed, const std::string& type) {
    const std::string name = text_of(analyzed, "name");
    const std::string semantic = text_of(analyzed, "semantic_type");
    const std::size_t distinct = count_of(analyzed, "distinct_count");
    const double null_rate = analyzed.value("null_rate", 0.0);
    json values = json::array();
    json counts = json::array();
    if (analyzed.contains("top_values") && analyzed["top_values"].is_array()) {
        for (const json& top : analyzed["top_values"]) {
            if (top.is_object() && top.contains("value") && !empty_value(top["value"])) {
                values.push_back(value_text(top["value"]));
                counts.push_back(top.value("count", 0));
            }
        }
    }
    std::string kind;
    if (distinct == 0 || null_rate >= 1.0) {
        kind = "empty";
    } else if (distinct == 1) {
        kind = "constant";
    } else if (semantic == "numeric" || semantic == "categorical" || semantic == "boolean" ||
               semantic == "datetime" || semantic == "free_text" || semantic == "identifier") {
        kind = semantic;
    } else {
        kind = "unsupported";
    }
    // lake#1242: the engine calls a number a key only when its name ends in `_id` or `_key`
    // (zetta#1216), so a file's row number (`Index`) or a `Customer Id` written as integers reads
    // as a measure. A column whose name says it numbers or names the row is an identifier.
    if ((kind == "numeric" || kind == "categorical") && row_naming_name(name)) {
        kind = "identifier";
    }
    const bool two = distinct == 2 && kind != "datetime" && kind != "free_text" &&
                     kind != "identifier" && kind != "unsupported";
    if (two && kind != "boolean") {
        // Two values are a yes-or-no answer whatever their spelling (free/pro, 0/1).
        kind = "boolean";
    }

    json feature{{"use", true}};
    if (kind == "numeric") {
        feature["reason"] = "a number";
    } else if (kind == "boolean") {
        feature["reason"] = "two values";
    } else if (kind == "categorical" && distinct <= max_category_values) {
        feature["reason"] = "a category of " + std::to_string(distinct) + " values";
    } else {
        feature["use"] = false;
        if (kind == "categorical") {
            feature["reason"] = "a category of " + std::to_string(distinct) +
                                " values: more than the model learns from (" +
                                std::to_string(max_category_values) + ")";
        } else if (kind == "datetime") {
            feature["reason"] = "a date or time: the first model learns from numbers and "
                                "categories, so Lake leaves dates out of it";
        } else if (kind == "free_text") {
            feature["reason"] = "free text: the first model reads numbers and categories, and "
                                "text needs a text methodology";
        } else if (kind == "identifier") {
            feature["reason"] = "an identifier: it names a row and says nothing a model can learn";
        } else if (kind == "empty") {
            feature["reason"] = "every value is empty";
        } else if (kind == "constant") {
            feature["reason"] = "every row has the same value, so it tells rows apart by nothing";
        } else {
            feature["reason"] = "nested values (objects or lists) the model cannot read";
        }
    }

    json predict{{"can", false}};
    if (kind == "boolean") {
        predict["can"] = true;
        predict["values"] = values;
    } else if (kind == "categorical" && distinct <= max_target_values) {
        predict["can"] = true;
        predict["values"] = values;
        predict["choose_value"] = true;
    } else if (kind == "categorical") {
        predict["reason"] = std::to_string(distinct) + " values: too many to look for one";
    } else if (kind == "numeric") {
        predict["reason"] = "a number with many values: the model answers a yes-or-no question, "
                            "so it predicts a column with two values or one value of a category";
    } else if (kind == "datetime") {
        predict["reason"] = "a date or time is not a yes-or-no answer";
    } else if (kind == "free_text") {
        predict["reason"] = "free text is not a yes-or-no answer";
    } else if (kind == "identifier") {
        predict["reason"] = "an identifier: every row has its own value";
    } else if (kind == "empty") {
        predict["reason"] = "every value is empty";
    } else if (kind == "constant") {
        predict["reason"] = "every row has the same value";
    } else {
        predict["reason"] = "nested values (objects or lists)";
    }

    json column{{"name", name},
                {"type", type},
                {"kind", kind},
                {"engine_kind", semantic},
                {"title", title_of(kind, distinct)},
                {"distinct_count", distinct},
                {"null_rate", null_rate},
                {"values", values},
                {"counts", counts},
                {"identifier", kind == "identifier"},
                {"feature", std::move(feature)},
                {"predict", std::move(predict)}};
    if (kind == "boolean") {
        column["predict"]["positive"] = positive_value(column);
    }
    return column;
}

bool outcome_name(std::string_view name) {
    const std::string word = lowered(name);
    for (const char* hint :
         {"churn",    "label",   "target",     "outcome", "convert", "won",
          "lost",     "cancel",  "default",    "fraud",   "popular", "active",
          "retain",   "purchas", "bought",     "click",   "success", "survived",
          "approved", "respond", "subscribed", "renew",   "left",    "attrition"}) {
        if (word.find(hint) != std::string::npos) {
            return true;
        }
    }
    return word.starts_with("is_") || word.starts_with("has_") || word.starts_with("is ") ||
           word.starts_with("has ");
}

bool predictable(const json& column) {
    return column.is_object() && column.contains("predict") &&
           column["predict"].value("can", false);
}

} // namespace

std::string value_text(const json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_null()) {
        return {};
    }
    if (value.is_number_float()) {
        const double number = value.get<double>();
        // 1.0 and 1 are the same value.
        if (number == static_cast<double>(static_cast<long long>(number)) && number > -9.0e15 &&
            number < 9.0e15) {
            return std::to_string(static_cast<long long>(number));
        }
    }
    return value.dump();
}

json detect_columns(const json& analysis, const json& stored) {
    std::map<std::string, json> analyzed;
    std::vector<std::string> order;
    if (analysis.is_object() && analysis.contains("columns") && analysis["columns"].is_array()) {
        for (const json& column : analysis["columns"]) {
            const std::string name = text_of(column, "name");
            if (!name.empty() && !analyzed.contains(name)) {
                analyzed.emplace(name, column);
                order.push_back(name);
            }
        }
    }
    std::map<std::string, std::string> types;
    std::vector<std::string> names;
    if (stored.is_array()) {
        for (const json& column : stored) {
            const std::string name = text_of(column, "name");
            if (!name.empty() && !types.contains(name)) {
                types.emplace(name, text_of(column, "type"));
                names.push_back(name);
            }
        }
    }
    for (const std::string& name : order) {
        if (!types.contains(name)) {
            names.push_back(name);
            types.emplace(name, std::string());
        }
    }
    json out = json::array();
    for (const std::string& name : names) {
        const auto found = analyzed.find(name);
        if (found == analyzed.end()) {
            out.push_back({{"name", name}, {"type", types[name]}, {"kind", nullptr}});
            continue;
        }
        out.push_back(describe(found->second, types[name]));
    }
    return out;
}

bool detected(const json& columns) {
    return columns.is_array() && !columns.empty() &&
           std::ranges::all_of(columns, [](const json& column) {
               return column.is_object() && column.contains("kind") && column["kind"].is_string();
           });
}

const json* find(const json& columns, std::string_view name) {
    if (!columns.is_array() || name.empty()) {
        return nullptr;
    }
    for (const json& column : columns) {
        if (text_of(column, "name") == name) {
            return &column;
        }
    }
    return nullptr;
}

std::string positive_value(const json& column) {
    const json values = column.value("values", json::array());
    const json counts = column.value("counts", json::array());
    if (!values.is_array() || values.empty()) {
        return {};
    }
    for (const json& value : values) {
        if (value.is_string() && yes_like(value.get<std::string>())) {
            return value;
        }
    }
    if (values.size() == 1) {
        return values[0];
    }
    const auto count = [&](std::size_t at) {
        return counts.is_array() && at < counts.size() && counts[at].is_number()
                   ? counts[at].get<double>()
                   : 0.0;
    };
    if (count(0) != count(1)) {
        return count(0) < count(1) ? values[0] : values[1];
    }
    const std::string first = values[0];
    const std::string second = values[1];
    return std::max(first, second);
}

json suggest(const json& columns) {
    json out{{"target", nullptr}, {"target_value", nullptr}, {"entity_column", nullptr}};
    if (!columns.is_array()) {
        return out;
    }
    const json* named = nullptr;
    const json* last_two = nullptr;
    const json* category = nullptr;
    for (const json& column : columns) {
        if (!predictable(column)) {
            continue;
        }
        if (text_of(column, "kind") == "boolean") {
            if (named == nullptr && outcome_name(text_of(column, "name"))) {
                named = &column;
            }
            last_two = &column;
        } else if (category == nullptr ||
                   count_of(column, "distinct_count") <= count_of(*category, "distinct_count")) {
            // The category with the fewest values; the later one on a tie.
            category = &column;
        }
    }
    const json* target = named != nullptr ? named : last_two != nullptr ? last_two : category;
    if (target != nullptr) {
        out["target"] = text_of(*target, "name");
        if (target == category) {
            const json values = target->at("predict").value("values", json::array());
            if (!values.empty()) {
                out["target_value"] = values[0];
            }
        }
    }
    for (const json& column : columns) {
        if (text_of(column, "kind") == "identifier" &&
            (target == nullptr || text_of(column, "name") != text_of(*target, "name"))) {
            out["entity_column"] = text_of(column, "name");
            break;
        }
    }
    return out;
}

Problem check_choice(const json& columns, const Choice& choice) {
    if (choice.target.empty()) {
        std::string listed;
        for (const json& column : columns) {
            if (predictable(column)) {
                listed += (listed.empty() ? "" : ", ") + text_of(column, "name");
            }
        }
        if (listed.empty()) {
            return {"None of this dataset's columns can be predicted yet: the model answers a "
                    "yes-or-no question, so it needs a column with two values (such as yes and "
                    "no) or a category of up to " +
                        std::to_string(max_target_values) +
                        " values. Add such a column to the file and start again.",
                    "/target"};
        }
        return {"Choose the column to predict: " + listed + ".", "/target"};
    }
    const json* target = find(columns, choice.target);
    if (target == nullptr) {
        return {"This dataset has no column named " + choice.target + ".", "/target"};
    }
    if (!predictable(*target)) {
        const std::string reason = target->at("predict").value("reason", std::string());
        return {choice.target + " cannot be predicted" + (reason.empty() ? "" : ": " + reason) +
                    ". Choose another column to predict.",
                "/target"};
    }
    if (!choice.target_value.empty() && text_of(*target, "kind") == "boolean") {
        const json values = target->value("values", json::array());
        if (std::ranges::find(values, json(choice.target_value)) == values.end()) {
            return {choice.target + " has no value " + choice.target_value + ".", "/target_value"};
        }
    }
    if (!choice.entity_column.empty()) {
        if (choice.entity_column == choice.target) {
            return {"The column that names a row cannot be the column to predict.",
                    "/entity_column"};
        }
        if (find(columns, choice.entity_column) == nullptr) {
            return {"This dataset has no column named " + choice.entity_column + ".",
                    "/entity_column"};
        }
    }
    return {};
}

std::string row_number_column(const json& columns) {
    std::string name = "row_number";
    for (int n = 2; find(columns, name) != nullptr; ++n) {
        name = "row_number_" + std::to_string(n);
    }
    return name;
}

bool trains_as_is(const json& columns, const Choice& choice, const Fixes& fixes) {
    if (!fixes.drop.empty() || fixes.dates_from_values || fixes.number_rows ||
        choice.entity_column.empty()) {
        return false;
    }
    const json* target = find(columns, choice.target);
    if (target == nullptr || !numeric_type(text_of(*target, "type"))) {
        return false;
    }
    json values = target->value("values", json::array());
    std::sort(values.begin(), values.end());
    if (values != json::array({"0", "1"})) {
        return false;
    }
    if (!choice.target_value.empty() && choice.target_value != "1") {
        return false;
    }
    for (const json& column : columns) {
        const std::string name = text_of(column, "name");
        if (name == choice.target || name == choice.entity_column) {
            continue;
        }
        if (!column.contains("feature") || !column["feature"].value("use", false)) {
            if (!engine_leaves_out(column)) {
                return false;
            }
        }
    }
    return true;
}

std::set<std::string> date_columns_in(const std::vector<json>& rows) {
    std::map<std::string, std::pair<std::size_t, std::size_t>> seen; // non-empty, dates
    for (const json& row : rows) {
        if (!row.is_object()) {
            continue;
        }
        for (const auto& [name, value] : row.items()) {
            if (empty_value(value)) {
                continue;
            }
            auto& [values, dates] = seen[name];
            ++values;
            if (value.is_string() && iso_instant(value.get<std::string>())) {
                ++dates;
            }
        }
    }
    std::set<std::string> out;
    for (const auto& [name, tally] : seen) {
        if (tally.first > 0 && static_cast<double>(tally.second) >= 0.95 * tally.first) {
            out.insert(name);
        }
    }
    return out;
}

Prepare prepare_rows(const json& columns,
                     const std::vector<json>& rows,
                     const Choice& choice,
                     const Fixes& fixes) {
    Prepare out;
    const json* target = find(columns, choice.target);
    if (target == nullptr) {
        out.problem = "This dataset has no column named " + choice.target + ".";
        return out;
    }
    const std::string positive =
        !choice.target_value.empty() ? choice.target_value : positive_value(*target);
    if (positive.empty()) {
        out.problem = "Choose the value of " + choice.target + " the model looks for.";
        return out;
    }
    Prepared prepared;
    prepared.copy = true;
    bool numbered = choice.entity_column.empty() || fixes.number_rows;
    if (!numbered) {
        // The engine keys a model's rows by the column that names a row, so a name that repeats
        // (a first name) merges rows and their answers; the rows are numbered instead, and the
        // column stays an input like any other.
        std::set<std::string> names;
        std::size_t named = 0;
        for (const json& row : rows) {
            const auto value = row.is_object() ? row.find(choice.entity_column) : row.end();
            if (row.is_object() && value != row.end() && !empty_value(*value)) {
                ++named;
                names.insert(value_text(*value));
            }
        }
        if (names.size() != named || named != rows.size()) {
            numbered = true;
            prepared.note = choice.entity_column + " repeats (" +
                            plural(names.size(), "value", "different values") + " in " +
                            plural(rows.size(), "row", "rows") +
                            "), so Lake numbered the rows to tell them apart and kept " +
                            choice.entity_column + " as something the model learns from.";
        }
    }
    prepared.entity = numbered ? row_number_column(columns) : choice.entity_column;

    std::set<std::string> dates;
    if (fixes.dates_from_values) {
        dates = date_columns_in(rows);
    }
    std::vector<std::string> inputs;
    for (const json& column : columns) {
        const std::string name = text_of(column, "name");
        if (name == choice.target || (!numbered && name == choice.entity_column)) {
            continue;
        }
        std::string reason;
        if (fixes.drop.contains(name)) {
            reason = "the engine asked for it to be left out";
        } else if (dates.contains(name)) {
            reason = "a date or time: the first model learns from numbers and categories, so "
                     "Lake leaves dates out of it";
        } else if (!column.contains("feature") || !column["feature"].value("use", false)) {
            reason = column.contains("feature")
                         ? column["feature"].value("reason", std::string("not usable"))
                         : std::string("its kind is not known");
        }
        if (!reason.empty()) {
            prepared.dropped.push_back({{"name", name}, {"reason", reason}});
            continue;
        }
        inputs.push_back(name);
        prepared.used.push_back(name);
    }
    prepared.header.push_back(prepared.entity);
    prepared.header.insert(prepared.header.end(), inputs.begin(), inputs.end());
    prepared.header.push_back(choice.target);

    std::size_t yes = 0;
    std::size_t no = 0;
    std::size_t number = 0;
    std::string content;
    for (const json& row : rows) {
        ++number;
        if (!row.is_object()) {
            ++prepared.skipped;
            continue;
        }
        const auto answer = row.find(choice.target);
        if (answer == row.end() || empty_value(*answer)) {
            ++prepared.skipped;
            continue;
        }
        json line = json::object();
        if (numbered) {
            line[prepared.entity] = number;
        } else {
            const auto name = row.find(prepared.entity);
            line[prepared.entity] = name == row.end() ? json(nullptr) : *name;
        }
        for (const std::string& input : inputs) {
            const auto value = row.find(input);
            line[input] = value == row.end() ? json(nullptr) : *value;
        }
        const bool matched = value_text(*answer) == positive;
        line[choice.target] = matched ? 1 : 0;
        (matched ? yes : no) += 1;
        content += line.dump();
        content += '\n';
        ++prepared.rows;
    }
    if (prepared.rows == 0) {
        out.problem = "No row has a value in " + choice.target +
                      ", so there is nothing to learn. Choose another column to predict.";
        return out;
    }
    if (yes == 0) {
        out.problem = "No row has " + choice.target + " = " + positive +
                      ". Choose a value that appears in the data.";
        return out;
    }
    if (no == 0) {
        out.problem = "Every row has " + choice.target + " = " + positive +
                      ", so there is nothing to tell apart. Choose another column or value to "
                      "predict.";
        return out;
    }
    prepared.content = std::move(content);
    prepared.label = {
        {"column", choice.target}, {"positive", positive}, {"rows_yes", yes}, {"rows_no", no}};
    out.prepared = std::move(prepared);
    return out;
}

namespace {

std::string csv_field(const std::string& text) {
    if (text.find_first_of(",\"\r\n") == std::string::npos) {
        return text;
    }
    std::string out = "\"";
    for (const char c : text) {
        out += c == '"' ? std::string("\"\"") : std::string(1, c);
    }
    return out + "\"";
}

} // namespace

std::string csv_content(const Prepared& prepared) {
    std::string out;
    for (std::size_t at = 0; at < prepared.header.size(); ++at) {
        out += (at == 0 ? "" : ",") + csv_field(prepared.header[at]);
    }
    out += '\n';
    std::size_t start = 0;
    while (start < prepared.content.size()) {
        std::size_t end = prepared.content.find('\n', start);
        if (end == std::string::npos) {
            end = prepared.content.size();
        }
        const json row = json::parse(prepared.content.substr(start, end - start), nullptr, false);
        start = end + 1;
        if (!row.is_object()) {
            continue;
        }
        for (std::size_t at = 0; at < prepared.header.size(); ++at) {
            const auto value = row.find(prepared.header[at]);
            out += (at == 0 ? "" : ",") +
                   csv_field(value == row.end() ? std::string() : value_text(*value));
        }
        out += '\n';
    }
    return out;
}

std::optional<Refusal> refusal(std::string_view message) {
    constexpr std::string_view marker = "transform_required: ";
    const auto at = message.find(marker);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view rest = message.substr(at + marker.size());
    const auto colon = rest.find(": ");
    Refusal out;
    if (colon == std::string_view::npos) {
        out.name = std::string(rest);
    } else {
        out.name = std::string(rest.substr(0, colon));
        out.description = std::string(rest.substr(colon + 2));
    }
    while (!out.name.empty() && std::isspace(static_cast<unsigned char>(out.name.back())) != 0) {
        out.name.pop_back();
    }
    if (out.name.empty()) {
        return std::nullopt;
    }
    return out;
}

std::optional<Fixes> fix_for(const Refusal& refused, const json& columns, const Choice& choice) {
    Fixes fixes;
    if (refused.name == "add_snapshot_date") {
        // Leave out every column the engine could read as a date: the detected ones, and any whose
        // values are dates.
        fixes.dates_from_values = true;
        for (const json& column : columns) {
            if (text_of(column, "kind") == "datetime" && text_of(column, "name") != choice.target) {
                fixes.drop.insert(text_of(column, "name"));
            }
        }
        return fixes;
    }
    if (refused.name == "add_entity_key" || refused.name == "capture_label") {
        // The engine could not find a row's name, or the column to predict as 0/1: the copy
        // numbers the rows and writes the answer as 0/1.
        fixes.number_rows = true;
        return fixes;
    }
    return std::nullopt;
}

std::string explain_refusal(const Refusal& refused, const json& columns, const Choice& choice) {
    const std::string& name = refused.name;
    if (name == "create_eligible_features") {
        std::string left;
        for (const json& column : columns) {
            const std::string column_name = text_of(column, "name");
            if (column_name == choice.target || column_name == choice.entity_column) {
                continue;
            }
            if (column.contains("feature") && !column["feature"].value("use", false)) {
                left += (left.empty() ? "" : "; ") + column_name + " (" +
                        column["feature"].value("reason", std::string()) + ")";
            }
        }
        return "No other column can teach the model about " + choice.target +
               (left.empty() ? std::string() : ": " + left) +
               ". Choose another column to predict, or add columns with numbers or categories to "
               "the file.";
    }
    if (name == "add_snapshot_date") {
        return "The engine still reads a date in this dataset after Lake left its date columns "
               "out. Remove the date column from the file, or choose another snapshot date, and "
               "train again.";
    }
    if (name == "capture_label") {
        return "The model needs a column to predict with two values, or a value of a category to "
               "look for. Choose the column to predict" +
               std::string(choice.target.empty() ? "" : " again (" + choice.target + ")") + ".";
    }
    if (name == "add_entity_key") {
        return "The engine could not tell the rows apart. Choose a column that names each row.";
    }
    if (name.find("aggregat") != std::string::npos || name.find("event") != std::string::npos) {
        return "This dataset has several rows for the same " +
               (choice.entity_column.empty() ? std::string("thing") : choice.entity_column) +
               " over time, and the model learns from one row each. Choose another column that "
               "names a row, or upload one row per " +
               (choice.entity_column.empty() ? std::string("thing") : choice.entity_column) + ".";
    }
    if (name.find("pivot") != std::string::npos) {
        return "This dataset keeps one measurement per row; the model learns from one row per "
               "thing with a column per measurement. Upload it with the measurements as columns.";
    }
    if (name.find("join") != std::string::npos) {
        return "The answer the model would learn is in another dataset. Choose a column of this "
               "dataset to predict, or add the answer to this file.";
    }
    return "The engine needs this dataset reshaped before it can train" +
           (refused.description.empty() ? std::string(".") : ": " + refused.description) +
           " Choose other columns, or change the file and start again.";
}

std::string explain_run_failure(std::string_view reason,
                                const Choice& choice,
                                std::string_view methodology) {
    const std::string word = lowered(reason);
    const std::string target = choice.target.empty() ? std::string("the column") : choice.target;
    if (label_free(methodology) || word.find("segmentation") != std::string::npos) {
        // Clustering (M-3): the engine's segmentation reasons, said for a person (lake#1203).
        if (word.find("no eligible feature") != std::string::npos ||
            word.find("no feature column") != std::string::npos) {
            return "Clustering groups rows by their numbers and short categories, and this "
                   "dataset has none to group by. Ask Orama to suggest another model.";
        }
        if (word.find("fewer rows") != std::string::npos ||
            word.find("no rows") != std::string::npos || word.find("empty") != std::string::npos) {
            return "Clustering needs at least " + std::to_string(clustering_minimum_rows) +
                   " rows, and the dataset has fewer. Add a file with more rows and start "
                   "again.";
        }
        if (word.find("entity") != std::string::npos) {
            return "Lake could not tell the rows apart by the column chosen to name a row. Try "
                   "again without one: Lake numbers the rows.";
        }
        return "Lake could not train the clustering model. Try again, or let Ask Orama fix "
               "it.";
    }
    if (word.find("single class") != std::string::npos ||
        word.find("one value") != std::string::npos) {
        return "Every row the model trained on has the same answer for " + target +
               ", so there was nothing to learn. Choose another column or value to predict.";
    }
    if (word.find("non-numeric") != std::string::npos && word.find("label") != std::string::npos) {
        return target + " could not be read as a yes-or-no answer. Choose the value the model "
                        "looks for, or another column to predict.";
    }
    if (word.find("no rows") != std::string::npos || word.find("empty") != std::string::npos) {
        return "The model had no rows to learn from. Check the file has rows with a value in " +
               target + ".";
    }
    // Anything else: never the engine's own words (lake#1204), which name its internals; they stay
    // in the error's `detail` for an agent.
    return "Lake could not train the model on these choices. Try again, or let Ask Orama fix "
           "it.";
}

std::string clustering_problem(const json& rows) {
    if (rows.is_number() && rows.get<double>() < clustering_minimum_rows) {
        return "Clustering needs at least " + std::to_string(clustering_minimum_rows) +
               " rows, and the dataset has fewer. Add a file with more rows and start again.";
    }
    return {};
}

std::optional<Outcome> confirm_outcome(const json& status) {
    if (!status.is_object() || !status.contains("steps") || !status["steps"].is_array()) {
        return std::nullopt;
    }
    const auto text_at = [](const json& value, const char* key) {
        return value.is_object() && value.contains(key) && value[key].is_string()
                   ? value[key].get<std::string>()
                   : std::string();
    };
    json model_step = json::object();
    for (const json& step : status["steps"]) {
        if (text_at(step, "step") == "model") {
            model_step = step;
        }
    }
    const bool clustering =
        label_free(text_at(status.value("training", json(nullptr)), "methodology"));
    const std::string dataset = text_at(status.value("dataset", json(nullptr)), "name");
    const std::string on = dataset.empty() ? std::string() : " on " + dataset;
    const std::string state = text_at(status, "state");
    if (text_at(model_step, "state") == "failed") {
        const std::string why = text_at(model_step.value("error", json(nullptr)), "message");
        return Outcome{false, why.empty() ? std::string("Lake could not train the model.") : why};
    }
    if (state == "training") {
        return Outcome{true,
                       std::string(clustering ? "Training started: a clustering model"
                                              : "Training started: the model") +
                           on +
                           ". The Quickstart page shows its progress; you can leave and come "
                           "back. " +
                           (clustering ? "When it finishes, Train → Models shows the segments."
                                       : "When it finishes, Lake publishes the model, and search "
                                         "shows its answer beside each result.")};
    }
    if (state == "done" || state == "trained") {
        return Outcome{true,
                       clustering ? "The clustering model is trained" + on +
                                        ". Train → Models shows the segments."
                                  : "The model is trained" + on +
                                        ". Search on the Quickstart page shows its answer beside "
                                        "each result."};
    }
    return std::nullopt;
}

std::string snapshot_date(std::string_view typed) {
    return iso_instant(typed) ? std::string(typed) : std::string();
}

// ---- a derived target (lake#1182) ----

namespace {

// A value as a number: a JSON number, or text that is one ("12.5"). nullopt otherwise.
std::optional<double> number_of(const json& value) {
    if (value.is_number()) {
        return value.get<double>();
    }
    if (!value.is_string()) {
        return std::nullopt;
    }
    const std::string& text = value.get_ref<const std::string&>();
    std::size_t from = 0;
    while (from < text.size() && std::isspace(static_cast<unsigned char>(text[from])) != 0) {
        ++from;
    }
    std::size_t to = text.size();
    while (to > from && std::isspace(static_cast<unsigned char>(text[to - 1])) != 0) {
        --to;
    }
    if (from == to) {
        return std::nullopt;
    }
    // strtod, not from_chars: the floating-point from_chars needs macOS 26.
    const std::string part = text.substr(from, to - from);
    char* end = nullptr;
    const double out = std::strtod(part.c_str(), &end);
    if (end != part.c_str() + part.size() || !std::isfinite(out)) {
        return std::nullopt;
    }
    return out;
}

// A number as a person writes it: 50, not 50.000000.
std::string number_text(double number) {
    return value_text(json(number));
}

bool numeric_rule(const Derived& derived) {
    return derived.above || derived.at_least || derived.below || derived.at_most;
}

// Whether `value` satisfies the rule; nullopt when the row has no answer.
std::optional<bool> holds(const Derived& derived, const json& value) {
    if (empty_value(value)) {
        return std::nullopt;
    }
    if (!numeric_rule(derived)) {
        return std::ranges::find(derived.values, value_text(value)) != derived.values.end();
    }
    const auto number = number_of(value);
    if (!number) {
        return std::nullopt;
    }
    return (!derived.above || *number > *derived.above) &&
           (!derived.at_least || *number >= *derived.at_least) &&
           (!derived.below || *number < *derived.below) &&
           (!derived.at_most || *number <= *derived.at_most);
}

} // namespace

DerivedParse parse_derived(const json& input, const json& columns) {
    DerivedParse out;
    if (!input.is_object()) {
        out.problem = {"derived_target is an object: {column, above, at_least, below, at_most, "
                       "values, name}.",
                       "/derived_target"};
        return out;
    }
    Derived derived;
    derived.column = text_of(input, "column");
    derived.name = text_of(input, "name");
    const json* source = find(columns, derived.column);
    if (derived.column.empty() || source == nullptr) {
        out.problem = {derived.column.empty()
                           ? "derived_target needs the column the target is derived from."
                           : "This dataset has no column named " + derived.column + ".",
                       "/derived_target/column"};
        return out;
    }
    for (const auto& [key, slot] : std::to_array<std::pair<const char*, std::optional<double>*>>(
             {{"above", &derived.above},
              {"at_least", &derived.at_least},
              {"below", &derived.below},
              {"at_most", &derived.at_most}})) {
        if (!input.contains(key) || input.at(key).is_null()) {
            continue;
        }
        const auto number = number_of(input.at(key));
        if (!number) {
            out.problem = {std::string(key) + " is a number.",
                           "/derived_target/" + std::string(key)};
            return out;
        }
        *slot = number;
    }
    if (input.contains("values") && input.at("values").is_array()) {
        for (const json& value : input.at("values")) {
            if (!empty_value(value)) {
                derived.values.push_back(value_text(value));
            }
        }
    }
    const bool numeric = numeric_rule(derived);
    if (numeric == !derived.values.empty()) {
        out.problem = {numeric ? "derived_target takes a numeric rule (above, at_least, below, "
                                 "at_most) or values, not both."
                               : "derived_target needs a rule: above, at_least, below or at_most "
                                 "a number, or the values that count as yes.",
                       "/derived_target"};
        return out;
    }
    if (numeric && text_of(*source, "kind") != "numeric") {
        out.problem = {derived.column + " is not a number (" + text_of(*source, "title") +
                           "), so a numeric rule cannot read it. Name the values that count as "
                           "yes instead.",
                       "/derived_target/column"};
        return out;
    }
    if ((derived.above && derived.at_least) || (derived.below && derived.at_most)) {
        out.problem = {"Use one lower bound (above or at_least) and one upper bound (below or "
                       "at_most).",
                       "/derived_target"};
        return out;
    }
    const auto low = derived.above ? derived.above : derived.at_least;
    const auto high = derived.below ? derived.below : derived.at_most;
    if (low && high && *low >= *high) {
        out.problem = {"The lower bound is not below the upper bound, so no row would count as "
                       "yes.",
                       "/derived_target"};
        return out;
    }
    if (!derived.name.empty()) {
        const bool plain = std::ranges::all_of(
            derived.name, [](unsigned char c) { return std::isalnum(c) != 0 || c == '_'; });
        if (!plain || derived.name.size() > 63 ||
            std::isdigit(static_cast<unsigned char>(derived.name[0])) != 0) {
            out.problem = {"name is letters, digits and underscores, not starting with a digit, "
                           "at most 63 characters.",
                           "/derived_target/name"};
            return out;
        }
        if (find(columns, derived.name) != nullptr) {
            out.problem = {"This dataset already has a column named " + derived.name +
                               ". Choose another name.",
                           "/derived_target/name"};
            return out;
        }
    } else {
        derived.name = derived_name(derived, columns);
    }
    out.derived = std::move(derived);
    return out;
}

std::string derived_rule(const Derived& derived) {
    std::string rule = derived.column;
    if (!numeric_rule(derived)) {
        rule += derived.values.size() == 1 ? " is " : " is one of ";
        for (std::size_t at = 0; at < derived.values.size(); ++at) {
            rule += (at == 0                           ? ""
                     : at + 1 == derived.values.size() ? " or "
                                                       : ", ") +
                    derived.values[at];
        }
        return rule;
    }
    std::string parts;
    const auto add = [&](const char* word, const std::optional<double>& bound) {
        if (bound) {
            parts +=
                (parts.empty() ? " " : " and ") + std::string(word) + " " + number_text(*bound);
        }
    };
    add("above", derived.above);
    add("at least", derived.at_least);
    add("below", derived.below);
    add("at most", derived.at_most);
    return rule + parts;
}

std::string derived_name(const Derived& derived, const json& columns) {
    std::string name;
    for (const char c : derived_rule(derived)) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) != 0) {
            name += static_cast<char>(std::tolower(u));
        } else if (c == '.' || c == '-') {
            name += c == '.' ? "_" : "minus_";
        } else if (!name.empty() && name.back() != '_') {
            name += '_';
        }
    }
    while (!name.empty() && name.back() == '_') {
        name.pop_back();
    }
    if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0])) != 0) {
        name = "target_" + name;
    }
    name = name.substr(0, 54);
    std::string unique = name;
    for (int n = 2; find(columns, unique) != nullptr; ++n) {
        unique = name + "_" + std::to_string(n);
    }
    return unique;
}

Derive derive_target(const json& columns, const std::vector<json>& rows, const Derived& derived) {
    Derive out;
    const std::string rule = derived_rule(derived);
    for (const json& column : columns) {
        json kept = column;
        if (text_of(column, "name") == derived.column) {
            kept["feature"] = {{"use", false},
                               {"reason",
                                "the column to predict is derived from it (" + rule +
                                    "), so it would give the answer away"}};
        }
        out.columns.push_back(std::move(kept));
    }
    std::size_t yes = 0;
    std::size_t no = 0;
    out.rows.reserve(rows.size());
    for (const json& row : rows) {
        json kept = row;
        if (row.is_object()) {
            const auto value = row.find(derived.column);
            const auto answer = value == row.end() ? std::optional<bool>() : holds(derived, *value);
            if (answer) {
                kept[derived.name] = *answer ? "yes" : "no";
                (*answer ? yes : no) += 1;
            }
        }
        out.rows.push_back(std::move(kept));
    }
    out.columns.push_back(
        {{"name", derived.name},
         {"type", "string"},
         {"kind", "boolean"},
         {"title", "yes or no, derived"},
         {"distinct_count", 2},
         {"null_rate",
          rows.empty() ? 0.0
                       : 1.0 - static_cast<double>(yes + no) / static_cast<double>(rows.size())},
         {"values", json::array({"yes", "no"})},
         {"counts", json::array({yes, no})},
         {"identifier", false},
         {"feature", {{"use", false}, {"reason", "the column to predict"}}},
         {"predict", {{"can", true}, {"values", json::array({"yes", "no"})}, {"positive", "yes"}}},
         {"derived", {{"from", derived.column}, {"rule", rule}}}});
    if (yes + no == 0) {
        out.problem = "No row of " + derived.column + " can be read for the rule " + rule +
                      (numeric_rule(derived) ? ": its values are empty or not numbers." : ".");
    } else if (yes == 0) {
        out.problem = "No row has " + rule +
                      ", so there is nothing to learn. Choose another "
                      "threshold or values.";
    } else if (no == 0) {
        out.problem = "Every row has " + rule +
                      ", so there is nothing to tell apart. Choose "
                      "another threshold or values.";
    }
    return out;
}

bool label_free(std::string_view methodology) {
    const std::string word = lowered(methodology);
    return word == "m-3" || word == "segmentation" || word == "clustering" || word == "cluster";
}

namespace {

// A date's day number, from the YYYY-MM-DD at the start of an ISO date or timestamp.
std::optional<std::chrono::sys_days> day_of(const json& value) {
    if (!value.is_string()) {
        return std::nullopt;
    }
    const std::string text = value.get<std::string>();
    if (!iso_instant(text)) {
        return std::nullopt;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    digits(text, 0, 4, year);
    digits(text, 5, 2, month);
    digits(text, 8, 2, day);
    return std::chrono::sys_days{std::chrono::year_month_day{
        std::chrono::year{year},
        std::chrono::month{static_cast<unsigned>(month)},
        std::chrono::day{static_cast<unsigned>(day)}}};
}

// An integer value: a JSON integer, or a float with nothing after the point.
std::optional<long long> integer_of(const json& value) {
    if (value.is_number_integer()) {
        return value.get<long long>();
    }
    if (value.is_number_float()) {
        const double number = value.get<double>();
        if (std::isfinite(number) && number == std::floor(number) && std::fabs(number) < 9.0e15) {
            return static_cast<long long>(number);
        }
    }
    return std::nullopt;
}

// Whether a column's values number the rows (lake#1242): in a table of at least
// `row_number_minimum_rows` rows every row has a whole number, no two are the same, and they
// count up or down in the file's order or fill a range with no gaps (a shuffled row number). A
// measure is rarely all of that over so many rows; zetta#1216 is why a small table is not judged
// by its values.
bool numbers_the_rows(const std::vector<json>& rows, const std::string& name) {
    if (rows.size() < row_number_minimum_rows) {
        return false;
    }
    std::vector<long long> numbers;
    numbers.reserve(rows.size());
    for (const json& row : rows) {
        const auto value = row.is_object() ? row.find(name) : row.end();
        if (!row.is_object() || value == row.end()) {
            return false;
        }
        const auto number = integer_of(*value);
        if (!number) {
            return false;
        }
        numbers.push_back(*number);
    }
    const bool rising = std::ranges::adjacent_find(numbers, std::greater_equal<>()) == numbers.end();
    const bool falling = std::ranges::adjacent_find(numbers, std::less_equal<>()) == numbers.end();
    if (rising || falling) {
        return true;
    }
    std::vector<long long> sorted = numbers;
    std::ranges::sort(sorted);
    if (std::ranges::adjacent_find(sorted) != sorted.end()) {
        return false;
    }
    return sorted.back() - sorted.front() + 1 == static_cast<long long>(sorted.size());
}

// Whether most of a column's text values are email addresses, web addresses or phone numbers.
bool holds_contact_details(const std::vector<json>& rows, const std::string& name) {
    std::size_t texts = 0;
    std::size_t contacts = 0;
    for (const json& row : rows) {
        const auto value = row.is_object() ? row.find(name) : row.end();
        if (!row.is_object() || value == row.end() || empty_value(*value) ||
            !value->is_string()) {
            continue;
        }
        ++texts;
        if (contact_value(value->get<std::string>())) {
            ++contacts;
        }
    }
    return texts > 0 && static_cast<double>(contacts) >= 0.8 * static_cast<double>(texts);
}

// A derived column's name: `wanted`, or `wanted_2`, `wanted_3` when a column has it.
std::string free_name(const json& columns, const std::vector<std::string>& taken, std::string wanted) {
    const auto used = [&](const std::string& name) {
        return find(columns, name) != nullptr || std::ranges::find(taken, name) != taken.end();
    };
    if (!used(wanted)) {
        return wanted;
    }
    for (int n = 2;; ++n) {
        const std::string next = wanted + "_" + std::to_string(n);
        if (!used(next)) {
            return next;
        }
    }
}

// A number derived from a date column for each row: its month, or its days before the column's
// latest date (how long ago it was, as of the newest row: a tenure for a subscription date).
struct DatePart {
    std::string source;
    std::string name;
    bool month = false; // false: days before the latest date
    std::chrono::sys_days latest{};
};

} // namespace

Prepare
prepare_unlabeled(const json& columns, const std::vector<json>& rows, const std::string& entity) {
    Prepare out;
    Prepared prepared;
    prepared.copy = true;
    bool numbered = entity.empty();
    if (!numbered) {
        std::set<std::string> names;
        for (const json& row : rows) {
            const auto value = row.is_object() ? row.find(entity) : row.end();
            if (!row.is_object() || value == row.end() || empty_value(*value)) {
                numbered = true;
                break;
            }
            names.insert(value_text(*value));
        }
        if (!numbered && names.size() != rows.size()) {
            numbered = true;
        }
        if (numbered) {
            prepared.note = entity + " repeats or is empty in some rows, so Lake numbered the "
                                     "rows to tell them apart.";
        }
    }
    prepared.entity = numbered ? row_number_column(columns) : entity;
    std::vector<std::string> inputs;
    std::vector<DatePart> parts;
    std::vector<std::string> derived_names;
    const auto drop = [&](const std::string& name, const std::string& reason) {
        prepared.dropped.push_back({{"name", name}, {"reason", reason}});
    };
    // Columns whose values are dates though the profile read them as text (the engine's own rule).
    const std::set<std::string> dated = date_columns_in(rows);
    for (const json& column : columns) {
        const std::string name = text_of(column, "name");
        if (!numbered && name == entity) {
            continue;
        }
        const std::string kind = text_of(column, "kind");
        if (kind == "datetime" || (dated.contains(name) && kind != "empty" && kind != "constant")) {
            // lake#1242: a date says when, which a model groups by through its parts: the month,
            // and how long before the latest date it is.
            std::set<unsigned> months;
            std::set<std::chrono::sys_days> days;
            for (const json& row : rows) {
                const auto value = row.is_object() ? row.find(name) : row.end();
                if (row.is_object() && value != row.end()) {
                    if (const auto day = day_of(*value)) {
                        days.insert(*day);
                        months.insert(static_cast<unsigned>(
                            std::chrono::year_month_day{*day}.month()));
                    }
                }
            }
            std::vector<std::string> made;
            if (months.size() >= 2) {
                DatePart part{name, free_name(columns, derived_names, name + " month"), true, {}};
                derived_names.push_back(part.name);
                made.push_back(part.name);
                parts.push_back(std::move(part));
            }
            if (days.size() >= 2) {
                DatePart part{name,
                              free_name(columns, derived_names, name + " days before latest"),
                              false,
                              *days.rbegin()};
                derived_names.push_back(part.name);
                made.push_back(part.name);
                parts.push_back(std::move(part));
            }
            if (made.empty()) {
                drop(name, "a date or time with too few different days to group rows by");
            } else {
                std::string listed;
                for (const std::string& part : made) {
                    listed += (listed.empty() ? "" : " and ") + part;
                }
                drop(name, "a date: Lake groups rows by its parts instead (" + listed + ")");
            }
            continue;
        }
        if (!column.contains("feature") || !column["feature"].value("use", false)) {
            drop(name,
                 column.contains("feature")
                     ? column["feature"].value("reason", std::string("not usable"))
                     : std::string("its kind is not known"));
            continue;
        }
        if (kind == "numeric" && numbers_the_rows(rows, name)) {
            drop(name,
                 "a row number: every row has its own whole number, in order, so it tells rows "
                 "apart by where they sit in the file and says nothing about them");
            continue;
        }
        if (kind == "categorical" || kind == "boolean") {
            if (personal_name(name)) {
                drop(name,
                     "a name or contact details: it says who a row is, not something to group "
                     "rows by");
                continue;
            }
            if (holds_contact_details(rows, name)) {
                drop(name,
                     "email addresses, web addresses or phone numbers: they say who a row is, "
                     "not something to group rows by");
                continue;
            }
        }
        inputs.push_back(name);
        prepared.used.push_back(name);
    }
    for (const DatePart& part : parts) {
        prepared.used.push_back(part.name);
        prepared.derived.push_back(
            {{"name", part.name},
             {"from", part.source},
             {"part", part.month ? "month" : "days before latest"}});
    }
    if (inputs.empty() && parts.empty()) {
        std::string why;
        std::size_t listed = 0;
        for (const json& dropped : prepared.dropped) {
            if (listed == 4) {
                why += "; and " + std::to_string(prepared.dropped.size() - listed) + " more";
                break;
            }
            why += (why.empty() ? "" : "; ") + dropped.value("name", std::string()) + " is " +
                   dropped.value("reason", std::string());
            ++listed;
        }
        out.problem = "Lake found nothing in this dataset to group rows by, so it did not train a "
                      "clustering model. Clustering needs a number, a category of at most " +
                      std::to_string(max_category_values) + " values, or a date" +
                      (why.empty() ? std::string(".") : " (" + why + ").") +
                      " Add such a column, or choose a model that predicts a column.";
        return out;
    }
    prepared.header.push_back(prepared.entity);
    prepared.header.insert(prepared.header.end(), inputs.begin(), inputs.end());
    for (const DatePart& part : parts) {
        prepared.header.push_back(part.name);
    }
    std::size_t number = 0;
    for (const json& row : rows) {
        ++number;
        if (!row.is_object()) {
            ++prepared.skipped;
            continue;
        }
        json line = json::object();
        if (numbered) {
            line[prepared.entity] = number;
        } else {
            line[prepared.entity] = row.at(entity);
        }
        for (const std::string& input : inputs) {
            const auto value = row.find(input);
            line[input] = value == row.end() ? json(nullptr) : *value;
        }
        for (const DatePart& part : parts) {
            const auto value = row.find(part.source);
            const auto day = value == row.end() ? std::nullopt : day_of(*value);
            if (!day) {
                line[part.name] = nullptr;
            } else if (part.month) {
                line[part.name] =
                    static_cast<unsigned>(std::chrono::year_month_day{*day}.month());
            } else {
                line[part.name] = (part.latest - *day).count();
            }
        }
        prepared.content += line.dump();
        prepared.content += '\n';
        ++prepared.rows;
    }
    if (prepared.rows == 0) {
        out.problem = "The dataset has no rows to group.";
        return out;
    }
    // The dataset trains as it is only when a unique column names every row and every other
    // column is an input as it stands; anything left out or derived needs the training copy, or
    // the engine reads the columns Lake left out (lake#1242: the row number `Index`).
    prepared.copy = numbered || !prepared.dropped.empty() || !parts.empty();
    out.prepared = std::move(prepared);
    return out;
}

} // namespace lake::api::quickstart_columns
