#pragma once

// A workspace's MCP connections (decision 60, lake#652, docs/api/mcp-connections.md): the
// capabilities over them, and what Lake's own MCP endpoint needs to re-export a connection's
// allowed tools under its prefix.

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/api/registry.hpp"

namespace lake::api {

// create_mcp_connection, list_mcp_connections, get_mcp_connection, update_mcp_connection,
// delete_mcp_connection, refresh_mcp_connection, test_mcp_connection, list_mcp_connection_tools,
// call_mcp_tool, read_mcp_tool, set_mcp_tool_auto_approve, list_mcp_connection_calls,
// begin_mcp_connection_sign_in, complete_mcp_connection_sign_in, create_mcp_connection_agent.
void register_mcp_connection_capabilities(Registry& registry);

// Whether a tools/call name is a connected tool's (`<prefix>__<tool>`): Lake's own capability
// names never hold "__".
bool is_mcp_exported_tool_name(std::string_view name);

// The tools/list entries of the workspace's connections marked to be exposed through Lake's MCP:
// each allowed tool as `<prefix>__<tool>`, its server's schema unchanged. Answers an empty array
// when the caller may not read the workspace or it holds none; never fails a tools/list.
liquid::runtime::task<nlohmann::json>
exported_mcp_tools(const Registry& registry, ApiContext& context, std::string workspace_id);

// A tools/call of an exported tool, as the caller, through read_mcp_tool or call_mcp_tool (so it
// is authorized, audited and metered exactly as theirs): the server's own result ({content,
// structuredContent?, isError}), or isError with Lake's refusal.
liquid::runtime::task<nlohmann::json> call_exported_mcp_tool(const Registry& registry,
                                                             ApiContext& context,
                                                             std::string workspace_id,
                                                             std::string name,
                                                             nlohmann::json arguments);

// The stream worker (docs/api/mcp-connections.md#a-connections-agent-and-streams), run by the store
// root's owner only, on a thread of its own (lake#980), never the maintenance thread that samples
// usage and runs deletions and orders: for each Ready workspace's connections, it mirrors
// the server's notifications to the connection's notifications topic (one session kept open per
// connection between passes, its subscriptions renewed when they change), starts a job of the
// connection's agent for each new event on its input topic, as the person who set the streams, and
// publishes each finished job's result to its output topic. Answers what one pass did.
struct McpStreamPass {
    std::size_t workspaces = 0;
    std::size_t connections = 0;
    std::size_t notifications = 0;
    std::size_t turns = 0;
    std::size_t results = 0;
    // lake#980: connections whose server is not contacted this pass because earlier attempts
    // failed, and whether the pass stopped at its time budget or connection count before reaching
    // every workspace (the next pass starts where this one stopped).
    std::size_t backing_off = 0;
    bool stopped_early = false;
    std::vector<std::string> problems;
};
// Bounds on what one outside server can cost a pass (lake#980, LSR-113).
struct McpStreamLimits {
    // A pass starts no further connection once this much time has gone; the next pass resumes at
    // the workspace where it stopped, so every workspace is reached in turn.
    std::chrono::milliseconds pass_budget{10'000};
    // Opening one connection's session: initialize, then each resource subscription. Past it the
    // remaining subscriptions are not sent, the session is dropped and the connection backs off.
    std::chrono::milliseconds connection_budget{5'000};
    // One HTTP exchange of a stream session (a call's own exchanges keep McpLimits' 30 s).
    std::chrono::milliseconds exchange_timeout{5'000};
    // After a failure the connection's server is not contacted again for `backoff_first`, doubled
    // after each further failure up to `backoff_max`; one success clears it.
    std::chrono::milliseconds backoff_first{30'000};
    std::chrono::milliseconds backoff_max{15 * 60'000};
};
class McpStreamWorker {
public:
    explicit McpStreamWorker(McpStreamLimits limits = {});
    ~McpStreamWorker();
    McpStreamPass pass(ApiContext& context);

private:
    struct Sessions;
    std::unique_ptr<Sessions> sessions_;
};

} // namespace lake::api
