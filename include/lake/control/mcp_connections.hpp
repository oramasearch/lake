#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/workspace_records.hpp"

namespace lake::control {

// A workspace's MCP connection (decision 60, lake#652, docs/api/mcp-connections.md): Lake as the
// client of a remote MCP server. It lives in the workspace Zetta of its workspace, in the sealed
// collection `lake_mcp_connections`, found by its space. Its credential is never on the record:
// the record names a workspace secret (decision 58), `lake-secret://<mount>/<name>#<field>`.

// What a person decided about one of the server's tools: whether people, agents and Ask Orama of
// the workspace may use it, and whether it changes something (then it is a proposal a person
// confirms, unless auto-approved for one unattended agent).
struct McpToolSetting {
    bool allowed = false;
    bool change = true;
};

// A person's grant that lets one unattended agent run one change tool without a person confirming
// each call. Per tool and per agent; recorded with who granted it and when.
struct McpAutoApprove {
    std::string tool;
    std::string agent; // the agent's id or name, as its launch credential names it
    std::string granted_by;
    std::string granted_at;
};

struct McpConnection {
    std::string id;
    std::string customer_id;
    std::string name;
    std::string prefix; // the handle in tool names (`<prefix>__<tool>`) and skills (`mcp:<prefix>`)
    std::string url;
    std::string transport = "auto"; // auto, streamable_http or sse
    std::string auth_kind = "none"; // none, bearer or oauth
    // bearer: the field holding the token; oauth: the secret lake writes the refresh token into.
    std::string secret_ref;
    // OAuth 2.1 (no credentials): issuer, authorization_endpoint, token_endpoint,
    // registration_endpoint, client_id, resource, scope, signed_in_at, signed_in_by.
    nlohmann::json oauth = nlohmann::json::object();
    // What the server offered at the last refresh (mcp_discovery_json): server, transport, tools,
    // resources, prompts, skipped.
    nlohmann::json discovery = nlohmann::json::object();
    std::map<std::string, McpToolSetting> tools;
    bool exported = false;
    std::string exported_by;
    std::vector<McpAutoApprove> auto_approve;
    // Its own agent, and its optional streams (workspace topics).
    std::string agent_id;
    std::string input_topic;
    std::string output_topic;
    std::string notifications_topic;
    std::vector<std::string> subscribe_resources;
    // The person whose authority the stream worker acts with: who last set a stream or the agent.
    std::string streams_set_by;
    // The stream worker's place: the last input event that started a turn, and the launches still
    // running whose results go to the output stream.
    std::uint64_t input_cursor = 0;
    std::vector<std::string> pending_launches;
    // A server that speaks only stdio, which Lake runs as a workspace App (lake#659): the App's id
    // and what it runs, {runtime, package, command, args, env_keys, secret_env_keys, mount_id,
    // app_name}. The URL is then the App's address inside the cluster, which Lake made and nobody
    // changes. Empty for a remote server.
    std::string app_id;
    nlohmann::json stdio = nlohmann::json::object();
    std::string status = "new"; // new, connected, needs_sign_in or failing
    std::string last_error;
    std::string refreshed_at;
    std::string created_by;
    std::string created_at;
    std::int64_t at_us = 0;
    std::int64_t lake_revision = 1;
};

struct McpConnectionCodec {
    using Value = McpConnection;
    static constexpr RecordCollection collection{"mcp_connections", 1};

    static std::string lookup(const McpConnection& connection);
    static nlohmann::json encode(const McpConnection& connection);
    static std::optional<McpConnection> decode(const StoredRecord& record);
};

std::string mcp_connections_lookup(const std::string& customer_id);

// A name's prefix: lower-case letters, digits and underscores, starting with a letter, at most 24
// bytes ("GitHub Issues" -> "github_issues"); empty when nothing of the name is usable.
std::string mcp_connection_prefix(std::string_view name);
// `<prefix>__<tool>`, the name a connected tool has outside its connection.
std::string mcp_exported_tool_name(const std::string& prefix, const std::string& tool);
bool known_mcp_transport(const std::string& transport);
bool known_mcp_auth_kind(const std::string& kind);
std::optional<std::string> mcp_connection_problem(const McpConnection& connection);

// Every connection of the space in this workspace, by name.
liquid::runtime::task<ZettaControlResult<std::vector<McpConnection>>>
list_mcp_connections(WorkspaceRecords& records, std::string customer_id);

// One connection by id; nullopt when this workspace holds none of the space.
liquid::runtime::task<ZettaControlResult<std::optional<McpConnection>>>
get_mcp_connection(WorkspaceRecords& records, std::string customer_id, std::string id);

// A new connection under a new id. `conflict` at /name when another connection of the workspace
// has the name or the prefix.
liquid::runtime::task<ZettaControlResult<McpConnection>>
record_mcp_connection(WorkspaceRecords& records, McpConnection connection);

// Read-modify-write at the revision read, rereading on a conflict. `change` edits the connection
// or answers why not (an ApiError-like {code, message, path}). `not_found` when there is none.
using McpConnectionChange =
    std::function<std::optional<ZettaControlError>(McpConnection& connection)>;
liquid::runtime::task<ZettaControlResult<McpConnection>> change_mcp_connection(
    WorkspaceRecords& records, std::string customer_id, std::string id, McpConnectionChange change);

liquid::runtime::task<ZettaControlResult<void>>
remove_mcp_connection(WorkspaceRecords& records, std::string customer_id, std::string id);

// One OAuth sign-in in progress (lake#652): the PKCE verifier and where the code goes back, kept
// ten minutes in `lake_mcp_oauth_states`, keyed by the state's random part, and taken once. It
// names the server, issuer, token endpoint and resource it was begun for, so its code is exchanged
// only there and only while the connection still names them (lake#927).
struct McpOAuthState {
    std::string state;
    std::string customer_id;
    std::string connection_id;
    std::string verifier;
    std::string redirect_uri;
    std::string client_id;
    std::string client_secret; // from registration, when the server issued one
    std::string mount_id;
    std::string secret_name;
    std::string started_by;
    std::string url;
    std::string issuer;
    std::string token_endpoint;
    std::string resource;
};
struct McpOAuthStateCodec {
    using Value = McpOAuthState;
    static constexpr RecordCollection collection{"mcp_oauth_states", 1, 10LL * 60 * 1000};
    static std::string lookup(const McpOAuthState& state);
    static nlohmann::json encode(const McpOAuthState& state);
    static std::optional<McpOAuthState> decode(const StoredRecord& record);
};

liquid::runtime::task<ZettaControlResult<void>> record_mcp_oauth_state(WorkspaceRecords& records,
                                                                       McpOAuthState state);
// Reads and deletes the state, so a code is exchanged at most once; `not_found` when it is
// unknown, expired, of another space or already taken.
liquid::runtime::task<ZettaControlResult<McpOAuthState>>
take_mcp_oauth_state(WorkspaceRecords& records, std::string customer_id, std::string state);

} // namespace lake::control
