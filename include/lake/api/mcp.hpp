#pragma once
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "lake/api/registry.hpp"

namespace lake::api {

inline constexpr std::string_view mcp_protocol_version = "2025-06-18";

// The `tools` array of tools/list for one audience. Input schemas are audience_input_schema and
// output schemas success_envelope_schema: the schemas OpenAPI gives the same capability's REST
// route, byte for byte.
nlohmann::json mcp_tools(const Registry& registry, SchemaAudience audience);

// Hex SHA-256 of the tool list (names, descriptions and schemas). Two lake builds serve the same
// tools exactly when their digests agree.
std::string mcp_tool_list_digest(const Registry& registry);

// The Mcp-Session-Id initialize answers: the tool-list digest, a dot, and 128 random bits. Lake
// keeps no session state; the identifier carries the tool list the client was given.
std::string mcp_session_id(std::string_view tool_list_digest);

// True when a client's session began on a different tool list — lake was upgraded since it
// initialized — so it must be sent notifications/tools/list_changed. An absent identifier is
// false: lake cannot know which list such a client holds.
bool mcp_session_tool_list_changed(std::string_view session_id, std::string_view tool_list_digest);

// {"jsonrpc":"2.0","method":"notifications/tools/list_changed"}
nlohmann::json mcp_tool_list_changed_notification();

// Dispatch one JSON-RPC 2.0 request (initialize, ping, tools/list, tools/call). Returns the
// response body, or nullopt for notifications. initialize declares tools.listChanged. tools/list
// lists mcp_tools for the context's schema_audience. tools/call runs Registry::call with the given
// context, so authorization is exactly the REST path's.
std::optional<std::string> mcp_dispatch(const Registry& registry,
                                        const nlohmann::json& request,
                                        ApiContext& context,
                                        std::string_view version);
liquid::runtime::task<std::optional<std::string>> mcp_dispatch_async(const Registry& registry,
                                                                     const nlohmann::json& request,
                                                                     ApiContext& context,
                                                                     std::string_view version);

} // namespace lake::api
