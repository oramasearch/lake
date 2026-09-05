// lake — the HTTP adapter. Beast2 (HTTP/1) on Capy + Corosio, the same stack
// Zetta serves with (zetta/src/core/api/http_server.cpp). This is the ONE
// translation unit that knows about beast2 / boost::http / Boost.JSON. It
// serves the hello page, the registry's JSON and binary HTTP renderings, the
// OpenAPI document and MCP (POST /mcp). Every capability call goes through
// Registry::call so authorization, validation and audit remain centralized.

#include "lake/api/server.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/beast2/http_server.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/buffers/buffer_copy.hpp>
#include <boost/capy/buffers/vector_dynamic_buffer.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/io/push_to.hpp>
#include <boost/capy/read.hpp>
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
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "lake/api/apps.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/mcp.hpp"
#include "lake/api/oauth.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/rest.hpp"
#include "lake/api/sync_store.hpp"
#include "lake/api/web_ui.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/callers.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/pki.hpp"
#include "lake/control/store.hpp"
#include "lake/search/index.hpp"
#include "lake/telemetry/metrics.hpp"
#include "lake/transparency/log.hpp"
#include "lake/transparency/timestamps.hpp"
#include "lake/transparency/tuf.hpp"
#include "lake/version.hpp"

#include <liquid/git.hpp>
#include <liquid/hash.hpp>
#include <liquid/repository.hpp>
#include <liquid/runtime.hpp>
#include <liquid/store.hpp>
#include <liquid/sync.hpp>

namespace lake::api {
namespace {

constexpr auto sync_content_type = "application/vnd.liquid.sync-v1";
constexpr auto sync_error_content_type = "application/vnd.liquid.error-v1";
constexpr auto maximum_sync_body_size = std::size_t{64} * 1024 * 1024;

std::string base64_encode(std::span<const std::byte> input) {
    if (input.empty()) {
        return {};
    }
    std::string output(4 * ((input.size() + 2) / 3), '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()),
                                     reinterpret_cast<const unsigned char*>(input.data()),
                                     static_cast<int>(input.size()));
    output.resize(static_cast<std::size_t>(size));
    return output;
}

SyncStoreResult<std::vector<std::byte>> base64_decode(std::string_view input) {
    if (input.empty()) {
        return std::vector<std::byte>{};
    }
    if (input.size() % 4 != 0) {
        return std::unexpected(SyncStoreError{"invalid_input", "invalid base64 data", "export"});
    }
    std::vector<std::byte> output((input.size() / 4) * 3);
    const int size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(output.data()),
                                     reinterpret_cast<const unsigned char*>(input.data()),
                                     static_cast<int>(input.size()));
    if (size < 0) {
        return std::unexpected(SyncStoreError{"invalid_input", "invalid base64 data", "export"});
    }
    std::size_t padding = 0;
    if (!input.empty() && input.back() == '=') {
        ++padding;
    }
    if (input.size() > 1 && input[input.size() - 2] == '=') {
        ++padding;
    }
    output.resize(static_cast<std::size_t>(size) - padding);
    return output;
}

std::string node_id_text(const liquid::node_id& value) {
    std::string output;
    output.reserve(value.size() * 2);
    for (const std::byte byte : value) {
        output += std::format("{:02x}", std::to_integer<unsigned char>(byte));
    }
    return output;
}

SyncStoreResult<liquid::node_id> parse_node_id(std::string_view input) {
    if (input.size() != 32) {
        return std::unexpected(
            SyncStoreError{"invalid_input", "invalid node identifier", "export"});
    }
    liquid::node_id output{};
    for (std::size_t index = 0; index < output.size(); ++index) {
        unsigned value = 0;
        const auto parsed =
            std::from_chars(input.data() + index * 2, input.data() + index * 2 + 2, value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != input.data() + index * 2 + 2) {
            return std::unexpected(
                SyncStoreError{"invalid_input", "invalid node identifier", "export"});
        }
        output[index] = static_cast<std::byte>(value);
    }
    return output;
}

nlohmann::json sync_item_json(const liquid::sync_item& item) {
    return {{"kind", static_cast<unsigned>(item.kind)},
            {"author",
             {{"taap_id", item.author.taap_id},
              {"algorithm", item.author.algorithm},
              {"fingerprint", item.author.fingerprint},
              {"public_key", base64_encode(item.author.public_key)},
              {"key_fingerprint", item.author.key_fingerprint}}},
            {"repository", node_id_text(item.repository)},
            {"directory", node_id_text(item.directory)},
            {"root", item.root},
            {"name", item.name},
            {"reference", liquid::to_string(item.reference.value)},
            {"body", base64_encode(item.body)},
            {"signature", base64_encode(item.signature)}};
}

SyncStoreResult<liquid::sync_item> sync_item_from_json(const nlohmann::json& value) {
    const auto repository = parse_node_id(value.value("repository", ""));
    if (!repository) {
        return std::unexpected(repository.error());
    }
    const auto directory = parse_node_id(value.value("directory", ""));
    if (!directory) {
        return std::unexpected(directory.error());
    }
    const auto public_key = base64_decode(value.at("author").value("public_key", ""));
    if (!public_key) {
        return std::unexpected(public_key.error());
    }
    const auto body = base64_decode(value.value("body", ""));
    if (!body) {
        return std::unexpected(body.error());
    }
    const auto signature = base64_decode(value.value("signature", ""));
    if (!signature) {
        return std::unexpected(signature.error());
    }
    liquid::sync_item output;
    const unsigned kind = value.value("kind", 0U);
    if (kind < static_cast<unsigned>(liquid::sync_item_kind::drip) ||
        kind > static_cast<unsigned>(liquid::sync_item_kind::reference)) {
        return std::unexpected(SyncStoreError{"invalid_input", "invalid sync item kind", "export"});
    }
    output.kind = static_cast<liquid::sync_item_kind>(kind);
    output.author = {.taap_id = value.at("author").value("taap_id", ""),
                     .algorithm = value.at("author").value("algorithm", ""),
                     .fingerprint = value.at("author").value("fingerprint", ""),
                     .public_key = *public_key,
                     .key_fingerprint = value.at("author").value("key_fingerprint", "")};
    output.repository = *repository;
    output.directory = *directory;
    output.root = value.value("root", false);
    output.name = value.value("name", "");
    const std::string reference = value.value("reference", "");
    if (!reference.empty()) {
        const auto parsed = liquid::parse_digest(reference);
        if (!parsed) {
            return std::unexpected(
                SyncStoreError{"invalid_input", "invalid sync item reference", "export"});
        }
        output.reference = liquid::change_id{*parsed};
    }
    output.body = *body;
    output.signature = *signature;
    return output;
}

class ArchiveBlobSource final : public liquid::remote_blob_source {
public:
    explicit ArchiveBlobSource(std::map<std::string, std::vector<std::byte>> contents = {})
        : contents_(std::move(contents)) {}

    liquid::runtime::task<std::vector<liquid::digest>>
    find_missing(std::string, std::vector<liquid::digest> blobs) override {
        co_return blobs;
    }

    liquid::runtime::task<std::uint64_t> read_range(std::string,
                                                    liquid::digest blob,
                                                    std::uint64_t offset,
                                                    std::span<std::byte> output) override {
        const auto found = contents_.find(liquid::to_string(blob));
        if (found == contents_.end() || offset > found->second.size()) {
            co_return std::unexpected(liquid::error{
                liquid::errc::not_found, "archive object is unavailable", liquid::to_string(blob)});
        }
        const std::size_t count =
            std::min(output.size(), found->second.size() - static_cast<std::size_t>(offset));
        std::ranges::copy(found->second.begin() + static_cast<std::ptrdiff_t>(offset),
                          found->second.begin() + static_cast<std::ptrdiff_t>(offset + count),
                          output.begin());
        co_return count;
    }

    liquid::runtime::task<void>
    write(std::string, liquid::digest blob, std::vector<std::byte> contents) override {
        contents_.insert_or_assign(liquid::to_string(blob), std::move(contents));
        co_return liquid::result<void>{};
    }

