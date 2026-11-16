// SPDX-License-Identifier: MIT

#pragma once

// What the stream worker shares with the MCP connection capabilities (lake#652): the bearer a
// connection presents, resolved server-side from its workspace secret. Never a capability: the
// value never leaves Lake's process.

#include <optional>
#include <string>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/api/registry.hpp"
#include "lake/control/mcp_connections.hpp"

namespace lake::api {

// The connection's bearer (empty for none), or nullopt when it needs a person to sign in or its
// secret cannot be read.
liquid::runtime::task<std::optional<std::string>>
mcp_connection_bearer(const ApiContext& context, const control::McpConnection& connection);

// Whether the connection is a stdio server's App (lake#659) at the cluster Service address Lake
// made for it, `http://app-<12 hex of the App's id>.<namespace>.svc.cluster.local/mcp`: only then
// may its client reach that private address.
bool mcp_stdio_app_address(const control::McpConnection& connection);

// Whether the stream worker has something to do for the connection: notifications to mirror, an
// agent with an input stream, or finished jobs' results still to publish (lake#685).
bool mcp_connection_streams(const control::McpConnection& connection);

// The tools/list entries the connection re-exports through Lake's MCP (empty when it is not
// exported or allows no tool), and their hex SHA-256 ("" for none) (lake#684).
nlohmann::json mcp_exported_entries(const control::McpConnection& connection);
std::string mcp_exported_digest(const control::McpConnection& connection);

// Writes the connection's dispatch row in lake's store (migration 0079): whether it streams and
// the digest of what it re-exports; a connection with neither has no row. Ids and a digest only.
// Answers why it could not, or nullopt.
std::optional<std::string> record_mcp_connection_dispatch(const ApiContext& context,
                                                          const std::string& workspace_id,
                                                          const control::McpConnection& connection);

} // namespace lake::api
