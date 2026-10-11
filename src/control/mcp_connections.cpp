#include "lake/control/mcp_connections.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include "lake/control/store.hpp"

namespace lake::control {
namespace {

using json = nlohmann::json;

template <class T>
ZettaControlResult<T> refused(std::string code, std::string message, std::string path = {}) {
    return std::unexpected(ZettaControlError{std::move(code), std::move(message), std::move(path)});
}

std::string text(const json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}

std::string folded(std::string_view value) {
    std::string out(value);
    std::ranges::transform(
        out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

struct Read {
    McpConnection connection;
    std::int64_t revision = 0;
};

liquid::runtime::task<ZettaControlResult<Read>>
read_own(WorkspaceRecords& records, const std::string& customer_id, const std::string& id) {
    auto got = settled(co_await records.get(McpConnectionCodec::collection, id));
    if (!got && got.error().code != "invalid_input") {
        co_return ZettaControlResult<Read>(std::unexpected(got.error()));
    }
    std::optional<McpConnection> decoded;
    std::int64_t revision = 0;
    if (got && *got) {
        decoded = McpConnectionCodec::decode(**got);
        revision = (*got)->revision;
    }
    if (!decoded || decoded->customer_id != customer_id) {
        co_return refused<Read>("not_found", "no such MCP connection in this workspace", "/id");
    }
    co_return ZettaControlResult<Read>(Read{std::move(*decoded), revision});
}

} // namespace

std::string mcp_connections_lookup(const std::string& customer_id) {
    return "customer:" + customer_id;
}

std::string mcp_connection_prefix(std::string_view name) {
    std::string out;
    bool gap = false;
    for (const unsigned char c : name) {
        if (std::isalnum(c) != 0) {
            if (gap && !out.empty()) {
                out.push_back('_');
            }
            gap = false;
            if (out.empty() && std::isdigit(c) != 0) {
                continue; // a prefix starts with a letter
            }
            out.push_back(static_cast<char>(std::tolower(c)));
        } else {
            gap = true;
        }
        if (out.size() >= 24) {
            break;
        }
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    return out;
}

std::string mcp_exported_tool_name(const std::string& prefix, const std::string& tool) {
    return prefix + "__" + tool;
}

bool known_mcp_transport(const std::string& transport) {
    return transport == "auto" || transport == "streamable_http" || transport == "sse";
}

bool known_mcp_auth_kind(const std::string& kind) {
    return kind == "none" || kind == "bearer" || kind == "oauth";
}

std::optional<std::string> mcp_connection_problem(const McpConnection& connection) {
    if (connection.customer_id.empty() || connection.name.empty() || connection.prefix.empty() ||
        connection.url.empty() || !known_mcp_transport(connection.transport) ||
        !known_mcp_auth_kind(connection.auth_kind)) {
        return "MCP connection is invalid";
    }
    if (connection.auth_kind == "bearer" && connection.secret_ref.empty()) {
        return "a bearer connection names the secret holding its token";
    }
    return std::nullopt;
}

std::string McpConnectionCodec::lookup(const McpConnection& connection) {
    return mcp_connections_lookup(connection.customer_id);
}

json McpConnectionCodec::encode(const McpConnection& connection) {
    json tools = json::object();
    for (const auto& [name, setting] : connection.tools) {
        tools[name] = {{"allowed", setting.allowed}, {"change", setting.change}};
    }
    json grants = json::array();
    for (const McpAutoApprove& grant : connection.auto_approve) {
        grants.push_back({{"tool", grant.tool},
                          {"agent", grant.agent},
                          {"granted_by", grant.granted_by},
                          {"granted_at", grant.granted_at}});
    }
    return {{"customer_id", connection.customer_id},
            {"name", connection.name},
            {"prefix", connection.prefix},
            {"url", connection.url},
            {"transport", connection.transport},
            {"auth_kind", connection.auth_kind},
            {"secret_ref", connection.secret_ref},
            {"oauth", connection.oauth},
            {"discovery", connection.discovery},
            {"tools", std::move(tools)},
            {"exported", connection.exported},
            {"exported_by", connection.exported_by},
            {"auto_approve", std::move(grants)},
            {"agent_id", connection.agent_id},
            {"input_topic", connection.input_topic},
            {"output_topic", connection.output_topic},
            {"notifications_topic", connection.notifications_topic},
            {"subscribe_resources", connection.subscribe_resources},
            {"streams_set_by", connection.streams_set_by},
            {"input_cursor", connection.input_cursor},
            {"pending_launches", connection.pending_launches},
            {"status", connection.status},
            {"last_error", connection.last_error},
            {"refreshed_at", connection.refreshed_at},
            {"created_by", connection.created_by}};
}

std::optional<McpConnection> McpConnectionCodec::decode(const StoredRecord& record) {
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    const json& body = record.body;
    McpConnection connection;
    connection.id = record.id;
    connection.customer_id = text(body, "customer_id");
    connection.name = text(body, "name");
    connection.prefix = text(body, "prefix");
    connection.url = text(body, "url");
    connection.transport = text(body, "transport");
    connection.auth_kind = text(body, "auth_kind");
    connection.secret_ref = text(body, "secret_ref");
    connection.oauth = body.value("oauth", json::object());
    connection.discovery = body.value("discovery", json::object());
    for (const auto& [name, setting] : body.value("tools", json::object()).items()) {
        if (setting.is_object()) {
            connection.tools[name] = {setting.value("allowed", false),
                                      setting.value("change", true)};
        }
    }
    connection.exported = body.value("exported", false);
    connection.exported_by = text(body, "exported_by");
    for (const json& grant : body.value("auto_approve", json::array())) {
        if (grant.is_object()) {
            connection.auto_approve.push_back({text(grant, "tool"),
                                               text(grant, "agent"),
                                               text(grant, "granted_by"),
                                               text(grant, "granted_at")});
        }
    }
    connection.agent_id = text(body, "agent_id");
    connection.input_topic = text(body, "input_topic");
    connection.output_topic = text(body, "output_topic");
    connection.notifications_topic = text(body, "notifications_topic");
    for (const json& uri : body.value("subscribe_resources", json::array())) {
        if (uri.is_string()) {
            connection.subscribe_resources.push_back(uri.get<std::string>());
        }
    }
    connection.streams_set_by = text(body, "streams_set_by");
    if (body.contains("input_cursor") && body.at("input_cursor").is_number_integer()) {
        connection.input_cursor = body.at("input_cursor").get<std::uint64_t>();
    }
    for (const json& launch : body.value("pending_launches", json::array())) {
        if (launch.is_string()) {
            connection.pending_launches.push_back(launch.get<std::string>());
        }
    }
    connection.status = text(body, "status");
    connection.last_error = text(body, "last_error");
    connection.refreshed_at = text(body, "refreshed_at");
    connection.created_by = text(body, "created_by");
    connection.created_at = record_instant(record.at_us);
    connection.at_us = record.at_us;
    connection.lake_revision = record.lake_revision;
    if (mcp_connection_problem(connection)) {
        return std::nullopt;
    }
    return connection;
}

liquid::runtime::task<ZettaControlResult<std::vector<McpConnection>>>
list_mcp_connections(WorkspaceRecords& records, std::string customer_id) {
    RecordSet<McpConnectionCodec> set(records);
    auto listed = settled(co_await set.list(mcp_connections_lookup(customer_id)));
    if (!listed) {
        co_return ZettaControlResult<std::vector<McpConnection>>(std::unexpected(listed.error()));
    }
    std::erase_if(*listed,
                  [&](const McpConnection& row) { return row.customer_id != customer_id; });
    std::ranges::sort(*listed, [](const McpConnection& a, const McpConnection& b) {
        return folded(a.name) != folded(b.name) ? folded(a.name) < folded(b.name) : a.id < b.id;
    });
    co_return ZettaControlResult<std::vector<McpConnection>>(std::move(*listed));
}

liquid::runtime::task<ZettaControlResult<std::optional<McpConnection>>>
get_mcp_connection(WorkspaceRecords& records, std::string customer_id, std::string id) {
    using Found = std::optional<McpConnection>;
    auto read = settled(co_await read_own(records, customer_id, id));
    if (!read) {
        if (read.error().code == "not_found") {
            co_return ZettaControlResult<Found>(Found{});
        }
        co_return ZettaControlResult<Found>(std::unexpected(read.error()));
    }
    co_return ZettaControlResult<Found>(std::move(read->connection));
}

liquid::runtime::task<ZettaControlResult<McpConnection>>
record_mcp_connection(WorkspaceRecords& records, McpConnection connection) {
    if (auto problem = mcp_connection_problem(connection)) {
        co_return refused<McpConnection>("invalid_input", *problem);
    }
    auto existing = settled(co_await list_mcp_connections(records, connection.customer_id));
    if (!existing) {
        co_return ZettaControlResult<McpConnection>(std::unexpected(existing.error()));
    }
    for (const McpConnection& other : *existing) {
        if (folded(other.name) == folded(connection.name)) {
            co_return refused<McpConnection>("conflict",
                                             "this workspace already has an MCP connection named " +
                                                 other.name,
                                             "/name");
        }
        if (other.prefix == connection.prefix) {
            co_return refused<McpConnection>("conflict",
                                             "the name gives the tool prefix '" + other.prefix +
                                                 "', which " + other.name +
                                                 " already uses; choose another name",
                                             "/name");
        }
    }
    RecordSet<McpConnectionCodec> set(records);
    co_return settled(co_await set.append(std::move(connection)));
}

liquid::runtime::task<ZettaControlResult<McpConnection>>
change_mcp_connection(WorkspaceRecords& records,
                      std::string customer_id,
                      std::string id,
                      McpConnectionChange change) {
    ZettaControlError last{"conflict", "the MCP connection kept changing; try again"};
    for (int attempt = 0; attempt < 5; ++attempt) {
        auto read = settled(co_await read_own(records, customer_id, id));
        if (!read) {
            co_return ZettaControlResult<McpConnection>(std::unexpected(read.error()));
        }
        McpConnection next = read->connection;
        if (auto why = change(next)) {
            co_return ZettaControlResult<McpConnection>(std::unexpected(*why));
        }
        // Its space and id never change, so the write keeps its key and lookup.
        next.customer_id = read->connection.customer_id;
        next.id = read->connection.id;
        if (auto problem = mcp_connection_problem(next)) {
            co_return refused<McpConnection>("invalid_input", *problem);
        }
        auto written = settled(co_await records.put(McpConnectionCodec::collection,
                                                    id,
                                                    McpConnectionCodec::lookup(next),
                                                    McpConnectionCodec::encode(next),
                                                    RecordCondition{.if_revision = read->revision},
                                                    next.lake_revision + 1,
                                                    next.at_us));
        if (written) {
            auto decoded = McpConnectionCodec::decode(*written);
            if (!decoded) {
                co_return refused<McpConnection>("store_error", "the stored record did not decode");
            }
            co_return ZettaControlResult<McpConnection>(std::move(*decoded));
        }
        if (written.error().code != "conflict") {
            co_return ZettaControlResult<McpConnection>(std::unexpected(written.error()));
        }
        last = written.error();
    }
    co_return ZettaControlResult<McpConnection>(std::unexpected(last));
}

liquid::runtime::task<ZettaControlResult<void>>
remove_mcp_connection(WorkspaceRecords& records, std::string customer_id, std::string id) {
    ZettaControlError last{"conflict", "the MCP connection kept changing; try again"};
    for (int attempt = 0; attempt < 5; ++attempt) {
        auto read = settled(co_await read_own(records, customer_id, id));
        if (!read) {
            co_return ZettaControlResult<void>(std::unexpected(read.error()));
        }
        auto removed =
            settled(co_await records.remove(McpConnectionCodec::collection, id, read->revision));
        if (removed || removed.error().code == "not_found") {
            co_return ZettaControlResult<void>();
        }
        if (removed.error().code != "conflict") {
            co_return ZettaControlResult<void>(std::unexpected(removed.error()));
        }
        last = removed.error();
    }
    co_return ZettaControlResult<void>(std::unexpected(last));
}

std::string McpOAuthStateCodec::lookup(const McpOAuthState& state) {
    return mcp_connections_lookup(state.customer_id);
}

json McpOAuthStateCodec::encode(const McpOAuthState& state) {
    return {{"customer_id", state.customer_id},
            {"connection_id", state.connection_id},
            {"verifier", state.verifier},
            {"redirect_uri", state.redirect_uri},
            {"client_id", state.client_id},
            {"client_secret", state.client_secret},
            {"mount_id", state.mount_id},
            {"secret_name", state.secret_name},
            {"started_by", state.started_by}};
}

std::optional<McpOAuthState> McpOAuthStateCodec::decode(const StoredRecord& record) {
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    McpOAuthState state;
    state.state = record.id;
    state.customer_id = text(record.body, "customer_id");
    state.connection_id = text(record.body, "connection_id");
    state.verifier = text(record.body, "verifier");
    state.redirect_uri = text(record.body, "redirect_uri");
    state.client_id = text(record.body, "client_id");
    state.client_secret = text(record.body, "client_secret");
    state.mount_id = text(record.body, "mount_id");
    state.secret_name = text(record.body, "secret_name");
    state.started_by = text(record.body, "started_by");
    if (state.customer_id.empty() || state.connection_id.empty() || state.verifier.empty()) {
        return std::nullopt;
    }
    return state;
}

liquid::runtime::task<ZettaControlResult<void>> record_mcp_oauth_state(WorkspaceRecords& records,
                                                                       McpOAuthState state) {
    const std::string id = state.state;
    auto stored = settled(co_await records.append(McpOAuthStateCodec::collection,
                                                  McpOAuthStateCodec::lookup(state),
                                                  McpOAuthStateCodec::encode(state),
                                                  RecordWrite{.id = id}));
    if (!stored) {
        co_return ZettaControlResult<void>(std::unexpected(stored.error()));
    }
    co_return ZettaControlResult<void>();
}

liquid::runtime::task<ZettaControlResult<McpOAuthState>>
take_mcp_oauth_state(WorkspaceRecords& records, std::string customer_id, std::string state) {
    const auto unknown = [] {
        return refused<McpOAuthState>(
            "not_found",
            "that sign-in is unknown, finished or older than ten minutes; start again",
            "/state");
    };
    auto got = settled(co_await records.get(McpOAuthStateCodec::collection, state));
    if (!got || !*got) {
        if (!got && got.error().code != "invalid_input" && got.error().code != "not_found") {
            co_return ZettaControlResult<McpOAuthState>(std::unexpected(got.error()));
        }
        co_return unknown();
    }
    auto decoded = McpOAuthStateCodec::decode(**got);
    constexpr std::int64_t ten_minutes_us = 10LL * 60 * 1'000'000;
    const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    if (!decoded || decoded->customer_id != customer_id ||
        now_us - (*got)->at_us > ten_minutes_us) {
        co_return unknown();
    }
    // Taken once: the delete at the revision read is what claims it.
    auto removed =
        settled(co_await records.remove(McpOAuthStateCodec::collection, state, (*got)->revision));
    if (!removed) {
        co_return unknown();
    }
    co_return ZettaControlResult<McpOAuthState>(std::move(*decoded));
}

} // namespace lake::control
