// Lake as an MCP client (decision 60, lake#652, docs/api/mcp-connections.md#the-client). Pure
// protocol over an injected McpHttp: no HTTP library is named here.

#include "lake/api/mcp_client.hpp"

#include <algorithm>
#include <cctype>
#include <deque>
#include <set>

namespace lake::api {
namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(
        out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// The first `limit` bytes of `text`, cut at a UTF-8 character boundary.
std::string bounded(std::string_view text, std::size_t limit) {
    if (text.size() <= limit) {
        return std::string(text);
    }
    std::size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return std::string(text.substr(0, end));
}

std::string text_field(const json& object, const char* key) {
    if (!object.is_object()) {
        return {};
    }
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}

McpClientError client_error(std::string kind, std::string message, int status = 0) {
    return McpClientError{std::move(kind), std::move(message), status, 0, {}};
}

bool loopback_host(std::string_view host) {
    return host == "127.0.0.1" || host == "::1" || host == "localhost";
}

// `resource_metadata="<url>"` of a WWW-Authenticate challenge (RFC 9728 section 5.1).
std::string resource_metadata_of(std::string_view challenge) {
    const std::string folded = lower(challenge);
    const std::size_t at = folded.find("resource_metadata=");
    if (at == std::string::npos) {
        return {};
    }
    std::size_t start = at + std::string_view("resource_metadata=").size();
    if (start < challenge.size() && challenge[start] == '"') {
        const std::size_t end = challenge.find('"', start + 1);
        return end == std::string_view::npos
                   ? std::string()
                   : std::string(challenge.substr(start + 1, end - start - 1));
    }
    const std::size_t end = challenge.find_first_of(", ", start);
    return std::string(challenge.substr(start, end == std::string_view::npos ? end : end - start));
}

bool is_event_stream(const McpHttpResponse& response) {
    return lower(response.header("content-type")).starts_with("text/event-stream");
}

} // namespace

bool mcp_supported_protocol_version(std::string_view version) {
    return version == "2025-06-18" || version == "2025-03-26" || version == "2024-11-05";
}

std::string McpHttpResponse::header(std::string_view name) const {
    const auto found = headers.find(lower(name));
    return found == headers.end() ? std::string() : found->second;
}

// ---- Addresses and URLs ------------------------------------------------------------------------

bool mcp_public_address(const std::vector<std::uint8_t>& bytes) {
    const auto v4 = [](std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
        if (a == 0 || a == 10 || a == 127) {
            return false; // this network, private, loopback
        }
        if (a == 100 && (b & 0xC0) == 64) {
            return false; // carrier-grade NAT, 100.64.0.0/10
        }
        if (a == 169 && b == 254) {
            return false; // link-local, the cloud metadata address included
        }
        if (a == 172 && (b & 0xF0) == 16) {
            return false; // 172.16.0.0/12
        }
        if (a == 192 && b == 168) {
            return false;
        }
        if (a == 192 && b == 0 && (c == 0 || c == 2)) {
            return false; // IETF protocol assignments, TEST-NET-1
        }
        if (a == 198 && (b == 18 || b == 19)) {
            return false; // benchmarking
        }
        if ((a == 198 && b == 51 && c == 100) || (a == 203 && b == 0 && c == 113)) {
            return false; // TEST-NET-2 and -3
        }
        if (a >= 224) {
            return false; // multicast, reserved, broadcast
        }
        static_cast<void>(d);
        return true;
    };
    if (bytes.size() == 4) {
        return v4(bytes[0], bytes[1], bytes[2], bytes[3]);
    }
    if (bytes.size() != 16) {
        return false;
    }
    const bool zero_prefix =
        std::all_of(bytes.begin(), bytes.begin() + 10, [](auto b) { return b == 0; });
    if (zero_prefix && bytes[10] == 0xFF && bytes[11] == 0xFF) {
        return v4(bytes[12], bytes[13], bytes[14], bytes[15]); // IPv4-mapped
    }
    if (bytes[0] == 0x00 && bytes[1] == 0x64 && bytes[2] == 0xFF && bytes[3] == 0x9B &&
        std::all_of(bytes.begin() + 4, bytes.begin() + 12, [](auto b) { return b == 0; })) {
        return v4(bytes[12], bytes[13], bytes[14], bytes[15]); // NAT64, 64:ff9b::/96
    }
    if (std::all_of(bytes.begin(), bytes.begin() + 15, [](auto b) { return b == 0; })) {
        return false; // :: and ::1
    }
    if ((bytes[0] & 0xFE) == 0xFC) {
        return false; // unique local, fc00::/7
    }
    if (bytes[0] == 0xFE && (bytes[1] & 0xC0) == 0x80) {
        return false; // link-local, fe80::/10
    }
    if (bytes[0] == 0xFF) {
        return false; // multicast
    }
    if (bytes[0] == 0x20 && bytes[1] == 0x01 && bytes[2] == 0x0D && bytes[3] == 0xB8) {
        return false; // documentation, 2001:db8::/32
    }
    return true;
}

std::string McpUrl::origin() const {
    const bool standard =
        (scheme == "https" && port == "443") || (scheme == "http" && port == "80");
    const std::string shown = host.find(':') != std::string::npos ? "[" + host + "]" : host;
    return scheme + "://" + shown + (standard ? std::string() : ":" + port);
}

std::expected<McpUrl, std::string> parse_mcp_url(std::string_view url, bool allow_loopback_http) {
    if (url.empty() || url.size() > 2048) {
        return std::unexpected("the URL is empty or longer than 2048 bytes");
    }
    for (const char c : url) {
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F) {
            return std::unexpected("the URL holds a space or a control character");
        }
    }
    McpUrl parsed;
    const std::string folded = lower(url.substr(0, std::min<std::size_t>(url.size(), 8)));
    if (folded.starts_with("https://")) {
        parsed.scheme = "https";
        parsed.port = "443";
        url.remove_prefix(8);
    } else if (folded.starts_with("http://")) {
        parsed.scheme = "http";
        parsed.port = "80";
        url.remove_prefix(7);
    } else {
        return std::unexpected("an MCP server URL starts with https://");
    }
    if (url.find('#') != std::string_view::npos) {
        return std::unexpected("an MCP server URL has no fragment");
    }
    const std::size_t path_at = url.find_first_of("/?");
    const std::string_view authority = url.substr(0, path_at);
    parsed.path = path_at == std::string_view::npos ? "/" : std::string(url.substr(path_at));
    if (parsed.path.starts_with('?')) {
        parsed.path.insert(parsed.path.begin(), '/');
    }
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        return std::unexpected("the URL names no host, or carries a user name");
    }
    std::string_view port;
    if (authority.starts_with('[')) {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos) {
            return std::unexpected("the URL's IPv6 host is not closed");
        }
        parsed.host = lower(authority.substr(1, close - 1));
        const std::string_view rest = authority.substr(close + 1);
        if (!rest.empty()) {
            if (!rest.starts_with(':')) {
                return std::unexpected("the URL's host is followed by something other than a port");
            }
            port = rest.substr(1);
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        parsed.host = lower(authority.substr(0, colon));
        if (colon != std::string_view::npos) {
            port = authority.substr(colon + 1);
        }
    }
    if (!port.empty()) {
        if (port.size() > 5 ||
            !std::ranges::all_of(port, [](unsigned char c) { return std::isdigit(c) != 0; })) {
            return std::unexpected("the URL's port is not a number");
        }
        const int number = std::stoi(std::string(port));
        if (number < 1 || number > 65535) {
            return std::unexpected("the URL's port is out of range");
        }
        parsed.port = std::to_string(number);
    }
    if (parsed.host.empty()) {
        return std::unexpected("the URL names no host");
    }
    if (parsed.scheme == "http" && !(allow_loopback_http && loopback_host(parsed.host))) {
        return std::unexpected("an MCP server URL starts with https://");
    }
    return parsed;
}

