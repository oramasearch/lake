#pragma once

#include <array>
#include <expected>
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
    // What people call it, everywhere: the one source of agent names (lake#763). Two or more
    // title-case words, and "Agent" last for every agent but Ask Orama;
    // tests/test_agent_names.cpp checks the scheme and that no other spelling is used anywhere.
    std::string_view name;
    std::string_view purpose; // one sentence: what it does
    std::string_view used_in; // where a person meets it in lake
    std::string_view runs_as; // service | agent-runtime | offline
    bool chat = false;        // takes a conversation
    // Orama's testing agents (the Eval Generator Agent and the two corpus agents, lake#451): listed
    // on Design → Agents in their own section, but never offered in Ask Orama's *Talk to* list,
    // which holds only agents a person uses (lake#763).
    bool internal = false;
};

inline constexpr std::array<LibraryAgent, 9> agent_library{{
    {"agent-ask-orama",
     "Ask Orama",
     "Answers questions about anything in your Lake and hands work to the right agent, including "
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
    {"agent-application",
     "Application Agent",
     "Builds web and data apps in a private Lake repository, then creates and deploys the app "
     "only after you confirm its proposal.",
     "Ship → Apps → Build an App; Ask Orama",
     "service",
     true},
    {"agent-experiments",
     "Experiments Agent",
     "Designs training experiments, watches their trial runs, explains the result, and starts or "
     "promotes work only after you confirm its proposal.",
     "Train → Experiments → Run an Experiment; Ask Orama; Project Agent",
     "service",
     true},
    {"agent-autosuggest",
     "Auto-Suggest Agent",
     "Looks at your data and suggests projects worth building, best first. You pick which "
     "become projects.",
     "Design → Projects → Discover Opportunities",
     "service",
     true},
    {"agent-creator",
     "Agent Creator Agent",
     "Designs a new agent with you in a conversation, and creates it only when you approve.",
     "Design → Agents → Describe it to the Agent Factory",
     "agent-runtime",
     true},
    {"agent-eval-generator",
     "Eval Generator Agent",
     "Writes a realistic test case on request: a role, the problem they face and real-looking "
     "data for it, the way Orama tests its own agents. It publishes nothing.",
     "Orama's own agent tests; not offered in Ask Orama",
     "service",
     true,
     true},
    {"agent-autosuggest-corpus",
     "Auto-Suggest Corpus Agent",
     "Writes one example of what is worth building for a business situation you describe, "
     "checked against the test cases so it never repeats one.",
     "Orama's own agent tests; not offered in Ask Orama",
     "service",
     true,
     true},
    {"agent-project-corpus",
     "Project Corpus Agent",
     "Records a worked example of building (or changing) a pipeline on your own data, every "
     "step checked against what your workspace really returns. It changes nothing.",
     "Orama's own agent tests; not offered in Ask Orama",
     "service",
     true,
     true},
}};

const LibraryAgent* library_agent(std::string_view key);

// LAKE_AGENT_ENDPOINTS: `key=url` pairs separated by commas, set by the Orama operator for the
// agents it runs (orama-operator#185). Unknown keys and malformed pairs are ignored.
std::map<std::string, std::string> parse_agent_endpoints(std::string_view text);

// LAKE_AGENT_LEASE_SECONDS: key=seconds pairs, bounded to 60..3600; omitted keys use 600.
std::expected<std::map<std::string, int>, std::string> parse_agent_leases(std::string_view text);

// The library as the API and the pages show it: each agent with `deployed`, `internal` and its
// status.
nlohmann::json agent_library_json(const std::map<std::string, std::string>& endpoints);

// Ask Orama's *Talk to* entries from that library (lake#451, lake#763): Ask Orama first, then
// every other chat agent a person uses, as {id, name, purpose, used_in, running}. One this Lake
// does not run is listed with `running: false` rather than left out. Orama's testing agents
// (`internal`) are never offered.
nlohmann::json talk_to_agents(const nlohmann::json& library);

// What people call an agent, by its library key; any other id (an agent of the space) as given.
std::string agent_display_name(std::string_view key);

} // namespace lake::control
