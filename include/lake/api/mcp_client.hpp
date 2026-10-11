#pragma once

// Lake as an MCP client (decision 60, lake#652, docs/api/mcp-connections.md): the protocol a
// workspace's MCP connection speaks to a remote MCP server, Streamable HTTP or the older HTTP+SSE
// transport. Everything here is pure: the HTTP exchange is injected as an McpHttp, which the
// server implements over its outbound client (src/api/http_server.cpp, the one TU that names HTTP
// types) and tests implement against a local test server.
//
// The server on the other end is not trusted. Every exchange has a deadline and a byte limit,
// a response must answer the request it was sent for, a server's own requests (sampling, roots,
// elicitation) are never served because Lake declares none of those capabilities, and what a
// server describes (tool names, descriptions, schemas) is bounded before Lake keeps it.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace lake::api {

// The version Lake asks for, and the older ones it accepts from a server.
inline constexpr std::string_view mcp_client_protocol_version = "2025-06-18";
bool mcp_supported_protocol_version(std::string_view version);

// How an exchange with a server is bounded.
struct McpLimits {
    std::chrono::milliseconds timeout{30'000};        // one HTTP exchange, end to end
    std::size_t max_response_bytes = 4 * 1024 * 1024; // one response body or event stream
    std::size_t max_events = 512; // SSE events read while waiting for one answer
    std::size_t max_pages = 10;   // tools/list, resources/list, prompts/list pages
    std::size_t max_tools = 128;
    std::size_t max_resources = 100;
    std::size_t max_prompts = 100;
    std::size_t max_title_bytes = 256;
    std::size_t max_description_bytes = 2048;
    std::size_t max_schema_bytes = 16 * 1024;
    // The whole discovery a connection record keeps: tools past it are skipped, with a reason.
    std::size_t max_discovery_bytes = 512 * 1024;
};

// ---- The transport -----------------------------------------------------------------------------

struct McpHttpRequest {
    std::string method = "POST";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::chrono::milliseconds timeout{30'000};
    std::size_t max_response_bytes = 4 * 1024 * 1024;
};

struct McpHttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers; // names lower-cased
    std::string body;
    std::string error;      // the transport's failure; empty when an HTTP answer arrived
    bool timed_out = false; // the deadline passed before the answer (or `enough`) was read
    bool truncated = false; // the body reached max_response_bytes and reading stopped
    std::size_t sent_bytes = 0;

