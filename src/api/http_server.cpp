// lake — the HTTP adapter. Beast2 (HTTP/1) on Capy + Corosio, the same stack
// Zetta serves with (zetta/src/core/api/http_server.cpp). This is the ONE
// translation unit that knows about beast2 / boost::http / Boost.JSON. It
// serves the hello page, the registry's REST rendering
// (/v1/<capability>), the OpenAPI document and MCP (POST /mcp); every
// capability call goes through Registry::call, so REST and MCP authorize and
// validate identically.

#include "lake/api/server.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/beast2/http_server.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/io/push_to.hpp>
#include <boost/capy/timeout.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/corosio/tls_context.hpp>
#include <boost/corosio/tls_stream.hpp>
#include <boost/http/field.hpp>
#include <boost/http/json/json_sink.hpp>
#include <boost/http/request.hpp>
#include <boost/http/request_parser.hpp>
#include <boost/http/response_parser.hpp>
#include <boost/http/serializer.hpp>
#include <boost/http/server/cors.hpp>
#include <boost/http/server/router.hpp>
#include <boost/json.hpp>
#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/mcp.hpp"
#include "lake/api/oauth.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/rest.hpp"
#include "lake/api/sync_store.hpp"
#include "lake/api/web_ui.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/store.hpp"
#include "lake/search/index.hpp"
#include "lake/telemetry/metrics.hpp"
#include "lake/version.hpp"

#include <liquid/hash.hpp>
#include <liquid/runtime.hpp>
#include <liquid/store.hpp>

namespace lake::api {
namespace {

// Beast2 computes the framing from the payload size we declare; anything a
// handler set by hand would race it. Same rule as Zetta.
void stamp_response_headers(boost::http::route_params& route, std::string_view body) {
    route.res.erase(boost::http::field::etag);
    route.res.erase(boost::http::field::content_length);
    route.res.set_payload_size(body.size());
}

boost::http::route_task send_text(boost::http::route_params& route,
                                  boost::http::status status,
                                  std::string body,
                                  std::string_view content_type,
                                  std::string_view cache_control) {
    namespace http = boost::http;
    route.status(status);
    route.res.set(http::field::content_type, content_type);
    route.res.set(http::field::cache_control, cache_control);
    stamp_response_headers(route, body);
    auto [send_error] = co_await route.send(body);
    if (send_error)
        co_return http::route_error(send_error);
    co_return http::route_done;
}

// The bearer presented on a request, or empty ("Bearer <token>" only).
std::string bearer_of(const boost::http::route_params& route) {
    if (route.req.count(boost::http::field::authorization) == 0)
        return {};
    std::string_view header_value = route.req.at(boost::http::field::authorization);
    if (!header_value.starts_with("Bearer "))
        return {};
    return std::string(header_value.substr(7));
}

bool constant_time_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

// A capy WriteSink that collects a request body into a std::string — for the
// form-urlencoded bodies the server-rendered sign-in posts (json_sink parses
// JSON, which form bodies are not).
struct StringBodySink {
    std::string data;
    template <boost::capy::ConstBufferSequence CB>
    boost::capy::immediate<boost::capy::io_result<std::size_t>> write_some(const CB& buffers) {
        std::size_t total = 0;
        for (auto it = boost::capy::begin(buffers); it != boost::capy::end(buffers); ++it) {
            const boost::capy::const_buffer buffer(*it);
            data.append(static_cast<const char*>(buffer.data()), buffer.size());
            total += buffer.size();
        }
        return {{{}, total}};
    }
    template <boost::capy::ConstBufferSequence CB>
    boost::capy::immediate<boost::capy::io_result<std::size_t>> write(const CB& buffers) {
        return write_some(buffers);
    }
    boost::capy::immediate<boost::capy::io_result<>> write_eof() {
        return {{}};
    }
};

boost::capy::io_task<std::string> read_string_body(boost::http::route_params& route) {
    StringBodySink sink;
    auto [read_error, bytes_read] = co_await boost::capy::push_to(route.req_body, sink);
    static_cast<void>(bytes_read);
    if (read_error) {
        co_return {read_error, {}};
    }
    co_return {{}, std::move(sink.data)};
}

std::string cookie_value(const boost::http::route_params& route, std::string_view name) {
    if (route.req.count(boost::http::field::cookie) == 0)
        return {};
    std::string_view header = route.req.at(boost::http::field::cookie);
    std::size_t pos = 0;
    while (pos < header.size()) {
        const std::size_t semi = header.find(';', pos);
        std::string_view pair = header.substr(
            pos, semi == std::string_view::npos ? std::string_view::npos : semi - pos);
        while (!pair.empty() && pair.front() == ' ')
            pair.remove_prefix(1);
        const std::size_t eq = pair.find('=');
        if (eq != std::string_view::npos && pair.substr(0, eq) == name)
            return std::string(pair.substr(eq + 1));
        if (semi == std::string_view::npos)
            break;
        pos = semi + 1;
    }
    return {};
}

// Percent-decode an application/x-www-form-urlencoded token ('+' → space, %XX →
// byte).
std::string form_decode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out += ' ';
        } else if (in[i] == '%' && i + 2 < in.size() && hex(in[i + 1]) >= 0 &&
                   hex(in[i + 2]) >= 0) {
            out += static_cast<char>((hex(in[i + 1]) << 4) | hex(in[i + 2]));
            i += 2;
        } else {
            out += in[i];
        }
    }
    return out;
}

std::string form_field(std::string_view body, std::string_view name) {
    std::size_t pos = 0;
    while (pos < body.size()) {
        const std::size_t amp = body.find('&', pos);
        std::string_view pair =
            body.substr(pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);
        const std::size_t eq = pair.find('=');
        if (eq != std::string_view::npos && form_decode(pair.substr(0, eq)) == name)
            return form_decode(pair.substr(eq + 1));
        if (amp == std::string_view::npos)
            break;
        pos = amp + 1;
    }
    return {};
}

bool request_is_https(const boost::http::route_params& route) {
    return route.req.count("x-forwarded-proto") != 0 &&
           std::string_view(route.req.at("x-forwarded-proto")) == "https";
}

std::string query_value(const boost::http::route_params& route, std::string_view name) {
    for (const auto& parameter : route.url.params()) {
        if (parameter.key == name)
            return std::string(parameter.value);
    }
    return {};
}

std::string browser_session_cookie(const boost::http::route_params& route,
                                   std::string_view secret) {
    std::string cookie = "lake_session=" + std::string(secret) + "; HttpOnly; SameSite=Lax; Path=/";
    if (request_is_https(route)) {
        cookie += "; Secure";
    }
    return cookie;
}

std::vector<std::string> comma_separated(std::string_view value) {
    std::vector<std::string> result;
    std::size_t position = 0;
    while (position <= value.size()) {
        const std::size_t comma = value.find(',', position);
        std::string item(value.substr(
            position, comma == std::string_view::npos ? std::string_view::npos : comma - position));
        const std::size_t first = item.find_first_not_of(" \t\r\n");
        const std::size_t last = item.find_last_not_of(" \t\r\n");
        if (first != std::string::npos) {
            result.push_back(item.substr(first, last - first + 1));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        position = comma + 1;
    }
    return result;
}

nlohmann::json to_nlohmann(const boost::json::value& value) {
    return nlohmann::json::parse(boost::json::serialize(value), nullptr, false);
};

std::string error_body(std::string_view code, std::string_view message) {
    return nlohmann::json{{"ok", false},
                          {"error", {{"code", code}, {"message", message}, {"path", ""}}}}
        .dump();
}

std::string log_timestamp() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%FT%T}Z", now);
}

std::string json_string(const nlohmann::json& value, std::string_view key) {
    const auto field = value.find(key);
    if (field == value.end() || field->is_null()) {
        return {};
    }
    return field->get<std::string>();
}

control::Organization organization_from_json(const nlohmann::json& value) {
    return {json_string(value, "id"),
            json_string(value, "taap_id"),
            json_string(value, "display_name"),
            json_string(value, "created_at"),
            json_string(value, "updated_at")};
}

control::SsoProvider sso_provider_from_json(const nlohmann::json& value) {
    return {json_string(value, "id"),
            json_string(value, "organization_id"),
            json_string(value, "pool_id"),
            json_string(value, "name"),
            json_string(value, "protocol"),
            json_string(value, "issuer"),
            json_string(value, "metadata"),
            json_string(value, "signing_certificate"),
            json_string(value, "client_id"),
            value.value("attribute_mapping", nlohmann::json::object()),
            value.value("jit_provisioning", false),
            value.value("enabled", false),
            json_string(value, "tested_at"),
            json_string(value, "created_at"),
            json_string(value, "updated_at")};
}

control::FederationIssuer federation_issuer_from_json(const nlohmann::json& value) {
    return {json_string(value, "id"),
            json_string(value, "organization_id"),
            json_string(value, "issuer"),
            json_string(value, "claim"),
            json_string(value, "created_at")};
}

control::ProvisioningLog provisioning_log_from_json(const nlohmann::json& value) {
    return {json_string(value, "id"),
            json_string(value, "provider_id"),
            json_string(value, "resource"),
            json_string(value, "operation"),
            json_string(value, "external_id"),
            json_string(value, "outcome"),
            value.value("details", nlohmann::json::object()),
            json_string(value, "created_at")};
}

control::Customer customer_from_json(const nlohmann::json& value) {
    return {json_string(value, "id"),
            json_string(value, "customer_type"),
            json_string(value, "user_id"),
            json_string(value, "organization_id"),
            json_string(value, "name"),
            json_string(value, "created_at"),
            json_string(value, "updated_at")};
}

