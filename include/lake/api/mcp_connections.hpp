#pragma once

// A workspace's MCP connections (decision 60, lake#652, docs/api/mcp-connections.md): the
// capabilities over them, and what Lake's own MCP endpoint needs to re-export a connection's
// allowed tools under its prefix.

#include <string>
#include <string_view>

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

} // namespace lake::api