    const std::map<std::string, std::vector<std::byte>>& contents() const noexcept {
        return contents_;
    }

private:
    std::map<std::string, std::vector<std::byte>> contents_;
};

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

boost::http::route_task send_bytes(boost::http::route_params& route,
                                   boost::http::status status,
                                   std::string_view content_type,
                                   std::span<const std::byte> body,
                                   std::string_view etag = {},
                                   std::string_view revision = {},
                                   bool head = false) {
    namespace http = boost::http;
    route.status(status);
    route.res.set(http::field::content_type, content_type);
    route.res.set(http::field::cache_control, "no-store");
    route.res.erase(http::field::etag);
    route.res.erase(http::field::content_length);
    if (!etag.empty()) {
        route.res.set(http::field::etag, etag);
    }
    if (!revision.empty()) {
        route.res.set("X-Repo-Commit", revision);
    }
    route.res.set_payload_size(body.size());
    if (head) {
        auto [send_error] = co_await route.res_body.commit_eof(0);
        if (send_error) {
            co_return http::route_error(send_error);
        }
        co_return http::route_done;
    }
    std::size_t offset = 0;
    for (;;) {
        auto buffers = std::array<boost::capy::mutable_buffer, 16>{};
        const auto output = route.res_body.prepare(buffers);
        const auto remaining = body.subspan(offset);
        const auto copied = boost::capy::buffer_copy(
            output, boost::capy::const_buffer(remaining.data(), remaining.size()));
        offset += copied;
        if (offset == body.size()) {
            auto [send_error] = co_await route.res_body.commit_eof(copied);
            if (send_error) {
                co_return http::route_error(send_error);
            }
            co_return http::route_done;
        }
        auto [send_error] = co_await route.res_body.commit(copied);
        if (send_error) {
            co_return http::route_error(send_error);
        }
    }
}

boost::capy::task<std::expected<std::vector<unsigned char>, std::error_code>>
read_sync_body(boost::http::route_params& route) {
    auto body = std::vector<unsigned char>{};
    auto [read_error, size] = co_await boost::capy::read(
        route.req_body, boost::capy::dynamic_buffer(body, maximum_sync_body_size));
    static_cast<void>(size);
    if (read_error) {
        co_return std::unexpected(read_error);
    }
    co_return body;
}

std::span<const std::byte> as_bytes(const std::vector<unsigned char>& value) {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

liquid::errc liquid_error_code(std::string_view code) {
    if (code == "invalid_argument" || code == "invalid_input") {
        return liquid::errc::invalid_argument;
    }
    if (code == "corrupt") {
        return liquid::errc::corrupt;
    }
    if (code == "unauthorized") {
        return liquid::errc::unauthorized;
    }
    if (code == "not_found") {
        return liquid::errc::not_found;
    }
    if (code == "already_exists") {
        return liquid::errc::already_exists;
    }
    if (code == "conflict") {
        return liquid::errc::conflict;
    }
    if (code == "locked") {
        return liquid::errc::locked;
    }
    if (code == "busy") {
        return liquid::errc::busy;
    }
    if (code == "unavailable" || code == "store_unavailable") {
        return liquid::errc::unavailable;
    }
    if (code == "io") {
        return liquid::errc::io;
    }
    return liquid::errc::internal;
}

boost::http::status sync_status(const liquid::error& error) {
    switch (error.code) {
    case liquid::errc::invalid_argument:
    case liquid::errc::corrupt:
        return boost::http::status::bad_request;
    case liquid::errc::unauthorized:
        return boost::http::status::unauthorized;
    case liquid::errc::not_found:
        return boost::http::status::not_found;
    case liquid::errc::already_exists:
    case liquid::errc::conflict:
        return boost::http::status::conflict;
    case liquid::errc::locked:
        return boost::http::status::locked;
    case liquid::errc::busy:
        return boost::http::status::too_many_requests;
    case liquid::errc::unavailable:
        return boost::http::status::service_unavailable;
    default:
        return boost::http::status::internal_server_error;
    }
}

boost::http::route_task send_sync_error(boost::http::route_params& route,
                                        const liquid::error& error) {
    auto body = liquid::sync_server::encode_error(error);
    if (!body) {
        co_return co_await send_bytes(
            route, boost::http::status::internal_server_error, sync_error_content_type, {});
    }
    co_return co_await send_bytes(route, sync_status(error), sync_error_content_type, *body);
}

liquid::error sync_error_of(const ApiResult& result, const ApiContext& context) {
    if (context.sync_error) {
        return liquid::error{liquid_error_code(context.sync_error->code),
                             context.sync_error->message,
                             context.sync_error->context};
    }
    return liquid::error{
        liquid_error_code(result.error.code), result.error.message, result.error.path};
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

std::string route_value(const boost::http::route_params& route, std::string_view name) {
    for (const auto& [key, value] : route.params) {
        if (key == name) {
            return value;
        }
    }
    return {};
}

std::expected<std::string, std::string> decode_base64url(std::string value) {
    for (char& character : value) {
        if (character == '-') {
            character = '+';
        } else if (character == '_') {
            character = '/';
        }
    }
    while (value.size() % 4 != 0) {
        value += '=';
    }
    std::string decoded((value.size() / 4) * 3, '\0');
    const int written = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(decoded.data()),
                                        reinterpret_cast<const unsigned char*>(value.data()),
                                        static_cast<int>(value.size()));
    if (written < 0) {
        return std::unexpected("base64url value is invalid");
    }
    std::size_t padding = 0;
    if (!value.empty() && value.ends_with('=')) {
        ++padding;
    }
    if (value.size() > 1 && value.ends_with("==")) {
        ++padding;
    }
    decoded.resize(static_cast<std::size_t>(written) - padding);
    return decoded;
}

std::string request_origin(const boost::http::route_params& route) {
    const std::string scheme = request_is_https(route) ? "https" : "http";
    const std::string host = route.req.count(boost::http::field::host) == 0
                                 ? "localhost"
                                 : std::string(route.req.at(boost::http::field::host));
    return scheme + "://" + host;
}

std::string request_host(const boost::http::route_params& route) {
    if (route.req.count(boost::http::field::host) == 0) {
        return "localhost";
    }
    std::string host(route.req.at(boost::http::field::host));
    const std::size_t port = host.find(':');
    if (port != std::string::npos) {
        host.resize(port);
    }
    return host;
}

std::string application_redirect_location(const control::Application& application,
                                          const ApiResult& result) {
    if (application.redirect_uris.empty()) {
        return "/apps/" + application.id + "/sign-in";
    }
    std::string location = application.redirect_uris.front();
    const std::string token = result.value.value("access_token", "");
    if (token.empty()) {
        return location;
    }
    location += location.contains('#') ? "&" : "#";
    location += "access_token=" + oauth_url_encode(token);
    location += "&token_type=Bearer";
    location += "&expires_in=" + std::to_string(result.value.value("expires_in", 900));
    return location;
}

std::vector<std::string> form_lines(std::string_view value) {
    std::vector<std::string> output;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find('\n', start);
        std::string line(value.substr(
            start, end == std::string_view::npos ? value.size() - start : end - start));
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            output.push_back(std::move(line));
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return output;
}

std::optional<std::uint64_t> decimal_value(std::string_view value) {
    std::uint64_t output = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return output;
}

std::uint64_t query_page_limit(const boost::http::route_params& route) {
    const std::string text = query_value(route, "limit");
    if (text.empty()) {
        return 100;
    }
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() && value > 0 && value <= 1'000
               ? value
               : 100;
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

boost::http::status scim_status(const ApiResult& result) {
    if (result.ok) {
        return boost::http::status::ok;
    }
    if (result.error.code == "unauthorized") {
        return boost::http::status::unauthorized;
    }
    return boost::http::int_to_status(static_cast<unsigned>(rest_status(result)));
}

std::string scim_error_body(const ApiResult& result) {
    return nlohmann::json{
        {"schemas", nlohmann::json::array({"urn:ietf:params:scim:api:messages:2.0:Error"})},
        {"status", std::to_string(static_cast<unsigned>(scim_status(result)))},
        {"detail", result.error.message}}
        .dump();
}

int query_integer(const boost::http::route_params& route, std::string_view name, int fallback) {
    const std::string value = query_value(route, name);
    if (value.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed < 0 || parsed > 1000) {
        return fallback;
    }
    return static_cast<int>(parsed);
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

control::Application application_from_json(const nlohmann::json& value) {
    return {json_string(value, "id"),
            json_string(value, "customer_id"),
            json_string(value, "pool_id"),
            json_string(value, "name"),
            value.value("allowed_origins", std::vector<std::string>{}),
            value.value("redirect_uris", std::vector<std::string>{}),
            json_string(value, "custom_sign_in_domain"),
            value.value("theme", nlohmann::json::object()),
            json_string(value, "created_at"),
            json_string(value, "updated_at")};
}

control::ApplicationUser application_user_from_json(const nlohmann::json& value) {
    return {json_string(value, "application_id"),
            json_string(value, "user_id"),
            json_string(value, "taap_id"),
            json_string(value, "email"),
            json_string(value, "display_name"),
            json_string(value, "status"),
            json_string(value, "created_at"),
            json_string(value, "updated_at")};
}

control::ApplicationOrganization application_organization_from_json(const nlohmann::json& value) {
    return {json_string(value, "id"),
            json_string(value, "application_id"),
            json_string(value, "taap_id"),
            json_string(value, "name"),
            value.value("metadata", nlohmann::json::object()),
            json_string(value, "created_at"),
            json_string(value, "updated_at")};
}

control::ApplicationWebhookEvent application_event_from_json(const nlohmann::json& value) {
    return {json_string(value, "id"),
            json_string(value, "application_id"),
            json_string(value, "event"),
            value.value("payload", nlohmann::json::object()),
            json_string(value, "created_at")};
}

control::Repository repository_from_json(const nlohmann::json& value) {
    return {json_string(value, "id"),
            json_string(value, "customer_id"),
            json_string(value, "name"),
            json_string(value, "description"),
            json_string(value, "visibility"),
            json_string(value, "deleted_at"),
            json_string(value, "retention_until"),
            json_string(value, "created_at"),
            json_string(value, "updated_at")};
}

control::ForgeRecord forge_record_from_json(const nlohmann::json& value) {
    return {json_string(value, "id"),
            json_string(value, "repository_id"),
            json_string(value, "kind"),
            json_string(value, "created_at"),
            json_string(value, "updated_at"),
            value.value("value", nlohmann::json::object())};
}

RepositoryContentRef repository_content_ref_from_json(const nlohmann::json& value) {
    return {json_string(value, "name"), json_string(value, "change")};
}

RepositoryTreeEntry repository_tree_entry_from_json(const nlohmann::json& value) {
    return {json_string(value, "path"),
            json_string(value, "kind"),
            json_string(value, "blob"),
            value.value("mode", 0U),
            json_string(value, "node_id")};
}

RepositoryFile repository_file_from_json(const nlohmann::json& value) {
    return {json_string(value, "path"),
            json_string(value, "blob"),
            json_string(value, "contents"),
            value.value("size", std::uint64_t{0})};
}

RepositoryChange repository_change_from_json(const nlohmann::json& value) {
    return {json_string(value, "id"),
            value.value("parents", std::vector<std::string>{}),
            json_string(value, "author"),
            json_string(value, "message"),
            json_string(value, "occurred_at")};
}

RepositoryConflict repository_conflict_from_json(const nlohmann::json& value) {
    return {json_string(value, "path"), value.value("blobs", std::vector<std::string>{})};
}

RepositoryDiffEntry repository_diff_entry_from_json(const nlohmann::json& value) {
    RepositoryDiffEntry entry{
        .path = json_string(value, "path"),
        .old_path = json_string(value, "old_path"),
        .kind = json_string(value, "kind"),
        .changes = value.value("changes", std::vector<std::string>{}),
        .old_blob = json_string(value, "old_blob"),
        .new_blob = json_string(value, "new_blob"),
        .old_conflicts = value.value("old_conflicts", std::vector<std::string>{}),
        .new_conflicts = value.value("new_conflicts", std::vector<std::string>{}),
    };
    if (value.contains("old_mode") && value.at("old_mode").is_number_unsigned()) {
        entry.old_mode = value.at("old_mode").get<std::uint32_t>();
    }
    if (value.contains("new_mode") && value.at("new_mode").is_number_unsigned()) {
        entry.new_mode = value.at("new_mode").get<std::uint32_t>();
    }
    if (value.contains("line_hunks") && value.at("line_hunks").is_array()) {
        entry.line_hunks.emplace();
        for (const nlohmann::json& hunk : value.at("line_hunks")) {
            entry.line_hunks->push_back({hunk.value("old_line", std::uint64_t{0}),
                                         hunk.value("old_count", std::uint64_t{0}),
                                         hunk.value("new_line", std::uint64_t{0}),
                                         hunk.value("new_count", std::uint64_t{0}),
                                         hunk.value("removed_lines", std::vector<std::string>{}),
                                         hunk.value("added_lines", std::vector<std::string>{})});
        }
    }
    return entry;
}

RepositoryArtifact repository_artifact_from_json(const nlohmann::json& value) {
    RepositoryArtifact artifact{
        .id = json_string(value, "id"),
        .action = json_string(value, "action"),
        .exit_code = value.value("exit_code", 0),
        .cache_status = json_string(value, "cache_status"),
        .divergent = value.value("divergent", false),
        .verification = json_string(value, "verification"),
        .worker = json_string(value, "worker"),
        .observed_at = json_string(value, "observed_at"),
        .started_at = json_string(value, "started_at"),
        .finished_at = json_string(value, "finished_at"),
    };
    for (const nlohmann::json& output : value.value("outputs", nlohmann::json::array())) {
        artifact.outputs.push_back({json_string(output, "path"),
                                    output.value("mode", std::uint32_t{0}),
                                    json_string(output, "blob")});
    }
    if (value.contains("timing") && value.at("timing").is_object()) {
        const nlohmann::json& timing = value.at("timing");
        artifact.timing = {timing.value("queued_ms", std::uint64_t{0}),
                           timing.value("sandbox_ms", std::uint64_t{0}),
                           timing.value("exec_ms", std::uint64_t{0}),
                           timing.value("hash_ms", std::uint64_t{0}),
                           timing.value("store_ms", std::uint64_t{0})};
    }
    for (const nlohmann::json& attestation : value.value("attestations", nlohmann::json::array())) {
        artifact.attestations.push_back({json_string(attestation, "id"),
                                         json_string(attestation, "profile"),
                                         json_string(attestation, "subject"),
                                         json_string(attestation, "issuer"),
                                         attestation.value("verified", false),
                                         json_string(attestation, "reason")});
    }
    if (value.contains("trace") && value.at("trace").is_object()) {
        const nlohmann::json& trace = value.at("trace");
        artifact.trace = RepositoryArtifactTrace{json_string(trace, "id"),
                                                 trace.value("wall_ms", std::uint64_t{0}),
                                                 trace.value("actions", std::uint64_t{0}),
                                                 trace.value("dependencies", std::uint64_t{0}),
                                                 trace.value("cache_hits", std::uint64_t{0}),
                                                 trace.value("cache_misses", std::uint64_t{0})};
    }
    return artifact;
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

control::SecretMount secret_mount_from_json(const nlohmann::json& value) {
    return {json_string(value, "id"),
            json_string(value, "customer_id"),
            json_string(value, "organization_id"),
            json_string(value, "name"),
            json_string(value, "description"),
            json_string(value, "created_at"),
            json_string(value, "updated_at")};
}

control::SecretMetadata secret_metadata_from_json(const nlohmann::json& value) {
    std::vector<control::SecretVersionMetadata> versions;
    for (const nlohmann::json& version : value.at("versions")) {
        versions.push_back({version.at("version").get<std::uint64_t>(),
                            json_string(version, "created_at"),
                            json_string(version, "deleted_at"),
                            json_string(version, "destroyed_at")});
    }
    return {json_string(value, "mount_id"),
            json_string(value, "name"),
            value.at("current_version").get<std::uint64_t>(),
            value.value("custom_metadata", nlohmann::json::object()),
            std::move(versions),
            json_string(value, "created_at"),
            json_string(value, "updated_at")};
}

control::EncryptionKey encryption_key_from_json(const nlohmann::json& value) {
    return {json_string(value, "id"),
            json_string(value, "mount_id"),
            json_string(value, "name"),
            json_string(value, "derived_from"),
            value.at("current_version").get<std::uint64_t>(),
            json_string(value, "created_at"),
            json_string(value, "updated_at")};
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
                                std::vector<std::byte> contents,
                                std::uint64_t storage_quota_bytes) override {
        std::lock_guard lock(mutex_);
        const auto expected = liquid::parse_digest(digest_text);
        if (!expected) {
            return std::unexpected(error_of(expected.error()));
        }
        auto store = open_customer(customer_id);
        if (!store) {
            return std::unexpected(store.error());
        }
        const auto present = liquid::runtime::block_on((*store)->contains(*expected));
        if (!present) {
            return std::unexpected(error_of(present.error()));
        }
        auto stored_bytes = unique_stored_bytes_locked(customer_id);
        if (!stored_bytes) {
            return std::unexpected(stored_bytes.error());
        }
        if (!*present && exceeds_quota(*stored_bytes, contents.size(), storage_quota_bytes)) {
            return std::unexpected(quota_error(*stored_bytes, storage_quota_bytes));
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

    SyncStoreResult<std::uint64_t> unique_stored_bytes(const std::string& customer_id) override {
        std::lock_guard lock(mutex_);
        auto store = open_customer(customer_id);
        if (!store) {
            return std::unexpected(store.error());
        }
        return unique_stored_bytes_locked(customer_id);
    }

    SyncStoreResult<std::vector<std::pair<std::string, std::string>>>
    refs(const std::string& customer_id, const std::string& repository_id) override {
        std::lock_guard lock(mutex_);
        auto state = open_repository(customer_id, repository_id);
        if (!state) {
            return std::unexpected(state.error());
        }
        auto references = liquid::runtime::block_on((*state)->subject->refs());
        if (!references) {
            return std::unexpected(error_of(references.error()));
        }
        std::vector<std::pair<std::string, std::string>> output;
        output.reserve(references->size());
        for (const auto& reference : *references) {
            output.emplace_back(reference.name, liquid::to_string(reference.id.value));
        }
        return output;
    }

    SyncStoreResult<std::vector<std::byte>>
    find_missing(const std::string& customer_id,
                 const std::string& repository_id,
                 std::span<const std::byte> request) override {
        std::lock_guard lock(mutex_);
        auto state = open_repository(customer_id, repository_id);
        if (!state) {
            return std::unexpected(state.error());
        }
        auto server = liquid::sync_server{*(*state)->subject, *(*state)->objects};
        return from_liquid(liquid::runtime::block_on(server.handle_find_missing(request)));
    }

    SyncStoreResult<std::vector<std::byte>> push(const std::string& customer_id,
                                                 const std::string& repository_id,
                                                 std::span<const std::byte> request,
                                                 SyncPushAuthorizer authorize,
                                                 std::uint64_t storage_quota_bytes) override {
        std::lock_guard lock(mutex_);
        auto state = open_repository(customer_id, repository_id);
        if (!state) {
            return std::unexpected(state.error());
        }
        auto stored_bytes = unique_stored_bytes_locked(customer_id);
        if (!stored_bytes) {
            return std::unexpected(stored_bytes.error());
        }
        if (storage_quota_bytes != 0 && *stored_bytes >= storage_quota_bytes) {
            return std::unexpected(quota_error(*stored_bytes, storage_quota_bytes));
        }
        liquid::sync_server_options options;
        options.authorize =
            [authorize = std::move(authorize)](const liquid::sync_item& item,
                                               std::span<const liquid::sync_ref_update> updates)
            -> liquid::runtime::task<std::vector<liquid::sync_ref_verdict>> {
            std::vector<BranchRefUpdate> reference_updates;
            reference_updates.reserve(updates.size());
            for (const liquid::sync_ref_update& update : updates) {
                reference_updates.push_back(BranchRefUpdate{
                    .name = update.name,
                    .old_head = update.old_head
                                    ? std::optional{liquid::to_string(update.old_head->value)}
                                    : std::nullopt,
                    .new_head = liquid::to_string(update.new_head.value),
                    .fast_forward = update.fast_forward,
                });
            }
            const SyncPushItem input{
                .kind = item.kind == liquid::sync_item_kind::reference ? SyncPushItemKind::Reference
                                                                       : SyncPushItemKind::Change,
                .change_id = liquid::to_string(item.reference.value),
            };
            auto result = authorize(input, reference_updates);
            if (!result) {
                co_return std::unexpected(liquid_error(result.error()));
            }
            std::vector<liquid::sync_ref_verdict> verdicts;
            verdicts.reserve(result->size());
            for (const SyncRefVerdict& verdict : *result) {
                verdicts.push_back({
                    .name = verdict.name,
                    .verdict = verdict.verdict == SyncRefVerdictKind::Accepted
                                   ? liquid::sync_ref_verdict_kind::accepted
                                   : liquid::sync_ref_verdict_kind::refused,
                    .reason = verdict.reason,
                });
            }
            co_return verdicts;
        };
        auto server =
            liquid::sync_server{*(*state)->subject, *(*state)->objects, std::move(options)};
        return from_liquid(liquid::runtime::block_on(server.handle_push(request)));
    }

    SyncStoreResult<std::vector<std::byte>> pull(const std::string& customer_id,
                                                 const std::string& repository_id,
                                                 std::span<const std::byte> request) override {
        std::lock_guard lock(mutex_);
        auto state = open_repository(customer_id, repository_id);
        if (!state) {
            return std::unexpected(state.error());
        }
        auto server = liquid::sync_server{*(*state)->subject, *(*state)->objects};
        return from_liquid(liquid::runtime::block_on(server.handle_pull(request)));
    }

    SyncStoreResult<std::uint64_t> upload_chunks(const std::string& customer_id,
                                                 const std::string& repository_id,
                                                 std::span<const std::byte> request,
                                                 std::uint64_t storage_quota_bytes) override {
        std::lock_guard lock(mutex_);
        auto state = open_repository(customer_id, repository_id);
        if (!state) {
            return std::unexpected(state.error());
        }
        auto stored_bytes = unique_stored_bytes_locked(customer_id);
        if (!stored_bytes) {
            return std::unexpected(stored_bytes.error());
        }
        auto incoming_bytes = unique_incoming_bytes(*(*state)->objects, request);
        if (!incoming_bytes) {
            return std::unexpected(incoming_bytes.error());
        }
        if (exceeds_quota(*stored_bytes, *incoming_bytes, storage_quota_bytes)) {
            return std::unexpected(quota_error(*stored_bytes, storage_quota_bytes));
        }
        auto server = liquid::sync_server{*(*state)->subject, *(*state)->objects};
        auto stored = from_liquid(liquid::runtime::block_on(server.handle_chunks(request)));
        if (!stored) {
            return std::unexpected(stored.error());
        }
        return unique_stored_bytes_locked(customer_id);
    }

    SyncStoreResult<std::vector<std::byte>> read_chunk(const std::string& customer_id,
                                                       const std::string& repository_id,
                                                       const std::string& digest_text,
                                                       std::uint64_t offset,
                                                       std::uint64_t length) override {
        std::lock_guard lock(mutex_);
        if (length > std::numeric_limits<std::size_t>::max()) {
            return std::unexpected(SyncStoreError{
                "invalid_argument", "chunk range length is too large", "sync server"});
        }
        const auto digest = liquid::parse_digest(digest_text);
        if (!digest) {
            return std::unexpected(error_of(digest.error()));
        }
        auto state = open_repository(customer_id, repository_id);
        if (!state) {
            return std::unexpected(state.error());
        }
        auto server = liquid::sync_server{*(*state)->subject, *(*state)->objects};
        auto output = std::vector<std::byte>(static_cast<std::size_t>(length));
        auto read = liquid::runtime::block_on(server.handle_chunks(*digest, offset, output));
        if (!read) {
            return std::unexpected(error_of(read.error()));
        }
        output.resize(static_cast<std::size_t>(*read));
        return output;
    }

    SyncStoreResult<std::vector<RepositoryContentRef>>
    repository_refs(const std::string& customer_id, const std::string& repository_id) override {
        std::lock_guard lock(mutex_);
        auto repository = open_repository(customer_id, repository_id);
        if (!repository) {
            return std::unexpected(repository.error());
        }
        const auto refs = liquid::runtime::block_on((*repository)->subject->refs());
        if (!refs) {
            return std::unexpected(error_of(refs.error()));
        }
        std::vector<RepositoryContentRef> output;
        output.reserve(refs->size());
        for (const liquid::ref& ref : *refs) {
            output.push_back({ref.name, liquid::to_string(ref.id.value)});
        }
        return output;
    }

    SyncStoreResult<std::vector<RepositoryTreeEntry>>
    repository_tree(const std::string& customer_id,
                    const std::string& repository_id,
                    const std::string& ref) override {
        std::lock_guard lock(mutex_);
        auto repository = open_repository(customer_id, repository_id);
        if (!repository) {
            return std::unexpected(repository.error());
        }
        auto tree = tree_at_ref(*(*repository)->subject, ref);
        if (!tree) {
            return std::unexpected(tree.error());
        }
        std::vector<RepositoryTreeEntry> output;
        const auto walked =
            walk_tree(*(*repository)->subject, *(*repository)->objects, *tree, "", output);
        if (!walked) {
            return std::unexpected(walked.error());
        }
        return output;
    }

    SyncStoreResult<RepositoryFile> repository_file(const std::string& customer_id,
                                                    const std::string& repository_id,
                                                    const std::string& ref,
                                                    const std::string& path) override {
        std::lock_guard lock(mutex_);
        auto repository = open_repository(customer_id, repository_id);
        if (!repository) {
            return std::unexpected(repository.error());
        }
        auto tree = tree_at_ref(*(*repository)->subject, ref);
        if (!tree) {
            return std::unexpected(tree.error());
        }
        auto entry = find_file(*(*repository)->subject, *tree, path);
        if (!entry) {
            return std::unexpected(entry.error());
        }
        const auto bytes = liquid::runtime::block_on((*repository)->subject->blob(entry->ref));
        if (!bytes) {
            return std::unexpected(error_of(bytes.error()));
        }
        return RepositoryFile{path,
                              liquid::to_string(entry->ref),
                              display_contents(*bytes),
                              static_cast<std::uint64_t>(bytes->size()),
                              *bytes};
    }

    SyncStoreResult<std::vector<RepositoryChange>>
    repository_log(const std::string& customer_id, const std::string& repository_id) override {
        std::lock_guard lock(mutex_);
        auto repository = open_repository(customer_id, repository_id);
        if (!repository) {
            return std::unexpected(repository.error());
        }
        const auto changes = liquid::runtime::block_on((*repository)->subject->log());
        if (!changes) {
            return std::unexpected(error_of(changes.error()));
        }
        std::vector<RepositoryChange> output;
        output.reserve(changes->size());
        for (const liquid::change& change : *changes) {
            std::vector<std::string> parents;
            parents.reserve(change.parents.size());
            for (const liquid::change_ref& parent : change.parents) {
                parents.push_back(liquid::to_string(parent.id.value));
            }
            output.push_back({liquid::to_string(change.id.value),
                              std::move(parents),
                              change.author.taap_id,
                              change.message,
                              std::format("{:%FT%TZ}", change.at)});
        }
        return output;
    }

    SyncStoreResult<std::vector<RepositoryDiffEntry>>
    repository_diff(const std::string& customer_id,
                    const std::string& repository_id,
                    const std::string& change_id) override {
        std::lock_guard lock(mutex_);
        auto repository = open_repository(customer_id, repository_id);
        if (!repository) {
            return std::unexpected(repository.error());
        }
        const auto changes = liquid::runtime::block_on((*repository)->subject->log());
        if (!changes) {
            return std::unexpected(error_of(changes.error()));
        }
        const auto found = std::ranges::find_if(*changes, [&](const liquid::change& change) {
            return liquid::to_string(change.id.value) == change_id;
        });
        if (found == changes->end()) {
            return std::unexpected(SyncStoreError{"not_found", "no change " + change_id});
        }

        liquid::result<std::vector<liquid::tree_diff_entry>> differences;
        if (found->parents.empty()) {
            const auto current = liquid::runtime::block_on((*repository)->subject->at(found->id));
            if (!current) {
                return std::unexpected(error_of(current.error()));
            }
            differences = liquid::runtime::block_on(
                (*repository)->subject->diff(liquid::tree{}, current->root, {.text = true}));
        } else {
            differences = liquid::runtime::block_on(
                (*repository)->subject->diff(found->parents.front().id, found->id, {.text = true}));
        }
        if (!differences) {
            return std::unexpected(error_of(differences.error()));
        }

        std::vector<RepositoryDiffEntry> output;
        output.reserve(differences->size());
        for (const liquid::tree_diff_entry& difference : *differences) {
            RepositoryDiffEntry entry{
                .path = difference.path,
                .old_path = difference.old_path.value_or(""),
                .kind = node_kind_name(difference.kind),
                .old_mode = difference.old_mode,
                .new_mode = difference.new_mode,
                .old_blob = difference.old_blob ? liquid::to_string(*difference.old_blob) : "",
                .new_blob = difference.new_blob ? liquid::to_string(*difference.new_blob) : "",
            };
            entry.changes.reserve(difference.changes.size());
            for (const liquid::tree_diff_kind change : difference.changes) {
                entry.changes.push_back(tree_diff_kind_name(change));
            }
            for (const liquid::digest& conflict : difference.old_conflicts) {
                entry.old_conflicts.push_back(liquid::to_string(conflict));
            }
            for (const liquid::digest& conflict : difference.new_conflicts) {
                entry.new_conflicts.push_back(liquid::to_string(conflict));
            }
            if (difference.line_hunks) {
                entry.line_hunks.emplace();
                for (const liquid::line_change& hunk : *difference.line_hunks) {
                    auto removed = hunk_lines(*(*repository)->subject,
                                              difference.old_blob,
                                              hunk.old_line,
                                              hunk.old_count);
                    if (!removed) {
                        return std::unexpected(removed.error());
                    }
                    auto added = hunk_lines(*(*repository)->subject,
                                            difference.new_blob,
                                            hunk.new_line,
                                            hunk.new_count);
                    if (!added) {
                        return std::unexpected(added.error());
                    }
                    entry.line_hunks->push_back({hunk.old_line,
                                                 hunk.old_count,
                                                 hunk.new_line,
                                                 hunk.new_count,
                                                 std::move(*removed),
                                                 std::move(*added)});
                }
            }
            output.push_back(std::move(entry));
        }
        return output;
    }

    SyncStoreResult<RepositoryArtifactPage>
    repository_artifacts(const std::string& customer_id,
                         const std::string& repository_id,
                         const RepositoryArtifactFilter& filter) override {
        std::lock_guard lock(mutex_);
        auto repository = open_repository(customer_id, repository_id);
        if (!repository) {
            return std::unexpected(repository.error());
        }
        auto liquid_filter = liquid::result_filter{};
        auto action = optional_digest(filter.action);
        if (!action) {
            return std::unexpected(action.error());
        }
        liquid_filter.action = *action;
        liquid_filter.output =
            filter.output.empty() ? std::nullopt : std::optional<std::string>{filter.output};
        auto content = optional_digest(filter.content);
        if (!content) {
            return std::unexpected(content.error());
        }
        liquid_filter.content = *content;
        liquid_filter.page.limit = filter.limit;
        if (!filter.cursor.empty()) {
            liquid_filter.page.cursor = liquid::record_cursor{filter.cursor};
        }
        const auto page = liquid::runtime::block_on((*repository)->subject->results(liquid_filter));
        if (!page) {
            return std::unexpected(error_of(page.error()));
        }
        auto output = RepositoryArtifactPage{.done = page->done};
        output.items.reserve(page->records.size());
        for (const liquid::repository_result& record : page->records) {
            output.items.push_back(artifact_record(record));
        }
        if (page->next) {
            output.next_cursor = page->next->token;
        }
        return output;
    }

    SyncStoreResult<RepositoryArtifact> repository_artifact(const std::string& customer_id,
                                                            const std::string& repository_id,
                                                            const std::string& result_id) override {
        std::lock_guard lock(mutex_);
        auto repository = open_repository(customer_id, repository_id);
        if (!repository) {
            return std::unexpected(repository.error());
        }
        const auto parsed = liquid::parse_digest(result_id);
        if (!parsed) {
            return std::unexpected(error_of(parsed.error()));
        }
        const liquid::result_id id{*parsed};
        const auto results = liquid::runtime::block_on(
            (*repository)->subject->results({.id = id, .page = {.limit = 1}}));
        if (!results) {
            return std::unexpected(error_of(results.error()));
        }
        if (results->records.empty()) {
            return std::unexpected(SyncStoreError{"not_found", "no result " + result_id});
        }
        RepositoryArtifact output = artifact_record(results->records.front());

        if (results->records.front().value.provenance) {
            const auto provenance = liquid::runtime::block_on(
                liquid::build::verify_provenance(*(*repository)->subject, id));
            if (!provenance) {
                return std::unexpected(error_of(provenance.error()));
            }
            append_attestation(output,
                               provenance->attestation,
                               provenance->statement,
                               provenance->verified,
                               provenance->reason);
        }

        if (results->records.front().verification) {
            const auto verification = liquid::runtime::block_on(
                (*repository)
                    ->subject->attestations(
                        {.id = results->records.front().verification, .page = {.limit = 1}}));
            if (!verification) {
                return std::unexpected(error_of(verification.error()));
            }
            for (const liquid::repository_attestation& attestation : verification->records) {
                append_verified_attestation(output, attestation.id, attestation.value);
            }
        }

        auto attestation_page = liquid::record_page_options{.limit = 1'000};
        for (;;) {
            const auto attestations = liquid::runtime::block_on(
                (*repository)
                    ->subject->attestations({.subject = id.value, .page = attestation_page}));
            if (!attestations) {
                return std::unexpected(error_of(attestations.error()));
            }
            for (const liquid::repository_attestation& attestation : attestations->records) {
                append_verified_attestation(output, attestation.id, attestation.value);
            }
            if (attestations->done || !attestations->next) {
                break;
            }
            attestation_page.cursor = attestations->next;
        }

        auto sbom_page = liquid::record_page_options{.limit = 1'000};
        for (;;) {
            const auto sboms =
                liquid::runtime::block_on((*repository)->subject->sboms(id, sbom_page));
            if (!sboms) {
                return std::unexpected(error_of(sboms.error()));
            }
            for (const liquid::sbom_report& sbom : sboms->records) {
                append_attestation(
                    output, sbom.attestation, sbom.statement, sbom.verified, sbom.reason);
            }
            if (sboms->done || !sboms->next) {
                break;
            }
            sbom_page.cursor = sboms->next;
        }

        auto trace_page = liquid::record_page_options{.limit = 1'000};
        for (;;) {
            const auto traces =
                liquid::runtime::block_on((*repository)->subject->traces(trace_page));
            if (!traces) {
                return std::unexpected(error_of(traces.error()));
            }
            for (const liquid::build_trace& trace : traces->records) {
                const auto action = std::ranges::find_if(
                    trace.actions, [&](const liquid::build_trace_action& value) {
                        return liquid::to_string(value.result.value) == result_id;
                    });
                if (action != trace.actions.end()) {
                    output.trace = RepositoryArtifactTrace{
                        .id = liquid::to_string(trace.id),
                        .wall_ms = milliseconds(trace.wall_time),
                        .actions = static_cast<std::uint64_t>(trace.actions.size()),
                        .dependencies = static_cast<std::uint64_t>(trace.edges.size()),
                        .cache_hits = trace.cache.hits,
                        .cache_misses = trace.cache.misses,
                    };
                    break;
                }
            }
            if (output.trace || traces->done || !traces->next) {
                break;
            }
            trace_page.cursor = traces->next;
        }
        return output;
    }

    SyncStoreResult<std::vector<RepositoryConflict>>
    repository_conflicts(const std::string& customer_id,
                         const std::string& repository_id) override {
        std::lock_guard lock(mutex_);
        auto repository = open_repository(customer_id, repository_id);
        if (!repository) {
            return std::unexpected(repository.error());
        }
        const auto conflicts = liquid::runtime::block_on((*repository)->subject->conflicts());
        if (!conflicts) {
            return std::unexpected(error_of(conflicts.error()));
        }
        std::vector<RepositoryConflict> output;
        output.reserve(conflicts->size());
        for (const liquid::conflict& conflict : *conflicts) {
            std::vector<std::string> blobs;
            blobs.reserve(conflict.refs.size());
            for (const liquid::digest& blob : conflict.refs) {
                blobs.push_back(liquid::to_string(blob));
            }
            output.push_back({conflict.path, std::move(blobs)});
        }
        return output;
    }

    SyncStoreResult<RepositoryStorageSummary>
    repository_storage(const std::string& customer_id, const std::string& repository_id) override {
        std::lock_guard lock(mutex_);
        auto state = open_repository(customer_id, repository_id);
        if (!state) {
            return std::unexpected(state.error());
        }
        const auto changes = liquid::runtime::block_on((*state)->subject->log());
        if (!changes) {
            return std::unexpected(error_of(changes.error()));
        }
        RepositoryStorageSummary output;
        StorageAccumulator all_revisions;
        for (const liquid::change& change : *changes) {
            const auto snapshot = liquid::runtime::block_on((*state)->subject->at(change.id));
            if (!snapshot) {
                return std::unexpected(error_of(snapshot.error()));
            }
            StorageAccumulator revision;
            std::vector<RepositoryTreeEntry> entries;
            const auto walked = walk_tree(*(*state)->subject,
                                          *(*state)->objects,
                                          snapshot->root,
                                          "",
                                          entries,
                                          &revision,
                                          &all_revisions);
            if (!walked) {
                return std::unexpected(walked.error());
            }
            output.revisions.push_back(storage_revision(
                liquid::to_string(change.id.value), revision.logical_bytes, revision.unique_bytes));
            output.logical_bytes += revision.logical_bytes;
        }
        output.unique_bytes = all_revisions.unique_bytes;
        output.deduplicated_bytes = output.logical_bytes > output.unique_bytes
                                        ? output.logical_bytes - output.unique_bytes
                                        : 0;
        output.dedup_ratio = ratio(output.deduplicated_bytes, output.logical_bytes);
        return output;
    }

    SyncStoreResult<std::uint64_t>
    stage_repository_file(const std::string& customer_id,
                          const std::string& repository_id,
                          const std::string& oid,
                          std::vector<std::byte> contents,
                          std::uint64_t storage_quota_bytes) override {
        std::lock_guard lock(mutex_);
        if (!valid_lfs_oid(oid)) {
            return std::unexpected(SyncStoreError{"invalid_input", "invalid LFS SHA-256 oid"});
        }
        auto state = open_repository(customer_id, repository_id);
        if (!state) {
            return std::unexpected(state.error());
        }
        auto stored_bytes = unique_stored_bytes_locked(customer_id);
        if (!stored_bytes) {
            return std::unexpected(stored_bytes.error());
        }
        if (storage_quota_bytes != 0 && *stored_bytes >= storage_quota_bytes) {
            return std::unexpected(quota_error(*stored_bytes, storage_quota_bytes));
        }
        const auto written = liquid::runtime::block_on(
            (*state)->subject->write(staged_upload_path(oid), std::move(contents)));
        if (!written) {
            return std::unexpected(error_of(written.error()));
        }
        return unique_stored_bytes_locked(customer_id);
    }

    SyncStoreResult<std::string>
    commit_repository_files(const std::string& customer_id,
                            const std::string& repository_id,
                            const std::string& revision,
                            const std::string& message,
                            std::vector<RepositoryWriteOperation> operations) override {
        std::lock_guard lock(mutex_);
        auto state = open_repository(customer_id, repository_id);
        if (!state) {
            return std::unexpected(state.error());
        }
        if (revision.empty() || message.empty()) {
            return std::unexpected(
                SyncStoreError{"invalid_input", "revision and commit message are required"});
        }
        if (revision != "main") {
            return std::unexpected(
                SyncStoreError{"conflict", "Hub-compatible commits currently update main only"});
        }
        std::set<std::string> target_paths;
        std::set<std::string> staged_uploads;
        for (const RepositoryWriteOperation& operation : operations) {
            if (!valid_repository_path(operation.path)) {
                return std::unexpected(
                    SyncStoreError{"invalid_input", "invalid repository path", operation.path});
            }
            if (!target_paths.insert(operation.path).second) {
                return std::unexpected(SyncStoreError{
                    "invalid_input", "each commit operation must have a distinct path"});
            }
            if (operation.kind != RepositoryWriteKind::LfsFile) {
                continue;
            }
            if (!valid_lfs_oid(operation.oid) || !staged_uploads.insert(operation.oid).second) {
                return std::unexpected(SyncStoreError{
                    "invalid_input", "each LFS oid must name one staged file", operation.oid});
            }
            const auto staged = liquid::runtime::block_on(
                (*state)->subject->read(staged_upload_path(operation.oid)));
            if (!staged) {
                return std::unexpected(error_of(staged.error()));
            }
            if (staged->size() != operation.size) {
                return std::unexpected(
                    SyncStoreError{"invalid_input", "staged LFS size does not match commit"});
            }
        }
        for (RepositoryWriteOperation& operation : operations) {
            if (operation.kind == RepositoryWriteKind::DeletedFile ||
                operation.kind == RepositoryWriteKind::DeletedFolder) {
                const auto removed =
                    liquid::runtime::block_on((*state)->subject->remove(operation.path));
                if (!removed && removed.error().code != liquid::errc::not_found) {
                    return std::unexpected(error_of(removed.error()));
                }
                continue;
            }
            const auto removed =
                liquid::runtime::block_on((*state)->subject->remove(operation.path));
            if (!removed && removed.error().code != liquid::errc::not_found) {
                return std::unexpected(error_of(removed.error()));
            }
            if (operation.kind == RepositoryWriteKind::File) {
                const auto written = liquid::runtime::block_on(
                    (*state)->subject->write(operation.path, std::move(operation.contents)));
                if (!written) {
                    return std::unexpected(error_of(written.error()));
                }
                continue;
            }
            const auto moved = liquid::runtime::block_on(
                (*state)->subject->move(staged_upload_path(operation.oid), operation.path));
            if (!moved) {
                return std::unexpected(error_of(moved.error()));
            }
        }
        const auto change = liquid::runtime::block_on((*state)->subject->cut(message));
        if (!change) {
            return std::unexpected(error_of(change.error()));
        }
        return liquid::to_string(change->value);
    }

    SyncStoreResult<RepositoryModelProvenance>
    attach_model_provenance(const std::string& customer_id,
                            const std::string& repository_id,
                            const std::string& revision,
                            const std::string& dataset_repository_id,
                            const std::string& dataset_revision,
                            std::vector<std::byte> model_bytes,
                            std::vector<std::byte> run_bytes) override {
        std::lock_guard lock(mutex_);
        auto state = open_repository(customer_id, repository_id);
        if (!state) {
            return std::unexpected(state.error());
        }
        const auto model_revision = resolve_revision(*(*state)->subject, revision);
        if (!model_revision) {
            return std::unexpected(model_revision.error());
        }
        const auto model = liquid::decode_attestation(model_bytes);
        const auto run = liquid::decode_attestation(run_bytes);
        if (!model || !run) {
            return std::unexpected(SyncStoreError{
                "invalid_input", "provenance attestations are not canonical liquid records"});
        }
        const auto validated = validate_model_attestations(*model_revision, *model, *run);
        if (!validated) {
            return std::unexpected(validated.error());
        }
        if (text_attribute(*model, "training_dataset") != dataset_revision) {
            return std::unexpected(SyncStoreError{
                "invalid_input", "the model attestation names a different dataset revision"});
        }
        const auto model_id =
            liquid::runtime::block_on((*state)->subject->store_attestation(*model));
        if (!model_id) {
            return std::unexpected(error_of(model_id.error()));
        }
        const auto run_id = liquid::runtime::block_on((*state)->subject->store_attestation(*run));
        if (!run_id) {
            return std::unexpected(error_of(run_id.error()));
        }
        const nlohmann::json index{{"dataset_repository_id", dataset_repository_id},
                                   {"dataset_revision", dataset_revision},
                                   {"model_attestation", liquid::to_string(*model_id)},
                                   {"run_attestation", liquid::to_string(*run_id)}};
        const std::string serialized = index.dump();
        std::vector<std::byte> metadata;
        metadata.reserve(serialized.size());
        for (const char value : serialized) {
            metadata.push_back(std::byte{static_cast<unsigned char>(value)});
        }
        const auto saved = liquid::runtime::block_on(
            (*state)->subject->set_metadata(provenance_key(*model_revision), std::move(metadata)));
        if (!saved) {
            return std::unexpected(error_of(saved.error()));
        }
        return provenance_record(
            *model_revision, dataset_repository_id, *model_id, *run_id, *model, *run);
    }

    SyncStoreResult<std::optional<RepositoryModelProvenance>>
    model_provenance(const std::string& customer_id,
                     const std::string& repository_id,
                     const std::string& revision) override {
        std::lock_guard lock(mutex_);
        auto state = open_repository(customer_id, repository_id);
        if (!state) {
            return std::unexpected(state.error());
        }
        const auto model_revision = resolve_revision(*(*state)->subject, revision);
        if (!model_revision) {
            return std::unexpected(model_revision.error());
        }
        const auto metadata =
            liquid::runtime::block_on((*state)->subject->metadata(provenance_key(*model_revision)));
        if (!metadata) {
            return std::unexpected(error_of(metadata.error()));
        }
        if (!*metadata) {
            return std::optional<RepositoryModelProvenance>{};
        }
        const std::string serialized(reinterpret_cast<const char*>((*metadata)->data()),
                                     (*metadata)->size());
        const nlohmann::json index = nlohmann::json::parse(serialized, nullptr, false);
        if (index.is_discarded() || !index.is_object()) {
            return std::unexpected(SyncStoreError{"corrupt", "model provenance index is invalid"});
        }
        const auto model_id = liquid::parse_digest(index.value("model_attestation", ""));
        const auto run_id = liquid::parse_digest(index.value("run_attestation", ""));
        if (!model_id || !run_id) {
            return std::unexpected(
                SyncStoreError{"corrupt", "model provenance attestation digest is invalid"});
        }
        const auto model_page = liquid::runtime::block_on(
            (*state)->subject->attestations({.id = *model_id, .page = {.limit = 1}}));
        const auto run_page = liquid::runtime::block_on(
            (*state)->subject->attestations({.id = *run_id, .page = {.limit = 1}}));
        if (!model_page || !run_page) {
            return std::unexpected(error_of((!model_page ? model_page.error() : run_page.error())));
        }
        if (model_page->records.empty() || run_page->records.empty()) {
            return std::unexpected(
                SyncStoreError{"corrupt", "model provenance attestation is missing"});
        }
        return std::optional<RepositoryModelProvenance>{
            provenance_record(*model_revision,
                              index.value("dataset_repository_id", ""),
                              *model_id,
                              *run_id,
                              model_page->records.front().value,
                              run_page->records.front().value)};
    }

    SyncStoreResult<RepositoryChange> restore_repository_file(const std::string& customer_id,
                                                              const std::string& repository_id,
                                                              const std::string& change_id,
                                                              const std::string& path) override {
        std::lock_guard lock(mutex_);
        auto repository = open_repository(customer_id, repository_id);
        if (!repository) {
            return std::unexpected(repository.error());
        }
        const auto parsed = liquid::parse_digest(change_id);
        if (!parsed) {
            return std::unexpected(error_of(parsed.error()));
        }
        const liquid::change_id selected{*parsed};
        const auto snapshot = liquid::runtime::block_on((*repository)->subject->at(selected));
        if (!snapshot) {
            return std::unexpected(error_of(snapshot.error()));
        }
        const auto entry = find_file(*(*repository)->subject, snapshot->root, path);
        if (!entry) {
            return std::unexpected(entry.error());
        }
        const auto contents = liquid::runtime::block_on((*repository)->subject->blob(entry->ref));
        if (!contents) {
            return std::unexpected(error_of(contents.error()));
        }
        const auto parent = liquid::runtime::block_on((*repository)->subject->head());
        if (!parent) {
            return std::unexpected(error_of(parent.error()));
        }
        const auto written = liquid::runtime::block_on(
            (*repository)->subject->write(path, *contents, {.mode = entry->mode}));
        if (!written) {
            return std::unexpected(error_of(written.error()));
        }
        const std::string message = "Restore " + path + " from " + change_id;
        const auto restored = liquid::runtime::block_on((*repository)->subject->cut(message));
        if (!restored) {
            return std::unexpected(error_of(restored.error()));
        }
        return RepositoryChange{liquid::to_string(restored->value),
                                {liquid::to_string(parent->value)},
                                "",
                                message,
                                log_timestamp()};
    }

    SyncStoreResult<RepositoryPortableData> export_portable(const std::string& customer_id,
                                                            const std::string& repository_id,
                                                            bool include_git) override {
        RepositoryPortableData output;
        {
            std::lock_guard lock(mutex_);
            auto state = open_repository(customer_id, repository_id);
            if (!state) {
                return std::unexpected(state.error());
            }
            output.history["format"] = "liquid_portable_history/1";
            output.history["items"] = nlohmann::json::array();
            liquid::journal_cursor cursor;
            std::vector<liquid::digest> roots;
            for (;;) {
                std::array<liquid::sync_item, 256> page;
                const auto exported =
                    liquid::runtime::block_on((*state)->subject->export_drips(cursor, page));
                if (!exported) {
                    return std::unexpected(error_of(exported.error()));
                }
                for (std::size_t index = 0; index < exported->count; ++index) {
                    output.history["items"].push_back(sync_item_json(page[index]));
                }
                roots = exported->objects;
                cursor = exported->cursor;
                if (exported->done) {
                    break;
                }
            }
            ArchiveBlobSource archive;
            liquid::sync_session transfer(*(*state)->objects, archive, "lake-export");
            const auto sent = liquid::runtime::block_on(transfer.send(roots));
            if (!sent) {
                return std::unexpected(error_of(sent.error()));
            }
            output.history["roots"] = nlohmann::json::array();
            for (const liquid::digest& root : roots) {
                output.history["roots"].push_back(liquid::to_string(root));
            }
            output.history["objects"] = nlohmann::json::array();
            for (const auto& [id, contents] : archive.contents()) {
                output.history["objects"].push_back(
                    {{"digest", id}, {"contents", base64_encode(contents)}});
            }
            output.git = {{"requested", include_git}, {"status", "omitted"}};
            if (include_git) {
                const auto refs = liquid::runtime::block_on((*state)->subject->refs());
                if (!refs) {
                    return std::unexpected(error_of(refs.error()));
                }
                if (refs->empty()) {
                    output.git["status"] = "empty";
                } else {
                    static std::atomic<unsigned long> export_number{0};
                    const std::filesystem::path target =
                        std::filesystem::temp_directory_path() /
                        ("lake-export-" + repository_id + "-" + std::to_string(++export_number));
                    const auto exported = liquid::runtime::block_on(
                        liquid::export_git(*(*state)->subject, target, refs->front().name));
                    if (!exported) {
                        std::error_code ignored;
                        std::filesystem::remove_all(target, ignored);
                        return std::unexpected(error_of(exported.error()));
                    }
                    output.git = {{"requested", true},
                                  {"status", "included"},
                                  {"ref", refs->front().name},
                                  {"degradations", exported->degradations},
                                  {"files", nlohmann::json::array()}};
                    for (const auto& entry :
                         std::filesystem::recursive_directory_iterator(target)) {
                        if (!entry.is_regular_file()) {
                            continue;
                        }
                        std::ifstream input(entry.path(), std::ios::binary);
                        const std::vector<char> raw((std::istreambuf_iterator<char>(input)),
                                                    std::istreambuf_iterator<char>());
                        std::vector<std::byte> contents;
                        contents.reserve(raw.size());
                        for (const char byte : raw) {
                            contents.push_back(
                                static_cast<std::byte>(static_cast<unsigned char>(byte)));
                        }
                        output.git["files"].push_back(
                            {{"path",
                              std::filesystem::relative(entry.path(), target).generic_string()},
                             {"contents", base64_encode(contents)}});
                    }
                    std::error_code ignored;
                    std::filesystem::remove_all(target, ignored);
                }
            }
        }
        RepositoryArtifactFilter filter;
        filter.limit = 1'000;
        for (;;) {
            const auto page = repository_artifacts(customer_id, repository_id, filter);
            if (!page) {
                return std::unexpected(page.error());
            }
            for (const RepositoryArtifact& artifact : page->items) {
                const auto detailed = repository_artifact(customer_id, repository_id, artifact.id);
                if (!detailed) {
                    return std::unexpected(detailed.error());
                }
                output.artifacts.push_back(artifact_json(*detailed));
            }
            if (page->done || page->next_cursor.empty()) {
                break;
            }
            filter.cursor = page->next_cursor;
        }
        if (output.artifacts.empty()) {
            std::lock_guard lock(mutex_);
            auto state = open_repository(customer_id, repository_id);
            if (!state) {
                return std::unexpected(state.error());
            }
            const auto saved =
                liquid::runtime::block_on((*state)->subject->metadata("lake.export.artifacts"));
            if (!saved) {
                return std::unexpected(error_of(saved.error()));
            }
            if (*saved) {
                const std::string serialized(reinterpret_cast<const char*>((*saved)->data()),
                                             (*saved)->size());
                const nlohmann::json restored = nlohmann::json::parse(serialized, nullptr, false);
                if (restored.is_discarded() || !restored.is_array()) {
                    return std::unexpected(SyncStoreError{
                        "corrupt", "restored artifact evidence is invalid", "export"});
                }
                output.artifacts = restored;
            }
        }
        return output;
    }

    SyncStoreResult<void> import_portable(const std::string& customer_id,
                                          const std::string& repository_id,
                                          const RepositoryPortableData& portable) override {
        if (portable.history.value("format", "") != "liquid_portable_history/1" ||
            !portable.history.contains("items") || !portable.history.contains("roots") ||
            !portable.history.contains("objects")) {
            return std::unexpected(
                SyncStoreError{"invalid_input", "invalid portable history", "export"});
        }
        std::vector<liquid::sync_item> items;
        for (const nlohmann::json& value : portable.history.at("items")) {
            const auto item = sync_item_from_json(value);
            if (!item) {
                return std::unexpected(item.error());
            }
            items.push_back(*item);
        }
        std::vector<liquid::digest> roots;
        for (const nlohmann::json& value : portable.history.at("roots")) {
            const auto root = liquid::parse_digest(value.get<std::string>());
            if (!root) {
                return std::unexpected(error_of(root.error()));
            }
            roots.push_back(*root);
        }
        std::map<std::string, std::vector<std::byte>> objects;
        for (const nlohmann::json& value : portable.history.at("objects")) {
            const std::string digest = value.at("digest").get<std::string>();
            const auto parsed = liquid::parse_digest(digest);
            if (!parsed) {
                return std::unexpected(error_of(parsed.error()));
            }
            const auto contents = base64_decode(value.at("contents").get<std::string>());
            if (!contents) {
                return std::unexpected(contents.error());
            }
            const auto actual = liquid::hash(parsed->alg, *contents);
            if (!actual || liquid::to_string(*actual) != digest) {
                return std::unexpected(SyncStoreError{
                    "invalid_digest", "archive object digest does not match", digest});
            }
            objects.emplace(digest, *contents);
        }
        std::lock_guard lock(mutex_);
        auto state = open_repository(customer_id, repository_id);
        if (!state) {
            return std::unexpected(state.error());
        }
        ArchiveBlobSource archive(std::move(objects));
        liquid::sync_session transfer(*(*state)->objects, archive, "lake-export");
        const auto received = liquid::runtime::block_on(transfer.receive(roots));
        if (!received) {
            return std::unexpected(error_of(received.error()));
        }
        const auto imported = liquid::runtime::block_on((*state)->subject->import_drips(items));
        if (!imported) {
            return std::unexpected(error_of(imported.error()));
        }
        const std::string artifact_evidence = portable.artifacts.dump();
        std::vector<std::byte> artifact_bytes;
        artifact_bytes.reserve(artifact_evidence.size());
        for (const char byte : artifact_evidence) {
            artifact_bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
        }
        const auto saved = liquid::runtime::block_on(
            (*state)->subject->set_metadata("lake.export.artifacts", std::move(artifact_bytes)));
        if (!saved) {
            return std::unexpected(error_of(saved.error()));
        }
        return {};
    }

    SyncStoreResult<RepositoryGitImport> import_git(const std::string& customer_id,
                                                    const std::string& repository_id,
                                                    const std::string& source_directory) override {
        std::lock_guard lock(mutex_);
        auto state = open_repository(customer_id, repository_id);
        if (!state) {
            return std::unexpected(state.error());
        }
        const auto report = liquid::runtime::block_on(
            liquid::import_git(*(*state)->subject, std::filesystem::path(source_directory)));
        if (!report) {
            return std::unexpected(error_of(report.error()));
        }
        RepositoryGitImport output{.commits = report->commits, .blobs = report->blobs};
        const auto keys =
            liquid::runtime::block_on((*state)->subject->metadata_keys("git.commit."));
        if (!keys) {
            return std::unexpected(error_of(keys.error()));
        }
        for (const std::string& key : *keys) {
            const auto value = liquid::runtime::block_on((*state)->subject->metadata(key));
            if (!value) {
                return std::unexpected(error_of(value.error()));
            }
            if (!*value) {
                continue;
            }
            output.commit_changes[key.substr(std::string("git.commit.").size())] =
                std::string(reinterpret_cast<const char*>((*value)->data()), (*value)->size());
        }
        return output;
    }

private:
    struct RepositoryState {
        std::unique_ptr<liquid::repository> subject;
        liquid::store* objects{};
    };

    static SyncStoreError error_of(const liquid::error& error) {
        const std::string code(liquid::to_string(error.code));
        return SyncStoreError{code, error.message, error.context};
    }

    static std::uint32_t u32_le(std::span<const std::byte> bytes, std::size_t offset) {
        return std::to_integer<std::uint32_t>(bytes[offset]) |
               std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U |
               std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16U |
               std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24U;
    }

    static bool frame_magic(std::span<const std::byte> bytes) {
        constexpr std::array<std::byte, 7> prefix{std::byte{'L'},
                                                  std::byte{'Q'},
                                                  std::byte{'F'},
                                                  std::byte{'R'},
                                                  std::byte{'A'},
                                                  std::byte{'M'},
                                                  std::byte{1}};
        return bytes.size() >= 8 && std::ranges::equal(prefix, bytes.first(prefix.size())) &&
               (bytes[7] == std::byte{0} || bytes[7] == std::byte{1});
    }

    static std::string frame_key(std::span<const std::byte> header) {
        return std::string(reinterpret_cast<const char*>(header.data() + 16), 33);
    }

    static bool exceeds_quota(std::uint64_t stored, std::uint64_t additional, std::uint64_t quota) {
        return quota != 0 && additional != 0 && (stored >= quota || additional > quota - stored);
    }

    static SyncStoreError quota_error(std::uint64_t stored, std::uint64_t quota) {
        return SyncStoreError{"busy",
                              "storage quota exceeded; new writes are throttled while existing "
                              "content remains complete and readable",
                              std::format("stored_bytes={} quota_bytes={}", stored, quota),
                              "storage_quota"};
    }

    static SyncStoreResult<std::uint64_t>
    unique_incoming_bytes(liquid::store& store, std::span<const std::byte> request) {
        constexpr std::size_t header_size = 49;
        std::set<std::string> counted;
        std::uint64_t total = 0;
        std::size_t offset = 0;
        while (offset < request.size()) {
            if (request.size() - offset < header_size || !frame_magic(request.subspan(offset))) {
                return std::unexpected(
                    SyncStoreError{"invalid_argument", "invalid pack frame", "sync server"});
            }
            const std::uint32_t raw_size = u32_le(request, offset + 8);
            const std::uint32_t compressed_size = u32_le(request, offset + 12);
            if (compressed_size > request.size() - offset - header_size) {
                return std::unexpected(
                    SyncStoreError{"invalid_argument", "invalid pack frame length", "sync server"});
            }
            liquid::digest digest{};
            digest.alg =
                static_cast<liquid::algorithm>(std::to_integer<std::uint8_t>(request[offset + 16]));
            std::ranges::copy_n(request.begin() + static_cast<std::ptrdiff_t>(offset + 17),
                                digest.bytes.size(),
                                digest.bytes.begin());
            const std::string key = frame_key(request.subspan(offset, header_size));
            if (counted.insert(key).second) {
                const auto present = liquid::runtime::block_on(store.contains(digest));
                if (!present) {
                    return std::unexpected(error_of(present.error()));
                }
                if (!*present) {
                    total += raw_size;
                }
            }
            offset += header_size + compressed_size;
        }
        return total;
    }

    SyncStoreResult<std::uint64_t>
    unique_stored_bytes_locked(const std::string& customer_id) const {
        if (!path_component_is_safe(customer_id)) {
            return std::unexpected(SyncStoreError{"invalid_input", "invalid customer location"});
        }
        const std::filesystem::path packs =
            std::filesystem::path(root_) / customer_id / "store" / "packs";
        std::error_code filesystem_error;
        if (!std::filesystem::exists(packs, filesystem_error)) {
            if (filesystem_error) {
                return std::unexpected(SyncStoreError{
                    "io", "could not inspect customer store: " + filesystem_error.message()});
            }
            return std::uint64_t{0};
        }
        std::set<std::string> counted;
        std::uint64_t total = 0;
        for (const auto& entry : std::filesystem::directory_iterator(packs, filesystem_error)) {
            if (filesystem_error) {
                break;
            }
            if (!entry.is_regular_file() || entry.path().extension() != ".pack") {
                continue;
            }
            std::ifstream input(entry.path(), std::ios::binary);
            input.seekg(16);
            for (;;) {
                std::array<std::byte, 49> header{};
                input.read(reinterpret_cast<char*>(header.data()), header.size());
                if (input.gcount() == 0) {
                    break;
                }
                if (input.gcount() != static_cast<std::streamsize>(header.size()) ||
                    !frame_magic(header)) {
                    break;
                }
                const std::uint32_t raw_size = u32_le(header, 8);
                const std::uint32_t compressed_size = u32_le(header, 12);
                if (counted.insert(frame_key(header)).second) {
                    total += raw_size;
                }
                input.seekg(compressed_size, std::ios::cur);
                if (!input) {
                    return std::unexpected(SyncStoreError{
                        "io", "could not read customer pack", entry.path().string()});
                }
            }
        }
        if (filesystem_error) {
            return std::unexpected(SyncStoreError{
                "io", "could not enumerate customer store: " + filesystem_error.message()});
        }
        return total;
    }

    static liquid::error liquid_error(const SyncStoreError& error) {
        return liquid::error{liquid_error_code(error.code), error.message, error.context};
    }

    template <class T> static SyncStoreResult<T> from_liquid(liquid::result<T> result) {
        if (!result) {
            return std::unexpected(error_of(result.error()));
        }
        if constexpr (std::is_void_v<T>) {
            return {};
        } else {
            return std::move(*result);
        }
    }

    static SyncStoreResult<std::optional<liquid::digest>> optional_digest(const std::string& text) {
        if (text.empty()) {
            return std::optional<liquid::digest>{};
        }
        const auto value = liquid::parse_digest(text);
        if (!value) {
            return std::unexpected(error_of(value.error()));
        }
        return std::optional<liquid::digest>{*value};
    }

    static std::uint64_t milliseconds(std::chrono::milliseconds value) {
        return value.count() < 0 ? 0 : static_cast<std::uint64_t>(value.count());
    }

    static std::string cache_status_name(liquid::cache_status status) {
        switch (status) {
        case liquid::cache_status::miss:
            return "miss";
        case liquid::cache_status::hit:
            return "hit";
        case liquid::cache_status::divergent:
            return "divergent";
        }
        return "unknown";
    }

    static std::string timestamp(std::chrono::sys_seconds value) {
        return value == std::chrono::sys_seconds{} ? "" : std::format("{:%FT%TZ}", value);
    }

    static RepositoryArtifact artifact_record(const liquid::repository_result& record) {
        RepositoryArtifact output{
            .id = liquid::to_string(record.id.value),
            .action = liquid::to_string(record.value.action),
            .exit_code = record.value.exit,
            .timing = {.queued_ms = milliseconds(record.value.timing.queued),
                       .sandbox_ms = milliseconds(record.value.timing.sandbox),
                       .exec_ms = milliseconds(record.value.timing.exec),
                       .hash_ms = milliseconds(record.value.timing.hash),
                       .store_ms = milliseconds(record.value.timing.store)},
            .cache_status = cache_status_name(record.value.cache),
            .divergent = record.divergent,
            .verification = record.verification ? liquid::to_string(*record.verification) : "",
            .worker = record.value.worker,
            .observed_at = timestamp(record.value.observed_at),
            .started_at = record.value.started_at ? timestamp(*record.value.started_at) : "",
            .finished_at = record.value.finished_at ? timestamp(*record.value.finished_at) : "",
        };
        output.outputs.reserve(record.value.outputs.size());
        for (const liquid::action_output& artifact : record.value.outputs) {
            output.outputs.push_back(
                {artifact.path, artifact.mode, liquid::to_string(artifact.blob)});
        }
        return output;
    }

    static nlohmann::json artifact_json(const RepositoryArtifact& artifact) {
        nlohmann::json outputs = nlohmann::json::array();
        for (const RepositoryArtifactOutput& output : artifact.outputs) {
            outputs.push_back(
                {{"path", output.path}, {"mode", output.mode}, {"blob", output.blob}});
        }
        nlohmann::json attestations = nlohmann::json::array();
        for (const RepositoryArtifactAttestation& attestation : artifact.attestations) {
            attestations.push_back({{"id", attestation.id},
                                    {"profile", attestation.profile},
                                    {"subject", attestation.subject},
                                    {"issuer", attestation.issuer},
                                    {"verified", attestation.verified},
                                    {"reason", attestation.reason}});
        }
        nlohmann::json value = {{"id", artifact.id},
                                {"action", artifact.action},
                                {"exit_code", artifact.exit_code},
                                {"outputs", std::move(outputs)},
                                {"cache_status", artifact.cache_status},
                                {"divergent", artifact.divergent},
                                {"verification", artifact.verification},
                                {"worker", artifact.worker},
                                {"observed_at", artifact.observed_at},
                                {"started_at", artifact.started_at},
                                {"finished_at", artifact.finished_at},
                                {"attestations", std::move(attestations)}};
        if (artifact.trace) {
            value["trace"] = {{"id", artifact.trace->id},
                              {"wall_ms", artifact.trace->wall_ms},
                              {"actions", artifact.trace->actions},
                              {"dependencies", artifact.trace->dependencies},
                              {"cache_hits", artifact.trace->cache_hits},
                              {"cache_misses", artifact.trace->cache_misses}};
        }
        return value;
    }

    static void append_attestation(RepositoryArtifact& artifact,
                                   const liquid::digest& id,
                                   const liquid::attestation& statement,
                                   bool verified,
                                   const std::string& reason) {
        const std::string text = liquid::to_string(id);
        if (std::ranges::find(artifact.attestations, text, &RepositoryArtifactAttestation::id) !=
            artifact.attestations.end()) {
            return;
        }
        artifact.attestations.push_back({text,
                                         statement.profile,
                                         liquid::to_string(statement.subject),
                                         statement.issuer.taap_id,
                                         verified,
                                         reason});
    }

    static void append_verified_attestation(RepositoryArtifact& artifact,
                                            const liquid::digest& id,
                                            const liquid::attestation& statement) {
        const auto verification = liquid::verify_attestation(statement);
        if (!verification) {
            append_attestation(artifact, id, statement, false, verification.error().message);
            return;
        }
        append_attestation(artifact, id, statement, verification->verified, verification->reason);
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

    static bool path_component_is_safe(const std::string& value) {
        return !value.empty() && value != "." && value != ".." &&
               value.find_first_of("/\\\\") == std::string::npos;
    }

    struct BlobDescriptor {
        std::uint64_t size = 0;
        std::vector<std::pair<std::string, std::uint64_t>> chunks;
    };

    struct StorageAccumulator {
        std::uint64_t logical_bytes = 0;
        std::uint64_t unique_bytes = 0;
        std::set<std::string> chunks;

        void add(const BlobDescriptor& value) {
            logical_bytes += value.size;
            for (const auto& [chunk, size] : value.chunks) {
                if (chunks.insert(chunk).second) {
                    unique_bytes += size;
                }
            }
        }
    };

    static double ratio(std::uint64_t numerator, std::uint64_t denominator) {
        return denominator == 0 ? 0.0
                                : static_cast<double>(numerator) / static_cast<double>(denominator);
    }

    static RepositoryRevisionStorage storage_revision(const std::string& revision,
                                                      std::uint64_t logical_bytes,
                                                      std::uint64_t unique_bytes) {
        const std::uint64_t saved = logical_bytes > unique_bytes ? logical_bytes - unique_bytes : 0;
        return {revision, logical_bytes, unique_bytes, saved, ratio(saved, logical_bytes)};
    }

    static SyncStoreResult<BlobDescriptor> blob_descriptor(liquid::store& store,
                                                           const liquid::digest& id) {
        const auto encoded = liquid::runtime::block_on(store.get(id));
        if (!encoded) {
            return std::unexpected(error_of(encoded.error()));
        }
        std::vector<unsigned char> cbor;
        cbor.reserve(encoded->size());
        for (const std::byte value : *encoded) {
            cbor.push_back(static_cast<unsigned char>(value));
        }
        const nlohmann::json record = nlohmann::json::from_cbor(cbor, true, false);
        if (record.is_discarded() || !record.is_object() || record.value("kind", "") != "blob" ||
            !record.contains("size") || !record.at("size").is_number_unsigned() ||
            !record.contains("chunks") || !record.at("chunks").is_array() ||
            !record.contains("chunk_sizes") || !record.at("chunk_sizes").is_array() ||
            record.at("chunks").size() != record.at("chunk_sizes").size()) {
            return std::unexpected(
                SyncStoreError{"corrupt", "invalid liquid blob record", liquid::to_string(id)});
        }
        BlobDescriptor output;
        output.size = record.at("size").get<std::uint64_t>();
        std::uint64_t total = 0;
        for (std::size_t index = 0; index < record.at("chunks").size(); ++index) {
            const nlohmann::json& chunk = record.at("chunks").at(index);
            const nlohmann::json& size = record.at("chunk_sizes").at(index);
            if (!chunk.is_object() || !chunk.contains("alg") ||
                !chunk.at("alg").is_number_unsigned() || !chunk.contains("bytes") ||
                !chunk.at("bytes").is_binary() || !size.is_number_unsigned()) {
                return std::unexpected(SyncStoreError{
                    "corrupt", "invalid liquid blob chunk record", liquid::to_string(id)});
            }
            const auto& bytes = chunk.at("bytes").get_binary();
            if (bytes.size() != 32 || chunk.at("alg").get<std::uint64_t>() > 255) {
                return std::unexpected(SyncStoreError{
                    "corrupt", "invalid liquid blob chunk digest", liquid::to_string(id)});
            }
            std::string key(1, static_cast<char>(chunk.at("alg").get<std::uint64_t>()));
            key.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            const std::uint64_t chunk_size = size.get<std::uint64_t>();
            output.chunks.emplace_back(key, chunk_size);
            total += chunk_size;
        }
        if (total != output.size) {
            return std::unexpected(SyncStoreError{
                "corrupt", "liquid blob chunk sizes do not match", liquid::to_string(id)});
        }
        return output;
    }

    static SyncStoreResult<liquid::change_id> resolve_revision(liquid::repository& repository,
                                                               const std::string& revision) {
        if (revision.empty()) {
            return std::unexpected(SyncStoreError{"invalid_input", "revision is required"});
        }
        const auto refs = liquid::runtime::block_on(repository.refs());
        if (!refs) {
            return std::unexpected(error_of(refs.error()));
        }
        const auto named = std::ranges::find(*refs, revision, &liquid::ref::name);
        if (named != refs->end()) {
            return named->id;
        }
        const auto parsed = liquid::parse_digest(revision);
        if (!parsed) {
            return std::unexpected(SyncStoreError{"not_found", "no revision " + revision});
        }
        const liquid::change_id id{*parsed};
        const auto snapshot = liquid::runtime::block_on(repository.at(id));
        if (!snapshot) {
            return std::unexpected(SyncStoreError{"not_found", "no revision " + revision});
        }
        return id;
    }

    static bool valid_repository_path(std::string_view path) {
        if (path.empty() || path.front() == '/' || path.back() == '/' ||
            path.starts_with(".lake-hub-upload/")) {
            return false;
        }
        std::size_t offset = 0;
        while (offset < path.size()) {
            const std::size_t slash = path.find('/', offset);
            const std::string_view component = path.substr(
                offset, slash == std::string_view::npos ? path.size() - offset : slash - offset);
            if (component.empty() || component == "." || component == ".." ||
                component.find('\\') != std::string_view::npos) {
                return false;
            }
            if (slash == std::string_view::npos) {
                break;
            }
            offset = slash + 1;
        }
        return true;
    }

    static bool valid_lfs_oid(std::string_view oid) {
        return oid.size() == 64 && std::ranges::all_of(oid, [](unsigned char value) {
                   return std::isdigit(value) != 0 || (value >= 'a' && value <= 'f');
               });
    }

    static std::string staged_upload_path(std::string_view oid) {
        return ".lake-hub-upload/" + std::string(oid);
    }

    static std::string provenance_key(const liquid::change_id& revision) {
        return "lake.model.provenance." + liquid::to_string(revision.value);
    }

    static std::string text_attribute(const liquid::attestation& value, std::string_view name) {
        const auto found = std::ranges::find_if(
            value.attributes, [name](const auto& attribute) { return attribute.name == name; });
        if (found == value.attributes.end()) {
            return {};
        }
        const auto* text = std::get_if<std::string>(&found->value);
        return text == nullptr ? std::string{} : *text;
    }

    static SyncStoreResult<void> validate_model_attestations(const liquid::change_id& revision,
                                                             const liquid::attestation& model,
                                                             const liquid::attestation& run) {
        if (model.profile != "model_provenance/1" ||
            model.subject_kind != liquid::attestation_subject_kind::model ||
            liquid::to_string(model.subject) != liquid::to_string(revision.value)) {
            return std::unexpected(SyncStoreError{
                "invalid_input", "the model provenance attestation has the wrong subject"});
        }
        if (run.profile != "slsa_provenance/1" ||
            run.subject_kind != liquid::attestation_subject_kind::artifact ||
            liquid::to_string(run.subject) != liquid::to_string(revision.value)) {
            return std::unexpected(
                SyncStoreError{"invalid_input", "the run attestation has the wrong subject"});
        }
        const auto model_verification = liquid::verify_attestation(model);
        const auto run_verification = liquid::verify_attestation(run);
        if (!model_verification || !run_verification) {
            return std::unexpected(
                SyncStoreError{"invalid_input", "a provenance attestation could not be verified"});
        }
        if (!model_verification->verified || !run_verification->verified) {
            const std::string reason = !model_verification->verified ? model_verification->reason
                                                                     : run_verification->reason;
            return std::unexpected(
                SyncStoreError{"invalid_input", "a provenance attestation was refused", reason});
        }
        const auto dataset = liquid::parse_digest(text_attribute(model, "training_dataset"));
        const auto code = liquid::parse_digest(text_attribute(model, "training_code"));
        if (!dataset || !code || !run.slsa_provenance) {
            return std::unexpected(
                SyncStoreError{"invalid_input", "the provenance chain is incomplete"});
        }
        return {};
    }

    static RepositoryModelProvenance provenance_record(const liquid::change_id& revision,
                                                       const std::string& dataset_repository_id,
                                                       const liquid::digest& model_id,
                                                       const liquid::digest& run_id,
                                                       const liquid::attestation& model,
                                                       const liquid::attestation& run) {
        RepositoryModelProvenance output;
        output.model_revision = liquid::to_string(revision.value);
        output.dataset_repository_id = dataset_repository_id;
        output.dataset_revision = text_attribute(model, "training_dataset");
        output.code_revision = text_attribute(model, "training_code");
        output.model_attestation = liquid::to_string(model_id);
        output.run_attestation = liquid::to_string(run_id);
        output.runner = std::string(run.issuer.identifier());
        const auto model_verification = liquid::verify_attestation(model);
        const auto run_verification = liquid::verify_attestation(run);
        output.verified = model_verification && run_verification && model_verification->verified &&
                          run_verification->verified;
        if (!output.verified) {
            if (!model_verification || !run_verification) {
                output.reason = "an attestation could not be verified";
            } else {
                output.reason = !model_verification->verified ? model_verification->reason
                                                              : run_verification->reason;
            }
        }
        if (!run.slsa_provenance) {
            output.verified = false;
            output.reason = "the run predicate is missing";
            return output;
        }
        const auto& predicate = *run.slsa_provenance;
        output.parameters = predicate.build_definition.external_parameters.command;
        output.environment = predicate.build_definition.external_parameters.environment;
        const auto& environment = predicate.build_definition.internal_parameters.sandbox.digest;
        if (!environment.empty()) {
            output.environment_digest =
                environment.begin()->first + ":" + environment.begin()->second;
        }
        output.builder = predicate.run_details.builder.id;
        output.invocation_id = predicate.run_details.metadata.invocation_id;
        return output;
    }

    static SyncStoreResult<liquid::tree> tree_at_ref(liquid::repository& repository,
                                                     const std::string& ref_name) {
        const auto revision = resolve_revision(repository, ref_name);
        if (!revision) {
            return std::unexpected(revision.error());
        }
        const auto snapshot = liquid::runtime::block_on(repository.at(*revision));
        if (!snapshot) {
            return std::unexpected(error_of(snapshot.error()));
        }
        return snapshot->root;
    }

    static std::string node_kind_name(liquid::node_kind kind) {
        if (kind == liquid::node_kind::blob) {
            return "blob";
        }
        if (kind == liquid::node_kind::tree) {
            return "tree";
        }
        if (kind == liquid::node_kind::repo) {
            return "repository";
        }
        return "object";
    }

    static std::string node_id_text(const liquid::node_id& node) {
        std::string output;
        output.reserve(node.size() * 2);
        for (const std::byte value : node) {
            output += std::format("{:02x}", std::to_integer<unsigned int>(value));
        }
        return output;
    }

    static std::string tree_diff_kind_name(liquid::tree_diff_kind kind) {
        switch (kind) {
        case liquid::tree_diff_kind::added:
            return "added";
        case liquid::tree_diff_kind::removed:
            return "removed";
        case liquid::tree_diff_kind::modified:
            return "modified";
        case liquid::tree_diff_kind::moved:
            return "moved";
        case liquid::tree_diff_kind::mode_changed:
            return "mode changed";
        case liquid::tree_diff_kind::conflicted:
            return "conflicted";
        }
        return "changed";
    }

    static std::vector<std::string> text_lines(const std::vector<std::byte>& bytes) {
        const std::string_view text{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        std::vector<std::string> output;
        std::size_t offset = 0;
        while (offset < text.size()) {
            const std::size_t newline = text.find('\n', offset);
            const std::size_t length =
                newline == std::string_view::npos ? text.size() - offset : newline - offset;
            output.emplace_back(text.substr(offset, length));
            if (newline == std::string_view::npos) {
                break;
            }
            offset = newline + 1;
        }
        return output;
    }

    static SyncStoreResult<std::vector<std::string>>
    hunk_lines(liquid::repository& repository,
               const std::optional<liquid::digest>& blob,
               std::uint64_t first,
               std::uint64_t count) {
        if (count == 0) {
            return std::vector<std::string>{};
        }
        if (!blob || first == 0) {
            return std::unexpected(SyncStoreError{"invalid_input", "invalid text hunk range"});
        }
        const auto contents = liquid::runtime::block_on(repository.blob(*blob));
        if (!contents) {
            return std::unexpected(error_of(contents.error()));
        }
        const std::vector<std::string> lines = text_lines(*contents);
        const std::uint64_t offset = first - 1;
        if (offset > lines.size() || count > lines.size() - offset) {
            return std::unexpected(SyncStoreError{"invalid_input", "invalid text hunk range"});
        }
        return std::vector<std::string>(lines.begin() + static_cast<std::ptrdiff_t>(offset),
                                        lines.begin() +
                                            static_cast<std::ptrdiff_t>(offset + count));
    }

    static SyncStoreResult<void> walk_tree(liquid::repository& repository,
                                           liquid::store& objects,
                                           const liquid::tree& tree,
                                           const std::string& prefix,
                                           std::vector<RepositoryTreeEntry>& output,
                                           StorageAccumulator* revision = nullptr,
                                           StorageAccumulator* all_revisions = nullptr) {
        for (const liquid::tree_entry& entry : tree.entries) {
            const std::string path = prefix.empty() ? entry.name : prefix + "/" + entry.name;
            RepositoryTreeEntry item{path,
                                     node_kind_name(entry.kind),
                                     liquid::to_string(entry.ref),
                                     entry.mode,
                                     node_id_text(entry.node)};
            if (entry.kind == liquid::node_kind::blob) {
                const auto descriptor = blob_descriptor(objects, entry.ref);
                if (!descriptor) {
                    return std::unexpected(descriptor.error());
                }
                StorageAccumulator file;
                file.add(*descriptor);
                item.size = descriptor->size;
                item.stored_size = file.unique_bytes;
                if (revision != nullptr) {
                    revision->add(*descriptor);
                }
                if (all_revisions != nullptr) {
                    all_revisions->add(*descriptor);
                }
            }
            output.push_back(std::move(item));
            if (entry.kind != liquid::node_kind::tree) {
                continue;
            }
            const auto child = liquid::runtime::block_on(repository.tree(entry.ref));
            if (!child) {
                return std::unexpected(error_of(child.error()));
            }
            const auto walked =
                walk_tree(repository, objects, *child, path, output, revision, all_revisions);
            if (!walked) {
                return walked;
            }
        }
        return {};
    }

    static SyncStoreResult<liquid::tree_entry>
    find_file(liquid::repository& repository, const liquid::tree& tree, const std::string& path) {
        const std::size_t slash = path.find('/');
        const std::string part = path.substr(0, slash);
        const auto found = std::ranges::find(tree.entries, part, &liquid::tree_entry::name);
        if (found == tree.entries.end()) {
            return std::unexpected(SyncStoreError{"not_found", "no file " + path});
        }
        if (slash == std::string::npos) {
            if (found->kind != liquid::node_kind::blob) {
                return std::unexpected(SyncStoreError{"invalid_input", path + " is not a file"});
            }
            return *found;
        }
        if (found->kind != liquid::node_kind::tree) {
            return std::unexpected(SyncStoreError{"not_found", "no file " + path});
        }
        const auto child = liquid::runtime::block_on(repository.tree(found->ref));
        if (!child) {
            return std::unexpected(error_of(child.error()));
        }
        return find_file(repository, *child, path.substr(slash + 1));
    }

    static std::string display_contents(const std::vector<std::byte>& bytes) {
        std::string output;
        for (const std::byte value : bytes) {
            const unsigned char byte = static_cast<unsigned char>(value);
            if (byte == '\n' || byte == '\r' || byte == '\t' || std::isprint(byte)) {
                output += static_cast<char>(byte);
            } else {
                output += std::format("\\\\x{:02X}", byte);
            }
        }
        return output;
    }

    SyncStoreResult<RepositoryState*> open_repository(const std::string& customer_id,
                                                      const std::string& repository_id) {
        if (!path_component_is_safe(customer_id) || !path_component_is_safe(repository_id)) {
            return std::unexpected(SyncStoreError{"invalid_input", "invalid repository location"});
        }
        const std::string key = customer_id + "/" + repository_id;
        const auto existing = repositories_.find(key);
        if (existing != repositories_.end()) {
            return existing->second.get();
        }
        auto objects = open_customer(customer_id);
        if (!objects) {
            return std::unexpected(objects.error());
        }
        const std::filesystem::path repositories =
            std::filesystem::path(root_) / customer_id / "repositories";
        std::error_code filesystem_error;
        std::filesystem::create_directories(repositories, filesystem_error);
        if (filesystem_error) {
            return std::unexpected(SyncStoreError{
                "io", "could not create repository directory: " + filesystem_error.message(), key});
        }
        const std::filesystem::path path = repositories / repository_id;
        liquid::repository_options options;
        options.store = *objects;
        const bool has_marker = std::filesystem::exists(path, filesystem_error);
        if (filesystem_error) {
            return std::unexpected(SyncStoreError{"io",
                                                  "could not inspect repository directory: " +
                                                      filesystem_error.message(),
                                                  key});
        }
        liquid::result<liquid::repository> opened =
            has_marker ? liquid::runtime::block_on(liquid::repository::open(path, options))
                       : liquid::runtime::block_on(liquid::repository::create(path, options));
        if (!opened && !has_marker && opened.error().code == liquid::errc::already_exists) {
            opened = liquid::runtime::block_on(liquid::repository::open(path, options));
        }
        if (!opened) {
            return std::unexpected(error_of(opened.error()));
        }
        std::filesystem::create_directories(path, filesystem_error);
        if (filesystem_error) {
            return std::unexpected(SyncStoreError{
                "io", "could not create repository marker: " + filesystem_error.message(), key});
        }
        auto state = std::make_unique<RepositoryState>();
        state->subject = std::make_unique<liquid::repository>(std::move(*opened));
        state->objects = *objects;
        auto inserted = repositories_.emplace(key, std::move(state));
        return inserted.first->second.get();
    }

    std::string root_;
    std::mutex mutex_;
    std::map<std::string, std::unique_ptr<liquid::store>> stores_;
    std::map<std::string, std::unique_ptr<RepositoryState>> repositories_;
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
        co_return {{}, "could not send outbound request: " + write_error.message()};
    }

    http::parser_config limits(false);
    limits.body_limit = 2 * 1024 * 1024;
    http::response_parser parser(http::make_parser_config(limits));
    parser.reset();
    parser.start();
    auto [header_error] = co_await parser.read_header(stream);
    if (header_error) {
        co_return {{}, "could not read outbound response: " + header_error.message()};
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
            co_return {{}, "could not read outbound response body: " + read_error.message()};
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
            pending->response.error = "outbound URL must use HTTPS, except for loopback tests";
            finish(pending);
            co_return;
        }
        boost::corosio::resolver resolver(io_context_);
        auto [resolve_error, results] = co_await resolver.resolve(target.host, target.port);
        if (resolve_error) {
            pending->response.error = "could not resolve outbound host: " + resolve_error.message();
            finish(pending);
            co_return;
        }
        boost::corosio::tcp_socket socket(io_context_);
        socket.open();
        bool connected = false;
        std::string connect_error = "outbound host resolved no addresses";
        for (const auto& entry : results) {
            auto [error] = co_await socket.connect(entry.get_endpoint());
            if (!error) {
                connected = true;
                break;
            }
            connect_error = "could not connect to outbound host: " + error.message();
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
                    "outbound TLS handshake failed: " + handshake_error.message();
                finish(pending);
                co_return;
            }
            auto [timeout_error, reason] = co_await boost::capy::timeout(
                exchange_outbound(secure, pending->request, target, pending->response),
                std::chrono::seconds(30));
            exchange_error = timeout_error ? "outbound request timed out" : std::move(reason);
            [[maybe_unused]] auto shutdown = co_await secure.shutdown();
        } else {
            auto [timeout_error, reason] = co_await boost::capy::timeout(
                exchange_outbound(socket, pending->request, target, pending->response),
                std::chrono::seconds(30));
            exchange_error = timeout_error ? "outbound request timed out" : std::move(reason);
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
    mutable control::pki::Service pki;
    std::unique_ptr<transparency::Service> transparency;
    std::unique_ptr<transparency::CheckpointService> checkpoints;
    std::unique_ptr<transparency::TiledLog> transparency_log;
    std::unique_ptr<transparency::TimestampService> timestamping;
    std::unique_ptr<control::callers::StoreFederationIssuerPolicy> store_federation_policy;
    std::unique_ptr<control::callers::Service> callers;
    std::string openapi_body;
    mutable OutboundHttpClient outbound_http;
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
    std::chrono::steady_clock::time_point next_timestamp_renewal_check =
        std::chrono::steady_clock::now();

    explicit Impl(ServerOptions opts) : options(std::move(opts)), sync_store(options.store_root) {
        if (!options.store_root.empty()) {
            const auto valid = transparency::validate_log_configuration(options.log_configuration);
            if (!valid) {
                failure_reason = "invalid transparency log configuration: " + valid.error().message;
            }
            const auto valid_timestamp_policy =
                transparency::validate_timestamp_policy(options.timestamp_policy);
            if (!options.tsa_endpoint.empty() && !valid_timestamp_policy) {
                failure_reason =
                    "invalid timestamp policy: " + valid_timestamp_policy.error().message;
            }
            transparency =
                std::make_unique<transparency::Service>(options.store_root, options.node_name);
            checkpoints = std::make_unique<transparency::CheckpointService>(
                options.store_root, options.node_name, options.log_configuration);
            transparency_log =
                std::make_unique<transparency::TiledLog>(options.store_root, options.node_name);
            timestamping = std::make_unique<transparency::TimestampService>(options.store_root,
                                                                            options.node_name);
        }
        register_core_capabilities(registry, version_string());
        openapi_body = openapi_document(registry, version_string()).dump();
        metrics.set_gauge("lake_build_info",
                          {{"version", std::string(version_string())}, {"node", options.node_name}},
                          1.0);
        metrics.set_gauge("lake_up", {}, 0.0);
        options.store = telemetry::instrument_store(std::move(options.store), metrics);
        store_federation_policy =
            std::make_unique<control::callers::StoreFederationIssuerPolicy>(options.store.get());
        control::callers::FederationIssuerPolicy* issuer_policy =
            options.federation_issuer_policy ? options.federation_issuer_policy
                                             : store_federation_policy.get();
        callers = std::make_unique<control::callers::Service>(options.token_reviewer,
                                                              options.jwt_verifier,
                                                              options.cloud_identity_verifier,
                                                              issuer_policy);
    }

    transparency::Result<nlohmann::json> submit_transparency(const std::string& endpoint,
                                                             const nlohmann::json& request) const {
        const OutboundResponse response =
            outbound_http.request({"POST",
                                   endpoint,
                                   request.dump(),
                                   {{"Accept", "application/json"},
                                    {"Content-Type", "application/json"},
                                    {"User-Agent", "lake-transparency/1"}}});
        if (!response.error.empty()) {
            return std::unexpected(transparency::Error{
                "store_unavailable", "transparency endpoint request failed: " + response.error});
        }
        if (response.status < 200 || response.status >= 300) {
            return std::unexpected(transparency::Error{"store_unavailable",
                                                       "transparency endpoint returned HTTP " +
                                                           std::to_string(response.status)});
        }
        const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
        if (body.is_discarded()) {
            return std::unexpected(
                transparency::Error{"store_error", "transparency endpoint returned invalid JSON"});
        }
        return body;
    }

    transparency::TsaReply send_timestamp_request(std::string_view endpoint,
                                                  std::string_view request_der) const {
        const OutboundResponse response =
            outbound_http.request({"POST",
                                   std::string(endpoint),
                                   std::string(request_der),
                                   {{"Content-Type", "application/timestamp-query"},
                                    {"Accept", "application/timestamp-reply"}}});
        return transparency::TsaReply{response.status, response.body, response.error};
    }

    void configure_timestamp_context(ApiContext& context) const {
        context.timestamping = timestamping.get();
        context.tsa_endpoint = options.tsa_endpoint;
        context.tsa_trust_pem = options.tsa_trust_pem;
        context.timestamp_policy = options.timestamp_policy;
        context.tsa_sender = options.tsa_sender;
        if (!context.tsa_sender) {
            context.tsa_sender = [this](std::string_view endpoint, std::string_view request_der) {
                return send_timestamp_request(endpoint, request_der);
            };
        }
    }

    void renew_due_timestamps() {
        const auto now = std::chrono::steady_clock::now();
        if (!timestamping || options.tsa_endpoint.empty() || !options.store ||
            now < next_timestamp_renewal_check) {
            return;
        }
        next_timestamp_renewal_check = now + std::chrono::hours{1};
        const auto due =
            timestamping->due(std::chrono::system_clock::now(), options.timestamp_policy);
        if (!due) {
            std::println(stderr,
                         "{}",
                         nlohmann::json{{"event", "timestamp_renewal"},
                                        {"outcome", "failure"},
                                        {"error", due.error().message}}
                             .dump());
            return;
        }
        for (const std::string& checkpoint_digest : *due) {
            ApiContext context;
            context.store = options.store.get();
            context.node_name = options.node_name;
            context.acting_identity = "admin";
            context.authorized = true;
            context.operator_credential = true;
            context.authentication_method = "scheduled_timestamp_renewal";
            context.identity_seed = options.identity_seed;
            context.instance_kek = options.instance_kek;
            configure_timestamp_context(context);
            const ApiResult result = registry.call(
                "renew_timestamp_evidence", {{"checkpoint_digest", checkpoint_digest}}, context);
            if (!result.ok) {
                std::println(stderr,
                             "{}",
                             nlohmann::json{{"event", "timestamp_renewal"},
                                            {"outcome", "failure"},
                                            {"checkpoint_digest", checkpoint_digest},
                                            {"error", result.error.message}}
                                 .dump());
            }
        }
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
            "/fonts/ibm-plex-sans-400.woff2",
            "/fonts/ibm-plex-sans-500.woff2",
            "/fonts/ibm-plex-sans-600.woff2",
            "/fonts/ibm-plex-mono-400.woff2",
            "/fonts/ibm-plex-mono-500.woff2",
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
            "/repository/access",
            "/organization/access",
            "/team/access",
            "/repositories",
            "/repository",
            "/repository/tree",
            "/file",
            "/changes",
            "/change",
            "/conflicts",
            "/artifact",
            "/drive",
            "/drive/preview",
            "/drive/history",
            "/drive/share",
            "/search",
            "/metering",
            "/transparency",
            "/secret-mounts",
            "/secrets",
            "/encryption-keys",
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
            "/scim/v2/Users",
            "/scim/v2/Groups",
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
        if (path.starts_with("/scim/v2/Users/")) {
            return "/scim/v2/Users/:id";
        }
        if (path.starts_with("/scim/v2/Groups/")) {
            return "/scim/v2/Groups/:id";
        }
        return "unmatched";
    }

    ApiContext context_for(const boost::http::route_params& route) const {
        ApiContext ctx;
        ctx.store = options.store.get();
        ctx.search = options.search.get();
        ctx.pki = &pki;
        ctx.callers = callers.get();
        ctx.transparency = transparency.get();
        ctx.checkpoints = checkpoints.get();
        ctx.transparency_log = transparency_log.get();
        ctx.transparency_submitter = [this](const std::string& endpoint,
                                            const nlohmann::json& request) {
            return submit_transparency(endpoint, request);
        };
        configure_timestamp_context(ctx);
        ctx.node_name = options.node_name;
        ctx.identity_seed = options.identity_seed;
        ctx.instance_kek = options.instance_kek;
        ctx.sync_store = &sync_store;
        ctx.metering = options.metering;
        ctx.public_host = options.public_host;
        ctx.request_origin = request_origin(route);
        if (route.req.count(boost::http::field::origin) != 0) {
            ctx.caller_origin = std::string(route.req.at(boost::http::field::origin));
        }
        ctx.webhook_sender = [this](const std::string& url,
                                    const nlohmann::json& headers,
                                    const std::string& payload) {
            std::vector<std::pair<std::string, std::string>> outbound_headers;
            for (const auto& [name, value] : headers.items()) {
                if (value.is_string()) {
                    outbound_headers.emplace_back(name, value.get<std::string>());
                }
            }
            const OutboundResponse response =
                outbound_http.request({"POST", url, payload, std::move(outbound_headers)});
            return WebhookDeliveryResult{response.status, response.error};
        };
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
                ctx.credential_device_id = principal->device_id;
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
        if (route.req.count("liquid-offline-grant") != 0) {
            ctx.offline_grant_id = std::string(route.req.at("liquid-offline-grant"));
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

    struct VerifiedAcmeJws {
        nlohmann::json payload = nlohmann::json::object();
        std::string account_id;
        std::optional<control::pki::AcmeJwk> jwk;
    };

    std::expected<VerifiedAcmeJws, control::pki::Error>
    verify_acme_jws(std::string_view body,
                    std::string_view customer_id,
                    std::string_view expected_url,
                    bool new_account) {
        const nlohmann::json jws = nlohmann::json::parse(body, nullptr, false);
        if (jws.is_discarded() || !jws.is_object() || !jws.contains("protected") ||
            !jws.contains("payload") || !jws.contains("signature") ||
            !jws.at("protected").is_string() || !jws.at("payload").is_string() ||
            !jws.at("signature").is_string()) {
            return std::unexpected(
                control::pki::Error{"invalid_input", "ACME request must be a flattened JWS"});
        }
        const std::string protected_value = jws.at("protected").get<std::string>();
        auto decoded_protected = decode_base64url(protected_value);
        if (!decoded_protected) {
            return std::unexpected(control::pki::Error{"invalid_input", decoded_protected.error()});
        }
        const nlohmann::json header = nlohmann::json::parse(*decoded_protected, nullptr, false);
        if (header.is_discarded() || !header.is_object() || header.value("alg", "") != "ES256" ||
            header.value("url", "") != expected_url || header.value("nonce", "").empty()) {
            return std::unexpected(
                control::pki::Error{"invalid_input", "ACME protected header is invalid"});
        }
        auto consumed = pki.consume_acme_nonce(header.at("nonce").get<std::string>());
        if (!consumed) {
            return std::unexpected(consumed.error());
        }
        VerifiedAcmeJws verified;
        if (new_account) {
            if (!header.contains("jwk") || !header.at("jwk").is_object()) {
                return std::unexpected(
                    control::pki::Error{"invalid_input", "newAccount requires a JWK"});
            }
            const nlohmann::json& jwk = header.at("jwk");
            if (jwk.value("kty", "") != "EC" || jwk.value("crv", "") != "P-256" ||
                jwk.value("x", "").empty() || jwk.value("y", "").empty() ||
                header.contains("kid")) {
                return std::unexpected(
                    control::pki::Error{"invalid_input", "newAccount requires a P-256 JWK"});
            }
            verified.jwk = control::pki::AcmeJwk{jwk.at("x").get<std::string>(),
                                                 jwk.at("y").get<std::string>()};
        } else {
            const std::string kid = header.value("kid", "");
            const std::string marker = "/acme/" + std::string(customer_id) + "/account/";
            const std::size_t marker_at = kid.find(marker);
            if (marker_at == std::string::npos || header.contains("jwk")) {
                return std::unexpected(
                    control::pki::Error{"invalid_input", "ACME account URL is invalid"});
            }
            verified.account_id = kid.substr(marker_at + marker.size());
            if (verified.account_id.empty() || verified.account_id.contains('/')) {
                return std::unexpected(
                    control::pki::Error{"invalid_input", "ACME account URL is invalid"});
            }
        }
        const std::string payload_value = jws.at("payload").get<std::string>();
        const std::string signing_input = protected_value + "." + payload_value;
        auto signature = pki.verify_acme_signature(verified.account_id,
                                                   verified.jwk,
                                                   signing_input,
                                                   jws.at("signature").get<std::string>());
        if (!signature) {
            return std::unexpected(signature.error());
        }
        auto decoded_payload = decode_base64url(payload_value);
        if (!decoded_payload) {
            return std::unexpected(control::pki::Error{"invalid_input", decoded_payload.error()});
        }
        if (!decoded_payload->empty()) {
            verified.payload = nlohmann::json::parse(*decoded_payload, nullptr, false);
            if (verified.payload.is_discarded()) {
                return std::unexpected(
                    control::pki::Error{"invalid_input", "ACME JWS payload is invalid JSON"});
            }
        }
        return verified;
    }

    boost::http::route_task send_acme(boost::http::route_params& route,
                                      boost::http::status status,
                                      std::string body,
                                      std::string_view content_type,
                                      std::string_view location = {}) {
        auto nonce = pki.new_acme_nonce();
        if (!nonce) {
            co_return co_await send_text(route,
                                         boost::http::status::internal_server_error,
                                         error_body(nonce.error().code, nonce.error().message),
                                         "application/problem+json",
                                         "no-store");
        }
        route.res.set("Replay-Nonce", *nonce);
        if (!location.empty()) {
            route.res.set(boost::http::field::location, location);
        }
        co_return co_await send_text(route, status, std::move(body), content_type, "no-store");
    }

    boost::http::route_task acme_problem(boost::http::route_params& route,
                                         const control::pki::Error& error) {
        const bool bad_nonce = error.message.find("nonce") != std::string::npos;
        const std::string problem = bad_nonce ? "badNonce" : "malformed";
        const boost::http::status status = error.code == "not_found"
                                               ? boost::http::status::not_found
                                               : boost::http::status::bad_request;
        return send_acme(route,
                         status,
                         nlohmann::json{{"type", "urn:ietf:params:acme:error:" + problem},
                                        {"detail", error.message},
                                        {"status", static_cast<unsigned>(status)}}
                             .dump(),
                         "application/problem+json");
    }

    boost::http::route_task serve_acme_directory(boost::http::route_params& route) {
        const std::string customer_id = route_value(route, "customer_id");
        const std::string base = request_origin(route) + "/acme/" + customer_id;
        return send_acme(route,
                         boost::http::status::ok,
                         nlohmann::json{{"newNonce", base + "/new-nonce"},
                                        {"newAccount", base + "/new-account"},
                                        {"newOrder", base + "/new-order"}}
                             .dump(),
                         "application/json");
    }

    boost::http::route_task serve_acme_nonce(boost::http::route_params& route) {
        return send_acme(route, boost::http::status::ok, "", "application/octet-stream");
    }

    boost::http::route_task serve_acme_http01(boost::http::route_params& route) {
        ApiContext context = context_for(route);
        context.authentication_method = "acme-http-01";
        const ApiResult result = registry.call(
            "acme_http01_response", {{"token", route_value(route, "token")}}, context);
        if (!result.ok) {
            return acme_problem(route,
                                control::pki::Error{result.error.code, result.error.message});
        }
        return send_text(route,
                         boost::http::status::ok,
                         result.value.at("key_authorization").get<std::string>(),
                         "application/octet-stream",
                         "no-store");
    }

    boost::http::route_task serve_acme_post(boost::http::route_params& route,
                                            std::string capability_name) {
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return boost::http::route_error(read_error);
        }
        const std::string customer_id = route_value(route, "customer_id");
        const std::string expected_url = request_origin(route) + std::string(route.url.path());
        const bool new_account = capability_name == "acme_new_account";
        auto verified = verify_acme_jws(body, customer_id, expected_url, new_account);
        if (!verified) {
            co_return co_await acme_problem(route, verified.error());
        }
        ApiContext context = context_for(route);
        context.authentication_method = "acme";
        context.acting_identity =
            verified->account_id.empty() ? "acme-new-account" : "acme/" + verified->account_id;
        nlohmann::json input{{"customer_id", customer_id}};
        if (verified->jwk) {
            input["x"] = verified->jwk->x;
            input["y"] = verified->jwk->y;
        }
        if (!verified->account_id.empty()) {
            input["account_id"] = verified->account_id;
        }
        const std::string order_id = route_value(route, "order_id");
        const std::string authorization_id = route_value(route, "authorization_id");
        if (!order_id.empty()) {
            input["order_id"] = order_id;
        }
        if (!authorization_id.empty()) {
            input["authorization_id"] = authorization_id;
        }
        if (capability_name == "acme_new_order") {
            if (!verified->payload.is_object() || !verified->payload.contains("identifiers") ||
                !verified->payload.at("identifiers").is_array() ||
                verified->payload.at("identifiers").size() != 1 ||
                verified->payload.at("identifiers").at(0).value("type", "") != "dns") {
                co_return co_await acme_problem(
                    route,
                    control::pki::Error{"invalid_input", "ACME order requires one DNS identifier"});
            }
            input["identifier"] = verified->payload.at("identifiers").at(0).value("value", "");
        }
        if (capability_name == "acme_finalize_order") {
            input["csr"] = verified->payload.value("csr", "");
        }
        const ApiResult result = registry.call(capability_name, input, context);
        if (!result.ok) {
            co_return co_await acme_problem(
                route, control::pki::Error{result.error.code, result.error.message});
        }
        const std::string base = request_origin(route) + "/acme/" + customer_id;
        if (capability_name == "acme_new_account") {
            const std::string account =
                base + "/account/" + result.value.at("account_id").get<std::string>();
            co_return co_await send_acme(route,
                                         boost::http::status::created,
                                         nlohmann::json{{"status", "valid"}}.dump(),
                                         "application/json",
                                         account);
        }
        if (capability_name == "acme_new_order") {
            const std::string created_order = result.value.at("order_id").get<std::string>();
            const std::string created_authorization =
                result.value.at("authorization_id").get<std::string>();
            co_return co_await send_acme(
                route,
                boost::http::status::created,
                nlohmann::json{
                    {"status", result.value.at("status")},
                    {"identifiers",
                     nlohmann::json::array({{{"type", "dns"}, {"value", input.at("identifier")}}})},
                    {"authorizations",
                     nlohmann::json::array({base + "/authorization/" + created_authorization})},
                    {"finalize", base + "/finalize/" + created_order}}
                    .dump(),
                "application/json",
                base + "/order/" + created_order);
        }
        if (capability_name == "acme_authorization") {
            const std::string challenge =
                base + "/challenge/" + result.value.at("authorization_id").get<std::string>();
            co_return co_await send_acme(
                route,
                boost::http::status::ok,
                nlohmann::json{
                    {"status", result.value.at("status")},
                    {"identifier", {{"type", "dns"}, {"value", result.value.at("identifier")}}},
                    {"challenges",
                     nlohmann::json::array({{{"type", "http-01"},
                                             {"url", challenge},
                                             {"token", result.value.at("token")},
                                             {"status", result.value.at("status")}}})}}
                    .dump(),
                "application/json");
        }
        if (capability_name == "acme_http01_challenge") {
            co_return co_await send_acme(
                route,
                boost::http::status::ok,
                nlohmann::json{{"type", "http-01"}, {"status", result.value.at("status")}}.dump(),
                "application/json");
        }
        if (capability_name == "acme_certificate") {
            co_return co_await send_acme(route,
                                         boost::http::status::ok,
                                         result.value.at("certificate_pem").get<std::string>(),
                                         "application/pem-certificate-chain");
        }
        const std::string current_order = result.value.value("order_id", order_id);
        nlohmann::json order_document{{"status", result.value.at("status")}};
        if (result.value.value("has_certificate", capability_name == "acme_finalize_order")) {
            order_document["certificate"] = base + "/certificate/" + current_order;
        }
        co_return co_await send_acme(
            route, boost::http::status::ok, order_document.dump(), "application/json");
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

    boost::http::route_task serve_repository_page_error(boost::http::route_params& route,
                                                        const ApiContext& context,
                                                        const ApiResult& result) {
        if (!context.authorized && result.error.code == "unauthorized") {
            return redirect(route, "/login");
        }
        return serve_page_error(route, result);
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

    control::Result<control::Application>
    application_for_route(const boost::http::route_params& route) const {
        if (!options.store) {
            return std::unexpected(
                control::StoreError{"store_unavailable", "control-plane store is unavailable"});
        }
        return options.store->get_application(route_value(route, "id"));
    }

    control::Result<control::Application>
    application_for_domain(const boost::http::route_params& route) const {
        if (!options.store) {
            return std::unexpected(
                control::StoreError{"store_unavailable", "control-plane store is unavailable"});
        }
        return options.store->get_application_by_domain(request_host(route));
    }

    boost::http::route_task serve_applications_page(boost::http::route_params& route,
                                                    std::string error = {}) {
        ApiContext context = context_for(route);
        if (!context.authorized) {
            return redirect(route, "/login");
        }
        const ApiResult customer_rows = registry.call("list_customers", {{"limit", 500}}, context);
        if (!customer_rows.ok) {
            return serve_page_error(route, customer_rows);
        }
        std::vector<control::Customer> customers;
        std::vector<control::Application> applications;
        for (const auto& value : customer_rows.value.at("items")) {
            const control::Customer customer = customer_from_json(value);
            customers.push_back(customer);
            const ApiResult application_rows = registry.call(
                "list_applications", {{"customer_id", customer.id}, {"limit", 500}}, context);
            if (!application_rows.ok) {
                return serve_page_error(route, application_rows);
            }
            for (const auto& application : application_rows.value.at("items")) {
                applications.push_back(application_from_json(application));
            }
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_applications(applications, customers, error),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_application_register(boost::http::route_params& route) {
        namespace http = boost::http;
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return http::route_error(read_error);
        }
        ApiContext context = context_for(route);
        const ApiResult result =
            registry.call("register_application",
                          {{"customer_id", form_field(body, "customer_id")},
                           {"name", form_field(body, "name")},
                           {"allowed_origins", form_lines(form_field(body, "allowed_origins"))},
                           {"redirect_uris", form_lines(form_field(body, "redirect_uris"))},
                           {"custom_sign_in_domain", form_field(body, "custom_sign_in_domain")},
                           {"theme", {{"accent_color", form_field(body, "accent_color")}}}},
                          context);
        if (!result.ok) {
            co_return co_await serve_applications_page(route, result.error.message);
        }
        co_return co_await redirect(route, "/application?id=" + json_string(result.value, "id"));
    }

    boost::http::route_task serve_application_page(boost::http::route_params& route) {
        ApiContext context = context_for(route);
        if (!context.authorized) {
            return redirect(route, "/login");
        }
        const std::string id = query_value(route, "id");
        const ApiResult result = registry.call("get_application", {{"id", id}}, context);
        if (!result.ok) {
            return serve_page_error(route, result);
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_application(application_from_json(result.value)),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_application_users_page(boost::http::route_params& route) {
        ApiContext context = context_for(route);
        if (!context.authorized) {
            return redirect(route, "/login");
        }
        const std::string id = query_value(route, "id");
        const ApiResult application = registry.call("get_application", {{"id", id}}, context);
        const ApiResult rows = registry.call(
            "list_application_users", {{"application_id", id}, {"limit", 500}}, context);
        if (!application.ok) {
            return serve_page_error(route, application);
        }
        if (!rows.ok) {
            return serve_page_error(route, rows);
        }
        std::vector<control::ApplicationUser> users;
        for (const auto& value : rows.value.at("items")) {
            users.push_back(application_user_from_json(value));
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_application_users(application_from_json(application.value), users),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_application_user_suspend(boost::http::route_params& route) {
        namespace http = boost::http;
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return http::route_error(read_error);
        }
        ApiContext context = context_for(route);
        const std::string application_id = form_field(body, "application_id");
        const ApiResult result = registry.call(
            "suspend_application_user",
            {{"application_id", application_id}, {"user_id", form_field(body, "user_id")}},
            context);
        if (!result.ok) {
            co_return co_await serve_page_error(route, result);
        }
        co_return co_await redirect(route, "/application/users?id=" + application_id);
    }

    boost::http::route_task serve_application_organizations_page(boost::http::route_params& route) {
        ApiContext context = context_for(route);
        if (!context.authorized) {
            return redirect(route, "/login");
        }
        const std::string id = query_value(route, "id");
        const ApiResult application = registry.call("get_application", {{"id", id}}, context);
        const ApiResult rows = registry.call(
            "list_application_organizations", {{"application_id", id}, {"limit", 500}}, context);
        if (!application.ok) {
            return serve_page_error(route, application);
        }
        if (!rows.ok) {
            return serve_page_error(route, rows);
        }
        std::vector<control::ApplicationOrganization> organizations;
        for (const auto& value : rows.value.at("items")) {
            organizations.push_back(application_organization_from_json(value));
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_application_organizations(application_from_json(application.value),
                                                          organizations),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task
    serve_application_organization_create(boost::http::route_params& route) {
        namespace http = boost::http;
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return http::route_error(read_error);
        }
        ApiContext context = context_for(route);
        const std::string application_id = form_field(body, "application_id");
        const ApiResult result = registry.call("create_application_organization",
                                               {{"application_id", application_id},
                                                {"name", form_field(body, "name")},
                                                {"taap_id", form_field(body, "taap_id")}},
                                               context);
        if (!result.ok) {
            co_return co_await serve_page_error(route, result);
        }
        co_return co_await redirect(route, "/application/organizations?id=" + application_id);
    }

    boost::http::route_task
    serve_application_organization_delete(boost::http::route_params& route) {
        namespace http = boost::http;
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return http::route_error(read_error);
        }
        ApiContext context = context_for(route);
        const std::string application_id = form_field(body, "application_id");
        const ApiResult result =
            registry.call("delete_application_organization",
                          {{"application_id", application_id},
                           {"organization_id", form_field(body, "organization_id")}},
                          context);
        if (!result.ok) {
            co_return co_await serve_page_error(route, result);
        }
        co_return co_await redirect(route, "/application/organizations?id=" + application_id);
    }

    boost::http::route_task serve_application_events_page(boost::http::route_params& route) {
        ApiContext context = context_for(route);
        if (!context.authorized) {
            return redirect(route, "/login");
        }
        const std::string id = query_value(route, "id");
        const ApiResult application = registry.call("get_application", {{"id", id}}, context);
        const ApiResult rows = registry.call(
            "list_application_webhook_events", {{"application_id", id}, {"limit", 500}}, context);
        if (!application.ok) {
            return serve_page_error(route, application);
        }
        if (!rows.ok) {
            return serve_page_error(route, rows);
        }
        std::vector<control::ApplicationWebhookEvent> events;
        for (const auto& value : rows.value.at("items")) {
            events.push_back(application_event_from_json(value));
        }
        return send_text(
            route,
            boost::http::status::ok,
            web_ui_application_events(application_from_json(application.value), events),
            "text/html; charset=utf-8",
            "no-store");
    }

    boost::http::route_task serve_application_webhook_create(boost::http::route_params& route) {
        namespace http = boost::http;
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return http::route_error(read_error);
        }
        ApiContext context = context_for(route);
        const std::string application_id = form_field(body, "application_id");
        const ApiResult result =
            registry.call("create_application_webhook",
                          {{"application_id", application_id},
                           {"url", form_field(body, "url")},
                           {"events", comma_separated(form_field(body, "events"))}},
                          context);
        if (!result.ok) {
            co_return co_await serve_page_error(route, result);
        }
        auto application = options.store->get_application(application_id);
        auto rows = options.store->list_application_webhook_events(application_id, 500);
        if (!application || !rows) {
            co_return co_await send_text(route,
                                         http::status::internal_server_error,
                                         error_body("store_error", "could not render webhook page"),
                                         "application/json; charset=utf-8",
                                         "no-store");
        }
        co_return co_await send_text(
            route,
            http::status::ok,
            web_ui_application_events(*application, *rows, result.value.value("secret", "")),
            "text/html; charset=utf-8",
            "no-store");
    }

    boost::http::route_task serve_application_identity_page(boost::http::route_params& route,
                                                            std::string_view page,
                                                            std::string_view error = {}) {
        auto application = application_for_route(route);
        if (!application) {
            return send_text(route,
                             boost::http::status::not_found,
                             error_body(application.error().code, application.error().message),
                             "application/json; charset=utf-8",
                             "no-store");
        }
        std::string body;
        if (page == "sign-up") {
            body = web_ui_application_sign_up(*application, error);
        } else if (page == "sign-in") {
            body = web_ui_application_sign_in(*application, error);
        } else if (page == "mfa") {
            body = web_ui_application_mfa(*application, error);
        } else {
            body = web_ui_application_passkey_sign_in(*application, error);
        }
        return send_text(
            route, boost::http::status::ok, body, "text/html; charset=utf-8", "no-store");
    }

    boost::http::route_task serve_application_password(boost::http::route_params& route,
                                                       std::string_view capability) {
        namespace http = boost::http;
        auto application = application_for_route(route);
        if (!application) {
            co_return co_await send_text(
                route,
                http::status::not_found,
                error_body(application.error().code, application.error().message),
                "application/json; charset=utf-8",
                "no-store");
        }
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return http::route_error(read_error);
        }
        ApiContext context = context_for(route);
        context.public_host = request_host(route);
        nlohmann::json input = {{"pool_id", application->pool_id},
                                {"email", form_field(body, "email")},
                                {"password", form_field(body, "password")},
                                {"device_name", form_field(body, "device_name")}};
        if (capability == "sign_up_password") {
            input["display_name"] = form_field(body, "display_name");
            input["identity_document"] = form_field(body, "identity_document");
        }
        if (capability == "verify_mfa") {
            input["code"] = form_field(body, "code");
        }
        const ApiResult result = registry.call(std::string(capability), input, context);
        if (!result.ok && result.error.code == "mfa_required") {
            co_return co_await redirect(route, "/apps/" + application->id + "/mfa");
        }
        if (!result.ok) {
            const std::string page = capability == "sign_up_password"
                                         ? "sign-up"
                                         : (capability == "verify_mfa" ? "mfa" : "sign-in");
            co_return co_await serve_application_identity_page(route, page, result.error.message);
        }
        const std::string destination = application_redirect_location(*application, result);
        co_return co_await redirect(
            route, destination, browser_session_cookie(route, result.value.value("secret", "")));
    }

    boost::http::route_task serve_application_passkey(boost::http::route_params& route,
                                                      std::string_view capability) {
        namespace http = boost::http;
        auto application = application_for_route(route);
        if (!application) {
            co_return co_await send_text(
                route,
                http::status::not_found,
                error_body(application.error().code, application.error().message),
                "application/json; charset=utf-8",
                "no-store");
        }
        nlohmann::json input = {{"pool_id", application->pool_id}};
        if (capability == "sign_in_passkey") {
            http::json_sink sink;
            auto [read_error, bytes_read] = co_await boost::capy::push_to(route.req_body, sink);
            if (read_error || bytes_read == 0) {
                co_return co_await send_text(route,
                                             http::status::bad_request,
                                             error_body("invalid_input", "JSON body is required"),
                                             "application/json; charset=utf-8",
                                             "no-store");
            }
            input.update(to_nlohmann(sink.release()));
            input["pool_id"] = application->pool_id;
        }
        ApiContext context = context_for(route);
        context.public_host = request_host(route);
        const ApiResult result = registry.call(std::string(capability), input, context);
        const std::string body = rest_envelope(result).dump();
        route.status(http::int_to_status(static_cast<unsigned>(rest_status(result))));
        route.res.set(http::field::content_type, "application/json; charset=utf-8");
        route.res.set(http::field::cache_control, "no-store");
        if (result.ok && result.value.contains("secret")) {
            route.res.set(http::field::set_cookie,
                          browser_session_cookie(route, result.value.value("secret", "")));
        }
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
        if (send_error) {
            co_return http::route_error(send_error);
        }
        co_return http::route_done;
    }

    boost::http::route_task serve_application_oauth(boost::http::route_params& route) {
        namespace http = boost::http;
        auto application = application_for_route(route);
        if (!application) {
            co_return co_await send_text(
                route,
                http::status::not_found,
                error_body(application.error().code, application.error().message),
                "application/json; charset=utf-8",
                "no-store");
        }
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return http::route_error(read_error);
        }
        ApiContext context = context_for(route);
        context.public_host = request_host(route);
        const ApiResult result =
            registry.call("start_oauth",
                          {{"pool_id", application->pool_id},
                           {"provider", form_field(body, "provider")},
                           {"device_name", form_field(body, "device_name")},
                           {"identity_document", form_field(body, "identity_document")}},
                          context);
        if (!result.ok) {
            co_return co_await serve_application_identity_page(
                route, "sign-in", result.error.message);
        }
        co_return co_await redirect(
            route,
            result.value.value("authorization_url", "/apps/" + application->id + "/sign-in"));
    }

    boost::http::route_task serve_application_jwks(boost::http::route_params& route) {
        ApiContext context = context_for(route);
        const ApiResult result = registry.call(
            "application_jwks", {{"application_id", route_value(route, "id")}}, context);
        return send_text(route,
                         boost::http::int_to_status(static_cast<unsigned>(rest_status(result))),
                         result.ok ? result.value.dump() : rest_envelope(result).dump(),
                         "application/json; charset=utf-8",
                         result.ok ? "public, max-age=300" : "no-store");
    }

    boost::http::route_task serve_sign_up(boost::http::route_params& route) {
        const ApiContext context = context_for(route);
        if (context.authorized) {
            return redirect(route, context.operator_credential ? "/repositories" : "/sessions");
        }
        if (auto application = application_for_domain(route)) {
            return send_text(route,
                             boost::http::status::ok,
                             web_ui_application_sign_up(*application, ""),
                             "text/html; charset=utf-8",
                             "no-store");
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
        if (auto application = application_for_domain(route)) {
            return send_text(route,
                             boost::http::status::ok,
                             web_ui_application_sign_in(*application, ""),
                             "text/html; charset=utf-8",
                             "no-store");
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
        if (auto application = application_for_domain(route)) {
            return send_text(route,
                             boost::http::status::ok,
                             web_ui_application_mfa(*application, ""),
                             "text/html; charset=utf-8",
                             "no-store");
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
        if (auto application = application_for_domain(route)) {
            return send_text(route,
                             boost::http::status::ok,
                             web_ui_application_passkey_sign_in(*application, ""),
                             "text/html; charset=utf-8",
                             "no-store");
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
        const bool enterprise_provider = provider_name.starts_with("sso-");
        const std::string provider_id =
            enterprise_provider ? provider_name.substr(std::string("sso-").size()) : std::string();
        const auto provider = enterprise_provider
                                  ? options.store->get_sso_oidc_provider(provider_id)
                                  : options.store->get_oauth_provider(flow->pool_id, provider_name);
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
        control::Result<control::IssuedSession> issued =
            enterprise_provider
                ? options.store->sign_in_sso({provider_id,
                                              provider->issuer,
                                              identity.subject,
                                              identity.email,
                                              identity.display_name,
                                              flow->identity_document,
                                              flow->device_name,
                                              identity.email.empty()
                                                  ? nlohmann::json::object()
                                                  : nlohmann::json{{"email", identity.email}}})
                : options.store->sign_in_oauth(identity);
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
        const std::string issued_pool = issued->session.pool_id;
        const ApiResult application_result =
            application_session_result(std::move(issued), context, "session.started");
        if (!application_result.ok) {
            co_return co_await oauth_error(
                route, http::status::service_unavailable, application_result.error.message);
        }
        if (!record_direct_event(
                context,
                audit::EventDraft{
                    .kind = "authentication",
                    .object = (enterprise_provider ? "oidc/" : "oauth/") + provider_name,
                    .action = "sign_in",
                    .outcome = "success",
                    .method = enterprise_provider ? "oidc" : "oauth",
                    .device = flow->device_name,
                })) {
            co_return co_await oauth_error(
                route, http::status::service_unavailable, "OAuth audit recording failed");
        }
        std::string cookie = "lake_session=" + application_result.value.value("secret", "") +
                             "; HttpOnly; SameSite=Lax; Path=/";
        if (request_is_https(route)) {
            cookie += "; Secure";
        }
        std::string destination = "/repositories";
        if (issued_pool != "lake") {
            if (auto application = options.store->get_application_by_pool(issued_pool)) {
                destination = application_redirect_location(*application, application_result);
            }
        }
        co_return co_await redirect(route, destination, cookie);
    }

    boost::http::route_task serve_entity_access_page(boost::http::route_params& route,
                                                     std::string_view object_type) {
        ApiContext ctx = context_for(route);
        if (!ctx.authorized) {
            return redirect(route, "/login");
        }
        const std::string id = query_value(route, "id");
        if (id.empty()) {
            return redirect(route, "/organizations");
        }
        const ApiResult access_result = registry.call(
            "entity_effective_access", {{"object_type", object_type}, {"id", id}}, ctx);
        if (!access_result.ok) {
            return serve_page_error(route, access_result);
        }
        std::vector<control::EffectiveAccessEntry> entries;
        for (const nlohmann::json& value : access_result.value.at("items")) {
            entries.push_back(access_from_json(value));
        }
        return send_text(
            route,
            boost::http::status::ok,
            web_ui_effective_access(object_from_json(access_result.value.at("object")), entries),
            "text/html; charset=utf-8",
            "no-store");
    }

    boost::http::route_task serve_repositories_page(boost::http::route_params& route) {
        ApiContext ctx = context_for(route);
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
        if (ctx.authorized) {
            const ApiResult customers = registry.call("list_customers", {{"limit", 1000}}, ctx);
            if (!customers.ok) {
                return serve_page_error(route, customers);
            }
            for (const auto& value : customers.value.at("items")) {
                control::Customer customer = customer_from_json(value);
                owners.emplace(customer.id, std::move(customer));
            }
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_repositories(repositories, owners, ctx.authorized),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_exports_page(boost::http::route_params& route) {
        ApiContext ctx = context_for(route);
        const std::string repository_id = query_value(route, "repository_id");
        if (repository_id.empty()) {
            return redirect(route, "/repositories");
        }
        const ApiResult repository_result =
            registry.call("get_repository", {{"id", repository_id}}, ctx);
        if (!repository_result.ok) {
            return serve_repository_page_error(route, ctx, repository_result);
        }
        const ApiResult exports_result =
            registry.call("list_exports", {{"repository_id", repository_id}}, ctx);
        if (!exports_result.ok) {
            return serve_repository_page_error(route, ctx, exports_result);
        }
        std::vector<control::ForgeRecord> exports;
        for (const nlohmann::json& value : exports_result.value.at("items")) {
            exports.push_back(forge_record_from_json(value));
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_exports(repository_from_json(repository_result.value), exports),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_export_download(boost::http::route_params& route) {
        ApiContext ctx = context_for(route);
        const std::string repository_id = query_value(route, "repository_id");
        if (repository_id.empty()) {
            return redirect(route, "/repositories");
        }
        const ApiResult exported =
            registry.call("export_repository",
                          {{"repository_id", repository_id},
                           {"include_git", query_value(route, "include_git") == "true"}},
                          ctx);
        if (!exported.ok) {
            return serve_repository_page_error(route, ctx, exported);
        }
        route.res.set("content-disposition", "attachment; filename=lake-export.json");
        return send_text(route,
                         boost::http::status::ok,
                         exported.value.at("archive").dump(2),
                         "application/vnd.lake.export+json",
                         "no-store");
    }

    boost::http::route_task serve_repository_page(boost::http::route_params& route) {
        ApiContext ctx = context_for(route);
        const std::string id = query_value(route, "id");
        if (id.empty())
            return redirect(route, "/repositories");
        const ApiResult repository_result = registry.call("get_repository", {{"id", id}}, ctx);
        if (!repository_result.ok) {
            return serve_repository_page_error(route, ctx, repository_result);
        }
        const control::Repository repository = repository_from_json(repository_result.value);
        std::optional<control::Customer> owner;
        if (ctx.authorized) {
            const ApiResult owner_result =
                registry.call("get_customer", {{"id", repository.customer_id}}, ctx);
            if (owner_result.ok) {
                owner = customer_from_json(owner_result.value);
            }
        }
        const ApiResult refs_result = registry.call("repository_refs", {{"id", id}}, ctx);
        if (!refs_result.ok) {
            return serve_repository_page_error(route, ctx, refs_result);
        }
        std::vector<RepositoryContentRef> refs;
        for (const nlohmann::json& value : refs_result.value.at("items")) {
            refs.push_back(repository_content_ref_from_json(value));
        }
        return send_text(
            route,
            boost::http::status::ok,
            web_ui_repository(repository, owner ? &*owner : nullptr, refs, ctx.authorized),
            "text/html; charset=utf-8",
            "no-store");
    }

    boost::http::route_task serve_repository_tree_page(boost::http::route_params& route) {
        ApiContext ctx = context_for(route);
        const std::string id = query_value(route, "id");
        const std::string ref = query_value(route, "ref");
        if (id.empty() || ref.empty()) {
            return redirect(route, "/repositories");
        }
        const ApiResult repository_result = registry.call("get_repository", {{"id", id}}, ctx);
        if (!repository_result.ok) {
            return serve_repository_page_error(route, ctx, repository_result);
        }
        const ApiResult tree_result =
            registry.call("repository_tree", {{"id", id}, {"ref", ref}}, ctx);
        if (!tree_result.ok) {
            return serve_repository_page_error(route, ctx, tree_result);
        }
        std::vector<RepositoryTreeEntry> entries;
        for (const nlohmann::json& value : tree_result.value.at("items")) {
            entries.push_back(repository_tree_entry_from_json(value));
        }
        return send_text(
            route,
            boost::http::status::ok,
            web_ui_repository_tree(
                repository_from_json(repository_result.value), ref, entries, ctx.authorized),
            "text/html; charset=utf-8",
            "no-store");
    }

    boost::http::route_task serve_repository_file_page(boost::http::route_params& route) {
        ApiContext ctx = context_for(route);
        const std::string id = query_value(route, "repository_id");
        const std::string ref = query_value(route, "ref");
        const std::string path = query_value(route, "path");
        if (id.empty() || ref.empty() || path.empty()) {
            return redirect(route, "/repositories");
        }
        const ApiResult repository_result = registry.call("get_repository", {{"id", id}}, ctx);
        if (!repository_result.ok) {
            return serve_repository_page_error(route, ctx, repository_result);
        }
        const ApiResult file_result =
            registry.call("repository_file", {{"id", id}, {"ref", ref}, {"path", path}}, ctx);
        if (!file_result.ok) {
            return serve_repository_page_error(route, ctx, file_result);
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_repository_file(repository_from_json(repository_result.value),
                                                repository_file_from_json(file_result.value),
                                                ctx.authorized),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_changes_page(boost::http::route_params& route) {
        ApiContext ctx = context_for(route);
        const std::string id = query_value(route, "repository_id");
        if (id.empty()) {
            return redirect(route, "/repositories");
        }
        const ApiResult repository_result = registry.call("get_repository", {{"id", id}}, ctx);
        if (!repository_result.ok) {
            return serve_repository_page_error(route, ctx, repository_result);
        }
        const ApiResult changes_result = registry.call("list_changes", {{"id", id}}, ctx);
        if (!changes_result.ok) {
            return serve_repository_page_error(route, ctx, changes_result);
        }
        std::vector<RepositoryChange> changes;
        for (const nlohmann::json& value : changes_result.value.at("items")) {
            changes.push_back(repository_change_from_json(value));
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_repository_log(repository_from_json(repository_result.value),
                                               changes,
                                               ctx.authorized),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_change_page(boost::http::route_params& route) {
        ApiContext ctx = context_for(route);
        const std::string id = query_value(route, "repository_id");
        const std::string change_id = query_value(route, "id");
        if (id.empty() || change_id.empty()) {
            return redirect(route, "/repositories");
        }
        const ApiResult repository_result = registry.call("get_repository", {{"id", id}}, ctx);
        if (!repository_result.ok) {
            return serve_repository_page_error(route, ctx, repository_result);
        }
        const ApiResult change_result =
            registry.call("get_change", {{"id", id}, {"change_id", change_id}}, ctx);
        if (!change_result.ok) {
            return serve_repository_page_error(route, ctx, change_result);
        }
        const ApiResult diff_result =
            registry.call("change_diff", {{"id", id}, {"change_id", change_id}}, ctx);
        if (!diff_result.ok) {
            return serve_repository_page_error(route, ctx, diff_result);
        }
        std::vector<RepositoryDiffEntry> differences;
        for (const nlohmann::json& value : diff_result.value.at("items")) {
            differences.push_back(repository_diff_entry_from_json(value));
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_change(repository_from_json(repository_result.value),
                                       repository_change_from_json(change_result.value),
                                       differences,
                                       ctx.authorized),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_conflicts_page(boost::http::route_params& route) {
        ApiContext ctx = context_for(route);
        const std::string id = query_value(route, "repository_id");
        if (id.empty()) {
            return redirect(route, "/repositories");
        }
        const ApiResult repository_result = registry.call("get_repository", {{"id", id}}, ctx);
        if (!repository_result.ok) {
            return serve_repository_page_error(route, ctx, repository_result);
        }
        const ApiResult conflicts_result = registry.call("list_conflicts", {{"id", id}}, ctx);
        if (!conflicts_result.ok) {
            return serve_repository_page_error(route, ctx, conflicts_result);
        }
        std::vector<RepositoryConflict> conflicts;
        for (const nlohmann::json& value : conflicts_result.value.at("items")) {
            conflicts.push_back(repository_conflict_from_json(value));
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_repository_conflicts(repository_from_json(repository_result.value),
                                                     conflicts,
                                                     ctx.authorized),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_artifact_page(boost::http::route_params& route) {
        ApiContext ctx = context_for(route);
        const std::string id = query_value(route, "repository_id");
        const std::string result_id = query_value(route, "id");
        if (id.empty() || result_id.empty()) {
            return redirect(route, "/repositories");
        }
        const ApiResult repository_result = registry.call("get_repository", {{"id", id}}, ctx);
        if (!repository_result.ok) {
            return serve_repository_page_error(route, ctx, repository_result);
        }
        const ApiResult artifact_result =
            registry.call("get_artifact", {{"id", id}, {"result_id", result_id}}, ctx);
        if (!artifact_result.ok) {
            return serve_repository_page_error(route, ctx, artifact_result);
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_artifact(repository_from_json(repository_result.value),
                                         repository_artifact_from_json(artifact_result.value),
                                         ctx.authorized),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_artifacts_page(boost::http::route_params& route) {
        ApiContext ctx = context_for(route);
        const std::string id = query_value(route, "repository_id");
        if (id.empty()) {
            return redirect(route, "/repositories");
        }
        const ApiResult repository_result = registry.call("get_repository", {{"id", id}}, ctx);
        if (!repository_result.ok) {
            return serve_repository_page_error(route, ctx, repository_result);
        }
        const RepositoryArtifactFilter filter{
            .action = query_value(route, "action"),
            .output = query_value(route, "output"),
            .content = query_value(route, "content"),
            .cursor = query_value(route, "cursor"),
            .limit = query_page_limit(route),
        };
        const ApiResult result = registry.call("list_artifacts",
                                               {{"id", id},
                                                {"action", filter.action},
                                                {"output", filter.output},
                                                {"content", filter.content},
                                                {"cursor", filter.cursor},
                                                {"limit", filter.limit}},
                                               ctx);
        if (!result.ok) {
            return serve_repository_page_error(route, ctx, result);
        }
        RepositoryArtifactPage page{.next_cursor = json_string(result.value, "next_cursor"),
                                    .done = result.value.value("done", false)};
        for (const nlohmann::json& value : result.value.at("items")) {
            page.items.push_back(repository_artifact_from_json(value));
        }
        return send_text(
            route,
            boost::http::status::ok,
            web_ui_artifacts(
                repository_from_json(repository_result.value), page, filter, ctx.authorized),
            "text/html; charset=utf-8",
            "no-store");
    }

    boost::http::route_task serve_drive_page(boost::http::route_params& route) {
        ApiContext context = context_for(route);
        const std::string repository_id = query_value(route, "repository_id");
        const std::string ref =
            query_value(route, "ref").empty() ? "main" : query_value(route, "ref");
        const std::string path = query_value(route, "path");
        if (repository_id.empty()) {
            return redirect(route, "/repositories");
        }
        const ApiResult result =
            registry.call("drive_browse",
                          {{"repository_id", repository_id}, {"ref", ref}, {"path", path}},
                          context);
        if (!result.ok) {
            return serve_repository_page_error(route, context, result);
        }
        std::vector<RepositoryTreeEntry> entries;
        for (const nlohmann::json& value : result.value.at("items")) {
            entries.push_back(repository_tree_entry_from_json(value));
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_drive_folder(repository_from_json(result.value.at("repository")),
                                             ref,
                                             path,
                                             entries,
                                             context.authorized),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_drive_preview_page(boost::http::route_params& route) {
        ApiContext context = context_for(route);
        const std::string repository_id = query_value(route, "repository_id");
        const std::string ref =
            query_value(route, "ref").empty() ? "main" : query_value(route, "ref");
        const std::string path = query_value(route, "path");
        const ApiResult result =
            registry.call("drive_preview",
                          {{"repository_id", repository_id}, {"ref", ref}, {"path", path}},
                          context);
        if (!result.ok) {
            return serve_repository_page_error(route, context, result);
        }
        const RepositoryFile file{json_string(result.value, "path"),
                                  json_string(result.value, "blob"),
                                  "",
                                  result.value.value("size", std::uint64_t{0})};
        const DrivePreview preview{json_string(result.value, "format"),
                                   json_string(result.value, "media_type"),
                                   json_string(result.value, "html")};
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_drive_preview(repository_from_json(result.value.at("repository")),
                                              ref,
                                              file,
                                              preview,
                                              context.authorized),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_drive_history_page(boost::http::route_params& route) {
        ApiContext context = context_for(route);
        const std::string repository_id = query_value(route, "repository_id");
        const std::string ref =
            query_value(route, "ref").empty() ? "main" : query_value(route, "ref");
        const std::string path = query_value(route, "path");
        const ApiResult result =
            registry.call("drive_history",
                          {{"repository_id", repository_id}, {"ref", ref}, {"path", path}},
                          context);
        if (!result.ok) {
            return serve_repository_page_error(route, context, result);
        }
        std::vector<nlohmann::json> changes;
        for (const nlohmann::json& value : result.value.at("items")) {
            changes.push_back(value);
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_drive_history(repository_from_json(result.value.at("repository")),
                                              ref,
                                              path,
                                              changes,
                                              context.authorized),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task render_drive_share_page(boost::http::route_params& route,
                                                    ApiContext context,
                                                    const std::string& repository_id,
                                                    const std::string& ref,
                                                    const std::string& path,
                                                    std::string created_link = {},
                                                    std::string error = {}) {
        const ApiResult result =
            registry.call("drive_effective_access",
                          {{"repository_id", repository_id}, {"ref", ref}, {"path", path}},
                          context);
        if (!result.ok) {
            return serve_repository_page_error(route, context, result);
        }
        std::vector<control::EffectiveAccessEntry> access;
        for (const nlohmann::json& value : result.value.at("items")) {
            access.push_back(access_from_json(value));
        }
        std::vector<nlohmann::json> links;
        for (const nlohmann::json& value : result.value.at("links")) {
            links.push_back(value);
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_drive_share(repository_from_json(result.value.at("repository")),
                                            ref,
                                            path,
                                            access,
                                            links,
                                            created_link,
                                            error),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_drive_share_page(boost::http::route_params& route) {
        ApiContext context = context_for(route);
        if (!context.authorized) {
            return redirect(route, "/login");
        }
        const std::string ref =
            query_value(route, "ref").empty() ? "main" : query_value(route, "ref");
        return render_drive_share_page(route,
                                       std::move(context),
                                       query_value(route, "repository_id"),
                                       ref,
                                       query_value(route, "path"));
    }

    boost::http::route_task serve_drive_share_direct(boost::http::route_params& route) {
        namespace http = boost::http;
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return http::route_error(read_error);
        }
        ApiContext context = context_for(route);
        const std::string repository_id = form_field(body, "repository_id");
        const std::string ref = form_field(body, "ref");
        const std::string path = form_field(body, "path");
        const ApiResult result =
            registry.call("share_drive_folder",
                          {{"repository_id", repository_id},
                           {"ref", ref},
                           {"path", path},
                           {"principal_taap_id", form_field(body, "principal_taap_id")},
                           {"role", form_field(body, "role")},
                           {"attestation_digest", form_field(body, "attestation_digest")}},
                          context);
        if (!result.ok) {
            co_return co_await render_drive_share_page(
                route, context, repository_id, ref, path, {}, result.error.message);
        }
        co_return co_await redirect(
            route,
            "/drive/share?repository_id=" + oauth_url_encode(repository_id) +
                "&ref=" + oauth_url_encode(ref) + "&path=" + oauth_url_encode(path));
    }

    boost::http::route_task serve_drive_share_link_create(boost::http::route_params& route) {
        namespace http = boost::http;
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return http::route_error(read_error);
        }
        ApiContext context = context_for(route);
        const std::string repository_id = form_field(body, "repository_id");
        const std::string ref = form_field(body, "ref");
        const std::string path = form_field(body, "path");
        const ApiResult result = registry.call("create_drive_share_link",
                                               {{"repository_id", repository_id},
                                                {"ref", ref},
                                                {"path", path},
                                                {"role", form_field(body, "role")},
                                                {"expires_at", form_field(body, "expires_at")}},
                                               context);
        if (!result.ok) {
            co_return co_await render_drive_share_page(
                route, context, repository_id, ref, path, {}, result.error.message);
        }
        co_return co_await render_drive_share_page(
            route, context, repository_id, ref, path, json_string(result.value, "url"));
    }

    boost::http::route_task serve_drive_share_link_revoke(boost::http::route_params& route) {
        namespace http = boost::http;
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return http::route_error(read_error);
        }
        ApiContext context = context_for(route);
        const ApiResult result = registry.call(
            "revoke_drive_share_link", {{"link_id", form_field(body, "link_id")}}, context);
        const std::string repository_id = form_field(body, "repository_id");
        const std::string ref = form_field(body, "ref");
        const std::string path = form_field(body, "path");
        if (!result.ok) {
            co_return co_await render_drive_share_page(
                route, context, repository_id, ref, path, {}, result.error.message);
        }
        co_return co_await redirect(
            route,
            "/drive/share?repository_id=" + oauth_url_encode(repository_id) +
                "&ref=" + oauth_url_encode(ref) + "&path=" + oauth_url_encode(path));
    }

    boost::http::route_task serve_drive_restore(boost::http::route_params& route) {
        namespace http = boost::http;
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return http::route_error(read_error);
        }
        ApiContext context = context_for(route);
        const std::string repository_id = form_field(body, "repository_id");
        const std::string ref = form_field(body, "ref");
        const std::string path = form_field(body, "path");
        const ApiResult result = registry.call("restore_drive_version",
                                               {{"repository_id", repository_id},
                                                {"ref", ref},
                                                {"path", path},
                                                {"change_id", form_field(body, "change_id")}},
                                               context);
        if (!result.ok) {
            co_return co_await serve_repository_page_error(route, context, result);
        }
        co_return co_await redirect(
            route,
            "/drive/history?repository_id=" + oauth_url_encode(repository_id) +
                "&ref=" + oauth_url_encode(ref) + "&path=" + oauth_url_encode(path));
    }

    boost::http::route_task serve_drive_share_link(boost::http::route_params& route) {
        ApiContext context = context_for(route);
        const std::string ref =
            query_value(route, "ref").empty() ? "main" : query_value(route, "ref");
        const ApiResult result =
            registry.call("resolve_drive_share_link",
                          {{"token", route_value(route, "token")}, {"ref", ref}},
                          context);
        if (!result.ok) {
            return send_text(route,
                             boost::http::int_to_status(static_cast<unsigned>(rest_status(result))),
                             web_ui_drive_link("Shared folder", {}, result.error.message),
                             "text/html; charset=utf-8",
                             "no-store");
        }
        std::vector<RepositoryTreeEntry> entries;
        for (const nlohmann::json& value : result.value.at("items")) {
            entries.push_back(repository_tree_entry_from_json(value));
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_drive_link(json_string(result.value, "path"), entries, ""),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    control::Result<control::Repository> hub_repository(std::string_view hub_id) {
        if (!options.store) {
            return std::unexpected(
                control::StoreError{"store_unavailable", "no control-plane store is configured"});
        }
        if (const auto direct = options.store->get_repository(std::string(hub_id))) {
            return *direct;
        }
        if (hub_id.starts_with("models/") || hub_id.starts_with("datasets/")) {
            const std::size_t prefix = hub_id.find('/') + 1;
            if (const auto direct =
                    options.store->get_repository(std::string(hub_id.substr(prefix)))) {
                return *direct;
            }
        }
        std::string_view customer_name;
        std::string_view repository_name = hub_id;
        const std::size_t slash = hub_id.find('/');
        if (slash != std::string_view::npos) {
            customer_name = hub_id.substr(0, slash);
            repository_name = hub_id.substr(slash + 1);
        }
        const auto customers = options.store->list_customers(1'000);
        if (!customers) {
            return std::unexpected(customers.error());
        }
        std::optional<control::Repository> match;
        for (const control::Customer& customer : *customers) {
            if (!customer_name.empty() && customer.name != customer_name) {
                continue;
            }
            const auto repositories = options.store->list_repositories(customer.id, 1'000);
            if (!repositories) {
                return std::unexpected(repositories.error());
            }
            for (const control::Repository& repository : *repositories) {
                if (repository.name != repository_name || !repository.deleted_at.empty()) {
                    continue;
                }
                if (match) {
                    return std::unexpected(control::StoreError{
                        "conflict", "repository name is ambiguous without its customer"});
                }
                match = repository;
            }
        }
        if (!match) {
            return std::unexpected(control::StoreError{"not_found", "repository does not exist"});
        }
        return *match;
    }

    std::string hub_id(const boost::http::route_params& route, bool git = false) const {
        std::string name = route_value(route, "id");
        if (git) {
            name = route_value(route, "repo_git");
            if (name.ends_with(".git")) {
                name.resize(name.size() - 4);
            }
        }
        const std::string customer = route_value(route, "namespace");
        return customer.empty() ? name : customer + "/" + name;
    }

    std::string hub_origin(const boost::http::route_params& route) const {
        if (!options.public_host.empty()) {
            return options.public_host.ends_with('/')
                       ? options.public_host.substr(0, options.public_host.size() - 1)
                       : options.public_host;
        }
        const std::string host = route.req.count(boost::http::field::host) == 0
                                     ? "127.0.0.1"
                                     : std::string(route.req.at(boost::http::field::host));
        return (request_is_https(route) ? "https://" : "http://") + host;
    }

    boost::http::route_task send_hub_result(boost::http::route_params& route,
                                            const ApiResult& result,
                                            boost::http::status success = boost::http::status::ok) {
        if (!result.ok) {
            return send_text(route,
                             boost::http::int_to_status(static_cast<unsigned>(rest_status(result))),
                             error_body(result.error.code, result.error.message),
                             "application/json; charset=utf-8",
                             "no-store");
        }
        return send_text(
            route, success, result.value.dump(), "application/json; charset=utf-8", "no-store");
    }

    boost::http::route_task serve_hub_info(boost::http::route_params& route,
                                           std::string_view kind) {
        const auto repository = hub_repository(hub_id(route));
        if (!repository) {
            return send_text(route,
                             boost::http::status::not_found,
                             error_body(repository.error().code, repository.error().message),
                             "application/json; charset=utf-8",
                             "no-store");
        }
        ApiContext context = context_for(route);
        const std::string revision =
            route_value(route, "revision").empty() ? "main" : route_value(route, "revision");
        const ApiResult result = registry.call(kind == "model" ? "get_model" : "get_dataset",
                                               {{"id", repository->id}, {"revision", revision}},
                                               context);
        return send_hub_result(route, result);
    }

    boost::http::route_task serve_hub_tree(boost::http::route_params& route,
                                           std::string_view kind) {
        const auto repository = hub_repository(hub_id(route));
        if (!repository) {
            return send_text(route,
                             boost::http::status::not_found,
                             error_body(repository.error().code, repository.error().message),
                             "application/json; charset=utf-8",
                             "no-store");
        }
        ApiContext context = context_for(route);
        const ApiResult result =
            registry.call(kind == "model" ? "get_model" : "get_dataset",
                          {{"id", repository->id}, {"revision", route_value(route, "revision")}},
                          context);
        if (!result.ok) {
            return send_text(route,
                             boost::http::int_to_status(static_cast<unsigned>(rest_status(result))),
                             error_body(result.error.code, result.error.message),
                             "application/json; charset=utf-8",
                             "no-store");
        }
        nlohmann::json entries = nlohmann::json::array();
        for (const nlohmann::json& file : result.value.at("siblings")) {
            entries.push_back({{"type", "file"},
                               {"path", file.at("rfilename")},
                               {"size", file.at("size")},
                               {"oid", file.at("blobId")}});
        }
        return send_text(route,
                         boost::http::status::ok,
                         entries.dump(),
                         "application/json; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_hub_file(boost::http::route_params& route,
                                           std::string_view kind) {
        const auto repository = hub_repository(hub_id(route));
        if (!repository) {
            co_return co_await send_text(
                route,
                boost::http::status::not_found,
                error_body(repository.error().code, repository.error().message),
                "application/json; charset=utf-8",
                "no-store");
        }
        ApiContext context = context_for(route);
        const ApiResult result =
            registry.call(kind == "model" ? "resolve_model_file" : "resolve_dataset_file",
                          {{"id", repository->id},
                           {"revision", route_value(route, "revision")},
                           {"path", route_value(route, "file")}},
                          context);
        if (!result.ok) {
            co_return co_await send_text(
                route,
                boost::http::int_to_status(static_cast<unsigned>(rest_status(result))),
                error_body(result.error.code, result.error.message),
                "application/json; charset=utf-8",
                "no-store");
        }
        const std::string etag = "\"" + json_string(result.value, "blob") + "\"";
        co_return co_await send_bytes(route,
                                      boost::http::status::ok,
                                      "application/octet-stream",
                                      context.sync_response,
                                      etag,
                                      json_string(result.value, "revision"),
                                      route.req.method() == boost::http::method::head);
    }

    boost::http::route_task serve_model_page(boost::http::route_params& route) {
        const auto repository = hub_repository(hub_id(route));
        if (!repository) {
            return send_text(route,
                             boost::http::status::not_found,
                             web_ui_shell("lake — model", "not found", "<h1>Model not found</h1>"),
                             "text/html; charset=utf-8",
                             "no-store");
        }
        ApiContext context = context_for(route);
        const ApiResult result =
            registry.call("get_model", {{"id", repository->id}, {"revision", "main"}}, context);
        if (!result.ok) {
            return send_text(route,
                             boost::http::int_to_status(static_cast<unsigned>(rest_status(result))),
                             web_ui_shell("lake — model",
                                          "error",
                                          "<h1>Model unavailable</h1><p class=\"error\">" +
                                              web_ui_escape(result.error.message) + "</p>"),
                             "text/html; charset=utf-8",
                             "no-store");
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_model(result.value, context.authorized),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_hub_preupload(boost::http::route_params& route) {
        const auto repository = hub_repository(hub_id(route));
        if (!repository) {
            co_return co_await send_text(
                route,
                boost::http::status::not_found,
                error_body(repository.error().code, repository.error().message),
                "application/json; charset=utf-8",
                "no-store");
        }
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return boost::http::route_error(read_error);
        }
        nlohmann::json request = nlohmann::json::parse(body, nullptr, false);
        if (request.is_discarded() || !request.is_object()) {
            co_return co_await send_text(route,
                                         boost::http::status::bad_request,
                                         error_body("invalid_input", "invalid preupload body"),
                                         "application/json; charset=utf-8",
                                         "no-store");
        }
        request["id"] = repository->id;
        request["revision"] = route_value(route, "revision");
        ApiContext context = context_for(route);
        const ApiResult result = registry.call("prepare_repository_upload", request, context);
        co_return co_await send_hub_result(route, result);
    }

    boost::http::route_task serve_hub_commit(boost::http::route_params& route) {
        const auto repository = hub_repository(hub_id(route));
        if (!repository) {
            co_return co_await send_text(
                route,
                boost::http::status::not_found,
                error_body(repository.error().code, repository.error().message),
                "application/json; charset=utf-8",
                "no-store");
        }
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return boost::http::route_error(read_error);
        }
        nlohmann::json operations = nlohmann::json::array();
        std::string message = "Hugging Face upload";
        std::size_t offset = 0;
        while (offset < body.size()) {
            const std::size_t newline = body.find('\n', offset);
            const std::string_view line(body.data() + offset,
                                        newline == std::string::npos ? body.size() - offset
                                                                     : newline - offset);
            if (!line.empty()) {
                const nlohmann::json item = nlohmann::json::parse(line, nullptr, false);
                if (item.is_discarded() || !item.is_object()) {
                    co_return co_await send_text(
                        route,
                        boost::http::status::bad_request,
                        error_body("invalid_input", "invalid commit NDJSON"),
                        "application/json; charset=utf-8",
                        "no-store");
                }
                const std::string key = item.value("key", "");
                const nlohmann::json value = item.value("value", nlohmann::json::object());
                if (key == "header") {
                    message = value.value("summary", message);
                } else if (key == "file") {
                    operations.push_back({{"type", "file"},
                                          {"path", value.value("path", "")},
                                          {"content", value.value("content", "")}});
                } else if (key == "lfsFile") {
                    operations.push_back({{"type", "lfsFile"},
                                          {"path", value.value("path", "")},
                                          {"oid", value.value("oid", "")},
                                          {"size", value.value("size", std::uint64_t{0})}});
                } else if (key == "deletedFile" || key == "deletedFolder") {
                    operations.push_back({{"type", key}, {"path", value.value("path", "")}});
                }
            }
            if (newline == std::string::npos) {
                break;
            }
            offset = newline + 1;
        }
        ApiContext context = context_for(route);
        const ApiResult result = registry.call("commit_repository_files",
                                               {{"id", repository->id},
                                                {"revision", route_value(route, "revision")},
                                                {"message", message},
                                                {"operations", std::move(operations)}},
                                               context);
        if (!result.ok) {
            co_return co_await send_hub_result(route, result);
        }
        nlohmann::json response{{"commitOid", result.value.at("commitOid")},
                                {"commitUrl",
                                 hub_origin(route) + "/" + hub_id(route) + "/commit/" +
                                     json_string(result.value, "commitOid")}};
        co_return co_await send_text(route,
                                     boost::http::status::ok,
                                     response.dump(),
                                     "application/json; charset=utf-8",
                                     "no-store");
    }

    boost::http::route_task serve_hub_create(boost::http::route_params& route) {
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return boost::http::route_error(read_error);
        }
        const nlohmann::json request = nlohmann::json::parse(body, nullptr, false);
        if (request.is_discarded() || !request.is_object()) {
            co_return co_await send_text(route,
                                         boost::http::status::bad_request,
                                         error_body("invalid_input", "invalid create body"),
                                         "application/json; charset=utf-8",
                                         "no-store");
        }
        ApiContext context = context_for(route);
        const auto customers =
            options.store ? options.store->list_customers(1'000)
                          : control::Result<std::vector<control::Customer>>(
                                std::unexpected(control::StoreError{
                                    "store_unavailable", "no control-plane store is configured"}));
        if (!customers) {
            co_return co_await send_text(
                route,
                boost::http::status::service_unavailable,
                error_body(customers.error().code, customers.error().message),
                "application/json; charset=utf-8",
                "no-store");
        }
        std::string name = request.value("name", "");
        std::string customer_name = request.value("organization", "");
        const std::size_t slash = name.find('/');
        if (slash != std::string::npos) {
            customer_name = name.substr(0, slash);
            name = name.substr(slash + 1);
        }
        std::optional<control::Customer> owner;
        for (const control::Customer& customer : *customers) {
            const bool named = !customer_name.empty() && customer.name == customer_name;
            const bool personal = customer_name.empty() && !context.acting_user_id.empty() &&
                                  customer.user_id == context.acting_user_id;
            const bool operator_default = customer_name.empty() &&
                                          context.acting_identity == "admin" &&
                                          customers->size() == 1;
            if (named || personal || operator_default) {
                if (owner) {
                    co_return co_await send_text(
                        route,
                        boost::http::status::conflict,
                        error_body("conflict", "repository owner is ambiguous"),
                        "application/json; charset=utf-8",
                        "no-store");
                }
                owner = customer;
            }
        }
        if (!owner || name.empty()) {
            co_return co_await send_text(
                route,
                boost::http::status::bad_request,
                error_body("invalid_input", "name and a resolvable repository owner are required"),
                "application/json; charset=utf-8",
                "no-store");
        }
        const std::string visibility = request.value("private", true) ? "private" : "public";
        const ApiResult result = registry.call("create_repository",
                                               {{"customer_id", owner->id},
                                                {"name", name},
                                                {"description", ""},
                                                {"visibility", visibility}},
                                               context);
        if (!result.ok) {
            co_return co_await send_hub_result(route, result);
        }
        const std::string repo_id = owner->name + "/" + name;
        co_return co_await send_text(
            route,
            boost::http::status::ok,
            nlohmann::json({{"url", hub_origin(route) + "/" + repo_id}, {"repo_id", repo_id}})
                .dump(),
            "application/json; charset=utf-8",
            "no-store");
    }

    boost::http::route_task serve_hub_validate_yaml(boost::http::route_params& route) {
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return boost::http::route_error(read_error);
        }
        const nlohmann::json request = nlohmann::json::parse(body, nullptr, false);
        if (request.is_discarded() || !request.is_object() || !request.contains("content") ||
            !request.at("content").is_string() || !request.contains("repoType") ||
            !request.at("repoType").is_string()) {
            co_return co_await send_text(
                route,
                boost::http::status::bad_request,
                error_body("invalid_input", "content and repoType are required"),
                "application/json; charset=utf-8",
                "no-store");
        }
        co_return co_await send_text(route,
                                     boost::http::status::ok,
                                     nlohmann::json({{"warnings", nlohmann::json::array()},
                                                     {"errors", nlohmann::json::array()}})
                                         .dump(),
                                     "application/json; charset=utf-8",
                                     "no-store");
    }

    std::string hub_upload_signature(std::string_view repository_id,
                                     std::string_view oid,
                                     std::uint64_t size,
                                     std::uint64_t expires) const {
        const std::string payload = std::string(repository_id) + "\n" + std::string(oid) + "\n" +
                                    std::to_string(size) + "\n" + std::to_string(expires);
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        unsigned int digest_size = 0;
        if (HMAC(EVP_sha256(),
                 options.instance_kek.data(),
                 static_cast<int>(options.instance_kek.size()),
                 reinterpret_cast<const unsigned char*>(payload.data()),
                 payload.size(),
                 digest.data(),
                 &digest_size) == nullptr) {
            return {};
        }
        return control::identity_crypto::base64url_encode(
            std::string_view(reinterpret_cast<const char*>(digest.data()), digest_size));
    }

    boost::http::route_task serve_hub_lfs_batch(boost::http::route_params& route) {
        const auto repository = hub_repository(hub_id(route, true));
        if (!repository) {
            co_return co_await send_text(
                route,
                boost::http::status::not_found,
                error_body(repository.error().code, repository.error().message),
                "application/json; charset=utf-8",
                "no-store");
        }
        ApiContext context = context_for(route);
        const ApiResult authorized =
            registry.call("prepare_repository_upload",
                          {{"id", repository->id}, {"files", nlohmann::json::array()}},
                          context);
        if (!authorized.ok) {
            co_return co_await send_hub_result(route, authorized);
        }
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return boost::http::route_error(read_error);
        }
        const nlohmann::json request = nlohmann::json::parse(body, nullptr, false);
        if (request.is_discarded() || !request.is_object() ||
            request.value("operation", "") != "upload" || !request.contains("objects") ||
            !request.at("objects").is_array() || options.instance_kek.empty()) {
            co_return co_await send_text(route,
                                         boost::http::status::bad_request,
                                         error_body("invalid_input", "invalid LFS batch request"),
                                         "application/json; charset=utf-8",
                                         "no-store");
        }
        const std::uint64_t expires =
            static_cast<std::uint64_t>(
                std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())) +
            900;
        nlohmann::json objects = nlohmann::json::array();
        for (const nlohmann::json& object : request.at("objects")) {
            const std::string oid = object.value("oid", "");
            const std::uint64_t size = object.value("size", std::uint64_t{0});
            const std::string signature = hub_upload_signature(repository->id, oid, size, expires);
            const std::string href = hub_origin(route) + "/api/lfs/" + repository->id + "/" + oid +
                                     "?size=" + std::to_string(size) +
                                     "&expires=" + std::to_string(expires) +
                                     "&signature=" + signature;
            objects.push_back({{"oid", oid},
                               {"size", size},
                               {"authenticated", true},
                               {"actions", {{"upload", {{"href", href}}}}}});
        }
        co_return co_await send_text(
            route,
            boost::http::status::ok,
            nlohmann::json({{"transfer", "basic"}, {"objects", std::move(objects)}}).dump(),
            "application/vnd.git-lfs+json",
            "no-store");
    }

    boost::http::route_task serve_hub_lfs_upload(boost::http::route_params& route) {
        const std::string repository_id = route_value(route, "repository");
        const std::string oid = route_value(route, "oid");
        const auto size = decimal_value(query_value(route, "size"));
        const auto expires = decimal_value(query_value(route, "expires"));
        const std::string signature = query_value(route, "signature");
        const std::uint64_t now = static_cast<std::uint64_t>(
            std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
        if (!size || !expires || *expires < now || options.instance_kek.empty() ||
            !constant_time_equal(signature,
                                 hub_upload_signature(repository_id, oid, *size, *expires))) {
            co_return co_await send_text(
                route,
                boost::http::status::forbidden,
                error_body("unauthorized", "invalid or expired upload URL"),
                "application/json; charset=utf-8",
                "no-store");
        }
        auto body = co_await read_sync_body(route);
        if (!body) {
            co_return co_await send_text(route,
                                         boost::http::status::bad_request,
                                         error_body("invalid_input", "could not read LFS upload"),
                                         "application/json; charset=utf-8",
                                         "no-store");
        }
        ApiContext context = context_for(route);
        context.authorized = true;
        context.operator_credential = true;
        context.acting_identity = "admin";
        context.authentication_method = "signed-upload";
        context.sync_wire_request = true;
        context.sync_request = as_bytes(*body);
        const ApiResult result =
            registry.call("stage_repository_file",
                          {{"id", repository_id}, {"oid", oid}, {"size", *size}},
                          context);
        co_return co_await send_hub_result(route, result);
    }

    boost::http::route_task serve_search_page(boost::http::route_params& route) {
        ApiContext context = context_for(route);
        if (!context.authorized) {
            return redirect(route, "/login");
        }
        const search::Query query{.text = query_value(route, "query"),
                                  .customer = query_value(route, "customer"),
                                  .repository = query_value(route, "repository"),
                                  .ref = query_value(route, "ref"),
                                  .limit = 100,
                                  .change = query_value(route, "change"),
                                  .history = query_value(route, "history") == "1"};
        search::QueryResult search_result;
        if (!query.text.empty()) {
            nlohmann::json input{{"query", query.text}, {"limit", query.limit}};
            if (!query.customer.empty()) {
                input["customer"] = query.customer;
            }
            if (!query.repository.empty()) {
                input["repository"] = query.repository;
            }
            if (!query.ref.empty()) {
                input["ref"] = query.ref;
            }
            if (!query.change.empty()) {
                input["change"] = query.change;
            }
            if (query.history) {
                input["history"] = true;
            }
            const ApiResult result = registry.call("search", input, context);
            if (!result.ok) {
                return serve_page_error(route, result);
            }
            const std::string mode = result.value.value("mode", "hot");
            search_result.mode = mode == "scan"       ? search::QueryMode::Scan
                                 : mode == "snapshot" ? search::QueryMode::Snapshot
                                                      : search::QueryMode::Hot;
            search_result.scanned_changes = result.value.value("scanned_changes", std::size_t{0});
            for (const nlohmann::json& item : result.value.at("items")) {
                search_result.items.push_back(search::Hit{
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
                         web_ui_search(query, search_result),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_metering_page(boost::http::route_params& route) {
        ApiContext context = context_for(route);
        if (!context.authorized) {
            return redirect(route, "/login");
        }
        const std::string customer_id = query_value(route, "customer_id");
        nlohmann::json usage{{"customer_id", customer_id}, {"items", nlohmann::json::array()}};
        if (!customer_id.empty()) {
            const ApiResult result =
                registry.call("metering_usage", {{"customer_id", customer_id}}, context);
            if (!result.ok) {
                return serve_page_error(route, result);
            }
            usage = result.value;
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_metering(usage),
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
        nlohmann::json log_configuration;
        if (ctx.checkpoints) {
            const ApiResult configuration =
                registry.call("transparency_log_configuration", nlohmann::json::object(), ctx);
            if (!configuration.ok) {
                return serve_page_error(route, configuration);
            }
            log_configuration = configuration.value;
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_audit(events, filter, log_configuration),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_transparency_page(boost::http::route_params& route) {
        ApiContext context = context_for(route);
        const std::string identity = query_value(route, "identity");
        nlohmann::json scan;
        if (!identity.empty()) {
            const ApiResult result =
                registry.call("transparency_identity_scan", {{"identity", identity}}, context);
            if (!result.ok) {
                return serve_page_error(route, result);
            }
            scan = result.value;
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_transparency(identity, scan),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_secret_mounts_page(boost::http::route_params& route) {
        ApiContext context = context_for(route);
        if (!context.authorized) {
            return redirect(route, "/login");
        }
        nlohmann::json input{{"limit", 500}};
        const std::string customer_id = query_value(route, "customer_id");
        const std::string organization_id = query_value(route, "organization_id");
        if (!customer_id.empty()) {
            input["customer_id"] = customer_id;
        }
        if (!organization_id.empty()) {
            input["organization_id"] = organization_id;
        }
        const ApiResult result = registry.call("list_secret_mounts", input, context);
        if (!result.ok) {
            return serve_page_error(route, result);
        }
        std::vector<control::SecretMount> mounts;
        for (const nlohmann::json& item : result.value.at("items")) {
            mounts.push_back(secret_mount_from_json(item));
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_secret_mounts(mounts),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_secrets_page(boost::http::route_params& route) {
        ApiContext context = context_for(route);
        if (!context.authorized) {
            return redirect(route, "/login");
        }
        const std::string mount_id = query_value(route, "mount_id");
        if (mount_id.empty()) {
            return redirect(route, "/secret-mounts");
        }
        const ApiResult mount_result =
            registry.call("get_secret_mount", {{"mount_id", mount_id}}, context);
        if (!mount_result.ok) {
            return serve_page_error(route, mount_result);
        }
        const control::SecretMount mount = secret_mount_from_json(mount_result.value);
        const std::string name = query_value(route, "name");
        std::optional<control::SecretMetadata> metadata;
        if (!name.empty()) {
            const ApiResult metadata_result =
                registry.call("secret_metadata", {{"mount_id", mount_id}, {"name", name}}, context);
            if (!metadata_result.ok) {
                return serve_page_error(route, metadata_result);
            }
            metadata = secret_metadata_from_json(metadata_result.value);
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_secrets(mount, name, metadata ? &*metadata : nullptr),
                         "text/html; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_encryption_keys_page(boost::http::route_params& route) {
        ApiContext context = context_for(route);
        if (!context.authorized) {
            return redirect(route, "/login");
        }
        const std::string mount_id = query_value(route, "mount_id");
        if (mount_id.empty()) {
            return redirect(route, "/secret-mounts");
        }
        const ApiResult mount_result =
            registry.call("get_secret_mount", {{"mount_id", mount_id}}, context);
        if (!mount_result.ok) {
            return serve_page_error(route, mount_result);
        }
        const ApiResult keys_result = registry.call(
            "list_encryption_keys", {{"mount_id", mount_id}, {"limit", 500}}, context);
        if (!keys_result.ok) {
            return serve_page_error(route, keys_result);
        }
        std::vector<control::EncryptionKey> keys;
        for (const nlohmann::json& item : keys_result.value.at("items")) {
            keys.push_back(encryption_key_from_json(item));
        }
        return send_text(route,
                         boost::http::status::ok,
                         web_ui_encryption_keys(secret_mount_from_json(mount_result.value), keys),
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

    boost::http::route_task serve_font(boost::http::route_params& route, std::string_view name) {
        return send_text(route,
                         boost::http::status::ok,
                         std::string(web_ui_font(name)),
                         "font/woff2",
                         "public, max-age=31536000, immutable");
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

    boost::http::route_task send_scim(boost::http::route_params& route,
                                      const ApiResult& result,
                                      boost::http::status success_status) {
        namespace http = boost::http;
        const http::status status = result.ok ? success_status : scim_status(result);
        const std::string body = status == http::status::no_content
                                     ? std::string()
                                     : (result.ok ? result.value.dump() : scim_error_body(result));
        if (result.ok && success_status == http::status::created && result.value.contains("meta") &&
            result.value["meta"].is_object()) {
            const std::string location = result.value["meta"].value("location", "");
            if (!location.empty()) {
                route.res.set(http::field::location, location);
            }
        }
        if (!result.ok && status == http::status::unauthorized) {
            route.res.set(http::field::www_authenticate, "Bearer");
        }
        co_return co_await send_text(
            route, status, body, "application/scim+json; charset=utf-8", "no-store");
    }

    boost::http::route_task serve_scim_collection(boost::http::route_params& route,
                                                  std::string_view resource_type) {
        namespace http = boost::http;
        const bool users = resource_type == "Users";
        nlohmann::json input{{"bearer_token", bearer_of(route)}};
        std::string capability;
        http::status success_status = http::status::ok;
        if (route.req.method() == http::method::get) {
            capability = users ? "scim_list_users" : "scim_list_groups";
            input["filter"] = form_decode(query_value(route, "filter"));
            input["start_index"] = query_integer(route, "startIndex", 1);
            input["count"] = query_integer(route, "count", 100);
        } else {
            capability = users ? "scim_upsert_user" : "scim_upsert_group";
            success_status = http::status::created;
            http::json_sink sink;
            auto [read_error, bytes_read] = co_await boost::capy::push_to(route.req_body, sink);
            if (read_error || bytes_read == 0) {
                co_return co_await send_scim(
                    route,
                    ApiResult::failure("invalid_input", "SCIM resource body is required"),
                    success_status);
            }
            nlohmann::json resource = to_nlohmann(sink.release());
            if (resource.is_discarded() || !resource.is_object()) {
                co_return co_await send_scim(
                    route,
                    ApiResult::failure("invalid_input", "SCIM resource body is not valid JSON"),
                    success_status);
            }
            input["resource"] = std::move(resource);
        }
        ApiContext context = context_for(route);
        const ApiResult result = registry.call(capability, input, context);
        co_return co_await send_scim(route, result, success_status);
    }

    boost::http::route_task serve_scim_resource(boost::http::route_params& route,
                                                std::string_view resource_type) {
        namespace http = boost::http;
        const bool users = resource_type == "Users";
        std::string resource_id;
        for (const auto& [name, value] : route.params) {
            if (name == "id") {
                resource_id = value;
            }
        }
        nlohmann::json input{{"bearer_token", bearer_of(route)}, {"resource_id", resource_id}};
        std::string capability;
        http::status success_status = http::status::ok;
        if (route.req.method() == http::method::get) {
            capability = users ? "scim_get_user" : "scim_get_group";
        } else if (route.req.method() == http::method::delete_) {
            capability = users ? "scim_delete_user" : "scim_delete_group";
            success_status = http::status::no_content;
        } else {
            capability = users ? "scim_patch_user" : "scim_patch_group";
            http::json_sink sink;
            auto [read_error, bytes_read] = co_await boost::capy::push_to(route.req_body, sink);
            if (read_error || bytes_read == 0) {
                co_return co_await send_scim(
                    route,
                    ApiResult::failure("invalid_input", "SCIM PATCH body is required"),
                    success_status);
            }
            nlohmann::json patch = to_nlohmann(sink.release());
            if (patch.is_discarded() || !patch.is_object()) {
                co_return co_await send_scim(
                    route,
                    ApiResult::failure("invalid_input", "SCIM PATCH body is not valid JSON"),
                    success_status);
            }
            input["patch"] = std::move(patch);
        }
        ApiContext context = context_for(route);
        const ApiResult result = registry.call(capability, input, context);
        co_return co_await send_scim(route, result, success_status);
    }

    boost::http::route_task serve_sync_post(boost::http::route_params& route,
                                            std::string capability_name) {
        ApiContext context = context_for(route);
        const nlohmann::json input = {
            {"repository_id", route_value(route, "repository_id")},
        };
        auto body = co_await read_sync_body(route);
        if (!body) {
            const ApiResult result = registry.call(capability_name, input, context);
            if (!result.ok && result.error.code != "invalid_input") {
                co_return co_await send_sync_error(route, sync_error_of(result, context));
            }
            co_return co_await send_sync_error(route,
                                               liquid::error{
                                                   liquid::errc::invalid_argument,
                                                   "could not read repository sync request",
                                                   body.error().message(),
                                               });
        }
        context.sync_wire_request = true;
        context.sync_request = as_bytes(*body);
        const ApiResult result = registry.call(capability_name, input, context);
        if (!context.reauthorization_results.empty()) {
            const std::string encoded =
                control::identity_crypto::base64url_encode(context.reauthorization_results.dump());
            route.res.set("liquid-reauthorization", encoded);
        }
        if (!result.ok) {
            co_return co_await send_sync_error(route, sync_error_of(result, context));
        }
        co_return co_await send_bytes(
            route, boost::http::status::ok, sync_content_type, context.sync_response);
    }

    boost::http::route_task serve_sync_range(boost::http::route_params& route) {
        ApiContext context = context_for(route);
        context.sync_wire_request = true;
        nlohmann::json input = {
            {"repository_id", route_value(route, "repository_id")},
            {"digest", route_value(route, "digest")},
        };
        const auto offset = decimal_value(query_value(route, "offset"));
        const auto length = decimal_value(query_value(route, "length"));
        input["offset"] = offset ? nlohmann::json(*offset) : nlohmann::json("invalid");
        input["length"] = length ? nlohmann::json(*length) : nlohmann::json("invalid");
        const ApiResult result = registry.call("read_chunk", input, context);
        if (!result.ok) {
            co_return co_await send_sync_error(route, sync_error_of(result, context));
        }
        co_return co_await send_bytes(
            route, boost::http::status::ok, sync_content_type, context.sync_response);
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
                capability->name == "verify_mfa" || capability->name == "sign_in_passkey" ||
                capability->name == "recover_with_code" || capability->name == "complete_recovery";
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
    if (!impl.failure_reason.empty()) {
        return false;
    }

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
    for (const std::string_view font : {"ibm-plex-sans-400.woff2",
                                        "ibm-plex-sans-500.woff2",
                                        "ibm-plex-sans-600.woff2",
                                        "ibm-plex-mono-400.woff2",
                                        "ibm-plex-mono-500.woff2"}) {
        const std::string route_path = "/fonts/" + std::string(font);
        router.add(http::method::get,
                   route_path,
                   [&impl, font, route_path](http::route_params& route) -> http::route_task {
                       return impl.observe_request(route, route_path, impl.serve_font(route, font));
                   });
    }
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
    router.add(
        http::method::get, "/applications", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(
                route, "/applications", impl.serve_applications_page(route));
        });
    router.add(http::method::post,
               "/applications",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/applications", impl.serve_application_register(route));
               });
    router.add(
        http::method::get, "/application", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/application", impl.serve_application_page(route));
        });
    router.add(http::method::get,
               "/application/users",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/application/users", impl.serve_application_users_page(route));
               });
    router.add(http::method::post,
               "/application/users/suspend",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(route,
                                               "/application/users/suspend",
                                               impl.serve_application_user_suspend(route));
               });
    router.add(http::method::get,
               "/application/organizations",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(route,
                                               "/application/organizations",
                                               impl.serve_application_organizations_page(route));
               });
    router.add(http::method::post,
               "/application/organizations",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(route,
                                               "/application/organizations",
                                               impl.serve_application_organization_create(route));
               });
    router.add(http::method::post,
               "/application/organizations/delete",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(route,
                                               "/application/organizations/delete",
                                               impl.serve_application_organization_delete(route));
               });
    router.add(http::method::get,
               "/application/events",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/application/events", impl.serve_application_events_page(route));
               });
    router.add(http::method::post,
               "/application/webhooks",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(route,
                                               "/application/webhooks",
                                               impl.serve_application_webhook_create(route));
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
    router.add(http::method::get,
               "/repository/access",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(route,
                                               "/repository/access",
                                               impl.serve_entity_access_page(route, "repository"));
               });
    router.add(http::method::get,
               "/organization/access",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route,
                       "/organization/access",
                       impl.serve_entity_access_page(route, "organization"));
               });
    router.add(
        http::method::get, "/team/access", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(
                route, "/team/access", impl.serve_entity_access_page(route, "team"));
        });
    router.add(
        http::method::get, "/sign-up", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/sign-up", impl.serve_sign_up(route));
        });
    router.add(
        http::method::get,
        "/apps/:id/sign-up",
        [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(
                route, "/apps/:id/sign-up", impl.serve_application_identity_page(route, "sign-up"));
        });
    router.add(
        http::method::get,
        "/apps/:id/sign-in",
        [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(
                route, "/apps/:id/sign-in", impl.serve_application_identity_page(route, "sign-in"));
        });
    router.add(
        http::method::get, "/apps/:id/mfa", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(
                route, "/apps/:id/mfa", impl.serve_application_identity_page(route, "mfa"));
        });
    router.add(http::method::get,
               "/apps/:id/passkey-sign-in",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route,
                       "/apps/:id/passkey-sign-in",
                       impl.serve_application_identity_page(route, "passkey"));
               });
    router.add(http::method::post,
               "/apps/:id/identity/sign-up",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route,
                       "/apps/:id/identity/sign-up",
                       impl.serve_application_password(route, "sign_up_password"));
               });
    router.add(http::method::post,
               "/apps/:id/identity/sign-in",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route,
                       "/apps/:id/identity/sign-in",
                       impl.serve_application_password(route, "sign_in_password"));
               });
    router.add(http::method::post,
               "/apps/:id/identity/mfa/verify",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route,
                       "/apps/:id/identity/mfa/verify",
                       impl.serve_application_password(route, "verify_mfa"));
               });
    router.add(http::method::post,
               "/apps/:id/identity/passkey/begin",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route,
                       "/apps/:id/identity/passkey/begin",
                       impl.serve_application_passkey(route, "begin_passkey_sign_in"));
               });
    router.add(http::method::post,
               "/apps/:id/identity/passkey/sign-in",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route,
                       "/apps/:id/identity/passkey/sign-in",
                       impl.serve_application_passkey(route, "sign_in_passkey"));
               });
    router.add(http::method::post,
               "/apps/:id/identity/oauth",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/apps/:id/identity/oauth", impl.serve_application_oauth(route));
               });
    router.add(http::method::get,
               "/v1/apps/:id/.well-known/jwks.json",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(route,
                                               "/v1/apps/:id/.well-known/jwks.json",
                                               impl.serve_application_jwks(route));
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
        http::method::get, "/exports", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/exports", impl.serve_exports_page(route));
        });
    router.add(http::method::get,
               "/exports/download",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/exports/download", impl.serve_export_download(route));
               });
    router.add(
        http::method::get, "/repository", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/repository", impl.serve_repository_page(route));
        });
    router.add(http::method::get,
               "/repository/tree",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/repository/tree", impl.serve_repository_tree_page(route));
               });
    router.add(http::method::get, "/file", [&impl](http::route_params& route) -> http::route_task {
        return impl.observe_request(route, "/file", impl.serve_repository_file_page(route));
    });
    router.add(
        http::method::get, "/changes", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/changes", impl.serve_changes_page(route));
        });
    router.add(
        http::method::get, "/change", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/change", impl.serve_change_page(route));
        });
    router.add(
        http::method::get, "/conflicts", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/conflicts", impl.serve_conflicts_page(route));
        });
    router.add(
        http::method::get, "/artifacts", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/artifacts", impl.serve_artifacts_page(route));
        });
    router.add(
        http::method::get, "/artifact", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/artifact", impl.serve_artifact_page(route));
        });
    router.add(http::method::get, "/drive", [&impl](http::route_params& route) -> http::route_task {
        return impl.observe_request(route, "/drive", impl.serve_drive_page(route));
    });
    router.add(http::method::get,
               "/drive/preview",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/drive/preview", impl.serve_drive_preview_page(route));
               });
    router.add(http::method::get,
               "/drive/history",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/drive/history", impl.serve_drive_history_page(route));
               });
    router.add(http::method::post,
               "/drive/history/restore",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/drive/history/restore", impl.serve_drive_restore(route));
               });
    router.add(
        http::method::get, "/drive/share", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/drive/share", impl.serve_drive_share_page(route));
        });
    router.add(http::method::post,
               "/drive/share/direct",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/drive/share/direct", impl.serve_drive_share_direct(route));
               });
    router.add(http::method::post,
               "/drive/share/link",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/drive/share/link", impl.serve_drive_share_link_create(route));
               });
    router.add(http::method::post,
               "/drive/share/revoke",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/drive/share/revoke", impl.serve_drive_share_link_revoke(route));
               });
    router.add(
        http::method::get, "/share/:token", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/share/:token", impl.serve_drive_share_link(route));
        });
    router.add(http::method::post,
               "/api/repos/create",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/api/repos/create", impl.serve_hub_create(route));
               });
    router.add(http::method::post,
               "/api/validate-yaml",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/api/validate-yaml", impl.serve_hub_validate_yaml(route));
               });
    router.add(http::method::get,
               "/models/:id/resolve/:revision/*file",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(route,
                                               "/models/:id/resolve/:revision/*file",
                                               impl.serve_hub_file(route, "model"));
               });
    router.add(http::method::head,
               "/models/:id/resolve/:revision/*file",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(route,
                                               "/models/:id/resolve/:revision/*file",
                                               impl.serve_hub_file(route, "model"));
               });
    const auto add_hub_info = [&router, &impl](std::string_view pattern, std::string_view kind) {
        router.add(http::method::get,
                   pattern,
                   [&impl, pattern, kind](http::route_params& route) -> http::route_task {
                       return impl.observe_request(
                           route, std::string(pattern), impl.serve_hub_info(route, kind));
                   });
    };
    const auto add_hub_tree = [&router, &impl](std::string_view pattern, std::string_view kind) {
        router.add(http::method::get,
                   pattern,
                   [&impl, pattern, kind](http::route_params& route) -> http::route_task {
                       return impl.observe_request(
                           route, std::string(pattern), impl.serve_hub_tree(route, kind));
                   });
    };
    const auto add_hub_file =
        [&router, &impl](http::method method, std::string_view pattern, std::string_view kind) {
            router.add(method,
                       pattern,
                       [&impl, pattern, kind](http::route_params& route) -> http::route_task {
                           return impl.observe_request(
                               route, std::string(pattern), impl.serve_hub_file(route, kind));
                       });
        };
    add_hub_info("/api/models/:namespace/:id/revision/:revision", "model");
    add_hub_info("/api/models/:namespace/:id", "model");
    add_hub_info("/api/models/:id/revision/:revision", "model");
    add_hub_info("/api/models/:id", "model");
    add_hub_info("/api/datasets/:namespace/:id/revision/:revision", "dataset");
    add_hub_info("/api/datasets/:namespace/:id", "dataset");
    add_hub_info("/api/datasets/:id/revision/:revision", "dataset");
    add_hub_info("/api/datasets/:id", "dataset");
    add_hub_tree("/api/models/:namespace/:id/tree/:revision", "model");
    add_hub_tree("/api/models/:id/tree/:revision", "model");
    add_hub_tree("/api/datasets/:namespace/:id/tree/:revision", "dataset");
    add_hub_tree("/api/datasets/:id/tree/:revision", "dataset");
    add_hub_file(http::method::get, "/models/:namespace/:id/resolve/:revision/*file", "model");
    add_hub_file(http::method::head, "/models/:namespace/:id/resolve/:revision/*file", "model");
    add_hub_file(http::method::get, "/datasets/:namespace/:id/resolve/:revision/*file", "dataset");
    add_hub_file(http::method::head, "/datasets/:namespace/:id/resolve/:revision/*file", "dataset");
    add_hub_file(http::method::get, "/datasets/:id/resolve/:revision/*file", "dataset");
    add_hub_file(http::method::head, "/datasets/:id/resolve/:revision/*file", "dataset");
    add_hub_file(http::method::get, "/:namespace/:id/resolve/:revision/*file", "model");
    add_hub_file(http::method::head, "/:namespace/:id/resolve/:revision/*file", "model");
    add_hub_file(http::method::get, "/:id/resolve/:revision/*file", "model");
    add_hub_file(http::method::head, "/:id/resolve/:revision/*file", "model");
    const auto add_hub_post = [&router, &impl](std::string_view pattern, bool commit) {
        router.add(http::method::post,
                   pattern,
                   [&impl, pattern, commit](http::route_params& route) -> http::route_task {
                       return impl.observe_request(route,
                                                   std::string(pattern),
                                                   commit ? impl.serve_hub_commit(route)
                                                          : impl.serve_hub_preupload(route));
                   });
    };
    add_hub_post("/api/models/:namespace/:id/preupload/:revision", false);
    add_hub_post("/api/models/:id/preupload/:revision", false);
    add_hub_post("/api/datasets/:namespace/:id/preupload/:revision", false);
    add_hub_post("/api/datasets/:id/preupload/:revision", false);
    add_hub_post("/api/models/:namespace/:id/commit/:revision", true);
    add_hub_post("/api/models/:id/commit/:revision", true);
    add_hub_post("/api/datasets/:namespace/:id/commit/:revision", true);
    add_hub_post("/api/datasets/:id/commit/:revision", true);
    const auto add_lfs_batch = [&router, &impl](std::string_view pattern) {
        router.add(http::method::post,
                   pattern,
                   [&impl, pattern](http::route_params& route) -> http::route_task {
                       return impl.observe_request(
                           route, std::string(pattern), impl.serve_hub_lfs_batch(route));
                   });
    };
    add_lfs_batch("/datasets/:namespace/:repo_git/info/lfs/objects/batch");
    add_lfs_batch("/datasets/:repo_git/info/lfs/objects/batch");
    add_lfs_batch("/:namespace/:repo_git/info/lfs/objects/batch");
    add_lfs_batch("/:repo_git/info/lfs/objects/batch");
    router.add(http::method::put,
               "/api/lfs/:repository/:oid",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/api/lfs/:repository/:oid", impl.serve_hub_lfs_upload(route));
               });
    router.add(http::method::get,
               "/models/:namespace/:id",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/models/:namespace/:id", impl.serve_model_page(route));
               });
    router.add(
        http::method::get, "/models/:id", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/models/:id", impl.serve_model_page(route));
        });
    router.add(
        http::method::get, "/search", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/search", impl.serve_search_page(route));
        });
    router.add(
        http::method::get, "/metering", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/metering", impl.serve_metering_page(route));
        });
    router.add(http::method::get, "/audit", [&impl](http::route_params& route) -> http::route_task {
        return impl.observe_request(route, "/audit", impl.serve_audit_page(route));
    });
    router.add(
        http::method::get, "/transparency", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(
                route, "/transparency", impl.serve_transparency_page(route));
        });
    router.add(http::method::get,
               "/secret-mounts",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/secret-mounts", impl.serve_secret_mounts_page(route));
               });
    router.add(
        http::method::get, "/secrets", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/secrets", impl.serve_secrets_page(route));
        });
    router.add(http::method::get,
               "/encryption-keys",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/encryption-keys", impl.serve_encryption_keys_page(route));
               });
    router.add(http::method::get,
               "/v1/oauth/:provider/callback",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/v1/oauth/:provider/callback", impl.serve_oauth_callback(route));
               });
    router.add(http::method::get,
               "/scim/v2/Users",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/scim/v2/Users", impl.serve_scim_collection(route, "Users"));
               });
    router.add(http::method::post,
               "/scim/v2/Users",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/scim/v2/Users", impl.serve_scim_collection(route, "Users"));
               });
    router.add(http::method::get,
               "/scim/v2/Users/:id",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/scim/v2/Users/:id", impl.serve_scim_resource(route, "Users"));
               });
    router.add(http::method::patch,
               "/scim/v2/Users/:id",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/scim/v2/Users/:id", impl.serve_scim_resource(route, "Users"));
               });
    router.add(http::method::delete_,
               "/scim/v2/Users/:id",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/scim/v2/Users/:id", impl.serve_scim_resource(route, "Users"));
               });
    router.add(http::method::get,
               "/scim/v2/Groups",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/scim/v2/Groups", impl.serve_scim_collection(route, "Groups"));
               });
    router.add(http::method::post,
               "/scim/v2/Groups",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/scim/v2/Groups", impl.serve_scim_collection(route, "Groups"));
               });
    router.add(http::method::get,
               "/scim/v2/Groups/:id",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/scim/v2/Groups/:id", impl.serve_scim_resource(route, "Groups"));
               });
    router.add(http::method::patch,
               "/scim/v2/Groups/:id",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/scim/v2/Groups/:id", impl.serve_scim_resource(route, "Groups"));
               });
    router.add(http::method::delete_,
               "/scim/v2/Groups/:id",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/scim/v2/Groups/:id", impl.serve_scim_resource(route, "Groups"));
               });
    router.add(http::method::get,
               "/v1/openapi.json",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/v1/openapi.json", impl.serve_openapi(route));
               });
    router.add(http::method::get,
               "/acme/:customer_id/directory",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/acme/:customer_id/directory", impl.serve_acme_directory(route));
               });
    router.add(http::method::head,
               "/acme/:customer_id/new-nonce",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/acme/:customer_id/new-nonce", impl.serve_acme_nonce(route));
               });
    router.add(http::method::get,
               "/.well-known/acme-challenge/:token",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/.well-known/acme-challenge/:token", impl.serve_acme_http01(route));
               });
    const auto add_acme_post = [&router, &impl](std::string path, std::string capability_name) {
        router.add(http::method::post,
                   path,
                   [&impl, path, capability_name](http::route_params& route) -> http::route_task {
                       return impl.observe_request(
                           route, path, impl.serve_acme_post(route, capability_name));
                   });
    };
    add_acme_post("/acme/:customer_id/new-account", "acme_new_account");
    add_acme_post("/acme/:customer_id/new-order", "acme_new_order");
    add_acme_post("/acme/:customer_id/order/:order_id", "acme_order");
    add_acme_post("/acme/:customer_id/authorization/:authorization_id", "acme_authorization");
    add_acme_post("/acme/:customer_id/challenge/:authorization_id", "acme_http01_challenge");
    add_acme_post("/acme/:customer_id/finalize/:order_id", "acme_finalize_order");
    add_acme_post("/acme/:customer_id/certificate/:order_id", "acme_certificate");
    const auto add_sync_post = [&router, &impl](std::string path, std::string capability_name) {
        router.add(http::method::post,
                   path,
                   [&impl, path, capability_name](http::route_params& route) -> http::route_task {
                       return impl.observe_request(
                           route, path, impl.serve_sync_post(route, capability_name));
                   });
    };
    add_sync_post("/v1/repositories/:repository_id/sync/find-missing", "find_missing");
    add_sync_post("/v1/repositories/:repository_id/sync/push", "push");
    add_sync_post("/v1/repositories/:repository_id/sync/pull", "pull");
    add_sync_post("/v1/repositories/:repository_id/sync/chunks", "upload_chunks");
    router.add(http::method::get,
               "/v1/repositories/:repository_id/sync/chunks/:digest",
               [&impl](http::route_params& route) -> http::route_task {
                   const std::string path = "/v1/repositories/:repository_id/sync/chunks/:digest";
                   return impl.observe_request(route, path, impl.serve_sync_range(route));
               });
    for (const auto& capability : impl.registry.list()) {
        if (capability->transport != CapabilityTransport::Json) {
            continue;
        }
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

    auto parser_options = http::parser_config{true};
    parser_options.body_limit = maximum_sync_body_size;
    impl.server = std::make_unique<beast2::http_server>(
        impl.io_context,
        static_cast<std::size_t>(impl.options.worker_coroutines),
        std::move(router),
        http::make_parser_config(parser_options),
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

void ApiServer::drain() {
    impl_->renew_due_timestamps();
}

void ApiServer::wait_for_work(std::chrono::milliseconds max_idle) {
    Impl& impl = *impl_;
    std::unique_lock lock(impl.work_mutex);
    impl.work_cv.wait_for(lock, max_idle, [&impl] { return !impl.is_running.load(); });
}

} // namespace lake::api
