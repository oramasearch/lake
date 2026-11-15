// Agent names (lake#763): the Agent Library (include/lake/control/agent_library.hpp) is the one
// source of what each agent is called. These tests fail when a name breaks the naming scheme, when
// any other spelling of an agent's name appears anywhere in lake (code, the web app, the in-app
// guide, docs, tests, tools), when the docs' catalogue table differs from the catalogue, or when
// code pairs an agent's key with its name outside the catalogue.
#include "lake/control/agent_library.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using lake::control::agent_library;

const fs::path root(LAKE_TEST_SOURCE_DIR);

std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

bool word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// The files a person or an agent reads lake's agent names in. History is left as it was written:
// release notes and dated records say what things were called then. Zetta's own OpenAPI document
// describes Zetta's generators, not lake's agents. This file names the old spellings on purpose.
std::vector<fs::path> scanned_files() {
    static const std::set<std::string> extensions{".md",  ".cpp", ".hpp", ".h",  ".ts",
                                                  ".tsx", ".py",  ".mjs", ".js", ".sh"};
    std::vector<fs::path> files;
    for (const char* top : {"docs", "frontend/src", "frontend/tests", "src", "include", "tests",
                            "tools"}) {
        for (const auto& entry : fs::recursive_directory_iterator(root / top)) {
            if (!entry.is_regular_file() || !extensions.contains(entry.path().extension().string())) {
                continue;
            }
            const std::string relative = fs::relative(entry.path(), root).generic_string();
            const std::string name = entry.path().filename().string();
            if (relative.starts_with("docs/releases/") ||
                relative == "tests/test_agent_names.cpp" ||
                relative.find("node_modules/") != std::string::npos ||
                std::regex_search(name, std::regex(R"(\d{4}-\d{2}-\d{2})"))) {
                continue;
            }
            files.push_back(entry.path());
        }
    }
    for (const char* file : {"CLAUDE.md", "AGENTS.md", "README.md"}) {
        if (fs::exists(root / file)) {
            files.push_back(root / file);
        }
    }
    return files;
}

std::size_t line_of(const std::string& text, std::size_t at) {
    return 1 + static_cast<std::size_t>(std::count(text.begin(), text.begin() + at, '\n'));
}

// Whether `text` at `at` continues with "Agent", allowing a line break and the comment or string
// marks a wrapped line starts with.
bool continues_with(const std::string& text, std::size_t at, std::string_view word) {
    while (at < text.size() && std::string_view(" \t\r\n\"*/#").find(text[at]) != std::string_view::npos) {
        ++at;
    }
    return text.compare(at, word.size(), word) == 0;
}

} // namespace

TEST(AgentNames, EveryNameFollowsTheScheme) {
    std::set<std::string_view> names;
    for (const auto& agent : agent_library) {
        const std::string name(agent.name);
        EXPECT_TRUE(names.insert(agent.name).second) << "two agents are called " << name;
        // Title-case words, one space between them, nothing before or after.
        std::istringstream words(name);
        std::vector<std::string> parts;
        for (std::string word; words >> word;) {
            parts.push_back(word);
            EXPECT_TRUE(std::isupper(static_cast<unsigned char>(word[0])) != 0)
                << name << ": every word starts with a capital";
            const auto hyphen = word.find('-');
            if (hyphen != std::string::npos) {
                EXPECT_TRUE(hyphen + 1 < word.size() &&
                            std::isupper(static_cast<unsigned char>(word[hyphen + 1])) != 0)
                    << name << ": a hyphenated word is capitalized on both sides";
            }
        }
        EXPECT_GE(parts.size(), 2U) << name;
        std::string joined;
        for (const auto& part : parts) {
            joined += (joined.empty() ? "" : " ") + part;
        }
        EXPECT_EQ(joined, name) << "one space between words";
        if (agent.key == "agent-ask-orama") {
            EXPECT_EQ(name, "Ask Orama");
        } else {
            EXPECT_EQ(parts.back(), "Agent") << name << ": every agent but Ask Orama ends in Agent";
        }
    }
    // lake#763.
    EXPECT_EQ(lake::control::agent_display_name("agent-creator"), "Agent Creator Agent");
}