std::expected<std::string, std::string> resolve_mcp_url(const McpUrl& base,
                                                        std::string_view reference) {
    if (reference.empty() || reference.size() > 2048 || reference.starts_with("//")) {
        return std::unexpected("the server named an endpoint Lake will not follow");
    }
    const std::string folded =
        lower(reference.substr(0, std::min<std::size_t>(8, reference.size())));
    if (folded.starts_with("https://") || folded.starts_with("http://")) {
        auto other = parse_mcp_url(reference, true);
        if (!other || other->origin() != base.origin()) {
            return std::unexpected("the server named an endpoint on another origin");
        }
        return other->origin() + other->path;
    }
    if (reference.find("://") != std::string_view::npos) {
        return std::unexpected("the server named an endpoint Lake will not follow");
    }
    if (reference.starts_with('/')) {
        return base.origin() + std::string(reference);
    }
    const std::string directory = base.path.substr(0, base.path.find('?'));
    return base.origin() + directory.substr(0, directory.rfind('/') + 1) + std::string(reference);
}

// ---- Server-Sent Events ------------------------------------------------------------------------

std::vector<SseEvent> SseParser::feed(std::string_view bytes) {
    std::vector<SseEvent> out;
    for (const char c : bytes) {
        if (after_cr_ && c == '\n') {
            after_cr_ = false;
            continue;
        }
        after_cr_ = false;
        if (c == '\r' || c == '\n') {
            after_cr_ = c == '\r';
            line(line_, out);
            line_.clear();
            continue;
        }
        line_.push_back(c);
    }
    return out;
}

void SseParser::line(std::string_view text, std::vector<SseEvent>& out) {
    if (text.empty()) {
        if (has_data_) {
            current_.data = std::move(data_);
            out.push_back(std::move(current_));
        }
        current_ = SseEvent{};
        data_.clear();
        has_data_ = false;
        return;
    }
    if (text.starts_with(':')) {
        return;
    }
    const std::size_t colon = text.find(':');
    const std::string_view field = text.substr(0, colon);
    std::string_view value =
        colon == std::string_view::npos ? std::string_view() : text.substr(colon + 1);
    if (value.starts_with(' ')) {
        value.remove_prefix(1);
    }
    if (field == "data") {
        if (has_data_) {
            data_.push_back('\n');
        }
        data_.append(value);
        has_data_ = true;
    } else if (field == "event") {
        current_.event = value.empty() ? "message" : std::string(value);
    } else if (field == "id" && value.find('\0') == std::string_view::npos) {
        current_.id = std::string(value);
    }
}

