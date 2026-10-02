#include "lake/api/datasets.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>
#include <vector>

namespace lake::api::datasets {

namespace {

std::string lowered(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return out;
}

// Words the workspace's SQL subset reads as syntax, so a table named one cannot be queried.
constexpr std::array<std::string_view, 25> keywords = {
    "all",    "and",   "as",    "asc",  "by",    "desc",   "distinct", "from",  "group",
    "having", "in",    "is",    "join", "like",  "limit",  "not",      "null",  "offset",
    "on",     "or",    "order", "select", "union", "where", "with"};

struct Line {
    std::string_view text;
    std::size_t number = 0;
};

// Split one CSV line into fields as the workspace does; `closed` is false when a quoted field
// runs past the end of the line.
std::vector<std::string> fields_of(std::string_view line, bool& closed) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if (quoted) {
            if (character == '"') {
                if (index + 1 < line.size() && line[index + 1] == '"') {
                    field.push_back('"');
                    ++index;
                } else {
                    quoted = false;
                }
            } else {
                field.push_back(character);
            }
        } else if (character == '"') {
            quoted = true;
        } else if (character == ',') {
            fields.push_back(std::move(field));
            field.clear();
        } else {
            field.push_back(character);
        }
    }
    fields.push_back(std::move(field));
    closed = !quoted;
    return fields;
}

}  // namespace

bool engine_database(std::string_view database) {
    constexpr std::string_view suffix = "_results";
    return (database.size() > suffix.size() && database.ends_with(suffix)) ||
           database == "forecasts" || database == "feedback" || database == "system";
}

std::string kind_of(std::string_view database) {
    return engine_database(database) ? "result" : "data";
}

std::string name_problem(std::string_view name) {
    if (name.empty()) {
        return "a name is required";
    }
    if (name.size() > 63) {
        return "a name is at most 63 characters";
    }
    const auto letter = [](char character) {
        return std::isalpha(static_cast<unsigned char>(character)) != 0 || character == '_';
    };
    if (!letter(name.front())) {
        return "'" + std::string(name) + "' must start with a letter or an underscore";
    }
    for (const char character : name) {
        if (!letter(character) && std::isdigit(static_cast<unsigned char>(character)) == 0) {
            return "'" + std::string(name) + "' can hold only letters, digits and underscores " +
                   "(not '" + std::string(1, character) + "'), so SQL can query it";
        }
    }
    const std::string word = lowered(name);
    if (std::ranges::find(keywords, word) != keywords.end()) {
        return "'" + std::string(name) + "' is a SQL keyword; choose another name";
    }
    return {};
}

std::string csv_problem(std::string_view content) {
    std::vector<Line> lines;
    std::size_t number = 0;
    std::size_t start = 0;
    while (start <= content.size()) {
        std::size_t end = content.find('\n', start);
        if (end == std::string_view::npos) {
            end = content.size();
        }
        ++number;
        std::string_view text = content.substr(start, end - start);
        if (!text.empty() && text.back() == '\r') {
            text.remove_suffix(1);
        }
        lines.push_back({text, number});
        if (end == content.size()) {
            break;
        }
        start = end + 1;
    }
    std::size_t columns = 0;
    bool header_seen = false;
    for (const Line& line : lines) {
        if (line.text.empty()) {
            continue;
        }
        bool closed = true;
        const std::vector<std::string> fields = fields_of(line.text, closed);
        const std::string at = "line " + std::to_string(line.number) + ": ";
        if (!closed) {
            return at + "a quoted field is not closed (a row must stay on one line)";
        }
        if (!header_seen) {
            header_seen = true;
            std::set<std::string> names;
            for (std::size_t index = 0; index < fields.size(); ++index) {
                if (fields[index].empty()) {
                    return at + "header column " + std::to_string(index + 1) + " has no name";
                }
                if (!names.insert(fields[index]).second) {
                    return at + "the header names column '" + fields[index] + "' twice";
                }
            }
            columns = fields.size();
            continue;
        }
        if (fields.size() != columns) {
            return at + std::to_string(fields.size()) + " fields, but the header has " +
                   std::to_string(columns) + " columns";
        }
    }
    if (!header_seen) {
        return "the file has no header row";
    }
    return {};
}

std::string id_of(std::string_view database, std::string_view name) {
    return std::string(database.empty() ? std::string_view("datasets") : database) + "/" +
           std::string(name);
}

std::optional<DatasetRef> parse_id(std::string_view id) {
    std::size_t split = id.find('/');
    if (split == std::string_view::npos) {
        split = id.find('.');
    }
    DatasetRef ref;
    if (split == std::string_view::npos) {
        ref = {"datasets", std::string(id)};
    } else {
        ref = {std::string(id.substr(0, split)), std::string(id.substr(split + 1))};
    }
    if (ref.database.empty() || ref.name.empty()) {
        return std::nullopt;
    }
    return ref;
}

void annotate_list(nlohmann::json& listed) {
    if (!listed.is_object() || !listed.contains("datasets") || !listed["datasets"].is_array()) {
        return;
    }
    for (nlohmann::json& dataset : listed["datasets"]) {
        if (!dataset.is_object()) {
            continue;
        }
        if (!dataset.contains("name") && dataset.contains("dataset") &&
            dataset["dataset"].is_string()) {
            dataset["name"] = dataset["dataset"];
        }
        const std::string database =
            dataset.contains("database") && dataset["database"].is_string()
                ? dataset["database"].get<std::string>()
                : std::string("datasets");
        const std::string name = dataset.contains("name") && dataset["name"].is_string()
                                     ? dataset["name"].get<std::string>()
                                     : std::string();
        if (!name.empty() && !dataset.contains("id")) {
            dataset["id"] = id_of(database, name);
        }
        dataset["kind"] = kind_of(database);
    }
}

}  // namespace lake::api::datasets