control::Repository repository_from_json(const nlohmann::json& value) {
    return {json_string(value, "id"),
            json_string(value, "customer_id"),
            json_string(value, "name"),
            json_string(value, "description"),
            json_string(value, "visibility"),
            json_string(value, "created_at"),
            json_string(value, "updated_at")};
}

control::Team team_from_json(const nlohmann::json& value) {
    return {json_string(value, "id"),
            json_string(value, "organization_id"),
            json_string(value, "name"),
            json_string(value, "display_name"),
            json_string(value, "created_at"),
            json_string(value, "updated_at")};
}

control::Membership membership_from_json(const nlohmann::json& value) {
    return {json_string(value, "id"),
            json_string(value, "organization_id"),
            json_string(value, "team_id"),
            json_string(value, "principal_type"),
            json_string(value, "principal_id"),
            json_string(value, "principal_taap_id"),
            json_string(value, "role"),
            json_string(value, "attestation_digest"),
            json_string(value, "granted_by_taap_id"),
            json_string(value, "created_at"),
            json_string(value, "updated_at")};
}

control::Invitation invitation_from_json(const nlohmann::json& value) {
    return {json_string(value, "id"),
            json_string(value, "organization_id"),
            json_string(value, "team_id"),
            json_string(value, "invitee_type"),
            json_string(value, "invitee"),
            json_string(value, "role"),
            json_string(value, "expires_at"),
            json_string(value, "status"),
            json_string(value, "accepted_membership_id"),
            json_string(value, "created_by_taap_id"),
            json_string(value, "created_at"),
            json_string(value, "updated_at")};
}

control::PolicyObject object_from_json(const nlohmann::json& value) {
    return {json_string(value, "id"),
            json_string(value, "object_type"),
            json_string(value, "external_id"),
            json_string(value, "parent_id"),
            json_string(value, "created_at"),
            json_string(value, "updated_at"),
            value.value("inherit", true)};
}

control::Grant grant_from_json(const nlohmann::json& value) {
    return {json_string(value, "id"),
            json_string(value, "principal_taap_id"),
            json_string(value, "object_id"),
            json_string(value, "role"),
            json_string(value, "attestation_digest"),
            json_string(value, "granted_by_taap_id"),
            json_string(value, "created_at"),
            json_string(value, "updated_at")};
}

control::EffectiveAccessEntry access_from_json(const nlohmann::json& value) {
    return {json_string(value, "principal_taap_id"),
            json_string(value, "source_object_id"),
            json_string(value, "role"),
            value.at("actions").get<std::vector<std::string>>(),
            value.value("inherited", false)};
}

class LiquidCustomerStores final : public SyncStore {
public:
    explicit LiquidCustomerStores(std::string root) : root_(std::move(root)) {}

    SyncStoreResult<std::vector<std::string>>
    find_missing(const std::string& customer_id, const std::vector<std::string>& digests) override {
        std::lock_guard lock(mutex_);
        auto store = open_customer(customer_id);
        if (!store) {
            return std::unexpected(store.error());
        }
        std::vector<std::string> missing;
        for (const auto& text : digests) {
            const auto digest = liquid::parse_digest(text);
            if (!digest) {
                return std::unexpected(error_of(digest.error()));
            }
            const auto contains = liquid::runtime::block_on((*store)->contains(*digest));
            if (!contains) {
                return std::unexpected(error_of(contains.error()));
            }
            if (!*contains) {
                missing.push_back(text);
            }
        }
        return missing;
    }

    SyncStoreResult<void> write(const std::string& customer_id,
                                const std::string& digest_text,
                                std::vector<std::byte> contents) override {
        std::lock_guard lock(mutex_);
        const auto expected = liquid::parse_digest(digest_text);
        if (!expected) {
            return std::unexpected(error_of(expected.error()));
        }
        auto store = open_customer(customer_id);
        if (!store) {
            return std::unexpected(store.error());
        }
        const auto stored = liquid::runtime::block_on((*store)->put(std::move(contents)));
        if (!stored) {
            return std::unexpected(error_of(stored.error()));
        }
        if (liquid::to_string(*stored) != liquid::to_string(*expected)) {
            return std::unexpected(
                SyncStoreError{"invalid_input", "chunk bytes do not match their digest"});
        }
        return {};
    }

    SyncStoreResult<std::vector<std::byte>> read(const std::string& customer_id,
                                                 const std::string& digest_text) override {
        std::lock_guard lock(mutex_);
        const auto digest = liquid::parse_digest(digest_text);
        if (!digest) {
            return std::unexpected(error_of(digest.error()));
        }
        auto store = open_customer(customer_id);
        if (!store) {
            return std::unexpected(store.error());
        }
        const auto contents = liquid::runtime::block_on((*store)->get(*digest));
        if (!contents) {
            return std::unexpected(error_of(contents.error()));
        }
        return *contents;
    }

private:
    static SyncStoreError error_of(const liquid::error& error) {
        const std::string code(liquid::to_string(error.code));
        return SyncStoreError{code == "unavailable" ? "store_unavailable" : code, error.message};
    }

    SyncStoreResult<liquid::store*> open_customer(const std::string& customer_id) {
        if (root_.empty()) {
            return std::unexpected(
                SyncStoreError{"store_unavailable", "LAKE_STORE_ROOT is not configured"});
        }
        const auto existing = stores_.find(customer_id);
        if (existing != stores_.end()) {
            return existing->second.get();
        }
        const std::filesystem::path path = std::filesystem::path(root_) / customer_id / "store";
        auto opened = liquid::runtime::block_on(liquid::store::open(path));
        if (!opened) {
            return std::unexpected(error_of(opened.error()));
        }
        auto inserted =
            stores_.emplace(customer_id, std::make_unique<liquid::store>(std::move(*opened)));
        return inserted.first->second.get();
    }

    std::string root_;
    std::mutex mutex_;
    std::map<std::string, std::unique_ptr<liquid::store>> stores_;
};

struct OutboundRequest {
    std::string method = "GET";
    std::string url;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

struct OutboundResponse {
    int status = 0;
    std::string body;
    std::string error;
};

struct OutboundTarget {
    bool secure = true;
    std::string host;
    std::string port;
    std::string path;
};

bool parse_outbound_url(std::string_view url, OutboundTarget& target) {
    if (url.starts_with("https://")) {
        url.remove_prefix(8);
        target.secure = true;
        target.port = "443";
    } else if (url.starts_with("http://")) {
        url.remove_prefix(7);
        target.secure = false;
        target.port = "80";
    } else {
        return false;
    }
    const std::size_t path_start = url.find_first_of("/?#");
    const std::string_view authority =
        path_start == std::string_view::npos ? url : url.substr(0, path_start);
    target.path = path_start == std::string_view::npos ? "/" : std::string(url.substr(path_start));
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        return false;
    }
    if (authority.starts_with('[')) {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos) {
            return false;
        }
        target.host = authority.substr(1, close - 1);
        const std::string_view rest = authority.substr(close + 1);
        if (!rest.empty()) {
            if (!rest.starts_with(':')) {
                return false;
            }
            target.port = rest.substr(1);
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon == std::string_view::npos) {
            target.host = authority;
        } else {
            target.host = authority.substr(0, colon);
            target.port = authority.substr(colon + 1);
        }
    }
    const bool loopback =
        target.host == "127.0.0.1" || target.host == "::1" || target.host == "localhost";
    return !target.host.empty() && !target.port.empty() && (target.secure || loopback);
}

std::string outbound_host_header(const OutboundTarget& target) {
    const bool default_port =
        (target.secure && target.port == "443") || (!target.secure && target.port == "80");
    const bool ipv6 = target.host.find(':') != std::string::npos;
    const std::string host = ipv6 ? "[" + target.host + "]" : target.host;
    return default_port ? host : host + ":" + target.port;
}

std::string system_ca_bundle() {
    std::vector<std::string> paths;
    if (const char* configured = std::getenv("SSL_CERT_FILE");
        configured != nullptr && configured[0] != '\0') {
        paths.emplace_back(configured);
    }
    paths.insert(paths.end(),
                 {"/etc/ssl/cert.pem",
                  "/opt/homebrew/etc/openssl@3/cert.pem",
                  "/usr/local/etc/openssl@3/cert.pem",
                  "/etc/ssl/certs/ca-certificates.crt",
                  "/etc/pki/tls/certs/ca-bundle.crt"});
    for (const std::string& path : paths) {
        std::ifstream input(path, std::ios::binary);
        if (input) {
            std::string contents((std::istreambuf_iterator<char>(input)),
                                 std::istreambuf_iterator<char>());
            if (!contents.empty()) {
                return contents;
            }
        }
    }
    return {};
}

bool add_ca_certificates(boost::corosio::tls_context& context, std::string_view bundle) {
    constexpr std::string_view begin_marker = "-----BEGIN CERTIFICATE-----";
    constexpr std::string_view end_marker = "-----END CERTIFICATE-----";
    std::size_t position = 0;
    bool added = false;
    while (position < bundle.size()) {
        const std::size_t begin = bundle.find(begin_marker, position);
        if (begin == std::string_view::npos) {
            break;
        }
        const std::size_t end = bundle.find(end_marker, begin);
        if (end == std::string_view::npos) {
            break;
        }
        const std::size_t block_end = end + end_marker.size();
        const std::string certificate(bundle.substr(begin, block_end - begin));
        if (!context.add_certificate_authority(certificate + "\n")) {
            added = true;
        }
        position = block_end;
    }
    return added;
}