// ---- What a server offers ----------------------------------------------------------------------

bool mcp_tool_name(std::string_view name) {
    return !name.empty() && name.size() <= 128 && std::ranges::all_of(name, [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.';
    });
}

std::expected<McpTool, std::string> mcp_tool_from(const json& tool, const McpLimits& limits) {
    if (!tool.is_object()) {
        return std::unexpected("an entry that is not an object");
    }
    McpTool out;
    out.name = text_field(tool, "name");
    const std::string shown = bounded(out.name, 64);
    if (!mcp_tool_name(out.name)) {
        return std::unexpected("tool '" + shown + "': its name is not an MCP tool name");
    }
    const json annotations = tool.value("annotations", json::object());
    out.title = text_field(tool, "title");
    if (out.title.empty()) {
        out.title = text_field(annotations, "title");
    }
    out.title = bounded(out.title, limits.max_title_bytes);
    out.description = bounded(text_field(tool, "description"), limits.max_description_bytes);
    const json schema = tool.value("inputSchema", json());
    if (!schema.is_object() || (schema.contains("type") && schema.at("type") != "object")) {
        return std::unexpected("tool '" + shown + "': its input schema is not an object schema");
    }
    if (schema.dump().size() > limits.max_schema_bytes) {
        return std::unexpected("tool '" + shown + "': its input schema is larger than " +
                               std::to_string(limits.max_schema_bytes / 1024) + " KiB");
    }
    out.input_schema = schema;
    if (!out.input_schema.contains("type")) {
        out.input_schema["type"] = "object";
    }
    const json output = tool.value("outputSchema", json());
    if (output.is_object() && output.dump().size() <= limits.max_schema_bytes) {
        out.output_schema = output;
    }
    if (annotations.is_object()) {
        if (const auto hint = annotations.find("readOnlyHint");
            hint != annotations.end() && hint->is_boolean()) {
            out.read_only_hint = hint->get<bool>();
        }
        if (const auto hint = annotations.find("destructiveHint");
            hint != annotations.end() && hint->is_boolean()) {
            out.destructive_hint = hint->get<bool>();
        }
    }
    return out;
}

json mcp_tool_json(const McpTool& tool) {
    json out{{"name", tool.name},
             {"title", tool.title},
             {"description", tool.description},
             {"input_schema", tool.input_schema}};
    out["output_schema"] = tool.output_schema.is_object() ? tool.output_schema : json(nullptr);
    out["read_only_hint"] = tool.read_only_hint ? json(*tool.read_only_hint) : json(nullptr);
    out["destructive_hint"] = tool.destructive_hint ? json(*tool.destructive_hint) : json(nullptr);
    return out;
}

std::optional<McpTool> mcp_tool_from_json(const json& stored) {
    if (!stored.is_object() || !mcp_tool_name(text_field(stored, "name"))) {
        return std::nullopt;
    }
    McpTool tool;
    tool.name = text_field(stored, "name");
    tool.title = text_field(stored, "title");
    tool.description = text_field(stored, "description");
    tool.input_schema = stored.value("input_schema", json::object());
    tool.output_schema = stored.value("output_schema", json());
    if (stored.contains("read_only_hint") && stored.at("read_only_hint").is_boolean()) {
        tool.read_only_hint = stored.at("read_only_hint").get<bool>();
    }
    if (stored.contains("destructive_hint") && stored.at("destructive_hint").is_boolean()) {
        tool.destructive_hint = stored.at("destructive_hint").get<bool>();
    }
    return tool;
}

json mcp_discovery_json(const McpDiscovery& discovery) {
    json tools = json::array();
    for (const McpTool& tool : discovery.tools) {
        tools.push_back(mcp_tool_json(tool));
    }
    json resources = json::array();
    for (const McpResource& resource : discovery.resources) {
        resources.push_back({{"uri", resource.uri},
                             {"name", resource.name},
                             {"description", resource.description},
                             {"mime_type", resource.mime_type}});
    }
    json prompts = json::array();
    for (const McpPrompt& prompt : discovery.prompts) {
        prompts.push_back({{"name", prompt.name},
                           {"description", prompt.description},
                           {"arguments", prompt.arguments}});
    }
    return {{"server",
             {{"name", discovery.server.name},
              {"version", discovery.server.version},
              {"protocol_version", discovery.server.protocol_version},
              {"instructions", discovery.server.instructions},
              {"tools", discovery.server.tools},
              {"resources", discovery.server.resources},
              {"resources_subscribe", discovery.server.resources_subscribe},
              {"prompts", discovery.server.prompts},
              {"logging", discovery.server.logging}}},
            {"transport", discovery.transport},
            {"tools", std::move(tools)},
            {"resources", std::move(resources)},
            {"prompts", std::move(prompts)},
            {"skipped", discovery.skipped}};
}

