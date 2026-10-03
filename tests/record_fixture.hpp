#pragma once

#include <cctype>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/control/workspace_records.hpp"
#include "lake/control/zetta_control.hpp"

namespace lake::test {

// The part of a workspace Zetta's `sql` capability that lake's workspace records use
// (docs/architecture/workspace-records.md): CREATE DATABASE, CREATE TABLE, INSERT with an explicit
// column list and SELECT * with one equality predicate, answering Zetta's envelope values and its
// "does not exist" / "already exists" messages. One instance is one workspace's storage; tests
// that route several workspaces keep one per workspace endpoint. It also records each request's
// encoded GET line length, so tests can hold writes under Zetta's start-line limit.
class RecordTables {
public:
    using json = nlohmann::json;
    std::map<std::string, std::map<std::string, std::vector<json>>> databases;
    std::size_t longest_statement = 0;

    static bool handles(std::string_view capability, const json& input) {
        return capability == "sql" && input.is_object() &&
               control::names_workspace_records(input.value("statement", std::string()));
    }

    control::ZettaControlResult<json> run(const json& input) {
        const std::string statement = input.value("statement", std::string());
        longest_statement = std::max(longest_statement, encoded_size(statement));
        tokens_ = tokenize(statement);
        position_ = 0;
        if (accept("CREATE")) {
            if (accept("DATABASE")) {
                const std::string name = next();
                if (databases.contains(name)) {
                    return fail("database \"" + name + "\" already exists or the name is invalid");
                }
                databases[name];
                return ok("CREATE DATABASE");
            }
            accept("TABLE");
            const auto [database, table] = qualified();
            if (!databases.contains(database)) {
                return fail("database \"" + database + "\" does not exist");
            }
            if (databases[database].contains(table)) {
                return fail("table \"" + table + "\" already exists or the name is invalid");
            }
            databases[database][table];
            return ok("CREATE TABLE");
        }
        if (accept("INSERT")) {
            accept("INTO");
            const auto [database, table] = qualified();
            if (!databases.contains(database)) {
                return fail("database \"" + database + "\" does not exist");
            }
            if (!databases[database].contains(table)) {
                return fail("table \"" + table + "\" does not exist");
            }
            std::vector<std::string> columns;
            accept("(");
            while (!accept(")")) {
                columns.push_back(next());
                accept(",");
            }
            accept("VALUES");
            accept("(");
            json row = json::object();
            for (const std::string& column : columns) {
                row[column] = value();
                accept(",");
            }
            accept(")");
            databases[database][table].push_back(std::move(row));
            return ok("INSERT 0 1");
        }
        if (accept("SELECT")) {
            accept("*");
            accept("FROM");
            const auto [database, table] = qualified();
            if (!databases.contains(database)) {
                return fail("database \"" + database + "\" does not exist");
            }
            if (!databases[database].contains(table)) {
                return fail("table \"" + table + "\" does not exist");
            }
            accept("WHERE");
            const std::string column = next();
            accept("=");
            const json wanted = value();
            json rows = json::array();
            for (const json& row : databases[database][table]) {
                if (row.contains(column) && row.at(column) == wanted) {
                    rows.push_back(row);
                }
            }
            return json{{"columns", json::array()},
                        {"messages", json::array({"SELECT " + std::to_string(rows.size())})},
                        {"rows", std::move(rows)}};
        }
        return fail("unsupported statement");
    }

    // Every row of a collection, for tests that inspect or tamper with what the workspace holds.
    std::vector<json>& rows(std::string_view collection) {
        return databases[std::string(control::workspace_records_database)][std::string(collection)];
    }

private:
    std::vector<std::string> tokens_;
    std::size_t position_ = 0;

    static std::size_t encoded_size(std::string_view text) {
        std::size_t size = std::string_view("GET /v1/sql?statement= HTTP/1.1").size();
        for (const unsigned char c : text) {
            size += std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~' ? 1 : 3;
        }
        return size;
    }

    static std::vector<std::string> tokenize(std::string_view text) {
        std::vector<std::string> tokens;
        std::size_t index = 0;
        while (index < text.size()) {
            const char c = text[index];
            if (std::isspace(static_cast<unsigned char>(c)) != 0) {
                ++index;
            } else if (c == '\'') {
                std::string literal = "'";
                ++index;
                while (index < text.size()) {
                    if (text[index] == '\'' && index + 1 < text.size() && text[index + 1] == '\'') {
                        literal += '\'';
                        index += 2;
                    } else if (text[index] == '\'') {
                        ++index;
                        break;
                    } else {
                        literal += text[index++];
                    }
                }
                tokens.push_back(std::move(literal));
            } else if (c == '(' || c == ')' || c == ',' || c == '=' || c == '*') {
                tokens.emplace_back(1, c);
                ++index;
            } else {
                std::string word;
                while (index < text.size() &&
                       std::isspace(static_cast<unsigned char>(text[index])) == 0 &&
                       std::string_view("(),=*'").find(text[index]) == std::string_view::npos) {
                    word += text[index++];
                }
                tokens.push_back(std::move(word));
            }
        }
        return tokens;
    }

    bool accept(std::string_view word) {
        if (position_ >= tokens_.size()) {
            return false;
        }
        const std::string& token = tokens_[position_];
        if (token.size() != word.size()) {
            return false;
        }
        for (std::size_t index = 0; index < word.size(); ++index) {
            if (std::toupper(static_cast<unsigned char>(token[index])) != word[index]) {
                return false;
            }
        }
        ++position_;
        return true;
    }

    std::string next() {
        return position_ < tokens_.size() ? tokens_[position_++] : std::string();
    }

    std::pair<std::string, std::string> qualified() {
        const std::string name = next();
        const auto dot = name.find('.');
        return {name.substr(0, dot), dot == std::string::npos ? std::string() : name.substr(dot + 1)};
    }

    json value() {
        const std::string token = next();
        if (token.starts_with('\'')) {
            return token.substr(1);
        }
        return json::parse(token, nullptr, false);
    }

    static control::ZettaControlResult<json> ok(std::string message) {
        return json{{"columns", json::array()},
                    {"messages", json::array({std::move(message)})},
                    {"rows", json::array()}};
    }

    static control::ZettaControlResult<json> fail(std::string message) {
        return std::unexpected(control::ZettaControlError{"engine_error", std::move(message)});
    }
};

} // namespace lake::test