    std::string header(std::string_view name) const;
};

// Called with the body read so far after each chunk; true stops reading (the answer is in), so a
// server that keeps an event stream open after answering does not hold the exchange to its
// deadline.
using McpEnough = std::function<bool(std::string_view body)>;

// A long-lived response, read as it arrives: an HTTP+SSE server's event stream, or a Streamable
// HTTP server's GET stream of notifications.
class McpEventStream {
public:
    virtual ~McpEventStream() = default;
    // The status and headers, once the stream opened; `error` when it did not.
    virtual const McpHttpResponse& head() const = 0;
    struct Read {
        std::string bytes;
        bool closed = false; // the server ended the stream (or the transport failed: error)
        std::string error;
    };
    // The next bytes, waiting at most `wait`; empty bytes and !closed: nothing yet.
    virtual Read read(std::chrono::milliseconds wait) = 0;
};

class McpHttp {
public:
    virtual ~McpHttp() = default;
    virtual McpHttpResponse exchange(const McpHttpRequest& request, const McpEnough& enough) = 0;
    // Opens a GET (or other) whose answer is read as a stream. Never null.
    virtual std::unique_ptr<McpEventStream> open(const McpHttpRequest& request) = 0;
};

// The server's transport (src/api/http_server.cpp): HTTPS only, every address the name resolves
// to public. `allow_private_addresses` (tests, and a Lake started with
// LAKE_MCP_ALLOW_PRIVATE_ADDRESSES=1 for local development) also allows loopback and private
// addresses, and plain http to loopback.
std::shared_ptr<McpHttp> make_mcp_http(bool allow_private_addresses);

// ---- Addresses and URLs ------------------------------------------------------------------------

// Whether an address may be reached by an MCP connection: not loopback, private (RFC 1918, ULA),
// link-local (the cloud metadata address included), carrier-grade NAT, unspecified, multicast or
// reserved. An IPv4-mapped or NAT64 IPv6 address is judged by its IPv4 address. `bytes` is 4 or
// 16 bytes in network order.
bool mcp_public_address(const std::vector<std::uint8_t>& bytes);

struct McpUrl {
    std::string scheme; // https or http
    std::string host;   // lower-cased, without brackets
    std::string port;   // explicit, or the scheme's
    std::string path;   // with the query; "/" when empty
    std::string origin() const;
};
// Parses an MCP server URL: https (http only to loopback, and only when `allow_loopback_http`),
// no user information, no fragment, at most 2048 bytes. The error says why not.
std::expected<McpUrl, std::string> parse_mcp_url(std::string_view url, bool allow_loopback_http);
// `reference` (absolute, or a path) resolved against `base`; refused unless it has base's origin.
std::expected<std::string, std::string> resolve_mcp_url(const McpUrl& base,
                                                        std::string_view reference);

// ---- Server-Sent Events ------------------------------------------------------------------------

struct SseEvent {
    std::string event = "message";
    std::string data;
    std::string id;
};
// An incremental text/event-stream parser (the WHATWG rules: CR, LF or CRLF line ends, `:`
// comments, multi-line data joined by LF, an event dispatched at a blank line).
class SseParser {
public:
    // Feeds bytes; answers the events they completed.
    std::vector<SseEvent> feed(std::string_view bytes);
    std::size_t pending() const {
        return line_.size() + data_.size();
    }

private:
    void line(std::string_view text, std::vector<SseEvent>& out);
    std::string line_;
    bool after_cr_ = false;
    SseEvent current_;
    std::string data_;
    bool has_data_ = false;
};

// ---- What a server offers ----------------------------------------------------------------------

struct McpTool {
    std::string name;
    std::string title;
    std::string description;
    nlohmann::json input_schema = nlohmann::json::object();
    nlohmann::json output_schema; // null when the server gave none
    std::optional<bool> read_only_hint;
    std::optional<bool> destructive_hint;
};
struct McpResource {
    std::string uri;
    std::string name;
    std::string description;
    std::string mime_type;
};
struct McpPrompt {
    std::string name;
    std::string description;
    nlohmann::json arguments = nlohmann::json::array();
};
struct McpServerInfo {
    std::string name;
    std::string version;
    std::string protocol_version;
    std::string instructions; // bounded like a description; data, never instructions to Lake
    bool tools = false;
    bool resources = false;
    bool resources_subscribe = false;
    bool prompts = false;
    bool logging = false;
};
struct McpDiscovery {
    McpServerInfo server;
    std::vector<McpTool> tools;
    std::vector<McpResource> resources;
    std::vector<McpPrompt> prompts;
    // What was left out, and why ("tool 'x': its name is not an MCP tool name").
    std::vector<std::string> skipped;
    std::string transport; // streamable_http or sse, as negotiated
};

// Checks and bounds one tools/list entry; the reason when it is left out.
std::expected<McpTool, std::string> mcp_tool_from(const nlohmann::json& tool,
                                                  const McpLimits& limits);
bool mcp_tool_name(std::string_view name);

nlohmann::json mcp_tool_json(const McpTool& tool);
std::optional<McpTool> mcp_tool_from_json(const nlohmann::json& stored);
nlohmann::json mcp_discovery_json(const McpDiscovery& discovery);

// ---- The client --------------------------------------------------------------------------------

struct McpClientError {
    // unreachable: no answer (DNS, connect, TLS, refused address); timeout; too_large; protocol:
    // the answer is not MCP; auth_required: 401 (`resource_metadata` from WWW-Authenticate when
    // given); forbidden: 403; server_error: a JSON-RPC error or another HTTP status;
    // unsupported_version.
    std::string kind;
    std::string message;
    int status = 0;
    int rpc_code = 0;
    std::string resource_metadata;
};

struct McpCredential {
    std::string bearer; // empty: none. Sent only to the connection's own origin.
};

struct McpClientOptions {
    std::string url;
    // streamable_http, sse, or auto: Streamable HTTP, then the older transport when the server
    // refuses the POST of initialize with 400, 404 or 405.
    std::string transport = "auto";
    McpCredential credential;
    McpLimits limits;
    bool allow_private_addresses = false;
    std::string client_version = "lake";
};

// What one session of exchanges cost on the wire, for metering.
struct McpTraffic {
    std::uint64_t sent_bytes = 0;
    std::uint64_t received_bytes = 0;
    std::uint64_t exchanges = 0;
};

// One notification the server sent while Lake waited for an answer, or on the GET stream.
struct McpNotification {
    std::string method;
    nlohmann::json params = nlohmann::json::object();
};

// One session with a server: initialize on first use, then requests; blocking, so callers run it
// on the blocking pool. Not thread-safe; one per call or per subscription.
class McpClient {
public:
    McpClient(std::shared_ptr<McpHttp> http, McpClientOptions options);
    ~McpClient();
    McpClient(const McpClient&) = delete;
    McpClient& operator=(const McpClient&) = delete;