std::string mcp_error_text(const McpClientError& error) {
    const std::string said = bounded(error.message, 300);
    if (error.kind == "timeout") {
        return "The MCP server did not answer in time." + (said.empty() ? "" : " " + said);
    }
    if (error.kind == "too_large") {
        return "The MCP server's answer was larger than Lake accepts." +
               (said.empty() ? "" : " " + said);
    }
    if (error.kind == "auth_required") {
        return "The MCP server asks for sign-in: its credential is missing, expired or refused.";
    }
    if (error.kind == "forbidden") {
        return "The MCP server refused the credential for this call (HTTP 403).";
    }
    if (error.kind == "unreachable") {
        return "Lake could not reach the MCP server: " + said;
    }
    if (error.kind == "unsupported_version") {
        return "The MCP server speaks a protocol version Lake does not: " + said;
    }
    if (error.kind == "protocol") {
        return "The MCP server's answer is not MCP: " + said;
    }
    if (error.rpc_code != 0) {
        return "The MCP server refused the call: " + said;
    }
    if (error.status != 0) {
        return "The MCP server answered HTTP " + std::to_string(error.status) +
               (said.empty() ? "." : ": " + said);
    }
    return said.empty() ? "The MCP server call failed." : said;
}

// ---- The client --------------------------------------------------------------------------------

struct McpClient::Legacy {
    std::unique_ptr<McpEventStream> stream;
    SseParser parser;
    std::string endpoint;
    std::deque<SseEvent> inbox;
    std::size_t received = 0;
};

McpClient::McpClient(std::shared_ptr<McpHttp> http, McpClientOptions options)
    : http_(std::move(http)), options_(std::move(options)) {}

McpClient::~McpClient() = default;

std::vector<std::pair<std::string, std::string>> McpClient::headers(bool json_body) const {
    std::vector<std::pair<std::string, std::string>> out{
        {"Accept", "application/json, text/event-stream"},
        {"User-Agent", "Lake-MCP-Client/" + options_.client_version}};
    if (json_body) {
        out.emplace_back("Content-Type", "application/json");
    }
    if (initialized_ && !protocol_version_.empty()) {
        out.emplace_back("MCP-Protocol-Version", protocol_version_);
    }
    if (!session_id_.empty()) {
        out.emplace_back("Mcp-Session-Id", session_id_);
    }
    if (!options_.credential.bearer.empty()) {
        out.emplace_back("Authorization", "Bearer " + options_.credential.bearer);
    }
    return out;
}

void McpClient::keep_notification(const json& message) {
    if (notifications_.size() >= 256) {
        return;
    }
    McpNotification notification;
    notification.method = bounded(text_field(message, "method"), 128);
    const json params = message.value("params", json::object());
    if (params.is_object() && params.dump().size() <= 64 * 1024) {
        notification.params = params;
    }
    notifications_.push_back(std::move(notification));
}

std::expected<json, McpClientError> McpClient::answer_from(const McpHttpResponse& response,
                                                           const json& id) {
    std::vector<json> messages;
    if (is_event_stream(response)) {
        SseParser parser;
        std::size_t events = 0;
        for (const SseEvent& event : parser.feed(response.body)) {
            if (++events > options_.limits.max_events) {
                break;
            }
            json parsed = json::parse(event.data, nullptr, false);
            if (!parsed.is_discarded()) {
                messages.push_back(std::move(parsed));
            }
        }
    } else {
        json parsed = json::parse(response.body, nullptr, false);
        if (parsed.is_discarded()) {
            return std::unexpected(client_error(response.truncated ? "too_large" : "protocol",
                                                "the answer is not JSON",
                                                response.status));
        }
        messages.push_back(std::move(parsed));
    }
    std::vector<json> flat;
    for (json& message : messages) {
        if (message.is_array()) {
            for (json& item : message) {
                flat.push_back(std::move(item));
            }
        } else {
            flat.push_back(std::move(message));
        }
    }
    for (const json& message : flat) {
        if (!message.is_object()) {
            continue;
        }
        const bool is_response = message.contains("result") || message.contains("error");
        if (message.contains("method")) {
            if (!message.contains("id")) {
                keep_notification(message);
            }
            // A request from the server (sampling, roots, elicitation) is never served: Lake
            // declared none of those capabilities.
            continue;
        }
        if (!is_response || message.value("id", json()) != id) {
            continue;
        }
        if (message.contains("error")) {
            const json error = message.at("error");
            McpClientError refused = client_error("server_error", text_field(error, "message"));
            if (error.is_object() && error.contains("code") &&
                error.at("code").is_number_integer()) {
                refused.rpc_code = error.at("code").get<int>();
            }
            if (refused.message.empty()) {
                refused.message = "the server answered a JSON-RPC error";
            }
            return std::unexpected(std::move(refused));
        }
        if (!message.at("result").is_object()) {
            return std::unexpected(client_error("protocol", "the result is not an object"));
        }
        return message.at("result");
    }
    if (response.truncated) {
        return std::unexpected(client_error("too_large",
                                            "more than " +
                                                std::to_string(options_.limits.max_response_bytes) +
                                                " bytes without the answer"));
    }
    if (response.timed_out) {
        return std::unexpected(client_error("timeout", "no answer within the deadline"));
    }
    return std::unexpected(
        client_error("protocol", "the server ended its answer without answering"));
}

