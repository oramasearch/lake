#pragma once

#include <array>
#include <map>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lake::control {

// The Agent Library (docs/api/train-agents.md, lake#228): every specialized agent Orama ships,
// each its own private repository and image. People do not create or delete these; lake lists
// them, says what each is for and where a person meets it, and reports whether this lake runs it.
struct LibraryAgent {
    std::string_view key;     // the repository name, and the key in LAKE_AGENT_ENDPOINTS
    std::string_view name;    // what people call it
    std::string_view purpose; // one sentence: what it does
    std::string_view used_in; // where a person meets it in lake
    std::string_view runs_as; // service | agent-runtime | offline
    bool chat = false;        // takes a conversation in Ask Orama
};

inline constexpr std::array<LibraryAgent, 7> agent_library{{
    {"agent-ask-orama",
     "Ask Orama",
     "Answers questions about anything in your lake and hands work to the right agent, including "
     "the ones you build. It never changes anything until you confirm.",
     "Ask Orama, at the top of every page",
     "service",
     true},
    {"agent-project",
     "Project Agent",
     "Builds a pipeline from what you want to predict, and proposes changes to an existing one "
     "for you to review and apply.",
     "Design → a project → Build a Pipeline; on a pipeline, Ask the Project Agent",
     "service",
     true},
    {"agent-autosuggest",
     "Auto-Suggest",
     "Looks at your data and suggests projects worth building, best first. You pick which "
     "become projects.",
     "Design → Projects → Discover Opportunities",
     "service",
     true},
    {"agent-creator",
     "AgentCreator",
     "Designs a new agent with you in a conversation, and creates it only when you approve.",
     "Train → Agents → Build it with the Agent Factory",
     "agent-runtime",
     true},
    {"agent-eval-generator",
     "Eval Generator",
     "Writes realistic test cases for agents: a role, its problem and real-looking data, which "
     "Orama's agents are scored against.",
     "Used by Orama to measure its agents; not yet available to chat",
     "offline",
     false},
    {"agent-autosuggest-corpus",
     "Auto-Suggest Corpus Generator",
     "Writes the examples Auto-Suggest learns from, kept apart from the test cases.",
     "Used by Orama to train Auto-Suggest; not yet available to chat",
     "offline",
     false},
    {"agent-project-corpus",
     "Project Corpus Generator",
     "Records worked examples of building pipelines on real data, which the Project Agent learns "
     "from.",
     "Used by Orama to train the Project Agent; not yet available to chat",
     "offline",
     false},
}};

const LibraryAgent* library_agent(std::string_view key);

// LAKE_AGENT_ENDPOINTS: `key=url` pairs separated by commas, set by the Orama operator for the
// agents it runs (orama-operator#185). Unknown keys and malformed pairs are ignored.
std::map<std::string, std::string> parse_agent_endpoints(std::string_view text);

// The library as the API and the pages show it: each agent with `deployed` and its status.
nlohmann::json agent_library_json(const std::map<std::string, std::string>& endpoints);

} // namespace lake::control
