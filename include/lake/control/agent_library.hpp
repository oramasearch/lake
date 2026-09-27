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
     "The general agent for all of lake. It works as the signed-in person and can delegate to any "
     "agent as a subagent, including the ones you build.",
     "The Ask Orama panel on every page",
     "service",
     true},
    {"agent-project",
     "Project Agent",
     "Builds and changes Pipelines by calling tools, from a project's intent and the data it can "
     "reach.",
     "Design: Create a Solution and revised plans; Ask Orama's subagent for anything "
     "Pipeline-shaped",
     "service",
     true},
    {"agent-autosuggest",
     "Auto-Suggest",
     "Reads a Workspace Profile and recommends the Opportunities worth building, ranked, each "
     "one a prompt that can become a Project.",
     "Design: Discover Opportunities; Ask Orama's subagent for what could I build",
     "service",
     true},
    {"agent-creator",
     "AgentCreator",
     "Proposes a new agent from a conversation and creates it only after you approve the "
     "proposal.",
     "Train: Agents, Create with the Agent Factory",
     "agent-runtime",
     true},
    {"agent-eval-generator",
     "Eval Generator",
     "Researches a role and a pain point and writes an evaluation profile: a situation plus its "
     "real data, the test set the other agents are scored against.",
     "Offline: how Orama measures its agents",
     "offline",
     false},
    {"agent-autosuggest-corpus",
     "Auto-Suggest Corpus Generator",
     "Generates the situations and correct recommendations Auto-Suggest learns from, never "
     "overlapping the evaluation set.",
     "Offline: training data for Auto-Suggest",
     "offline",
     false},
    {"agent-project-corpus",
     "Project Corpus Generator",
     "Records tool-calling trajectories that build Pipelines against a live node, the examples "
     "the Project Agent learns from.",
     "Offline: training data for the Project Agent",
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