std::expected<json, McpClientError> McpClient::send_streamable(const json& message,
                                                               bool notification) {
    McpHttpRequest request;
    request.method = "POST";
    request.url = url_.origin() + url_.path;
    request.headers = headers(true);
    request.body = message.dump();
    request.timeout = options_.limits.timeout;
    request.max_response_bytes = options_.limits.max_response_bytes;
    const json id = notification ? json() : message.at("id");
    // Stop reading once the answer to this request has arrived on an event stream.
    auto parser = std::make_shared<SseParser>();
    auto read = std::make_shared<std::size_t>(0);
    auto events = std::make_shared<std::size_t>(0);
    const std::size_t max_events = options_.limits.max_events;
    McpEnough enough = [parser, read, events, id, notification, max_events](std::string_view body) {
        if (notification || body.size() < *read) {
            return false;
        }
        const auto completed = parser->feed(body.substr(*read));
        *read = body.size();
        for (const SseEvent& event : completed) {
            if (++*events > max_events) {
                return true;
            }
            const json parsed = json::parse(event.data, nullptr, false);
            const auto answers = [&](const json& item) {
                return item.is_object() && !item.contains("method") &&
                       (item.contains("result") || item.contains("error")) &&
                       item.value("id", json()) == id;
            };
            if (answers(parsed) || (parsed.is_array() && std::ranges::any_of(parsed, answers))) {
                return true;
            }
        }
        return false;
    };
    McpHttpResponse response = http_->exchange(request, enough);
    ++traffic_.exchanges;
    traffic_.sent_bytes += request.body.size();
    traffic_.received_bytes += response.body.size();
    if (!response.error.empty() && response.status == 0) {
        return std::unexpected(
            client_error(response.timed_out ? "timeout" : "unreachable", response.error));
    }
    if (const std::string session = response.header("mcp-session-id");
        !session.empty() && message.value("method", "") == "initialize") {
        if (session.size() > 1024 ||
            !std::ranges::all_of(session, [](unsigned char c) { return c >= 0x21 && c <= 0x7E; })) {
            return std::unexpected(client_error("protocol", "the session id is not visible ASCII"));
        }
        session_id_ = session;
    }
    if (response.status == 401) {
        McpClientError refused = client_error("auth_required", "HTTP 401", 401);
        refused.resource_metadata = resource_metadata_of(response.header("www-authenticate"));
        return std::unexpected(std::move(refused));
    }
    if (response.status == 403) {
        return std::unexpected(client_error("forbidden", "HTTP 403", 403));
    }
    if (response.status == 404 && !session_id_.empty() &&
        message.value("method", "") != "initialize") {
        return std::unexpected(client_error("session_expired", "the session ended", 404));
    }
    if (notification) {
        if (response.status >= 200 && response.status < 300) {
            return json::object();
        }
        return std::unexpected(
            client_error("server_error", bounded(response.body, 300), response.status));
    }
    if (response.status < 200 || response.status >= 300) {
        return std::unexpected(
            client_error("server_error", bounded(response.body, 300), response.status));
    }
    return answer_from(response, id);
}

std::expected<void, McpClientError> McpClient::open_legacy() {
    legacy_ = std::make_unique<Legacy>();
    McpHttpRequest request;
    request.method = "GET";
    request.url = url_.origin() + url_.path;
    request.headers = headers(false);
    request.headers[0].second = "text/event-stream";
    request.timeout = options_.limits.timeout;
    request.max_response_bytes = options_.limits.max_response_bytes;
    legacy_->stream = http_->open(request);
    const McpHttpResponse& head = legacy_->stream->head();
    if (!head.error.empty() && head.status == 0) {
        return std::unexpected(
            client_error(head.timed_out ? "timeout" : "unreachable", head.error));
    }
    if (head.status == 401) {
        McpClientError refused = client_error("auth_required", "HTTP 401", 401);
        refused.resource_metadata = resource_metadata_of(head.header("www-authenticate"));
        return std::unexpected(std::move(refused));
    }
    if (head.status != 200 || !is_event_stream(head)) {
        return std::unexpected(client_error("server_error",
                                            "the server opened no event stream (HTTP " +
                                                std::to_string(head.status) + ")",
                                            head.status));
    }
    const auto deadline = Clock::now() + options_.limits.timeout;
    std::size_t events = 0;
    while (Clock::now() < deadline) {
        const auto wait =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        McpEventStream::Read read =
            legacy_->stream->read(std::min(wait, std::chrono::milliseconds(1000)));
        legacy_->received += read.bytes.size();
        traffic_.received_bytes += read.bytes.size();
        if (legacy_->received > options_.limits.max_response_bytes) {
            return std::unexpected(client_error("too_large", "the event stream named no endpoint"));
        }
        for (SseEvent& event : legacy_->parser.feed(read.bytes)) {
            if (++events > options_.limits.max_events) {
                return std::unexpected(
                    client_error("protocol", "the event stream named no endpoint"));
            }
            if (event.event == "endpoint" && legacy_->endpoint.empty()) {
                auto endpoint = resolve_mcp_url(url_, event.data);
                if (!endpoint) {
                    return std::unexpected(client_error("protocol", endpoint.error()));
                }
                legacy_->endpoint = *endpoint;
            } else {
                legacy_->inbox.push_back(std::move(event));
            }
        }
        if (!legacy_->endpoint.empty()) {
            return {};
        }
        if (read.closed) {
            return std::unexpected(
                client_error("protocol", "the event stream ended before naming its endpoint"));
        }
    }
    return std::unexpected(client_error("timeout", "the event stream named no endpoint in time"));
}

