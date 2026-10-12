#pragma once
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

// The digest a workspace's MCP endpoint (/w/<workspace>/mcp) names its tool list by: Lake's own
// (`lake_digest`) when the workspace re-exports no connection's tools, otherwise the hex SHA-256
// of Lake's digest and each re-exporting connection's id and exported-tools digest, in connection
// order (lake#684). Exporting a connection, or changing what it exports, changes it.
struct McpExportedDigest {
    std::string connection_id;
    std::string digest;
};
std::string mcp_workspace_tool_list_digest(std::string_view lake_digest,
                                           std::vector<McpExportedDigest> exported);

// What each session last received from tools/list, in this process (lake#684). Without it a
// session that began on an older list would be told on every request until it initialized again;
// with it, the notice stops once the session has listed the current tools. Bounded: the oldest
// sessions are forgotten first, and a forgotten or another replica's session is judged by the
// list it began on, so at worst it is told once more.
class McpSessionLists {
public:
    explicit McpSessionLists(std::size_t capacity = 10000) : capacity_(capacity) {}
    // The session listed the tools whose digest is `digest`.
    void listed(const std::string& session_id, const std::string& digest);
    // Whether the session must be told: its last list (or the list it began on) is not `digest`.
    bool changed(const std::string& session_id, std::string_view digest) const;

private:
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::map<std::string, std::string> listed_;
    std::deque<std::string> order_;
};

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
