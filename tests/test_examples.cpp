// Copy-paste examples run as given (lake#439). Every REST, curl and MCP example that lake shows a
// customer — the Ship pages' "call it from your code" blocks, the Docs section (/docs) and the API
// documents under docs/api — is pulled out of the rendered HTML and checked against the capability
// registry's input schema with the registry's own validator, so an example that omits a required
// field such as `workspace_id` fails here instead of on a customer's first paste.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/markdown.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/web_ui.hpp"

namespace {

using json = nlohmann::json;

struct Example {
    std::string capability;
    json input;
    std::string where;
};

const lake::api::Registry& registry() {
    static const lake::api::Registry* built = [] {
        auto* registry = new lake::api::Registry();
        lake::api::register_core_capabilities(*registry, "0.0.1");
        return registry;
    }();
    return *built;
}

bool is_capability(const std::string& name) {
    return !name.empty() && registry().find(name) != nullptr;
}

std::string unescape(std::string_view text) {
    static const std::vector<std::pair<std::string_view, std::string_view>> entities{
        {"&quot;", "\""}, {"&#39;", "'"}, {"&#x27;", "'"}, {"&lt;", "<"}, {"&gt;", ">"},
        {"&amp;", "&"}};
    std::string out;
    for (std::size_t at = 0; at < text.size();) {
        bool replaced = false;
        if (text[at] == '&') {
            for (const auto& [entity, character] : entities) {
                if (text.substr(at).starts_with(entity)) {
                    out += character;
                    at += entity.size();
                    replaced = true;
                    break;
                }
            }
        }
        if (!replaced) {
            out += text[at++];
        }
    }
    return out;
}

// The text of every <code> element, in document order, entities decoded.
std::vector<std::string> code_elements(std::string_view html) {
    std::vector<std::string> out;
    for (std::size_t at = html.find("<code"); at != std::string_view::npos;
         at = html.find("<code", at)) {
        const std::size_t open_end = html.find('>', at);
        const std::size_t close = html.find("</code>", open_end);
        if (open_end == std::string_view::npos || close == std::string_view::npos) {
            break;
        }
        std::string inner(html.substr(open_end + 1, close - open_end - 1));
        // Highlighting markup inside a code block is not part of its text.
        std::string text;
        bool tag = false;
        for (const char c : inner) {
            if (c == '<') {
                tag = true;
            } else if (c == '>') {
                tag = false;
            } else if (!tag) {
                text += c;
            }
        }
        out.push_back(unescape(text));
        at = close + 7;
    }
    return out;
}

std::string percent_decode(std::string_view text) {
    std::string out;
    for (std::size_t at = 0; at < text.size(); ++at) {
        if (text[at] == '%' && at + 2 < text.size() &&
            std::isxdigit(static_cast<unsigned char>(text[at + 1])) &&
            std::isxdigit(static_cast<unsigned char>(text[at + 2]))) {
            out += static_cast<char>(std::stoi(std::string(text.substr(at + 1, 2)), nullptr, 16));
            at += 2;
        } else if (text[at] == '+') {
            out += ' ';
        } else {
            out += text[at];
        }
    }
    return out;
}

// A GET example's query string as the input REST builds from it: integers, numbers and booleans
// the schema declares are read as such.
json query_input(std::string_view query, const lake::api::Capability& capability) {
    json input = json::object();
    const json properties = capability.input_schema.value("properties", json::object());
    std::size_t at = 0;
    while (at <= query.size()) {
        const std::size_t end = std::min(query.find('&', at), query.size());
        const std::string_view pair = query.substr(at, end - at);
        if (!pair.empty()) {
            const std::size_t equals = pair.find('=');
            const std::string key = percent_decode(pair.substr(0, equals));
            const std::string value =
                equals == std::string_view::npos ? std::string() : percent_decode(pair.substr(equals + 1));
            const std::string type =
                properties.contains(key) ? properties[key].value("type", std::string()) : "";
            if ((type == "integer" || type == "number") && !value.empty()) {
                input[key] = json::parse(value, nullptr, false);
            } else if (type == "boolean") {
                input[key] = value == "true";
            } else {
                input[key] = value;
            }
        }
        at = end + 1;
    }
    return input;
}

// Every balanced {...} in `text`, outermost only, honouring JSON strings.
std::vector<std::string> braced(std::string_view text) {
    std::vector<std::string> out;
    for (std::size_t start = text.find('{'); start != std::string_view::npos;) {
        int depth = 0;
        bool string = false;
        bool escaped = false;
        std::size_t at = start;
        for (; at < text.size(); ++at) {
            const char c = text[at];
            if (string) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    string = false;
                }
            } else if (c == '"') {
                string = true;
            } else if (c == '{') {
                ++depth;
            } else if (c == '}' && --depth == 0) {
                break;
            }
        }
        if (at >= text.size()) {
            break;
        }
        out.emplace_back(text.substr(start, at - start + 1));
        start = text.find('{', at + 1);
    }
    return out;
}