template <class Stream>
boost::capy::io_task<std::string> exchange_outbound(Stream& stream,
                                                    const OutboundRequest& request,
                                                    const OutboundTarget& target,
                                                    OutboundResponse& response) {
    namespace http = boost::http;
    const http::method method = http::string_to_method(request.method);
    if (method == http::method::unknown) {
        co_return {{}, "unsupported outbound HTTP method"};
    }
    http::request message(method, target.path, http::version::http_1_1);
    boost::system::error_code ignored;
    message.set(http::field::host, outbound_host_header(target), ignored);
    message.set(http::field::connection, "close", ignored);
    for (const auto& [name, value] : request.headers) {
        message.set(name, value, ignored);
    }
    message.set_content_length(request.body.size());
    http::serializer serializer(http::make_serializer_config(http::serializer_config{}));
    serializer.set_message(message);
    auto request_sink = serializer.sink_for(stream);
    auto [write_error, written] =
        co_await request_sink.write_eof(boost::capy::make_buffer(std::string_view(request.body)));
    if (write_error) {
        co_return {{}, "could not send OAuth provider request: " + write_error.message()};
    }

    http::parser_config limits(false);
    limits.body_limit = 2 * 1024 * 1024;
    http::response_parser parser(http::make_parser_config(limits));
    parser.reset();
    parser.start();
    auto [header_error] = co_await parser.read_header(stream);
    if (header_error) {
        co_return {{}, "could not read OAuth provider response: " + header_error.message()};
    }
    response.status = static_cast<int>(parser.get().status_int());
    auto body = parser.source_for(stream);
    boost::capy::const_buffer chunks[8];
    for (;;) {
        auto [read_error, buffers] = co_await body.pull(chunks);
        if (read_error == boost::capy::error::eof) {
            break;
        }
        if (read_error) {
            co_return {{}, "could not read OAuth provider body: " + read_error.message()};
        }
        std::size_t consumed = 0;
        for (const boost::capy::const_buffer& buffer : buffers) {
            response.body.append(static_cast<const char*>(buffer.data()), buffer.size());
            consumed += buffer.size();
        }
        body.consume(consumed);
    }
    co_return {{}, std::string()};
}

class OutboundHttpClient {
public:
    OutboundHttpClient() {
        boost::capy::run_async(io_context_.get_executor())(keepalive());
        worker_ = std::thread([this] { io_context_.run(); });
    }

    ~OutboundHttpClient() {
        io_context_.stop();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    OutboundResponse request(OutboundRequest request) {
        auto exchange = std::make_shared<Pending>();
        exchange->request = std::move(request);
        boost::capy::run_async(io_context_.get_executor())(perform(exchange));
        std::unique_lock lock(exchange->mutex);
        exchange->finished.wait(lock, [&] { return exchange->done; });
        return std::move(exchange->response);
    }

private:
    struct Pending {
        OutboundRequest request;
        OutboundResponse response;
        std::mutex mutex;
        std::condition_variable finished;
        bool done = false;
    };

    boost::capy::task<> keepalive() {
        boost::corosio::timer timer(io_context_);
        for (;;) {
            timer.expires_after(std::chrono::hours(1));
            [[maybe_unused]] auto result = co_await timer.wait();
        }
    }

    std::shared_ptr<const boost::corosio::tls_context> tls_context_for(const std::string& host,
                                                                       std::string& error) {
        if (const auto found = tls_contexts_.find(host); found != tls_contexts_.end()) {
            return found->second;
        }
        const std::string bundle = system_ca_bundle();
        if (bundle.empty()) {
            error = "no system CA bundle is available";
            return {};
        }
        auto context = std::make_shared<boost::corosio::tls_context>();
        context->set_hostname(host);
        if (!add_ca_certificates(*context, bundle)) {
            error = "the system CA bundle contained no usable certificates";
            return {};
        }
        if (const std::error_code verify_error =
                context->set_verify_mode(boost::corosio::tls_verify_mode::peer)) {
            error = "could not enable TLS peer verification: " + verify_error.message();
            return {};
        }
        tls_contexts_[host] = context;
        return context;
    }

    boost::capy::task<> perform(std::shared_ptr<Pending> pending) {
        OutboundTarget target;
        if (!parse_outbound_url(pending->request.url, target)) {
            pending->response.error =
                "OAuth provider URL must use HTTPS, except for loopback tests";
            finish(pending);
            co_return;
        }
        boost::corosio::resolver resolver(io_context_);
        auto [resolve_error, results] = co_await resolver.resolve(target.host, target.port);
        if (resolve_error) {
            pending->response.error =
                "could not resolve OAuth provider: " + resolve_error.message();
            finish(pending);
            co_return;
        }
        boost::corosio::tcp_socket socket(io_context_);
        socket.open();
        bool connected = false;
        std::string connect_error = "OAuth provider resolved no addresses";
        for (const auto& entry : results) {
            auto [error] = co_await socket.connect(entry.get_endpoint());
            if (!error) {
                connected = true;
                break;
            }
            connect_error = "could not connect to OAuth provider: " + error.message();
        }
        if (!connected) {
            pending->response.error = connect_error;
            finish(pending);
            co_return;
        }

        std::string exchange_error;
        if (target.secure) {
            std::string context_error;
            const auto context = tls_context_for(target.host, context_error);
            if (!context) {
                pending->response.error = context_error;
                finish(pending);
                co_return;
            }
            boost::corosio::openssl_stream secure(&socket, *context);
            auto [handshake_error] = co_await secure.handshake(boost::corosio::tls_stream::client);
            if (handshake_error) {
                pending->response.error =
                    "OAuth provider TLS handshake failed: " + handshake_error.message();
                finish(pending);
                co_return;
            }
            auto [timeout_error, reason] = co_await boost::capy::timeout(
                exchange_outbound(secure, pending->request, target, pending->response),
                std::chrono::seconds(30));
            exchange_error = timeout_error ? "OAuth provider request timed out" : std::move(reason);
            [[maybe_unused]] auto shutdown = co_await secure.shutdown();
        } else {
            auto [timeout_error, reason] = co_await boost::capy::timeout(
                exchange_outbound(socket, pending->request, target, pending->response),
                std::chrono::seconds(30));
            exchange_error = timeout_error ? "OAuth provider request timed out" : std::move(reason);
        }
        pending->response.error = std::move(exchange_error);
        finish(pending);
    }

    void finish(const std::shared_ptr<Pending>& pending) {
        {
            std::lock_guard lock(pending->mutex);
            pending->done = true;
        }
        pending->finished.notify_all();
    }

    boost::corosio::io_context io_context_;
    std::thread worker_;
    std::map<std::string, std::shared_ptr<const boost::corosio::tls_context>> tls_contexts_;
};

} // namespace

struct ApiServer::Impl {
    ServerOptions options;
    telemetry::Metrics metrics;
    mutable LiquidCustomerStores sync_store;
    Registry registry;
    std::string openapi_body;
    OutboundHttpClient outbound_http;
    struct CachedJwks {
        std::string document;
        std::chrono::steady_clock::time_point fetched_at;
    };
    std::map<std::string, CachedJwks> jwks_cache;
    boost::corosio::io_context io_context;
    std::unique_ptr<boost::beast2::http_server> server;
    std::thread io_thread;
    std::atomic<bool> is_running{false};
    unsigned short bound_port = 0;
    std::string failure_reason;
    std::mutex work_mutex;
    std::condition_variable work_cv;

    explicit Impl(ServerOptions opts) : options(std::move(opts)), sync_store(options.store_root) {
        register_core_capabilities(registry, version_string());
        openapi_body = openapi_document(registry, version_string()).dump();
        metrics.set_gauge("lake_build_info",
                          {{"version", std::string(version_string())}, {"node", options.node_name}},
                          1.0);
        metrics.set_gauge("lake_up", {}, 0.0);
        options.store = telemetry::instrument_store(std::move(options.store), metrics);
    }

    void record_request(const boost::http::route_params& route,
                        std::string_view route_pattern,
                        std::chrono::steady_clock::time_point start) {
        const double duration_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const std::string method(route.req.method_text());
        const std::string status = std::to_string(route.res.status_int());
        const telemetry::Labels labels{
            {"method", method}, {"route", std::string(route_pattern)}, {"status", status}};
        metrics.increment_counter("lake_http_requests_total", labels);
        metrics.observe_histogram("lake_http_request_duration_seconds", labels, duration_seconds);

        std::println("{}",
                     nlohmann::json{{"ts", log_timestamp()},
                                    {"method", method},
                                    {"path", std::string(route.url.path())},
                                    {"route", route_pattern},
                                    {"status", route.res.status_int()},
                                    {"duration_ms", duration_seconds * 1000.0},
                                    {"node", options.node_name}}
                         .dump());
    }

    boost::http::route_task observe_request(boost::http::route_params& route,
                                            std::string route_pattern,
                                            boost::http::route_task task) {
        const auto start = std::chrono::steady_clock::now();
        auto result = co_await std::move(task);
        record_request(route, route_pattern, start);
        co_return result;
    }

    std::string route_pattern_for(const boost::http::route_params& route) const {
        static constexpr std::string_view fixed_routes[] = {
            "/",
            "/style.css",
            "/healthz",
            "/readyz",
            "/metrics",
            "/login",
            "/session",
            "/logout",
            "/customers",
            "/organizations",
            "/sso",
            "/teams",
            "/members",
            "/invitations",
            "/grants",
            "/access",
            "/repositories",
            "/repository",
            "/search",
            "/audit",
            "/sign-up",
            "/sign-in",
            "/mfa",
            "/passkeys",
            "/passkey-sign-in",
            "/sessions",
            "/tokens",
            "/identity/sign-up",
            "/identity/sign-in",
            "/identity/mfa/verify",
            "/identity/mfa/enroll",
            "/identity/mfa/disable",
            "/identity/oauth",
            "/identity/session/revoke",
            "/identity/device/revoke",
            "/identity/token/create",
            "/customers",
            "/repositories",
            "/repository",
            "/v1/openapi.json",
            "/mcp",
        };
        const std::string path(route.url.path());
        for (const std::string_view pattern : fixed_routes) {
            if (path == pattern)
                return std::string(pattern);
        }
        for (const auto& capability : registry.list()) {
            const std::string pattern = rest_path(*capability);
            if (path == pattern)
                return pattern;
        }
        if (path.starts_with("/v1/oauth/") && path.ends_with("/callback")) {
            return "/v1/oauth/:provider/callback";
        }
        return "unmatched";
    }