std::expected<json, McpClientError> McpClient::send_legacy(const json& message, bool notification) {
    if (!legacy_ || legacy_->endpoint.empty()) {
        return std::unexpected(client_error("protocol", "no event stream is open"));
    }
    McpHttpRequest request;
    request.method = "POST";
    request.url = legacy_->endpoint;
    request.headers = headers(true);
    request.body = message.dump();
    request.timeout = options_.limits.timeout;
    request.max_response_bytes = 64 * 1024;
    McpHttpResponse response = http_->exchange(request, {});
    ++traffic_.exchanges;
    traffic_.sent_bytes += request.body.size();
    traffic_.received_bytes += response.body.size();
    if (!response.error.empty() && response.status == 0) {
        return std::unexpected(
            client_error(response.timed_out ? "timeout" : "unreachable", response.error));
    }
    if (response.status == 401) {
        return std::unexpected(client_error("auth_required", "HTTP 401", 401));
    }
    if (response.status < 200 || response.status >= 300) {
        return std::unexpected(
            client_error("server_error", bounded(response.body, 300), response.status));
    }
    if (notification) {
        return json::object();
    }
    const json id = message.at("id");
    const auto deadline = Clock::now() + options_.limits.timeout;
    std::size_t events = 0;
    for (;;) {
        while (!legacy_->inbox.empty()) {
            SseEvent event = std::move(legacy_->inbox.front());
            legacy_->inbox.pop_front();
            if (++events > options_.limits.max_events) {
                return std::unexpected(
                    client_error("protocol", "too many events without the answer"));
            }
            McpHttpResponse one;
            one.status = 200;
            one.headers["content-type"] = "application/json";
            one.body = std::move(event.data);
            auto answered = answer_from(one, id);
            if (answered || answered.error().kind == "server_error") {
                return answered;
            }
        }
        if (Clock::now() >= deadline) {
            return std::unexpected(client_error("timeout", "no answer within the deadline"));
        }
        const auto wait =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        McpEventStream::Read read =
            legacy_->stream->read(std::min(wait, std::chrono::milliseconds(1000)));
        legacy_->received += read.bytes.size();
        traffic_.received_bytes += read.bytes.size();
        if (legacy_->received > options_.limits.max_response_bytes) {
            return std::unexpected(
                client_error("too_large", "the event stream passed its byte limit"));
        }
        for (SseEvent& event : legacy_->parser.feed(read.bytes)) {
            legacy_->inbox.push_back(std::move(event));
        }
        if (read.closed && legacy_->inbox.empty()) {
            return std::unexpected(client_error(
                "unreachable", read.error.empty() ? "the event stream ended" : read.error));
        }
    }
}

std::expected<json, McpClientError> McpClient::send(const std::string& method, json params) {
    const bool notification = method.starts_with("notifications/");
    json message{{"jsonrpc", "2.0"}, {"method", method}};
    if (!params.is_null()) {
        message["params"] = std::move(params);
    }
    if (!notification) {
        message["id"] = next_id_++;
    }
    return transport_ == "sse" ? send_legacy(message, notification)
                               : send_streamable(message, notification);
}

std::expected<McpServerInfo, McpClientError>
McpClient::initialize_on(const std::string& transport) {
    transport_ = transport;
    session_id_.clear();
    protocol_version_.clear();
    initialized_ = false;
    if (transport == "sse") {
        if (auto opened = open_legacy(); !opened) {
            return std::unexpected(opened.error());
        }
    }
    auto answered =
        send("initialize",
             {{"protocolVersion", std::string(mcp_client_protocol_version)},
              {"capabilities", json::object()},
              {"clientInfo", {{"name", "lake"}, {"version", options_.client_version}}}});
    if (!answered) {
        return std::unexpected(answered.error());
    }
    McpServerInfo info;
    info.protocol_version = text_field(*answered, "protocolVersion");
    if (!mcp_supported_protocol_version(info.protocol_version)) {
        return std::unexpected(
            client_error("unsupported_version", "'" + bounded(info.protocol_version, 40) + "'"));
    }
    const json server = answered->value("serverInfo", json::object());
    info.name = bounded(text_field(server, "name"), 128);
    info.version = bounded(text_field(server, "version"), 64);
    info.instructions =
        bounded(text_field(*answered, "instructions"), options_.limits.max_description_bytes);
    const json capabilities = answered->value("capabilities", json::object());
    if (capabilities.is_object()) {
        info.tools = capabilities.contains("tools");
        info.prompts = capabilities.contains("prompts");
        info.logging = capabilities.contains("logging");
        if (capabilities.contains("resources")) {
            info.resources = true;
            const json resources = capabilities.at("resources");
            info.resources_subscribe =
                resources.is_object() && resources.value("subscribe", json(false)) == json(true);
        }
    }
    protocol_version_ = info.protocol_version;
    initialized_ = true;
    if (auto sent = send("notifications/initialized", json()); !sent) {
        return std::unexpected(sent.error());
    }
    server_ = info;
    return info;
}