// The examples in one rendered page. A capability is named by `/v1/<name>` or by a code span that
// is exactly its name; the JSON objects that follow are its inputs. A JSON-RPC `tools/call`
// names its own tool. `strict`: every JSON object after a capability is an input (the Docs and
// the pages show no answers as JSON). Otherwise only objects that carry `customer_id` or
// `workspace_id` are inputs, so documented answers are not mistaken for requests.
std::vector<Example> examples_in(std::string_view html, const std::string& where, bool strict) {
    std::vector<Example> out;
    std::string current;
    for (const std::string& code : code_elements(html)) {
        // One shell block can hold several curl commands; each names its own capability.
        std::vector<std::string> units;
        std::istringstream lines(code);
        std::string line;
        std::string unit;
        while (std::getline(lines, line)) {
            if (line.starts_with("curl ") && !unit.empty()) {
                units.push_back(std::move(unit));
                unit.clear();
            }
            unit += line + "\n";
        }
        units.push_back(std::move(unit));
        for (const std::string& text : units) {
            std::string trimmed = text;
            while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
                trimmed.pop_back();
            }
            if (is_capability(trimmed)) {
                current = trimmed;
            }
            for (std::size_t at = text.find("/v1/"); at != std::string::npos;
                 at = text.find("/v1/", at + 4)) {
                std::size_t end = at + 4;
                while (end < text.size() &&
                       (std::islower(static_cast<unsigned char>(text[end])) || text[end] == '_')) {
                    ++end;
                }
                const std::string name = text.substr(at + 4, end - at - 4);
                if (!is_capability(name) || (end < text.size() && text[end] == '.')) {
                    continue;
                }
                current = name;
                if (end < text.size() && text[end] == '?') {
                    // A query ends at whitespace or a quote; a <placeholder> is one value.
                    std::size_t query_end = end + 1;
                    while (query_end < text.size() &&
                           !std::isspace(static_cast<unsigned char>(text[query_end])) &&
                           text[query_end] != '"' && text[query_end] != '\'' &&
                           text[query_end] != '`') {
                        const std::size_t closing = text.find('>', query_end);
                        query_end = text[query_end] == '<' && closing != std::string::npos
                                        ? closing + 1
                                        : query_end + 1;
                    }
                    out.push_back({name,
                                   query_input(std::string_view(text).substr(
                                                   end + 1, query_end - end - 1),
                                               *registry().find(name)),
                                   where + ": " + text.substr(at, query_end - at)});
                }
            }
            for (const std::string& object : braced(text)) {
                const json parsed = json::parse(object, nullptr, false);
                if (!parsed.is_object()) {
                    continue;
                }
                if (parsed.value("method", std::string()) == "tools/call") {
                    const json params = parsed.value("params", json::object());
                    out.push_back({params.value("name", std::string()),
                                   params.value("arguments", json::object()),
                                   where + ": " + object});
                    continue;
                }
                if (parsed.contains("jsonrpc") || current.empty()) {
                    continue;
                }
                if (strict || parsed.contains("customer_id") || parsed.contains("workspace_id")) {
                    out.push_back({current, parsed, where + ": " + object});
                }
            }
        }
    }
    return out;
}

void expect_valid(const std::vector<Example>& examples) {
    for (const Example& example : examples) {
        const auto capability = registry().find(example.capability);
        ASSERT_NE(capability, nullptr) << "no capability " << example.capability << " for "
                                       << example.where;
        const lake::api::ApiResult checked = lake::api::validate_input(*capability, example.input);
        EXPECT_TRUE(checked.ok) << example.capability << " refuses the example as given ("
                                << checked.error.message << " at " << checked.error.path
                                << "): " << example.where;
    }
}

std::set<std::string> capabilities_of(const std::vector<Example>& examples) {
    std::set<std::string> out;
    for (const Example& example : examples) {
        out.insert(example.capability);
    }
    return out;
}

} // namespace

TEST(Examples, EveryDocsExampleRunsAgainstTheRegistrySchema) {
    std::vector<Example> all;
    for (const lake::api::WebUiGuidePage& guide : lake::api::web_ui_guide()) {
        const auto examples = examples_in(
            lake::api::markdown_to_html(guide.markdown), "/docs/" + std::string(guide.slug), true);
        expect_valid(examples);
        all.insert(all.end(), examples.begin(), examples.end());
    }
    const std::set<std::string> covered = capabilities_of(all);
    for (const std::string expected : {"create_workspace", "workspace_status", "workspace_act",
                                       "workspace_sql", "run_pipeline", "agent_turn", "predict",
                                       "search_index"}) {
        EXPECT_TRUE(covered.contains(expected)) << "the Docs show a " << expected << " example";
    }
    for (const Example& example : all) {
        const auto capability = registry().find(example.capability);
        const json required = capability->input_schema.value("required", json::array());
        if (std::ranges::find(required, json("workspace_id")) != required.end()) {
            EXPECT_EQ(example.input.value("workspace_id", std::string()), "<workspace id>")
                << "static Docs show a clear placeholder: " << example.where;
        }
    }
}

TEST(Examples, EveryApiDocumentRequestRunsAgainstTheRegistrySchema) {
    const std::filesystem::path root = std::filesystem::path(LAKE_TEST_SOURCE_DIR) / "docs" / "api";
    std::size_t checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().extension() != ".md") {
            continue;
        }
        std::ifstream file(entry.path());
        std::stringstream source;
        source << file.rdbuf();
        const auto examples = examples_in(lake::api::markdown_to_html(source.str()),
                                          "docs/api/" + entry.path().filename().string(),
                                          false);
        expect_valid(examples);
        checked += examples.size();
    }
    EXPECT_GE(checked, 20U) << "the experiments and streams requests at least";
}