    ApiContext context_for(const boost::http::route_params& route) const {
        ApiContext ctx;
        ctx.store = options.store.get();
        ctx.search = options.search.get();
        ctx.node_name = options.node_name;
        ctx.identity_seed = options.identity_seed;
        ctx.instance_kek = options.instance_kek;
        ctx.sync_store = &sync_store;
        ctx.public_host = options.public_host;
        if (route.req.count(boost::http::field::user_agent) != 0) {
            ctx.device = std::string(route.req.at(boost::http::field::user_agent));
        }
        // Authorization: the bearer wins; else the HttpOnly session cookie (its
        // value IS the admin token, validated identically), so the server-rendered
        // pages authenticate with the cookie set by POST /session and the token
        // never touches the page.
        std::string bearer = bearer_of(route);
        if (!bearer.empty()) {
            ctx.authentication_method = "bearer";
        } else {
            bearer = cookie_value(route, "lake_session");
            if (!bearer.empty()) {
                ctx.authentication_method = "session";
            }
        }
        if (!bearer.empty() && options.store) {
            if (auto principal = options.store->context_for(bearer)) {
                ctx.authorized = true;
                ctx.session_credential = true;
                ctx.acting_user_id = principal->user_id;
                ctx.acting_identity = principal->taap_id;
                ctx.pool_id = principal->pool_id;
                ctx.scopes = principal->scopes;
            }
        }
        if (!options.admin_token.empty() && !bearer.empty() &&
            constant_time_equal(bearer, options.admin_token)) {
            ctx.authorized = true;
            ctx.acting_identity = "admin";
            ctx.operator_credential = true;
        }
        if (route.req.count("x-request-id") != 0) {
            ctx.request_id = std::string(route.req.at("x-request-id"));
        } else {
            ctx.request_id = control::new_uuid_v4();
        }
        return ctx;
    }

    // Authentication and session events happen outside the registry, so they are
    // recorded here through the same audit::record path the registry uses.
    bool record_direct_event(const ApiContext& context, audit::EventDraft event) const {
        if (context.store == nullptr) {
            return true;
        }
        if (event.principal.empty()) {
            event.principal =
                context.acting_identity.empty() ? "anonymous" : context.acting_identity;
        }
        if (event.request_id.empty()) {
            event.request_id = context.request_id;
        }
        const audit::IdentityOptions identity_options{
            .configured_seed = context.identity_seed,
            .instance_kek = context.instance_kek,
        };
        return audit::record(*context.store, event, identity_options).has_value();
    }

    boost::http::route_task serve_web_root(boost::http::route_params& route) {
        return send_text(
            route,
            boost::http::status::ok,
            web_ui_hello(options.node_name, version_string(), context_for(route).authorized),
            "text/html; charset=utf-8",
            "no-store");
    }

    boost::http::route_task redirect(boost::http::route_params& route,
                                     std::string_view location,
                                     std::string_view set_cookie = {}) {
        namespace http = boost::http;
        route.status(http::status::found);
        route.res.set(http::field::location, location);
        if (!set_cookie.empty())
            route.res.set(http::field::set_cookie, set_cookie);
        route.res.set(http::field::cache_control, "no-store");
        stamp_response_headers(route, "");
        auto [send_error] = co_await route.send("");
        if (send_error)
            co_return http::route_error(send_error);
        co_return http::route_done;
    }