std::expected<McpServerInfo, McpClientError> McpClient::initialize() {
    if (initialized_ && server_) {
        return *server_;
    }
    auto parsed = parse_mcp_url(options_.url, options_.allow_private_addresses);
    if (!parsed) {
        return std::unexpected(client_error("unreachable", parsed.error()));
    }
    url_ = std::move(*parsed);
    if (options_.transport == "sse") {
        return initialize_on("sse");
    }
    auto streamable = initialize_on("streamable_http");
    if (streamable || options_.transport == "streamable_http") {
        return streamable;
    }
    const int status = streamable.error().status;
    if (streamable.error().kind == "server_error" && streamable.error().rpc_code == 0 &&
        (status == 400 || status == 404 || status == 405)) {
        return initialize_on("sse");
    }
    return streamable;
}

std::expected<json, McpClientError> McpClient::request(const std::string& method, json params) {
    if (auto ready = initialize(); !ready) {
        return std::unexpected(ready.error());
    }
    auto answered = send(method, params);
    if (!answered && answered.error().kind == "session_expired") {
        initialized_ = false;
        if (auto again = initialize_on(transport_); !again) {
            return std::unexpected(again.error());
        }
        answered = send(method, std::move(params));
    }
    return answered;
}

std::expected<std::vector<McpTool>, McpClientError>
McpClient::list_tools(std::vector<std::string>& skipped) {
    std::vector<McpTool> tools;
    std::set<std::string> seen;
    std::string cursor;
    std::size_t budget = 0;
    for (std::size_t page = 0; page < options_.limits.max_pages; ++page) {
        json params = json::object();
        if (!cursor.empty()) {
            params["cursor"] = cursor;
        }
        auto answered = request("tools/list", std::move(params));
        if (!answered) {
            return std::unexpected(answered.error());
        }
        const json listed = answered->value("tools", json());
        if (!listed.is_array()) {
            return std::unexpected(client_error("protocol", "tools/list answered no tools array"));
        }
        for (const json& entry : listed) {
            auto tool = mcp_tool_from(entry, options_.limits);
            if (!tool) {
                skipped.push_back(tool.error());
                continue;
            }
            if (!seen.insert(tool->name).second) {
                skipped.push_back("tool '" + tool->name + "': listed twice");
                continue;
            }
            const std::size_t size = mcp_tool_json(*tool).dump().size();
            if (tools.size() >= options_.limits.max_tools ||
                budget + size > options_.limits.max_discovery_bytes) {
                skipped.push_back("tool '" + tool->name + "': past the " +
                                  std::to_string(options_.limits.max_tools) +
                                  " tools or the size Lake keeps for one connection");
                continue;
            }
            budget += size;
            tools.push_back(std::move(*tool));
        }
        cursor = text_field(*answered, "nextCursor");
        if (cursor.empty() || cursor.size() > 1024) {
            break;
        }
    }
    return tools;
}

std::expected<McpDiscovery, McpClientError> McpClient::discover() {
    McpDiscovery discovery;
    auto info = initialize();
    if (!info) {
        return std::unexpected(info.error());
    }
    discovery.server = *info;
    discovery.transport = transport_;
    if (info->tools) {
        auto tools = list_tools(discovery.skipped);
        if (!tools) {
            return std::unexpected(tools.error());
        }
        discovery.tools = std::move(*tools);
    }
    const auto pages =
        [&](const char* method,
            const char* key,
            const std::function<void(const json&)>& each) -> std::expected<void, McpClientError> {
        std::string cursor;
        for (std::size_t page = 0; page < options_.limits.max_pages; ++page) {
            json params = json::object();
            if (!cursor.empty()) {
                params["cursor"] = cursor;
            }
            auto answered = request(method, std::move(params));
            if (!answered) {
                return std::unexpected(answered.error());
            }
            const json listed = answered->value(key, json());
            if (!listed.is_array()) {
                return std::unexpected(
                    client_error("protocol", std::string(method) + " answered no list"));
            }
            for (const json& entry : listed) {
                each(entry);
            }
            cursor = text_field(*answered, "nextCursor");
            if (cursor.empty() || cursor.size() > 1024) {
                break;
            }
        }
        return {};
    };
    if (info->resources) {
        auto listed = pages("resources/list", "resources", [&](const json& entry) {
            const std::string uri = text_field(entry, "uri");
            if (uri.empty() || uri.size() > 1024 ||
                discovery.resources.size() >= options_.limits.max_resources) {
                return;
            }
            discovery.resources.push_back({uri,
                                           bounded(text_field(entry, "name"), 256),
                                           bounded(text_field(entry, "description"), 512),
                                           bounded(text_field(entry, "mimeType"), 128)});
        });
        // A server that declares resources but fails to list them still offers its tools.
        if (!listed) {
            discovery.skipped.push_back("resources: " + mcp_error_text(listed.error()));
        }
    }
    if (info->prompts) {
        auto listed = pages("prompts/list", "prompts", [&](const json& entry) {
            const std::string name = text_field(entry, "name");
            if (name.empty() || name.size() > 256 ||
                discovery.prompts.size() >= options_.limits.max_prompts) {
                return;
            }
            json arguments = entry.value("arguments", json::array());
            if (!arguments.is_array() || arguments.dump().size() > 8192) {
                arguments = json::array();
            }
            discovery.prompts.push_back(
                {name, bounded(text_field(entry, "description"), 512), std::move(arguments)});
        });
        if (!listed) {
            discovery.skipped.push_back("prompts: " + mcp_error_text(listed.error()));
        }
    }
    return discovery;
}

