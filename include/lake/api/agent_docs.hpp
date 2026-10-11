#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "lake/api/registry.hpp"

// What an agent needs to learn Lake, generated from the one capability registry so it never
// drifts from what MCP and REST serve (docs/api/mcp.md): the tools grouped as the product is, the
// worked flows, /llms.txt, /llms-full.txt, and the downloadable Claude skill. Pure; no I/O.
namespace lake::api::agent_docs {

struct Tool {
    std::string name, title, description;
    bool writes = false;
    bool requires_auth = true;
};

struct Group {
    std::string title, summary;
    std::vector<Tool> tools;
};

// Every MCP tool (the registry's JSON capabilities), in product groups: Workspace (Design, Train,
// Ship), Experiments, Transcripts, Solutions, Drives, Search, Streams, Agents, Apps and packages,
// Services, Storage, Repositories, Organizations and access, Usage and billing, Identity and
// sessions, Secrets, keys and certificates, Audit and transparency, Server. Groups keep the
// registry's order; empty groups are left out.
std::vector<Group> tool_groups(const Registry& registry);

// The group a capability is listed under. Every group names its capabilities on purpose; a name
// none of them claims lands in `ungrouped`, which a test keeps empty (lake#642).
inline constexpr std::string_view ungrouped = "Other";
std::string_view group_of(std::string_view capability);

// The names of every read tool: a read-only MCP key is scoped to exactly these.
std::vector<std::string> read_tool_names(const Registry& registry);

struct FlowStep {
    std::string tool, note;
};
struct Flow {
    std::string title, summary;
    std::vector<FlowStep> steps;
};
// The worked flows the page, llms.txt and the skill teach: build and run a pipeline, load a
// dataset, train and call a model, stand up a service, a volume, a drive of each kind, a secret, a
// transcript, search everything, search a published index, and see usage and the bill.
const std::vector<Flow>& flows();

// `origin` is lake's public origin ("https://lake.example"); the MCP endpoint is origin + "/mcp".
std::string llms_txt(const Registry& registry, std::string_view origin);
std::string llms_full_txt(const Registry& registry, std::string_view origin);
std::string skill_md(const Registry& registry, std::string_view origin);

} // namespace lake::api::agent_docs