    std::expected<McpServerInfo, McpClientError> initialize();
    // initialize, then every page of tools/list, resources/list and prompts/list the server
    // declares, each bounded by the limits.
    std::expected<McpDiscovery, McpClientError> discover();
    std::expected<std::vector<McpTool>, McpClientError>
    list_tools(std::vector<std::string>& skipped);
    // tools/call: the result as the server sent it ({content, structuredContent?, isError?}).
    std::expected<nlohmann::json, McpClientError> call_tool(const std::string& name,
                                                            const nlohmann::json& arguments);
    std::expected<nlohmann::json, McpClientError> read_resource(const std::string& uri);
    std::expected<nlohmann::json, McpClientError> get_prompt(const std::string& name,
                                                             const nlohmann::json& arguments);
    std::expected<void, McpClientError> subscribe_resource(const std::string& uri);
    // Any request, for what the methods above do not cover.
    std::expected<nlohmann::json, McpClientError> request(const std::string& method,
                                                          nlohmann::json params);

    // Notifications that arrived with answers since the last call.
    std::vector<McpNotification> take_notifications();

    // Streamable HTTP: opens the GET stream a server pushes notifications on (after initialize).
    // The older transport: the session's own event stream already carries them. Then waits up to
    // `wait` for the next notifications. An error ends the subscription; the caller reconnects.
    std::expected<std::vector<McpNotification>, McpClientError>
    listen(std::chrono::milliseconds wait);

    // Ends the session: DELETE with its session id (Streamable HTTP), or closes the stream.
    void close();

    const McpTraffic& traffic() const {
        return traffic_;
    }
    const std::string& transport() const {
        return transport_;
    }
    const std::string& session_id() const {
        return session_id_;
    }

private:
    struct Legacy;
    std::expected<nlohmann::json, McpClientError> send(const std::string& method,
                                                       nlohmann::json params);
    std::expected<nlohmann::json, McpClientError> send_streamable(const nlohmann::json& message,
                                                                  bool notification);
    std::expected<nlohmann::json, McpClientError> send_legacy(const nlohmann::json& message,
                                                              bool notification);
    std::expected<void, McpClientError> open_legacy();
    std::expected<McpServerInfo, McpClientError> initialize_on(const std::string& transport);
    std::vector<std::pair<std::string, std::string>> headers(bool json_body) const;
    // Reads one JSON-RPC answer for `id` from an event-stream body or a JSON body.
    std::expected<nlohmann::json, McpClientError> answer_from(const McpHttpResponse& response,
                                                              const nlohmann::json& id);
    void keep_notification(const nlohmann::json& message);

    std::shared_ptr<McpHttp> http_;
    McpClientOptions options_;
    McpUrl url_;
    std::string transport_;
    std::string session_id_;
    std::string protocol_version_;
    bool initialized_ = false;
    std::optional<McpServerInfo> server_;
    std::int64_t next_id_ = 1;
    McpTraffic traffic_;
    std::vector<McpNotification> notifications_;
    std::unique_ptr<Legacy> legacy_;
    std::unique_ptr<McpEventStream> listening_;
    SseParser listening_parser_;
};

// A JSON-RPC error object's or an HTTP failure's words for a person, without the server's body
// beyond 300 bytes.
std::string mcp_error_text(const McpClientError& error);

} // namespace lake::api
