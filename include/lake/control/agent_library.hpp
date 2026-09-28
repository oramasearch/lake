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

inline constexpr std::array<LibraryAgent, 9> agent_library{{
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
    {"agent-application",
     "ApplicationAgent",
     "Builds web and data apps in a private Lake repository, then creates and deploys the app "
     "only after you confirm its proposal.",
     "Ship → Apps → Build an App; Ask Orama",
     "service",
     true},
    {"agent-experiments",
     "ExperimentsAgent",
     "Designs training studies, watches their trials, explains the result, and starts or promotes "
     "work only after you confirm its proposal.",
     "Train → Experiments → Run an Experiment; Ask Orama; Project Agent",
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
     "Writes a realistic test case on request: a role, the problem they face and real-looking "
     "data for it, the way Orama tests its own agents. It publishes nothing.",
     "Ask Orama → Talk to Eval Generator, or ask Ask Orama for a test case",
     "service",
     true},
    {"agent-autosuggest-corpus",
     "Auto-Suggest Corpus Generator",
     "Writes one example of what is worth building for a business situation you describe, "
     "checked against the test cases so it never repeats one.",
     "Ask Orama → Talk to Auto-Suggest Corpus Generator",
     "service",
     true},
    {"agent-project-corpus",
     "Project Corpus Generator",
     "Records a worked example of building (or changing) a pipeline on your own data, every "
     "step checked against what your workspace really returns. It changes nothing.",
     "Ask Orama → Talk to Project Corpus Generator",
     "service",
     true},
}};

const LibraryAgent* library_agent(std::string_view key);

// LAKE_AGENT_ENDPOINTS: `key=url` pairs separated by commas, set by the Orama operator for the
// agents it runs (orama-operator#185). Unknown keys and malformed pairs are ignored.
std::map<std::string, std::string> parse_agent_endpoints(std::string_view text);

// The library as the API and the pages show it: each agent with `deployed` and its status.
nlohmann::json agent_library_json(const std::map<std::string, std::string>& endpoints);

} // namespace lake::control