    boost::http::route_task serve_login(boost::http::route_params& route) {
        if (context_for(route).authorized)
            return redirect(route, "/repositories");
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_login(""),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    // POST /session — the sign-in form. A matching token becomes the HttpOnly
    // lake_session cookie.
    boost::http::route_task serve_session(boost::http::route_params& route) {
        namespace http = boost::http;
        ApiContext context = context_for(route);
        context.authentication_method = "admin_token";
        StringBodySink sink;
        auto [read_error, bytes_read] = co_await boost::capy::push_to(route.req_body, sink);
        if (read_error)
            co_return http::route_error(read_error);
        const std::string token = form_field(sink.data, "token");
        const bool ok = !options.admin_token.empty() && !token.empty() &&
                        constant_time_equal(token, options.admin_token);
        if (!ok) {
            if (!record_direct_event(context,
                                     audit::EventDraft{
                                         .kind = "authentication",
                                         .object = "lake",
                                         .action = "sign_in",
                                         .outcome = "failure",
                                         .method = context.authentication_method,
                                         .device = context.device,
                                     })) {
                co_return co_await send_text(
                    route,
                    http::status::service_unavailable,
                    error_body("store_error", "could not record authentication failure"),
                    "application/json; charset=utf-8",
                    "no-store");
            }
            const std::string body = web_ui_login("That token did not match.");
            route.status(http::status::unauthorized);
            route.res.set(http::field::content_type, "text/html; charset=utf-8");
            route.res.set(http::field::cache_control, "no-store");
            stamp_response_headers(route, body);
            auto [send_error] = co_await route.send(body);
            if (send_error)
                co_return http::route_error(send_error);
            co_return http::route_done;
        }
        context.authorized = true;
        context.acting_identity = "admin";
        if (!record_direct_event(context,
                                 audit::EventDraft{
                                     .kind = "authentication",
                                     .object = "lake",
                                     .action = "sign_in",
                                     .outcome = "success",
                                     .method = context.authentication_method,
                                     .device = context.device,
                                 }) ||
            !record_direct_event(context,
                                 audit::EventDraft{
                                     .kind = "session",
                                     .object = "lake_session",
                                     .action = "issue",
                                     .outcome = "success",
                                     .method = context.authentication_method,
                                     .device = context.device,
                                 })) {
            co_return co_await send_text(
                route,
                http::status::service_unavailable,
                error_body("store_error", "could not record authentication event"),
                "application/json; charset=utf-8",
                "no-store");
        }
        std::string cookie = "lake_session=" + token + "; HttpOnly; SameSite=Strict; Path=/";
        if (request_is_https(route))
            cookie += "; Secure";
        co_return co_await redirect(route, "/repositories", cookie);
    }

    boost::http::route_task serve_logout(boost::http::route_params& route) {
        const ApiContext context = context_for(route);
        if (!record_direct_event(context,
                                 audit::EventDraft{
                                     .kind = "session",
                                     .object = "lake_session",
                                     .action = "revoke",
                                     .outcome = context.authorized ? "revoked" : "denied",
                                     .method = context.authentication_method,
                                     .device = context.device,
                                 })) {
            return send_text(route,
                             boost::http::status::service_unavailable,
                             error_body("store_error", "could not record session revocation"),
                             "application/json; charset=utf-8",
                             "no-store");
        }
        return redirect(route, "/", "lake_session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
    }

    boost::http::route_task serve_page_error(boost::http::route_params& route,
                                             const ApiResult& result) {
        return send_text(route,
                         boost::http::int_to_status(static_cast<unsigned>(rest_status(result))),
                         rest_envelope(result).dump(),
                         "application/json; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_customers_page(boost::http::route_params& route) {
        ApiContext ctx = context_for(route);
        if (!ctx.authorized)
            return redirect(route, "/login");
        std::vector<control::Customer> customers;
        const ApiResult rows = registry.call("list_customers", {{"limit", 500}}, ctx);
        if (!rows.ok) {
            return serve_page_error(route, rows);
        }
        for (const auto& value : rows.value.at("items")) {
            customers.push_back(customer_from_json(value));
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_customers(customers),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_organizations_page(boost::http::route_params& route) {
        ApiContext ctx = context_for(route);
        if (!ctx.authorized)
            return redirect(route, "/login");
        const ApiResult rows = registry.call("list_organizations", {{"limit", 500}}, ctx);
        if (!rows.ok) {
            return serve_page_error(route, rows);
        }
        std::vector<control::Organization> organizations;
        for (const auto& value : rows.value.at("items")) {
            organizations.push_back(organization_from_json(value));
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_organizations(organizations),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_sso_page(boost::http::route_params& route) {
        ApiContext ctx = context_for(route);
        if (!ctx.authorized) {
            return redirect(route, "/login");
        }
        const std::string organization_id = query_value(route, "organization_id");
        if (organization_id.empty()) {
            return redirect(route, "/organizations");
        }
        const ApiResult provider_rows = registry.call(
            "list_sso_providers", {{"organization_id", organization_id}, {"limit", 100}}, ctx);
        if (!provider_rows.ok) {
            return serve_page_error(route, provider_rows);
        }
        const ApiResult issuer_rows = registry.call(
            "list_federation_issuers", {{"organization_id", organization_id}, {"limit", 500}}, ctx);
        if (!issuer_rows.ok) {
            return serve_page_error(route, issuer_rows);
        }
        std::vector<control::SsoProvider> providers;
        for (const nlohmann::json& value : provider_rows.value.at("items")) {
            providers.push_back(sso_provider_from_json(value));
        }
        std::vector<control::FederationIssuer> issuers;
        for (const nlohmann::json& value : issuer_rows.value.at("items")) {
            issuers.push_back(federation_issuer_from_json(value));
        }
        std::vector<control::ProvisioningLog> logs;
        std::string provider_id = query_value(route, "provider_id");
        if (provider_id.empty() && !providers.empty()) {
            provider_id = providers.front().id;
        }
        if (!provider_id.empty()) {
            const ApiResult log_rows = registry.call(
                "list_sso_provisioning_logs", {{"provider_id", provider_id}, {"limit", 100}}, ctx);
            if (!log_rows.ok) {
                return serve_page_error(route, log_rows);
            }
            for (const nlohmann::json& value : log_rows.value.at("items")) {
                logs.push_back(provisioning_log_from_json(value));
            }
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_sso(organization_id, providers, issuers, logs),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_teams_page(boost::http::route_params& route) {
        ApiContext ctx = context_for(route);
        if (!ctx.authorized)
            return redirect(route, "/login");
        const std::string organization_id = query_value(route, "organization_id");
        if (organization_id.empty())
            return redirect(route, "/organizations");
        const ApiResult rows = registry.call(
            "list_teams", {{"organization_id", organization_id}, {"limit", 500}}, ctx);
        if (!rows.ok) {
            return serve_page_error(route, rows);
        }
        std::vector<control::Team> teams;
        for (const auto& value : rows.value.at("items")) {
            teams.push_back(team_from_json(value));
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_teams(organization_id, teams),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_members_page(boost::http::route_params& route) {
        ApiContext ctx = context_for(route);
        if (!ctx.authorized)
            return redirect(route, "/login");
        const std::string organization_id = query_value(route, "organization_id");
        const std::string team_id = query_value(route, "team_id");
        if (organization_id.empty())
            return redirect(route, "/organizations");
        nlohmann::json input{{"organization_id", organization_id}, {"limit", 500}};
        if (!team_id.empty()) {
            input["team_id"] = team_id;
        }
        const ApiResult rows = registry.call("list_memberships", input, ctx);
        if (!rows.ok) {
            return serve_page_error(route, rows);
        }
        std::vector<control::Membership> memberships;
        for (const auto& value : rows.value.at("items")) {
            memberships.push_back(membership_from_json(value));
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_memberships(organization_id, team_id, memberships),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_invitations_page(boost::http::route_params& route) {
        ApiContext ctx = context_for(route);
        if (!ctx.authorized)
            return redirect(route, "/login");
        const std::string organization_id = query_value(route, "organization_id");
        const std::string team_id = query_value(route, "team_id");
        if (organization_id.empty())
            return redirect(route, "/organizations");
        nlohmann::json input{{"organization_id", organization_id}, {"limit", 500}};
        if (!team_id.empty()) {
            input["team_id"] = team_id;
        }
        const ApiResult rows = registry.call("list_invitations", input, ctx);
        if (!rows.ok) {
            return serve_page_error(route, rows);
        }
        std::vector<control::Invitation> invitations;
        for (const auto& value : rows.value.at("items")) {
            invitations.push_back(invitation_from_json(value));
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_invitations(organization_id, team_id, invitations),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_grants_page(boost::http::route_params& route) {
        ApiContext ctx = context_for(route);
        if (!ctx.authorized)
            return redirect(route, "/login");
        const std::string object_id = query_value(route, "object_id");
        if (object_id.empty())
            return redirect(route, "/organizations");
        const ApiResult object_result = registry.call("get_object", {{"id", object_id}}, ctx);
        if (!object_result.ok) {
            return serve_page_error(route, object_result);
        }
        const ApiResult grant_result =
            registry.call("list_grants", {{"object_id", object_id}, {"limit", 500}}, ctx);
        if (!grant_result.ok) {
            return serve_page_error(route, grant_result);
        }
        std::vector<control::Grant> grants;
        for (const auto& value : grant_result.value.at("items")) {
            grants.push_back(grant_from_json(value));
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_grants(object_from_json(object_result.value), grants),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_access_page(boost::http::route_params& route) {
        ApiContext ctx = context_for(route);
        if (!ctx.authorized)
            return redirect(route, "/login");
        const std::string object_id = query_value(route, "object_id");
        if (object_id.empty())
            return redirect(route, "/organizations");
        const ApiResult object_result = registry.call("get_object", {{"id", object_id}}, ctx);
        if (!object_result.ok) {
            return serve_page_error(route, object_result);
        }
        const ApiResult access_result =
            registry.call("effective_access", {{"object_id", object_id}}, ctx);
        if (!access_result.ok) {
            return serve_page_error(route, access_result);
        }
        std::vector<control::EffectiveAccessEntry> entries;
        for (const auto& value : access_result.value.at("items")) {
            entries.push_back(access_from_json(value));
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_effective_access(object_from_json(object_result.value), entries),
                         "text/html; charset=utf-8",
                         "no-store");
        return redirect(route, "/", "lake_session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
    }

    boost::http::route_task serve_sign_up(boost::http::route_params& route) {
        const ApiContext context = context_for(route);
        if (context.authorized) {
            return redirect(route, context.operator_credential ? "/repositories" : "/sessions");
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_sign_up(""),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_sign_in(boost::http::route_params& route) {
        const ApiContext context = context_for(route);
        if (context.authorized) {
            return redirect(route, context.operator_credential ? "/repositories" : "/sessions");
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_sign_in(""),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_mfa(boost::http::route_params& route) {
        const ApiContext context = context_for(route);
        if (context.operator_credential) {
            return redirect(route, "/repositories");
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_mfa("", "", {}, context.authorized),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_passkeys(boost::http::route_params& route) {
        const ApiContext context = context_for(route);
        if (!context.authorized) {
            return redirect(route, "/sign-in");
        }
        if (context.operator_credential) {
            return redirect(route, "/repositories");
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_passkeys(""),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_passkey_sign_in(boost::http::route_params& route) {
        const ApiContext context = context_for(route);
        if (context.authorized) {
            return redirect(route, context.operator_credential ? "/repositories" : "/sessions");
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_passkey_sign_in(""),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_sessions(boost::http::route_params& route) {
        const ApiContext context = context_for(route);
        if (!context.authorized) {
            return redirect(route, "/sign-in");
        }
        if (context.operator_credential) {
            return redirect(route, "/repositories");
        }
        std::vector<control::Session> sessions;
        std::vector<control::Device> devices;
        std::string error;
        if (auto rows = options.store->list_sessions(context.acting_user_id, context.pool_id)) {
            sessions = std::move(*rows);
        } else {
            error = rows.error().message;
        }
        if (auto rows = options.store->list_devices(context.acting_user_id)) {
            devices = std::move(*rows);
        } else if (error.empty()) {
            error = rows.error().message;
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_sessions(sessions, devices, error),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_tokens(boost::http::route_params& route) {
        const ApiContext context = context_for(route);
        if (!context.authorized) {
            return redirect(route, "/sign-in");
        }
        if (context.operator_credential) {
            return redirect(route, "/repositories");
        }
        std::vector<control::Session> sessions;
        std::string error;
        if (auto rows = options.store->list_sessions(context.acting_user_id, context.pool_id)) {
            sessions = std::move(*rows);
        } else {
            error = rows.error().message;
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_tokens(sessions, "", error),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_identity_sign_up(boost::http::route_params& route) {
        namespace http = boost::http;
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return http::route_error(read_error);
        }
        ApiContext context = context_for(route);
        const ApiResult result =
            registry.call("sign_up_password",
                          {{"email", form_field(body, "email")},
                           {"display_name", form_field(body, "display_name")},
                           {"password", form_field(body, "password")},
                           {"device_name", form_field(body, "device_name")},
                           {"identity_document", form_field(body, "identity_document")}},
                          context);
        if (!result.ok) {
            co_return co_await send_text(route,
                                         http::int_to_status(rest_status(result)),
                                         web_ui_sign_up(result.error.message),
                                         "text/html; charset=utf-8",
                                         "no-store");
        }
        co_return co_await redirect(
            route, "/sessions", browser_session_cookie(route, result.value.value("secret", "")));
    }

    boost::http::route_task serve_identity_sign_in(boost::http::route_params& route) {
        namespace http = boost::http;
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return http::route_error(read_error);
        }
        ApiContext context = context_for(route);
        const ApiResult result = registry.call("sign_in_password",
                                               {{"email", form_field(body, "email")},
                                                {"password", form_field(body, "password")},
                                                {"device_name", form_field(body, "device_name")}},
                                               context);
        if (!result.ok && result.error.code == "mfa_required") {
            co_return co_await redirect(route, "/mfa");
        }
        if (!result.ok) {
            co_return co_await send_text(route,
                                         http::int_to_status(rest_status(result)),
                                         web_ui_sign_in(result.error.message),
                                         "text/html; charset=utf-8",
                                         "no-store");
        }
        co_return co_await redirect(
            route, "/sessions", browser_session_cookie(route, result.value.value("secret", "")));
    }

    boost::http::route_task serve_identity_mfa_verify(boost::http::route_params& route) {
        namespace http = boost::http;
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return http::route_error(read_error);
        }
        ApiContext context = context_for(route);
        const ApiResult result = registry.call("verify_mfa",
                                               {{"email", form_field(body, "email")},
                                                {"password", form_field(body, "password")},
                                                {"code", form_field(body, "code")},
                                                {"device_name", form_field(body, "device_name")}},
                                               context);
        if (!result.ok) {
            co_return co_await send_text(route,
                                         http::int_to_status(rest_status(result)),
                                         web_ui_mfa(result.error.message, "", {}, false),
                                         "text/html; charset=utf-8",
                                         "no-store");
        }
        co_return co_await redirect(
            route, "/sessions", browser_session_cookie(route, result.value.value("secret", "")));
    }

    boost::http::route_task serve_identity_mfa_enroll(boost::http::route_params& route) {
        namespace http = boost::http;
        auto [read_error, body] = co_await read_string_body(route);
        static_cast<void>(body);
        if (read_error) {
            co_return http::route_error(read_error);
        }
        ApiContext context = context_for(route);
        const ApiResult result = registry.call("enroll_mfa", nlohmann::json::object(), context);
        if (!result.ok) {
            co_return co_await send_text(
                route,
                http::int_to_status(rest_status(result)),
                web_ui_mfa(result.error.message, "", {}, context.authorized),
                "text/html; charset=utf-8",
                "no-store");
        }
        std::vector<std::string> recovery_codes;
        for (const nlohmann::json& code :
             result.value.value("recovery_codes", nlohmann::json::array())) {
            if (code.is_string()) {
                recovery_codes.push_back(code.get<std::string>());
            }
        }
        co_return co_await send_text(
            route,
            http::status::ok,
            web_ui_mfa("", result.value.value("otpauth_uri", ""), recovery_codes, true),
            "text/html; charset=utf-8",
            "no-store");
    }

    boost::http::route_task serve_identity_mfa_disable(boost::http::route_params& route) {
        namespace http = boost::http;
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return http::route_error(read_error);
        }
        ApiContext context = context_for(route);
        const ApiResult result =
            registry.call("disable_mfa", {{"code", form_field(body, "code")}}, context);
        if (!result.ok) {
            co_return co_await send_text(
                route,
                http::int_to_status(rest_status(result)),
                web_ui_mfa(result.error.message, "", {}, context.authorized),
                "text/html; charset=utf-8",
                "no-store");
        }
        co_return co_await redirect(route, "/mfa");
    }

    boost::http::route_task serve_identity_oauth(boost::http::route_params& route) {
        namespace http = boost::http;
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return http::route_error(read_error);
        }
        ApiContext context = context_for(route);
        const ApiResult result =
            registry.call("start_oauth",
                          {{"provider", form_field(body, "provider")},
                           {"device_name", form_field(body, "device_name")},
                           {"identity_document", form_field(body, "identity_document")}},
                          context);
        if (!result.ok) {
            co_return co_await send_text(route,
                                         http::int_to_status(rest_status(result)),
                                         web_ui_sign_in(result.error.message),
                                         "text/html; charset=utf-8",
                                         "no-store");
        }
        co_return co_await redirect(route, result.value.value("authorization_url", "/sign-in"));
    }

    boost::http::route_task serve_identity_session_revoke(boost::http::route_params& route) {
        namespace http = boost::http;
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return http::route_error(read_error);
        }
        ApiContext context = context_for(route);
        const ApiResult result = registry.call(
            "revoke_session", {{"session_id", form_field(body, "session_id")}}, context);
        if (!result.ok) {
            co_return co_await send_text(route,
                                         http::int_to_status(rest_status(result)),
                                         web_ui_sessions({}, {}, result.error.message),
                                         "text/html; charset=utf-8",
                                         "no-store");
        }
        const std::string return_to =
            form_field(body, "return_to") == "/tokens" ? "/tokens" : "/sessions";
        co_return co_await redirect(route, return_to);
    }

    boost::http::route_task serve_identity_device_revoke(boost::http::route_params& route) {
        namespace http = boost::http;
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return http::route_error(read_error);
        }
        ApiContext context = context_for(route);
        const ApiResult result =
            registry.call("revoke_device", {{"device_id", form_field(body, "device_id")}}, context);
        if (!result.ok) {
            co_return co_await send_text(route,
                                         http::int_to_status(rest_status(result)),
                                         web_ui_sessions({}, {}, result.error.message),
                                         "text/html; charset=utf-8",
                                         "no-store");
        }
        co_return co_await redirect(route, "/sessions");
    }

    boost::http::route_task serve_identity_token_create(boost::http::route_params& route) {
        namespace http = boost::http;
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return http::route_error(read_error);
        }
        ApiContext context = context_for(route);
        const std::string expiry = form_field(body, "expires_in_seconds");
        const int expires_in_seconds = expiry.empty() ? 2592000 : std::atoi(expiry.c_str());
        const ApiResult result =
            registry.call("create_token",
                          {{"name", form_field(body, "name")},
                           {"scopes", comma_separated(form_field(body, "scopes"))},
                           {"expires_in_seconds", expires_in_seconds},
                           {"static", form_field(body, "static") == "true"},
                           {"reason", form_field(body, "reason")}},
                          context);
        std::vector<control::Session> sessions;
        if (context.authorized && !context.operator_credential) {
            if (auto rows = options.store->list_sessions(context.acting_user_id, context.pool_id)) {
                sessions = std::move(*rows);
            }
        }
        if (!result.ok) {
            co_return co_await send_text(route,
                                         http::int_to_status(rest_status(result)),
                                         web_ui_tokens(sessions, "", result.error.message),
                                         "text/html; charset=utf-8",
                                         "no-store");
        }
        co_return co_await send_text(route,
                                     http::status::ok,
                                     web_ui_tokens(sessions, result.value.value("secret", ""), ""),
                                     "text/html; charset=utf-8",
                                     "no-store");
    }

    std::optional<std::string> jwks_for(const std::string& url, bool refresh) {
        const auto found = jwks_cache.find(url);
        if (!refresh && found != jwks_cache.end() &&
            std::chrono::steady_clock::now() - found->second.fetched_at < std::chrono::hours(1)) {
            return found->second.document;
        }
        OutboundResponse response = outbound_http.request(
            {"GET", url, "", {{"Accept", "application/json"}, {"User-Agent", "lake-oauth/1"}}});
        if (!response.error.empty() || response.status < 200 || response.status >= 300) {
            return std::nullopt;
        }
        const nlohmann::json document = nlohmann::json::parse(response.body, nullptr, false);
        if (document.is_discarded() || !document.contains("keys") || !document["keys"].is_array()) {
            return std::nullopt;
        }
        jwks_cache[url] = {response.body, std::chrono::steady_clock::now()};
        return response.body;
    }

    boost::http::route_task oauth_error(boost::http::route_params& route,
                                        boost::http::status status,
                                        std::string_view message) {
        ApiContext context = context_for(route);
        context.authentication_method = "oauth";
        if (!record_direct_event(context,
                                 audit::EventDraft{
                                     .kind = "authentication",
                                     .object = "oauth",
                                     .action = "sign_in",
                                     .outcome = "failure",
                                     .method = "oauth",
                                     .device = context.device,
                                     .details = {{"message", message}},
                                 })) {
            return send_text(route,
                             boost::http::status::service_unavailable,
                             error_body("store_error", "could not record OAuth failure"),
                             "application/json; charset=utf-8",
                             "no-store");
        }
        return send_text(route,
                         status,
                         error_body("unauthorized", message),
                         "application/json; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_oauth_callback(boost::http::route_params& route) {
        namespace http = boost::http;
        std::string provider_name;
        for (const auto& [name, value] : route.params) {
            if (name == "provider") {
                provider_name = value;
            }
        }
        const std::string code = query_value(route, "code");
        const std::string state = query_value(route, "state");
        if (!options.store || provider_name.empty() || code.empty() || state.empty()) {
            co_return co_await oauth_error(
                route, http::status::bad_request, "OAuth callback is incomplete");
        }
        const auto flow = options.store->consume_oauth(provider_name, state);
        if (!flow) {
            co_return co_await oauth_error(
                route, http::status::forbidden, "OAuth state is invalid or expired");
        }
        const auto provider = options.store->get_oauth_provider(flow->pool_id, provider_name);
        if (!provider) {
            co_return co_await oauth_error(
                route, http::status::forbidden, "OAuth provider is not registered");
        }
        const OAuthEndpoints endpoints = oauth_endpoints(*provider);
        const std::string callback = oauth_callback_url(options.public_host, provider_name);
        std::string token_body = "grant_type=authorization_code";
        token_body += "&code=" + oauth_url_encode(code);
        token_body += "&redirect_uri=" + oauth_url_encode(callback);
        token_body += "&client_id=" + oauth_url_encode(provider->client_id);
        token_body += "&client_secret=" + oauth_url_encode(provider->client_secret);
        token_body += "&code_verifier=" + oauth_url_encode(flow->code_verifier);
        const OutboundResponse token_response =
            outbound_http.request({"POST",
                                   endpoints.token,
                                   token_body,
                                   {{"Accept", "application/json"},
                                    {"Content-Type", "application/x-www-form-urlencoded"},
                                    {"User-Agent", "lake-oauth/1"}}});
        if (!token_response.error.empty() || token_response.status < 200 ||
            token_response.status >= 300) {
            co_return co_await oauth_error(
                route, http::status::bad_gateway, "OAuth token exchange failed");
        }
        const nlohmann::json token = nlohmann::json::parse(token_response.body, nullptr, false);
        if (token.is_discarded()) {
            co_return co_await oauth_error(
                route, http::status::bad_gateway, "OAuth token response was invalid");
        }

        control::OAuthIdentity identity;
        identity.pool_id = flow->pool_id;
        identity.provider = provider_name;
        identity.device_name = flow->device_name;
        identity.identity_document = flow->identity_document;
        if (provider_name == "github") {
            const std::string access_token = token.value("access_token", "");
            if (access_token.empty()) {
                co_return co_await oauth_error(
                    route, http::status::bad_gateway, "GitHub did not return an access token");
            }
            const OutboundResponse user_response =
                outbound_http.request({"GET",
                                       endpoints.user,
                                       "",
                                       {{"Accept", "application/vnd.github+json"},
                                        {"Authorization", "Bearer " + access_token},
                                        {"User-Agent", "lake-oauth/1"}}});
            const nlohmann::json user = nlohmann::json::parse(user_response.body, nullptr, false);
            if (!user_response.error.empty() || user_response.status < 200 ||
                user_response.status >= 300 || user.is_discarded() || !user.contains("id")) {
                co_return co_await oauth_error(
                    route, http::status::bad_gateway, "GitHub user verification failed");
            }
            identity.subject = user["id"].is_string() ? user["id"].get<std::string>()
                                                      : std::to_string(user["id"].get<long long>());
            identity.email = user.value("email", "");
            identity.display_name = user.value("name", user.value("login", std::string()));
        } else {
            const std::string id_token = token.value("id_token", "");
            std::optional<std::string> jwks = jwks_for(endpoints.jwks, false);
            auto claims =
                jwks ? control::identity_crypto::verify_oidc_id_token(
                           id_token, *jwks, provider->issuer, provider->client_id, flow->nonce)
                     : std::nullopt;
            if (!claims) {
                jwks = jwks_for(endpoints.jwks, true);
                claims =
                    jwks ? control::identity_crypto::verify_oidc_id_token(
                               id_token, *jwks, provider->issuer, provider->client_id, flow->nonce)
                         : std::nullopt;
            }
            if (!claims) {
                co_return co_await oauth_error(
                    route, http::status::forbidden, "OpenID Connect id token verification failed");
            }
            identity.subject = claims->subject;
            identity.email = claims->email;
            identity.display_name = claims->display_name;
        }
        const auto issued = options.store->sign_in_oauth(identity);
        if (!issued) {
            co_return co_await oauth_error(
                route, http::status::forbidden, "OAuth identity could not sign in");
        }
        ApiContext context = context_for(route);
        context.authentication_method = "oauth";
        if (const auto principal = options.store->context_for(issued->secret)) {
            context.authorized = true;
            context.acting_user_id = principal->user_id;
            context.acting_identity = principal->taap_id;
        }
        if (!record_direct_event(context,
                                 audit::EventDraft{
                                     .kind = "authentication",
                                     .object = "oauth/" + provider_name,
                                     .action = "sign_in",
                                     .outcome = "success",
                                     .method = "oauth",
                                     .device = flow->device_name,
                                 })) {
            co_return co_await oauth_error(
                route, http::status::service_unavailable, "OAuth audit recording failed");
        }
        std::string cookie = "lake_session=" + issued->secret + "; HttpOnly; SameSite=Lax; Path=/";
        if (request_is_https(route)) {
            cookie += "; Secure";
        }
        co_return co_await redirect(route, "/repositories", cookie);
    }
    boost::http::route_task serve_repositories_page(boost::http::route_params& route) {
        ApiContext ctx = context_for(route);
        if (!ctx.authorized)
            return redirect(route, "/login");
        std::vector<control::Repository> repositories;
        std::map<std::string, control::Customer> owners;
        nlohmann::json input{{"limit", 500}};
        const std::string customer_id = query_value(route, "customer_id");
        if (!customer_id.empty()) {
            input["customer_id"] = customer_id;
        }
        const ApiResult rows = registry.call("list_repositories", input, ctx);
        if (!rows.ok) {
            return serve_page_error(route, rows);
        }
        for (const auto& value : rows.value.at("items")) {
            repositories.push_back(repository_from_json(value));
        }
        const ApiResult customers = registry.call("list_customers", {{"limit", 1000}}, ctx);
        if (!customers.ok) {
            return serve_page_error(route, customers);
        }
        for (const auto& value : customers.value.at("items")) {
            control::Customer customer = customer_from_json(value);
            owners.emplace(customer.id, std::move(customer));
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_repositories(repositories, owners),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_repository_page(boost::http::route_params& route) {
        ApiContext ctx = context_for(route);
        if (!ctx.authorized)
            return redirect(route, "/login");
        const std::string id = query_value(route, "id");
        if (id.empty())
            return redirect(route, "/repositories");
        const ApiResult repository_result = registry.call("get_repository", {{"id", id}}, ctx);
        if (!repository_result.ok) {
            return serve_page_error(route, repository_result);
        }
        const control::Repository repository = repository_from_json(repository_result.value);
        const ApiResult owner_result =
            registry.call("get_customer", {{"id", repository.customer_id}}, ctx);
        std::optional<control::Customer> owner;
        if (owner_result.ok) {
            owner = customer_from_json(owner_result.value);
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_repository(repository, owner ? &*owner : nullptr),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_search_page(boost::http::route_params& route) {
        ApiContext context = context_for(route);
        if (!context.authorized) {
            return redirect(route, "/login");
        }
        const std::string query = query_value(route, "query");
        std::vector<search::Hit> hits;
        if (!query.empty()) {
            const ApiResult result =
                registry.call("search", {{"query", query}, {"limit", 100}}, context);
            if (!result.ok) {
                return serve_page_error(route, result);
            }
            for (const nlohmann::json& item : result.value.at("items")) {
                hits.push_back(search::Hit{
                    .customer = item.at("customer").get<std::string>(),
                    .repository = item.at("repository").get<std::string>(),
                    .path = item.at("path").get<std::string>(),
                    .change = item.at("change").get<std::string>(),
                    .snippet = item.at("snippet").get<std::string>(),
                    .score = item.at("score").get<double>(),
                });
            }
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_search(query, hits),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_audit_page(boost::http::route_params& route) {
        ApiContext ctx = context_for(route);
        if (!ctx.authorized)
            return redirect(route, "/login");
        const audit::Filter filter{
            .principal = query_value(route, "principal"),
            .object = query_value(route, "object"),
            .outcome = query_value(route, "outcome"),
            .from = query_value(route, "from"),
            .to = query_value(route, "to"),
            .limit = 500,
        };
        nlohmann::json input = {{"limit", filter.limit}};
        if (!filter.principal.empty())
            input["principal"] = filter.principal;
        if (!filter.object.empty())
            input["object"] = filter.object;
        if (!filter.outcome.empty())
            input["outcome"] = filter.outcome;
        if (!filter.from.empty())
            input["from"] = filter.from;
        if (!filter.to.empty())
            input["to"] = filter.to;
        const ApiResult result = registry.call("audit", input, ctx);
        if (!result.ok) {
            return serve_page_error(route, result);
        }
        std::vector<audit::Event> events;
        for (const nlohmann::json& item : result.value.value("items", nlohmann::json::array())) {
            events.push_back(audit::event_from_json(item));
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_audit(events, filter),
                         "text/html; charset=utf-8",
                         "no-store");
    }
    boost::http::route_task serve_style(boost::http::route_params& route) {
        static const std::string css = web_ui_stylesheet();
        return send_text(route,
                         boost::http::status::ok,
                         css,
                         "text/css; charset=utf-8",
                         "public, max-age=86400");
    }

    boost::http::route_task serve_openapi(boost::http::route_params& route) {
        return send_text(route,
                         boost::http::status::ok,
                         openapi_body,
                         "application/json; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_healthz(boost::http::route_params& route) {
        return send_text(route,
                         boost::http::status::ok,
                         R"({"ok":true})",
                         "application/json; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_readyz(boost::http::route_params& route) {
        if (!options.store) {
            return send_text(
                route,
                boost::http::status::service_unavailable,
                error_body("store_unavailable", "no control-plane store is configured"),
                "application/json; charset=utf-8",
                "no-store");
        }
        if (auto result = options.store->ping(); !result) {
            return send_text(route,
                             boost::http::status::service_unavailable,
                             error_body("store_unavailable", result.error().message),
                             "application/json; charset=utf-8",
                             "no-store");
        }
        return send_text(route,
                         boost::http::status::ok,
                         nlohmann::json{{"ok", true}, {"store", options.store->kind()}}.dump(),
                         "application/json; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_metrics(boost::http::route_params& route) {
        if (!context_for(route).authorized) {
            return send_text(route,
                             boost::http::status::forbidden,
                             error_body("unauthorized", "a valid admin bearer token is required"),
                             "application/json; charset=utf-8",
                             "no-store");
        }
        return send_text(route,
                         boost::http::status::ok,
                         metrics.prometheus_text(),
                         "text/plain; version=0.0.4",
                         "no-store");
    }

    // One REST rendering of a capability: GET reads (input in the query string,
    // coerced by the input schema), POST actions (input as the JSON body). Auth,
    // validation, the call and the envelope are Registry::call + rest_*, exactly
    // the MCP path.
    boost::http::route_task serve_capability(boost::http::route_params& route,
                                             std::shared_ptr<const Capability> capability) {
        namespace http = boost::http;
        nlohmann::json input = nlohmann::json::object();
        std::string body;
        std::string set_cookie;
        http::status status = http::status::ok;

        if (route.req.method() == http::method::get) {
            std::vector<std::pair<std::string, std::string>> query;
            for (const auto& parameter : route.url.params())
                query.emplace_back(parameter.key, parameter.value);
            input = rest_input_from_query(*capability, query);
        } else {
            http::json_sink sink;
            auto [read_error, bytes_read] = co_await boost::capy::push_to(route.req_body, sink);
            if (read_error) {
                status = http::status::bad_request;
                body = error_body("invalid_input", "could not read the request body as JSON");
            } else if (bytes_read > 0) {
                input = to_nlohmann(sink.release());
                if (input.is_discarded()) {
                    status = http::status::bad_request;
                    body = error_body("invalid_input", "the request body is not valid JSON");
                }
            }
        }
        if (body.empty()) {
            ApiContext ctx = context_for(route);
            const ApiResult result = registry.call(capability->name, input, ctx);
            status = http::int_to_status(static_cast<unsigned>(rest_status(result)));
            body = rest_envelope(result).dump();
            const bool browser_sign_in =
                capability->name == "sign_up_password" || capability->name == "sign_in_password" ||
                capability->name == "verify_mfa" || capability->name == "sign_in_passkey";
            if (result.ok && browser_sign_in && result.value.contains("secret") &&
                result.value["secret"].is_string()) {
                set_cookie =
                    browser_session_cookie(route, result.value["secret"].get<std::string>());
            }
        }
        route.status(status);
        route.res.set(http::field::content_type, "application/json; charset=utf-8");
        route.res.set(http::field::cache_control, "no-store");
        if (!set_cookie.empty()) {
            route.res.set(http::field::set_cookie, set_cookie);
        }
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
        if (send_error)
            co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // POST /mcp — JSON-RPC 2.0 over HTTP. Notifications get 202 and no body.
    boost::http::route_task serve_mcp(boost::http::route_params& route) {
        namespace http = boost::http;
        std::string body;
        http::status status = http::status::ok;
        http::json_sink sink;
        auto [read_error, bytes_read] = co_await boost::capy::push_to(route.req_body, sink);
        nlohmann::json request;
        if (read_error || bytes_read == 0 ||
            (request = to_nlohmann(sink.release())).is_discarded()) {
            status = http::status::bad_request;
            body = nlohmann::json{{"jsonrpc", "2.0"},
                                  {"id", nullptr},
                                  {"error", {{"code", -32700}, {"message", "parse error"}}}}
                       .dump();
        } else {
            ApiContext ctx = context_for(route);
            const auto response = mcp_dispatch(registry, request, ctx, version_string());
            if (!response) {
                status = http::status::accepted;
            } else {
                body = *response;
            }
        }
        route.status(status);
        route.res.set(http::field::content_type, "application/json; charset=utf-8");
        route.res.set(http::field::cache_control, "no-store");
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
        if (send_error)
            co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // Anything no route claimed. The router falls through (route_next) past the
    // last handler and nothing answers otherwise — a request must never hang, so
    // this trailing middleware terminates every miss with a 404.
    boost::http::route_task serve_not_found(boost::http::route_params& route) {
        return send_text(route,
                         boost::http::status::not_found,
                         error_body("not_found", "no such route"),
                         "application/json; charset=utf-8",
                         "no-store");
    }
};

ApiServer::ApiServer(ServerOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

ApiServer::~ApiServer() {
    stop();
}

bool ApiServer::start() {
    namespace http = boost::http;
    namespace corosio = boost::corosio;
    namespace beast2 = boost::beast2;
    Impl& impl = *impl_;
    if (impl.is_running.load())
        return true;
    impl.failure_reason.clear();

    if (impl.options.store) {
        const audit::IdentityOptions identity_options{
            .configured_seed = impl.options.identity_seed,
            .instance_kek = impl.options.instance_kek,
        };
        auto signing_identity = audit::identity(*impl.options.store, identity_options);
        if (!signing_identity) {
            impl.failure_reason = signing_identity.error().message;
            return false;
        }
    }

    http::router<http::route_params> router;
    router.use([&impl, cors = http::cors()](http::route_params& route) -> http::route_task {
        const auto start = std::chrono::steady_clock::now();
        auto result = co_await cors(route);
        if (result.what() != http::route_what::next) {
            impl.record_request(route, impl.route_pattern_for(route), start);
        }
        co_return result;
    });
    router.add(http::method::get, "/", [&impl](http::route_params& route) -> http::route_task {
        return impl.observe_request(route, "/", impl.serve_web_root(route));
    });
    router.add(
        http::method::get, "/style.css", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/style.css", impl.serve_style(route));
        });
    router.add(
        http::method::get, "/healthz", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/healthz", impl.serve_healthz(route));
        });
    router.add(
        http::method::get, "/readyz", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/readyz", impl.serve_readyz(route));
        });
    router.add(
        http::method::get, "/metrics", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/metrics", impl.serve_metrics(route));
        });
    router.add(http::method::get, "/login", [&impl](http::route_params& route) -> http::route_task {
        return impl.observe_request(route, "/login", impl.serve_login(route));
    });
    router.add(
        http::method::post, "/session", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/session", impl.serve_session(route));
        });
    router.add(
        http::method::post, "/logout", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/logout", impl.serve_logout(route));
        });
    router.add(
        http::method::get, "/customers", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/customers", impl.serve_customers_page(route));
        });
    router.add(http::method::get,
               "/organizations",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/organizations", impl.serve_organizations_page(route));
               });
    router.add(http::method::get, "/sso", [&impl](http::route_params& route) -> http::route_task {
        return impl.observe_request(route, "/sso", impl.serve_sso_page(route));
    });
    router.add(http::method::get, "/teams", [&impl](http::route_params& route) -> http::route_task {
        return impl.observe_request(route, "/teams", impl.serve_teams_page(route));
    });
    router.add(
        http::method::get, "/members", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/members", impl.serve_members_page(route));
        });
    router.add(
        http::method::get, "/invitations", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/invitations", impl.serve_invitations_page(route));
        });
    router.add(
        http::method::get, "/grants", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/grants", impl.serve_grants_page(route));
        });
    router.add(
        http::method::get, "/access", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/access", impl.serve_access_page(route));
        });
    router.add(
        http::method::get, "/sign-up", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/sign-up", impl.serve_sign_up(route));
        });
    router.add(
        http::method::get, "/sign-in", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/sign-in", impl.serve_sign_in(route));
        });
    router.add(http::method::get, "/mfa", [&impl](http::route_params& route) -> http::route_task {
        return impl.observe_request(route, "/mfa", impl.serve_mfa(route));
    });
    router.add(
        http::method::get, "/passkeys", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/passkeys", impl.serve_passkeys(route));
        });
    router.add(http::method::get,
               "/passkey-sign-in",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/passkey-sign-in", impl.serve_passkey_sign_in(route));
               });
    router.add(
        http::method::get, "/sessions", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/sessions", impl.serve_sessions(route));
        });
    router.add(
        http::method::get, "/tokens", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/tokens", impl.serve_tokens(route));
        });
    router.add(http::method::post,
               "/identity/sign-up",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/identity/sign-up", impl.serve_identity_sign_up(route));
               });
    router.add(http::method::post,
               "/identity/sign-in",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/identity/sign-in", impl.serve_identity_sign_in(route));
               });
    router.add(http::method::post,
               "/identity/mfa/verify",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/identity/mfa/verify", impl.serve_identity_mfa_verify(route));
               });
    router.add(http::method::post,
               "/identity/mfa/enroll",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/identity/mfa/enroll", impl.serve_identity_mfa_enroll(route));
               });
    router.add(http::method::post,
               "/identity/mfa/disable",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/identity/mfa/disable", impl.serve_identity_mfa_disable(route));
               });
    router.add(http::method::post,
               "/identity/oauth",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/identity/oauth", impl.serve_identity_oauth(route));
               });
    router.add(http::method::post,
               "/identity/session/revoke",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(route,
                                               "/identity/session/revoke",
                                               impl.serve_identity_session_revoke(route));
               });
    router.add(http::method::post,
               "/identity/device/revoke",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/identity/device/revoke", impl.serve_identity_device_revoke(route));
               });
    router.add(http::method::post,
               "/identity/token/create",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/identity/token/create", impl.serve_identity_token_create(route));
               });
    router.add(
        http::method::get, "/repositories", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(
                route, "/repositories", impl.serve_repositories_page(route));
        });
    router.add(
        http::method::get, "/repository", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/repository", impl.serve_repository_page(route));
        });
    router.add(
        http::method::get, "/search", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/search", impl.serve_search_page(route));
        });
    router.add(http::method::get, "/audit", [&impl](http::route_params& route) -> http::route_task {
        return impl.observe_request(route, "/audit", impl.serve_audit_page(route));
    });
    router.add(http::method::get,
               "/v1/oauth/:provider/callback",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/v1/oauth/:provider/callback", impl.serve_oauth_callback(route));
               });
    router.add(http::method::get,
               "/v1/openapi.json",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/v1/openapi.json", impl.serve_openapi(route));
               });
    for (const auto& capability : impl.registry.list()) {
        const auto method =
            rest_method(*capability) == "GET" ? http::method::get : http::method::post;
        const std::string path = rest_path(*capability);
        router.add(
            method, path, [&impl, capability, path](http::route_params& route) -> http::route_task {
                return impl.observe_request(route, path, impl.serve_capability(route, capability));
            });
    }
    router.add(http::method::post, "/mcp", [&impl](http::route_params& route) -> http::route_task {
        return impl.observe_request(route, "/mcp", impl.serve_mcp(route));
    });
    router.use([&impl](http::route_params& route) -> http::route_task {
        return impl.observe_request(route, "unmatched", impl.serve_not_found(route));
    });

    impl.server = std::make_unique<beast2::http_server>(
        impl.io_context,
        static_cast<std::size_t>(impl.options.worker_coroutines),
        std::move(router),
        http::make_parser_config(http::parser_config(true)),
        http::make_serializer_config(http::serializer_config()));

    corosio::ipv4_address address(impl.options.listen_address);
    auto bind_error = impl.server->bind(corosio::endpoint(address, impl.options.port));
    if (bind_error) {
        impl.failure_reason = "bind failed: " + bind_error.message();
        impl.server.reset();
        return false;
    }
    impl.bound_port = impl.server->local_endpoint().port();
    impl.server->start();
    impl.io_thread = std::thread([&impl] { impl.io_context.run(); });
    impl.is_running.store(true);
    impl.metrics.set_gauge("lake_up", {}, 1.0);
    return true;
}

void ApiServer::stop() {
    Impl& impl = *impl_;
    if (!impl.is_running.exchange(false))
        return;
    impl.metrics.set_gauge("lake_up", {}, 0.0);
    impl.server->stop();
    impl.io_context.stop();
    if (impl.io_thread.joinable())
        impl.io_thread.join();
    impl.work_cv.notify_all();
}

bool ApiServer::running() const {
    return impl_->is_running.load();
}
unsigned short ApiServer::port() const {
    return impl_->bound_port;
}
const std::string& ApiServer::reason() const {
    return impl_->failure_reason;
}

void ApiServer::drain() {}

void ApiServer::wait_for_work(std::chrono::milliseconds max_idle) {
    Impl& impl = *impl_;
    std::unique_lock lock(impl.work_mutex);
    impl.work_cv.wait_for(lock, max_idle, [&impl] { return !impl.is_running.load(); });
}

} // namespace lake::api