std::expected<json, McpClientError> McpClient::call_tool(const std::string& name,
                                                         const json& arguments) {
    return request(
        "tools/call",
        {{"name", name}, {"arguments", arguments.is_object() ? arguments : json::object()}});
}

std::expected<json, McpClientError> McpClient::read_resource(const std::string& uri) {
    return request("resources/read", {{"uri", uri}});
}

std::expected<json, McpClientError> McpClient::get_prompt(const std::string& name,
                                                          const json& arguments) {
    return request("prompts/get", {{"name", name}, {"arguments", arguments}});
}

std::expected<void, McpClientError> McpClient::subscribe_resource(const std::string& uri) {
    auto answered = request("resources/subscribe", {{"uri", uri}});
    if (!answered) {
        return std::unexpected(answered.error());
    }
    return {};
}

std::vector<McpNotification> McpClient::take_notifications() {
    std::vector<McpNotification> out;
    out.swap(notifications_);
    return out;
}

std::expected<std::vector<McpNotification>, McpClientError>
McpClient::listen(std::chrono::milliseconds wait) {
    if (auto ready = initialize(); !ready) {
        return std::unexpected(ready.error());
    }
    McpEventStream* stream = nullptr;
    SseParser* parser = nullptr;
    if (transport_ == "sse") {
        stream = legacy_->stream.get();
        parser = &legacy_->parser;
        for (SseEvent& event : legacy_->inbox) {
            json parsed = json::parse(event.data, nullptr, false);
            if (parsed.is_object() && parsed.contains("method") && !parsed.contains("id")) {
                keep_notification(parsed);
            }
        }
        legacy_->inbox.clear();
    } else {
        if (!listening_) {
            McpHttpRequest request;
            request.method = "GET";
            request.url = url_.origin() + url_.path;
            request.headers = headers(false);
            request.headers[0].second = "text/event-stream";
            request.timeout = options_.limits.timeout;
            request.max_response_bytes = options_.limits.max_response_bytes;
            listening_ = http_->open(request);
            listening_parser_ = SseParser{};
            const McpHttpResponse& head = listening_->head();
            if (!head.error.empty() && head.status == 0) {
                listening_.reset();
                return std::unexpected(
                    client_error(head.timed_out ? "timeout" : "unreachable", head.error));
            }
            if (head.status == 405) {
                listening_.reset();
                return std::unexpected(client_error(
                    "not_supported", "the server offers no stream of notifications", 405));
            }
            if (head.status == 404 && !session_id_.empty()) {
                listening_.reset();
                initialized_ = false;
                return std::unexpected(client_error("session_expired", "the session ended", 404));
            }
            if (head.status != 200 || !is_event_stream(head)) {
                const int status = head.status;
                listening_.reset();
                return std::unexpected(client_error("server_error",
                                                    "the server opened no event stream (HTTP " +
                                                        std::to_string(status) + ")",
                                                    status));
            }
        }
        stream = listening_.get();
        parser = &listening_parser_;
    }
    const auto deadline = Clock::now() + wait;
    do {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        McpEventStream::Read read = stream->read(std::max(
            std::chrono::milliseconds(1), std::min(left, std::chrono::milliseconds(1000))));
        traffic_.received_bytes += read.bytes.size();
        for (const SseEvent& event : parser->feed(read.bytes)) {
            json parsed = json::parse(event.data, nullptr, false);
            if (parsed.is_object() && parsed.contains("method") && !parsed.contains("id")) {
                keep_notification(parsed);
            }
        }
        if (parser->pending() > options_.limits.max_response_bytes) {
            return std::unexpected(client_error("too_large", "an event passed the byte limit"));
        }
        if (read.closed) {
            if (transport_ == "sse") {
                legacy_.reset();
                initialized_ = false;
            } else {
                listening_.reset();
            }
            if (!notifications_.empty()) {
                break;
            }
            return std::unexpected(
                client_error("unreachable", read.error.empty() ? "the stream ended" : read.error));
        }
        if (!notifications_.empty()) {
            break;
        }
    } while (Clock::now() < deadline);
    return take_notifications();
}

void McpClient::close() {
    listening_.reset();
    if (transport_ == "sse") {
        legacy_.reset();
    } else if (!session_id_.empty() && initialized_) {
        McpHttpRequest request;
        request.method = "DELETE";
        request.url = url_.origin() + url_.path;
        request.headers = headers(false);
        request.timeout = std::min(options_.limits.timeout, std::chrono::milliseconds(5000));
        request.max_response_bytes = 4096;
        const McpHttpResponse response = http_->exchange(request, {});
        traffic_.sent_bytes += 0;
        traffic_.received_bytes += response.body.size();
    }
    session_id_.clear();
    initialized_ = false;
    server_.reset();
}

} // namespace lake::api