TEST(AgentNames, NoOtherSpellingAppearsAnywhere) {
    // The spellings a catalogue name may drift to: run together ("ExperimentsAgent",
    // "AutoSuggestAgent"), and the old names lake used before lake#763.
    std::vector<std::string> wrong{"AgentCreator", "Corpus Generator", "AutoSuggest"};
    for (const auto& agent : agent_library) {
        std::string together;
        for (char c : agent.name) {
            if (c != ' ' && c != '-') {
                together += c;
            }
        }
        wrong.push_back(together);
    }
    // A name whose stem is itself several words ("Auto-Suggest", "Eval Generator") is always
    // followed by the rest of its name: "Auto-Suggest" alone is an old name.
    std::vector<std::string> stems;
    for (const auto& agent : agent_library) {
        std::string_view name = agent.name;
        if (name.ends_with(" Agent")) {
            const std::string stem(name.substr(0, name.size() - 6));
            if (stem.find_first_of(" -") != std::string::npos) {
                stems.push_back(stem);
            }
        }
    }
    std::vector<std::string> found;
    for (const fs::path& file : scanned_files()) {
        const std::string text = read(file);
        const std::string where = fs::relative(file, root).generic_string();
        for (const std::string& spelling : wrong) {
            for (std::size_t at = text.find(spelling); at != std::string::npos;
                 at = text.find(spelling, at + 1)) {
                const bool before = at > 0 && word_char(text[at - 1]);
                const bool after = at + spelling.size() < text.size() &&
                                   word_char(text[at + spelling.size()]);
                // A longer identifier (a test named ExperimentsAgentRestAndMcp...) or a test-name
                // pattern (`HttpResponseIsolation.AskOrama*`) is code, not a name a person reads.
                const bool pattern =
                    (at > 0 && text[at - 1] == '.') ||
                    (at + spelling.size() < text.size() && text[at + spelling.size()] == '*');
                if (!before && !after && !pattern) {
                    found.push_back(where + ":" + std::to_string(line_of(text, at)) + ": " +
                                    spelling);
                }
            }
        }
        for (const std::string& stem : stems) {
            for (std::size_t at = text.find(stem); at != std::string::npos;
                 at = text.find(stem, at + 1)) {
                if (at > 0 && word_char(text[at - 1])) {
                    continue;
                }
                const std::size_t end = at + stem.size();
                if (end < text.size() && (word_char(text[end]) || text[end] == '-')) {
                    continue;
                }
                bool named = continues_with(text, end, "Agent");
                // "Auto-Suggest" followed by the rest of another name ("Auto-Suggest Corpus Agent").
                for (const auto& agent : agent_library) {
                    const std::string_view other = agent.name;
                    if (other.size() > stem.size() && other.starts_with(stem) &&
                        other[stem.size()] == ' ') {
                        const std::string_view next = other.substr(stem.size() + 1);
                        named = named || continues_with(text, end, next.substr(0, next.find(' ')));
                    }
                }
                if (!named) {
                    found.push_back(where + ":" + std::to_string(line_of(text, at)) + ": " +
                                    stem + " without the rest of its name");
                }
            }
        }
    }
    std::string list;
    for (const auto& line : found) {
        list += "\n  " + line;
    }
    EXPECT_TRUE(found.empty())
        << "agent names other than the catalogue's (include/lake/control/agent_library.hpp):"
        << list;
}

TEST(AgentNames, TheDocsCatalogueTableIsTheCatalogue) {
    // docs/api/train-agents.md lists the catalogue as `| \`key\` | Name | ...`, in its order.
    std::istringstream text(read(root / "docs/api/train-agents.md"));
    std::vector<std::pair<std::string, std::string>> rows;
    for (std::string line; std::getline(text, line);) {
        if (!line.starts_with("| `agent-")) {
            continue;
        }
        const std::size_t key_end = line.find('`', 3);
        const std::size_t name_start = line.find("| ", key_end) + 2;
        const std::size_t name_end = line.find(" |", name_start);
        rows.emplace_back(line.substr(3, key_end - 3),
                          line.substr(name_start, name_end - name_start));
    }
    std::vector<std::pair<std::string, std::string>> catalogue;
    for (const auto& agent : agent_library) {
        catalogue.emplace_back(std::string(agent.key), std::string(agent.name));
    }
    EXPECT_EQ(rows, catalogue);
    // The in-app guide's Agents page names every agent.
    const std::string guide = read(root / "docs/guide/agents.md");
    for (const auto& agent : agent_library) {
        EXPECT_NE(guide.find("**" + std::string(agent.name) + "**"), std::string::npos)
            << "docs/guide/agents.md does not list " << agent.name;
    }
}

TEST(AgentNames, NoCodePairsAKeyWithItsNameOutsideTheCatalogue) {
    // A second key → name table (`{"agent-ask-orama", "Ask Orama"}`, `{ id: "agent-x", name: "X" }`)
    // is a copy that drifts: code reads the name from the catalogue (agent_display_name,
    // list_agent_library) instead. A name inside a sentence is not a pairing.
    std::vector<std::string> found;
    for (const fs::path& file : scanned_files()) {
        const std::string extension = file.extension().string();
        const std::string where = fs::relative(file, root).generic_string();
        // Tests check the catalogue's values, so they may write them down.
        if (extension == ".md" || where == "include/lake/control/agent_library.hpp" ||
            where.starts_with("tests/") || where.starts_with("frontend/tests/")) {
            continue;
        }
        std::istringstream text(read(file));
        std::vector<std::string> lines;
        for (std::string line; std::getline(text, line);) {
            lines.push_back(line);
        }
        for (std::size_t i = 0; i < lines.size(); ++i) {
            for (const auto& agent : agent_library) {
                const std::string key = "\"" + std::string(agent.key) + "\"";
                if (lines[i].find(key) == std::string::npos) {
                    continue;
                }
                for (std::size_t j = i >= 2 ? i - 2 : 0; j < std::min(lines.size(), i + 3); ++j) {
                    for (const auto& named : agent_library) {
                        for (const char quote : {'"', '\''}) {
                            const std::string literal =
                                std::string(1, quote) + std::string(named.name) + quote;
                            if (lines[j].find(literal) != std::string::npos) {
                                found.push_back(where + ":" + std::to_string(j + 1) + ": " +
                                                std::string(named.name) + " beside " + key);
                            }
                        }
                    }
                }
            }
        }
    }
    std::string list;
    for (const auto& line : found) {
        list += "\n  " + line;
    }
    EXPECT_TRUE(found.empty()) << "agent names written next to their keys outside the catalogue:"
                               << list;
}
