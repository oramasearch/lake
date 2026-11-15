#pragma once

#include <string_view>

#include <cstddef>
#include <string>
#include <vector>

#include <liquid/runtime.hpp>

#include "lake/api/registry.hpp"
#include "lake/control/zetta_control.hpp"

namespace lake::api {

// A stream Lake and its agents write themselves (agent tasks and events, app, build, run-metric and
// study events, inboxes). No tenant writes one, names one as an MCP connection's topic, or makes one a
// trigger (LSR-190, lake#996). `name` may carry the "topic:" prefix.
[[nodiscard]] bool system_stream_name(std::string_view name);


// An agent started by a workspace stream (lake#677, docs/api/agent-launches.md#stream-triggers):
// `set_agent_streams` names the streams whose new messages each start a run of a Python agent,
// with the message as its input, as the person who set it; `list_agent_streams` reads them. Each
// is a sealed `lake_agent_stream_triggers` record in the agent's workspace Zetta.
void register_agent_stream_capabilities(Registry& registry);

// Removes every stream trigger of an agent in a workspace (after delete_agent), answering how many
// it removed. A workspace whose records cannot be read answers its error.
liquid::runtime::task<control::ZettaControlResult<std::size_t>>
remove_agent_stream_triggers(const ApiContext& context,
                             std::string customer_id,
                             std::string workspace_id,
                             std::string agent_id);

// The stream-trigger worker, run by the store root's owner only, on the stream thread beside the
// MCP connection streams (lake#980): for each running workspace's triggers, it starts one job of the
// agent for each new message on the stream, as the person who set the trigger, and moves the
// trigger's cursor past it. A trigger whose agent is gone is removed. Answers what one pass did.
struct AgentStreamPass {
    std::size_t workspaces = 0;
    std::size_t triggers = 0;
    std::size_t runs = 0;
    std::size_t removed = 0;
    std::vector<std::string> problems;
};
class AgentStreamWorker {
public:
    AgentStreamPass pass(ApiContext& context);
};

} // namespace lake::api
