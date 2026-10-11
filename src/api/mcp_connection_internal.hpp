#pragma once

// What the stream worker shares with the MCP connection capabilities (lake#652): the bearer a
// connection presents, resolved server-side from its workspace secret. Never a capability: the
// value never leaves Lake's process.

#include <optional>
#include <string>

#include <liquid/runtime.hpp>

#include "lake/api/registry.hpp"
#include "lake/control/mcp_connections.hpp"

namespace lake::api {

// The connection's bearer (empty for none), or nullopt when it needs a person to sign in or its
// secret cannot be read.
liquid::runtime::task<std::optional<std::string>>
mcp_connection_bearer(const ApiContext& context, const control::McpConnection& connection);

} // namespace lake::api
