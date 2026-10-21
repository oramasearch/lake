// lake — the HTTP adapter. Beast2 (HTTP/1) on Capy + Corosio, the same stack
// Zetta serves with (zetta/src/core/api/http_server.cpp). This is the ONE
// translation unit that knows about beast2 / boost::http / Boost.JSON. It
// serves the hello page, the registry's JSON and binary HTTP renderings, the
// OpenAPI document and MCP (POST /mcp). Every capability call goes through
// Registry::call so authorization, validation and audit remain centralized.

#include "forge_records_api.hpp"
#include "http_connections.hpp"
#include "lake/api/agent_stream_triggers.hpp"
#include "lake/api/audit_recorder.hpp"
#include "lake/api/billing.hpp"
#include "lake/api/clerk_webhooks.hpp"
#include "lake/api/mcp_client.hpp"
#include "lake/api/mcp_connections.hpp"
#include "lake/api/metering_recorder.hpp"
#include "lake/api/orders.hpp"
#include "lake/api/request_recorder.hpp"
#include "lake/api/resend.hpp"
#include "lake/api/server.hpp"
#include "lake/api/stripe.hpp"
#include "lake/api/usage_recorder.hpp"
#include "lake/control/store_buckets.hpp"
#include "lake/thread.hpp"

#include "lake/control/agents.hpp"
#include "lake/control/agent_library.hpp"
#include "lake/control/agent_bus.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
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
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/capy/buffers.hpp>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <boost/capy/buffers/buffer_copy.hpp>
#include <boost/capy/buffers/vector_dynamic_buffer.hpp>
#include <boost/capy/delay.hpp>
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
#include <boost/url.hpp>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include "agent_task_records_api.hpp"
#include "lake/api/agent_docs.hpp"
#include "lake/api/apps.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/clerk.hpp"
#include "lake/api/datasets.hpp"
#include "lake/api/drive.hpp"
#include "lake/api/images.hpp"
#include "lake/api/mcp.hpp"
#include "lake/api/model_gateway.hpp"
#include "lake/api/oauth.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/rest.hpp"
#include "lake/api/saml.hpp"
#include "lake/api/store_owner.hpp"
#include "lake/api/sync_store.hpp"
#include "lake/api/transcription.hpp"
#include "lake/api/tunnel_clients.hpp"
#include "lake/api/web_app.hpp"
#include "lake/api/web_ui.hpp"
#include "lake/api/workspace_deletion.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/ask_messages.hpp"
#include "lake/control/callers.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/oci_records.hpp"
#include "lake/control/pki.hpp"
#include "lake/control/platform.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/saml.hpp"
#include "lake/control/store.hpp"
#include "lake/control/workspace_store.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "lake/search/index.hpp"
#include "lake/telemetry/metrics.hpp"
#include "lake/transparency/log.hpp"
#include "lake/transparency/timestamps.hpp"
#include "lake/transparency/tuf.hpp"
#include "lake/version.hpp"
#include "request_log.hpp"
#include "route_guard.hpp"
#include "secret_records_api.hpp"
#include "workspace_records_api.hpp"

#include <liquid/git.hpp>
#include <liquid/hash.hpp>
#include <liquid/identity.hpp>
#include <liquid/repository.hpp>
#include <liquid/runtime.hpp>
#include <liquid/store.hpp>
#include <liquid/sync.hpp>

namespace lake::api {
namespace {

// Closes a workspace store's search index to its own process's writes (lake#606). When lake
// drains, it removes the index's manifest so the next owner rebuilds it; a rebuild whose writes
// were still in flight must not put a manifest back afterwards, or the next process loads an index
// that lacks the writes the drain dropped. Replacing puts (the manifest, the artifact members) and
// removes hold the gate shared across their request, and the drain's invalidation and every store
// close take it exclusively, so each such write lands before the close or is refused after it.
// Write-once puts (segments and runs under new names) only check it: one landing after the close
// is an object no manifest names, never a manifest.
struct IndexWriteGate {
    std::shared_mutex mutex;
    bool closed = false;

    void close() {
        std::unique_lock lock(mutex);
        closed = true;
    }
};

search::Error closed_index_error() {
    return {"store_unavailable",
            "this workspace's store was closed here, so Lake writes no more of its search index; "
            "the next owner rebuilds it"};
}

class GatedIndexObjects final : public search::IndexObjects {
public:
    GatedIndexObjects(std::shared_ptr<search::IndexObjects> inner,
                      std::shared_ptr<IndexWriteGate> gate)
        : inner_(std::move(inner)), gate_(std::move(gate)) {}

    search::Result<std::optional<std::string>> get(const std::string& name) override {
        return inner_->get(name);
    }
    search::Result<void> put(const std::string& name, std::string bytes, bool once) override {
        if (once) {
            {
                std::shared_lock lock(gate_->mutex);
                if (gate_->closed) {
                    return std::unexpected(closed_index_error());
                }
            }
            return inner_->put(name, std::move(bytes), once);
        }
        std::shared_lock lock(gate_->mutex);
        if (gate_->closed) {
            return std::unexpected(closed_index_error());
        }
        return inner_->put(name, std::move(bytes), once);
    }
    search::Result<void> remove(const std::string& name) override {
        std::shared_lock lock(gate_->mutex);
        if (gate_->closed) {
            return std::unexpected(closed_index_error());
        }
        return inner_->remove(name);
    }
    search::Result<std::vector<std::string>> list() override {
        return inner_->list();
    }
    search::Result<void> sync() override {
        return inner_->sync();
    }

private:
    std::shared_ptr<search::IndexObjects> inner_;
    std::shared_ptr<IndexWriteGate> gate_;
};

// The workspace a /w/<workspace>/... path names: a UUID, or empty for any other path.
std::string workspace_path_id(std::string_view path) {
    if (!path.starts_with("/w/")) {
        return {};
    }
    const auto end = path.find('/', 3);
    if (end == std::string_view::npos) {
        return {};
    }
    const std::string id(path.substr(3, end - 3));
    if (id.size() != 36 ||
        std::ranges::any_of(id, [](unsigned char c) { return !std::isxdigit(c) && c != '-'; })) {
        return {};
    }
    return id;
}

constexpr auto sync_content_type = "application/vnd.liquid.sync-v1";
constexpr auto sync_error_content_type = "application/vnd.liquid.error-v1";
constexpr auto maximum_sync_body_size = std::size_t{64} * 1024 * 1024;

bool peer_url_is_safe(std::string_view value) {
    bool secure = false;
    if (value.starts_with("https://")) {
        secure = true;
        value.remove_prefix(8);
    } else if (value.starts_with("http://")) {
        value.remove_prefix(7);
    } else {
        return false;
    }
    const std::size_t end = value.find_first_of("/?#");
    const std::string_view authority = end == std::string_view::npos ? value : value.substr(0, end);
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        return false;
    }
    std::string_view host = authority;
    if (authority.starts_with('[')) {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos ||
            (close + 1 < authority.size() && authority[close + 1] != ':')) {
            return false;
        }
        host = authority.substr(1, close - 1);
    } else if (const std::size_t colon = authority.rfind(':'); colon != std::string_view::npos) {
        host = authority.substr(0, colon);
    }
    const bool loopback = host == "localhost" || host == "127.0.0.1" || host == "::1";
    return !host.empty() && (secure || loopback);
}

// lake#915: a replication pull reaches only public addresses unless the operator allows private
// ones, and the whole pull is bounded in time. liquid's sync client resolves the name again when it
// connects; the peer origin is one the operator configured (LAKE_REPLICATION_PEERS), so this check
// is not racing a name a caller controls.
liquid::runtime::task<void> replication_peer_is_public(std::string host, std::string port) {
    const auto executor = co_await boost::capy::this_coro::executor;
    boost::corosio::resolver resolver{executor.context()};
    auto [resolve_error, results] = co_await resolver.resolve(host, port);
    if (resolve_error) {
        co_return std::unexpected(
            liquid::error{liquid::errc::unavailable,
                          "could not resolve replication peer: " + resolve_error.message(),
                          host});
    }
    bool any = false;
    for (const auto& entry : results) {
        const auto endpoint = entry.get_endpoint();
        std::vector<std::uint8_t> bytes;
        if (endpoint.is_v4()) {
            const auto raw = endpoint.v4_address().to_bytes();
            bytes.assign(raw.begin(), raw.end());
        } else {
            const auto raw = endpoint.v6_address().to_bytes();
            bytes.assign(raw.begin(), raw.end());
        }
        if (!mcp_public_address(bytes)) {
            co_return std::unexpected(
                liquid::error{liquid::errc::invalid_argument,
                              "the replication peer's address is not a public address",
                              host});
        }
        any = true;
    }
    if (!any) {
        co_return std::unexpected(liquid::error{
            liquid::errc::unavailable, "the replication peer resolved no addresses", host});
    }
    co_return liquid::result<void>{};
}

liquid::runtime::task<liquid::sync_report> checked_replication_pull(liquid::sync_endpoint& endpoint,
                                                                    liquid::repository& destination,
                                                                    std::string host,
                                                                    std::string port,
                                                                    bool public_only) {
    if (public_only) {
        auto checked = co_await replication_peer_is_public(std::move(host), std::move(port));
        if (!checked) {
            co_return std::unexpected(checked.error());
        }
    }
    co_return co_await endpoint.pull(destination);
}

liquid::runtime::task<liquid::sync_report>
bounded_replication_pull(liquid::sync_endpoint& endpoint,
                         liquid::repository& destination,
                         std::string host,
                         std::string port,
                         bool public_only,
                         std::chrono::milliseconds limit) {
    auto [deadline_error, pulled] = co_await liquid::runtime::timeout(
        liquid::runtime::as_io(checked_replication_pull(
            endpoint, destination, std::move(host), std::move(port), public_only)),
        limit);
    if (deadline_error) {
        co_return std::unexpected(liquid::error{
            liquid::errc::unavailable,
            "replication pull did not finish within " +
                std::to_string(std::chrono::duration_cast<std::chrono::seconds>(limit).count()) +
                " s; a retry resumes from the saved cursor",
            "replicate_repository"});
    }
    co_return std::move(pulled);
}

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

// A drip, change or directory item names no change: its `reference` is liquid's empty value, no
// algorithm and 32 zero bytes. The archive writes that as "" (lake#776). Before, it was written
// with liquid::to_string as "unknown:" and 64 zeros, which parse_digest refuses, so every archive
// export_repository made was refused by import_repository; both spellings now read back as the
// empty value.
constexpr std::string_view empty_reference_before_776 =
    "unknown:0000000000000000000000000000000000000000000000000000000000000000";

bool is_empty_reference(const liquid::change_id& reference) {
    return reference.value.alg == liquid::algorithm{} &&
           std::ranges::all_of(reference.value.bytes, [](std::byte b) { return b == std::byte{}; });
}

std::string sync_item_reference_text(const liquid::change_id& reference) {
    return is_empty_reference(reference) ? std::string() : liquid::to_string(reference.value);
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
            {"reference", sync_item_reference_text(item.reference)},
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
    if (!reference.empty() && reference != empty_reference_before_776) {
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

struct RequestTiming {
    std::chrono::steady_clock::time_point started;
    std::shared_ptr<WorkspaceCallTiming> workspace_calls =
        std::make_shared<WorkspaceCallTiming>();
    // What the request's capability calls resolved (lake#384): its log line and request series.
    std::shared_ptr<RequestAttribution> attribution = std::make_shared<RequestAttribution>();
    std::optional<ApiContext> context;
};

std::mutex request_timings_mutex;
std::map<const boost::http::route_params*, std::shared_ptr<RequestTiming>> request_timings;

std::shared_ptr<RequestTiming> request_timing(const boost::http::route_params& route) {
    std::lock_guard lock(request_timings_mutex);
    const auto found = request_timings.find(&route);
    return found == request_timings.end() ? nullptr : found->second;
}

void start_request_timing(const boost::http::route_params& route,
                          std::chrono::steady_clock::time_point started) {
    std::lock_guard lock(request_timings_mutex);
    request_timings.insert_or_assign(&route, std::make_shared<RequestTiming>(started));
}

class RequestTimingScope {
public:
    RequestTimingScope(const boost::http::route_params& route,
                       std::chrono::steady_clock::time_point started)
        : route_(&route) {
        std::lock_guard lock(request_timings_mutex);
        request_timings.emplace(route_, std::make_shared<RequestTiming>(started));
    }

    ~RequestTimingScope() {
        std::lock_guard lock(request_timings_mutex);
        request_timings.erase(route_);
    }

private:
    const boost::http::route_params* route_;
};

class CatalogRouter {
public:
    explicit CatalogRouter(std::vector<std::string>& get_routes) : get_routes_(&get_routes) {}

    template <class Handler>
    void add(boost::http::method method, std::string_view pattern, Handler&& handler) {
        if (method == boost::http::method::get) {
            get_routes_->emplace_back(pattern);
        }
        router_.add(method, pattern, std::forward<Handler>(handler));
    }

    template <class Handler> void use(Handler&& handler) {
        router_.use(std::forward<Handler>(handler));
    }

    boost::http::router<boost::http::route_params> release() {
        return std::move(router_);
    }

private:
    std::vector<std::string>* get_routes_;
    boost::http::router<boost::http::route_params> router_;
};

// The Server-Timing of a response: the request's total, its workspace calls (one entry each, named
// by Zetta capability, docs/ui/performance.md), and its workspace store requests (lake#527): one
// `store` entry with their total and count, then one per kind.
std::string server_timing_of(const boost::http::route_params& route) {
    const auto timing = request_timing(route);
    const auto now = std::chrono::steady_clock::now();
    const double total_ms =
        timing ? std::chrono::duration<double, std::milli>(now - timing->started).count() : 0.0;
    const double workspace_ms =
        timing ? static_cast<double>(
                     timing->workspace_calls->nanoseconds.load(std::memory_order_relaxed)) /
                     1'000'000.0
               : 0.0;
    const std::size_t workspace_count =
        timing ? timing->workspace_calls->count.load(std::memory_order_relaxed) : 0;
    const std::size_t workspace_rounds =
        timing ? timing->workspace_calls->completed_rounds.load(std::memory_order_relaxed) : 0;
    std::string server_timing =
        std::format("total;dur={:.3f}, workspace;dur={:.3f};desc=\"{} calls\", "
                    "workspace_rounds;desc=\"{} rounds\"",
                    total_ms,
                    workspace_ms,
                    workspace_count,
                    workspace_rounds);
    if (timing) {
        // One entry per workspace call, named by its Zetta capability, so a slow page names the
        // engine read it waited on (docs/ui/performance.md).
        for (const auto& call : timing->workspace_calls->recorded()) {
            std::string name = "zetta_";
            for (const char c : call.capability) {
                name += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
            }
            server_timing += std::format(", {};dur={:.3f}",
                                         name,
                                         static_cast<double>(call.nanoseconds) / 1'000'000.0);
        }
        const control::ObjectRequestTally& store = timing->workspace_calls->store;
        if (const std::uint64_t requests = store.total_requests(); requests != 0) {
            server_timing += std::format(", store;dur={:.3f};desc=\"{} requests\"",
                                         static_cast<double>(store.total_nanoseconds()) / 1'000'000.0,
                                         requests);
            for (std::size_t index = 0; index < control::object_request_kinds; ++index) {
                const std::uint64_t count = store.count[index].load(std::memory_order_relaxed);
                if (count == 0) {
                    continue;
                }
                server_timing += std::format(
                    ", store_{};dur={:.3f};desc=\"{}\"",
                    control::object_request_kind_name(static_cast<control::ObjectRequestKind>(index)),
                    static_cast<double>(store.nanoseconds[index].load(std::memory_order_relaxed)) /
                        1'000'000.0,
                    count);
            }
        }
    }
    return server_timing;
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
    if (content_type.starts_with("text/html")) {
        // Only lake frames lake's pages (the Ask Orama panel, lake#258): no clickjacking.
        route.res.set("Content-Security-Policy", "frame-ancestors 'self'");
    }
    route.res.set("Server-Timing", server_timing_of(route));
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
    if (code == "unauthorized" || code == "unauthenticated") {
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
    if (sync_status(error) == boost::http::status::service_unavailable) {
        route.res.set(boost::http::field::retry_after, "1");
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

struct RegistryCredential {
    std::string username;
    std::string token;
};

RegistryCredential registry_credential_of(const boost::http::route_params& route) {
    const std::string bearer = bearer_of(route);
    if (!bearer.empty()) {
        return {.token = bearer};
    }
    if (route.req.count(boost::http::field::authorization) == 0) {
        return {};
    }
    const std::string_view header = route.req.at(boost::http::field::authorization);
    if (!header.starts_with("Basic ")) {
        return {};
    }
    const auto decoded = base64_decode(header.substr(6));
    if (!decoded) {
        return {};
    }
    const std::string credential(reinterpret_cast<const char*>(decoded->data()), decoded->size());
    const std::size_t separator = credential.find(':');
    if (separator == std::string::npos) {
        return {};
    }
    const std::string username = credential.substr(0, separator);
    if (username != "lake" && username != "app") {
        return {};
    }
    return {username, credential.substr(separator + 1)};
}

std::string registry_token_of(const boost::http::route_params& route) {
    return registry_credential_of(route).token;
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
// form-urlencoded bodies the sign-in and hosted-page forms post (json_sink parses
// JSON, which form bodies are not).
struct StringBodySink {
    std::string data;
    std::size_t limit = maximum_sync_body_size;

    template <boost::capy::ConstBufferSequence CB>
    boost::capy::immediate<boost::capy::io_result<std::size_t>> write_some(const CB& buffers) {
        const std::size_t incoming = boost::capy::buffer_size(buffers);
        if (data.size() > limit || incoming > limit - data.size()) {
            return {{std::make_error_code(std::errc::file_too_large), 0}};
        }
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

struct JsonBodySink {
    boost::http::json_sink parser;
    std::size_t size = 0;
    std::size_t limit = maximum_sync_body_size;

    template <boost::capy::ConstBufferSequence CB>
    boost::capy::immediate<boost::capy::io_result<std::size_t>> write_some(const CB& buffers) {
        return write(buffers);
    }

    template <boost::capy::ConstBufferSequence CB>
    boost::capy::immediate<boost::capy::io_result<std::size_t>> write(const CB& buffers) {
        const std::size_t incoming = boost::capy::buffer_size(buffers);
        if (size > limit || incoming > limit - size) {
            return {{std::make_error_code(std::errc::file_too_large), 0}};
        }
        auto [error, written] = parser.write(buffers).await_resume();
        size += written;
        return {{error, written}};
    }

    boost::capy::immediate<boost::capy::io_result<>> write_eof() {
        return parser.write_eof();
    }

    boost::json::value release() {
        return parser.release();
    }
};

bool body_is_too_large(const std::error_code& error) {
    return error == std::errc::file_too_large || error.message() == "body too large";
}

boost::capy::io_task<std::string> read_string_body(boost::http::route_params& route,
                                                   std::size_t limit = maximum_sync_body_size) {
    StringBodySink sink{.limit = limit};
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

struct MultipartFile {
    std::string name;
    std::vector<std::byte> contents;
};

struct MultipartForm {
    std::map<std::string, std::string> fields;
    std::vector<MultipartFile> files;
    std::string error;
};

struct StoredArtifact {
    std::vector<std::byte> bytes;
    std::string content_type;
    std::string digest;
};

template <class T> using ArtifactResult = std::expected<T, std::string>;

class ArtifactRegistry {
public:
    ArtifactRegistry(std::string_view store_root, std::string_view instance_name) {
        if (!store_root.empty() && safe_component(instance_name)) {
            root_ = std::filesystem::path(store_root) / std::string(instance_name) / "registry";
        }
    }

    [[nodiscard]] bool available() const {
        return !root_.empty();
    }

    // What the registries hold, for the Ship > Packages page: OCI repositories with their tags,
    // npm packages with their versions, Python projects with their files. Everything listed is
    // already a public read.
    nlohmann::json catalog() const {
        nlohmann::json out{{"oci", nlohmann::json::array()},
                           {"npm", nlohmann::json::array()},
                           {"python", nlohmann::json::array()}};
        if (!available()) {
            return out;
        }
        std::error_code error;
        const auto sorted_entries = [&](const std::filesystem::path& directory) {
            std::vector<std::filesystem::directory_entry> entries;
            for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
                entries.push_back(entry);
            }
            std::ranges::sort(entries, {}, [](const auto& entry) { return entry.path(); });
            return entries;
        };
        std::map<std::string, std::vector<std::string>> oci_repositories;
        const std::filesystem::path manifests_root = root_ / "manifests";
        for (const auto& manifest :
             std::filesystem::recursive_directory_iterator(manifests_root, error)) {
            if (!manifest.is_regular_file() || manifest.path().extension() != ".json") {
                continue;
            }
            const std::string repository =
                manifest.path().parent_path().lexically_relative(manifests_root).generic_string();
            // Tenant repositories live in their workspaces since lake#383; what is left of them
            // here waits for the one-shot copy and is not served.
            if (repository.contains('/')) {
                continue;
            }
            auto [entry, inserted] = oci_repositories.try_emplace(repository);
            entry->second.push_back(manifest.path().stem().string());
        }
        for (auto& [repository, tags] : oci_repositories) {
            std::ranges::sort(tags);
            out["oci"].push_back({{"name", repository}, {"versions", tags}});
        }
        for (const auto& package : sorted_entries(root_ / "npm")) {
            const auto metadata = read_text(package.path() / "metadata.json");
            const nlohmann::json document =
                metadata ? nlohmann::json::parse(*metadata, nullptr, false) : nlohmann::json();
            nlohmann::json versions = nlohmann::json::array();
            if (document.is_object() && document.contains("versions") &&
                document["versions"].is_object()) {
                for (const auto& [version, value] : document["versions"].items()) {
                    versions.push_back(version);
                }
            }
            out["npm"].push_back(
                {{"name",
                  document.is_object() ? document.value("name", package.path().filename().string())
                                       : package.path().filename().string()},
                 {"versions", versions}});
        }
        for (const auto& index : sorted_entries(root_ / "python" / "simple")) {
            if (index.path().extension() != ".json") {
                continue;
            }
            const auto text = read_text(index.path());
            const nlohmann::json files =
                text ? nlohmann::json::parse(*text, nullptr, false) : nlohmann::json();
            nlohmann::json versions = nlohmann::json::array();
            if (files.is_array()) {
                for (const auto& file : files) {
                    const std::string version = file.value("version", std::string());
                    if (!version.empty() &&
                        std::ranges::find(versions, nlohmann::json(version)) == versions.end()) {
                        versions.push_back(version);
                    }
                }
            }
            out["python"].push_back(
                {{"name", index.path().stem().string()}, {"versions", versions}});
        }
        return out;
    }

    ArtifactResult<std::string> begin_upload(std::string_view repository) {
        if (!safe_oci_repository(repository)) {
            return std::unexpected("OCI repository name is invalid");
        }
        const std::string id = control::new_uuid_v4();
        const auto saved = write(upload_path(id), {});
        if (!saved) {
            return std::unexpected(saved.error());
        }
        return id;
    }

    ArtifactResult<std::uint64_t> upload_size(std::string_view id) const {
        if (!available()) {
            return std::unexpected("artifact storage is not configured (LAKE_STORE_ROOT)");
        }
        if (!safe_component(id)) {
            return std::unexpected("OCI upload identifier is invalid");
        }
        std::unique_lock lock(mutex_);
        std::error_code error;
        const std::uintmax_t size = std::filesystem::file_size(upload_path(id), error);
        if (error) {
            return std::unexpected("could not inspect OCI upload: " + error.message());
        }
        return static_cast<std::uint64_t>(size);
    }

    ArtifactResult<std::uint64_t> append_upload(std::string_view id,
                                                std::span<const std::byte> bytes,
                                                std::uint64_t expected_offset) {
        if (!available()) {
            return std::unexpected("artifact storage is not configured (LAKE_STORE_ROOT)");
        }
        if (!safe_component(id)) {
            return std::unexpected("OCI upload identifier is invalid");
        }
        std::unique_lock lock(mutex_);
        const std::filesystem::path path = upload_path(id);
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return std::unexpected("could not create OCI upload directory: " + error.message());
        }
        const std::uintmax_t current_size = std::filesystem::file_size(path, error);
        if (error) {
            return std::unexpected("could not inspect OCI upload: " + error.message());
        }
        if (current_size != expected_offset) {
            return std::unexpected("OCI upload offset changed while appending");
        }
        std::ofstream output(path, std::ios::binary | std::ios::app);
        if (!output) {
            return std::unexpected("could not open OCI upload");
        }
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) {
            return std::unexpected("could not append OCI upload");
        }
        const std::uintmax_t completed_size = std::filesystem::file_size(path, error);
        if (error) {
            return std::unexpected("could not inspect appended OCI upload: " + error.message());
        }
        return static_cast<std::uint64_t>(completed_size);
    }

    ArtifactResult<void> truncate_upload(std::string_view id, std::uint64_t size) {
        if (!safe_component(id)) {
            return std::unexpected("OCI upload identifier is invalid");
        }
        std::unique_lock lock(mutex_);
        std::error_code error;
        std::filesystem::resize_file(upload_path(id), size, error);
        if (error) {
            return std::unexpected("could not restore OCI upload after a failed request: " +
                                   error.message());
        }
        return {};
    }

    ArtifactResult<std::string> finish_upload(std::string_view id, std::string_view digest) {
        if (!safe_component(id) || !valid_digest(digest)) {
            return std::unexpected("OCI upload or digest is invalid");
        }
        std::unique_lock lock(mutex_);
        const std::filesystem::path upload = upload_path(id);
        const auto actual = sha256_file(upload);
        if (!actual) {
            return std::unexpected(actual.error());
        }
        if (*actual != digest) {
            return std::unexpected("uploaded bytes do not match the requested digest");
        }
        const std::filesystem::path blob = blob_path(digest);
        std::error_code error;
        std::filesystem::create_directories(blob.parent_path(), error);
        if (error) {
            return std::unexpected("could not create OCI blob directory: " + error.message());
        }
        std::filesystem::rename(upload, blob, error);
        if (error) {
            return std::unexpected("could not publish OCI blob: " + error.message());
        }
        return *actual;
    }

    ArtifactResult<StoredArtifact> blob(std::string_view digest) const {
        if (!valid_digest(digest)) {
            return std::unexpected("OCI digest is invalid");
        }
        const auto contents = read(blob_path(digest));
        if (!contents) {
            return std::unexpected(contents.error());
        }
        return StoredArtifact{*contents, "application/octet-stream", std::string(digest)};
    }

    bool repository_has_blob(std::string_view repository, std::string_view digest) const {
        if (!safe_oci_repository(repository) || !valid_digest(digest)) {
            return false;
        }
        std::error_code error;
        const std::filesystem::path directory = root_ / "manifests" / std::string(repository);
        for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") {
                continue;
            }
            const auto pointer_text = read_text(entry.path());
            const nlohmann::json pointer =
                pointer_text ? nlohmann::json::parse(*pointer_text, nullptr, false)
                             : nlohmann::json();
            if (!pointer.is_object()) {
                continue;
            }
            const std::string manifest_digest = pointer.value("digest", "");
            if (manifest_digest == digest) {
                return true;
            }
            if (!valid_digest(manifest_digest)) {
                continue;
            }
            const auto manifest_bytes = read(blob_path(manifest_digest));
            if (!manifest_bytes) {
                continue;
            }
            const std::string document(reinterpret_cast<const char*>(manifest_bytes->data()),
                                       manifest_bytes->size());
            const nlohmann::json manifest = nlohmann::json::parse(document, nullptr, false);
            if (!manifest.is_object()) {
                continue;
            }
            const auto references = manifest_references(manifest);
            if (references && std::ranges::find(*references, digest) != references->end()) {
                return true;
            }
        }
        return false;
    }

    // A blob's size; nullopt when the registry does not hold it.
    std::optional<std::uint64_t> blob_size(std::string_view digest) const {
        if (!available() || !valid_digest(digest)) {
            return std::nullopt;
        }
        std::unique_lock lock(mutex_);
        std::error_code error;
        const std::uintmax_t size = std::filesystem::file_size(blob_path(digest), error);
        if (error) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(size);
    }

    // lake#383 phase B: moves a staged upload, under the registry's lock, to a name only the
    // completing request knows (`claimed/<id>-<random>`), so no PATCH to the upload id can append
    // to the bytes that are then checked and stored. An upload already claimed or unknown is an
    // error.
    ArtifactResult<std::filesystem::path> claim_upload(std::string_view id) {
        if (!available()) {
            return std::unexpected("artifact storage is not configured (LAKE_STORE_ROOT)");
        }
        if (!safe_component(id)) {
            return std::unexpected("OCI upload identifier is invalid");
        }
        std::unique_lock lock(mutex_);
        const std::filesystem::path claimed =
            root_ / "claimed" / (std::string(id) + "-" + control::new_uuid_v4());
        std::error_code error;
        std::filesystem::create_directories(claimed.parent_path(), error);
        if (error) {
            return std::unexpected("could not create the OCI claim directory: " + error.message());
        }
        std::filesystem::rename(upload_path(id), claimed, error);
        if (error) {
            return std::unexpected("OCI upload is unknown or already completing");
        }
        // The sweep measures a claimed upload's age from its claim.
        std::filesystem::last_write_time(claimed, std::filesystem::file_time_type::clock::now(),
                                         error);
        return claimed;
    }

    // Puts a claimed upload back under its id, so the client may retry the same completion.
    ArtifactResult<void> release_upload(std::string_view id, const std::filesystem::path& claimed) {
        if (!safe_component(id)) {
            return std::unexpected("OCI upload identifier is invalid");
        }
        std::unique_lock lock(mutex_);
        std::error_code error;
        std::filesystem::rename(claimed, upload_path(id), error);
        if (error) {
            return std::unexpected("could not restore the OCI upload: " + error.message());
        }
        return {};
    }

    // Removes a claimed upload once its bytes are recorded in the workspace's store, or once the
    // store refused them.
    static void remove_claimed(const std::filesystem::path& claimed) {
        std::error_code ignored;
        std::filesystem::remove(claimed, ignored);
    }

    // Removes staged and claimed uploads untouched for `max_age` (abandoned pushes); answers how
    // many. The maintenance thread runs it.
    std::size_t sweep_uploads(std::chrono::seconds max_age) {
        if (!available()) {
            return 0;
        }
        // Listed without the registry's lock; each file is checked again and removed under it,
        // briefly, so uploads and completions are not held up by the sweep.
        const auto cutoff = std::filesystem::file_time_type::clock::now() - max_age;
        std::vector<std::filesystem::path> candidates;
        for (const std::filesystem::path& directory : {root_ / "uploads", root_ / "claimed"}) {
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
                std::error_code entry_error;
                if (entry.is_regular_file(entry_error) &&
                    entry.last_write_time(entry_error) < cutoff && !entry_error) {
                    candidates.push_back(entry.path());
                }
            }
        }
        std::size_t removed = 0;
        for (const std::filesystem::path& path : candidates) {
            std::unique_lock lock(mutex_);
            std::error_code error;
            const auto written = std::filesystem::last_write_time(path, error);
            if (!error && written < cutoff && std::filesystem::remove(path, error)) {
                ++removed;
            }
        }
        return removed;
    }

    // The file that holds a blob in the instance registry; nullopt when it holds none.
    std::optional<std::filesystem::path> blob_file(std::string_view digest) const {
        if (!available() || !valid_digest(digest)) {
            return std::nullopt;
        }
        std::error_code error;
        const std::filesystem::path path = blob_path(digest);
        if (!std::filesystem::is_regular_file(path, error)) {
            return std::nullopt;
        }
        return path;
    }

    // `sha256:<hex>` of a file, read in windows.
    static ArtifactResult<std::string> file_digest(const std::filesystem::path& path) {
        return sha256_file(path);
    }

    // Every digest the instance's own (one-component) OCI repositories use: the manifests their
    // tags name and what those reference. The instance registry keeps these whatever moves.
    std::set<std::string> instance_digests() const {
        std::set<std::string> digests;
        if (!available()) {
            return digests;
        }
        std::error_code error;
        const std::filesystem::path manifests_root = root_ / "manifests";
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(manifests_root, error)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") {
                continue;
            }
            const std::string repository =
                entry.path().parent_path().lexically_relative(manifests_root).generic_string();
            if (repository.contains('/')) {
                continue;
            }
            const auto pointer_text = read_text(entry.path());
            const nlohmann::json pointer = pointer_text
                                               ? nlohmann::json::parse(*pointer_text, nullptr, false)
                                               : nlohmann::json();
            const std::string digest =
                pointer.is_object() ? pointer.value("digest", std::string()) : std::string();
            if (!valid_digest(digest)) {
                continue;
            }
            digests.insert(digest);
            const auto document = read_text(blob_path(digest));
            const nlohmann::json manifest =
                document ? nlohmann::json::parse(*document, nullptr, false) : nlohmann::json();
            if (const auto references = manifest.is_object() ? manifest_references(manifest)
                                                             : ArtifactResult<std::vector<std::string>>(
                                                                   std::unexpected("not a manifest"));
                references) {
                digests.insert(references->begin(), references->end());
            }
        }
        return digests;
    }

    // A blob's bytes as text (a manifest's document).
    ArtifactResult<std::string> blob_text(std::string_view digest) const {
        if (!valid_digest(digest)) {
            return std::unexpected("OCI digest is invalid");
        }
        return read_text(blob_path(digest));
    }

    static std::string digest_of(std::string_view bytes) {
        return sha256_digest(std::as_bytes(std::span(bytes.data(), bytes.size())));
    }

    static bool tag_name(std::string_view value) {
        return safe_component(value);
    }

    static bool digest_name(std::string_view value) {
        return valid_digest(value);
    }

    // The tags of tenant repositories (`<space>/...`) written here before lake#383, for the
    // one-shot copy: ordered by `<repository>:<tag>`, the `limit` after `after`.
    std::vector<LegacyImageTag> legacy_tenant_tags(std::string_view after, int limit) const {
        std::vector<LegacyImageTag> found;
        if (!available()) {
            return found;
        }
        std::error_code error;
        const std::filesystem::path manifests_root = root_ / "manifests";
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(manifests_root, error)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") {
                continue;
            }
            const std::string repository =
                entry.path().parent_path().lexically_relative(manifests_root).generic_string();
            const std::string tag = entry.path().stem().string();
            if (!repository.contains('/') || !(repository + ":" + tag > after)) {
                continue;
            }
            const auto pointer_text = read_text(entry.path());
            const nlohmann::json pointer =
                pointer_text ? nlohmann::json::parse(*pointer_text, nullptr, false)
                             : nlohmann::json();
            if (!pointer.is_object() || !valid_digest(pointer.value("digest", ""))) {
                continue;
            }
            found.push_back({repository, tag, pointer.value("digest", ""),
                             pointer.value("content_type",
                                           "application/vnd.oci.image.manifest.v1+json")});
        }
        std::ranges::sort(found, {}, [](const LegacyImageTag& row) {
            return row.repository + ":" + row.tag;
        });
        if (limit > 0 && found.size() > static_cast<std::size_t>(limit)) {
            found.resize(static_cast<std::size_t>(limit));
        }
        return found;
    }

    ArtifactResult<std::string> put_manifest(std::string_view repository,
                                             std::string_view reference,
                                             std::string content_type,
                                             std::span<const std::byte> bytes) {
        if (!safe_oci_repository(repository) || !safe_component(reference)) {
            return std::unexpected("OCI manifest name is invalid");
        }
        const std::string document(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const nlohmann::json manifest = nlohmann::json::parse(document, nullptr, false);
        if (manifest.is_discarded() || !manifest.is_object() ||
            manifest.value("schemaVersion", 0) != 2) {
            return std::unexpected("OCI manifest is not valid schema version 2 JSON");
        }
        const auto referenced = manifest_references(manifest);
        if (!referenced) {
            return std::unexpected(referenced.error());
        }
        for (const std::string& digest : *referenced) {
            if (!read(blob_path(digest))) {
                return std::unexpected("OCI manifest references an unknown blob: " + digest);
            }
        }
        const std::string digest = sha256_digest(bytes);
        const auto object_saved = write(blob_path(digest), bytes);
        if (!object_saved) {
            return std::unexpected(object_saved.error());
        }
        const nlohmann::json pointer{{"digest", digest}, {"content_type", content_type}};
        const auto pointer_saved = write_text(manifest_path(repository, reference), pointer.dump());
        if (!pointer_saved) {
            return std::unexpected(pointer_saved.error());
        }
        return digest;
    }

    ArtifactResult<StoredArtifact> manifest(std::string_view repository,
                                            std::string_view reference) const {
        if (!safe_oci_repository(repository) || !safe_component(reference)) {
            return std::unexpected("OCI manifest name is invalid");
        }
        if (valid_digest(reference)) {
            if (!repository_has_blob(repository, reference)) {
                return std::unexpected("OCI manifest is unknown in this repository");
            }
            auto object = blob(reference);
            if (object) {
                object->content_type = "application/vnd.oci.image.manifest.v1+json";
            }
            return object;
        }
        const auto pointer_text = read_text(manifest_path(repository, reference));
        if (!pointer_text) {
            return std::unexpected(pointer_text.error());
        }
        const nlohmann::json pointer = nlohmann::json::parse(*pointer_text, nullptr, false);
        if (pointer.is_discarded() || !pointer.is_object()) {
            return std::unexpected("stored OCI manifest pointer is invalid");
        }
        auto object = blob(pointer.value("digest", ""));
        if (object) {
            object->content_type =
                pointer.value("content_type", "application/vnd.oci.image.manifest.v1+json");
        }
        return object;
    }

    ArtifactResult<void>
    put_npm(std::string_view name, nlohmann::json metadata, std::string_view origin) {
        if (!safe_package(name) || !metadata.contains("_attachments") ||
            !metadata.at("_attachments").is_object()) {
            return std::unexpected("npm publication is missing package attachments");
        }
        for (const auto& [filename, attachment] : metadata.at("_attachments").items()) {
            if (!safe_component(filename) || !attachment.is_object() ||
                !attachment.contains("data") || !attachment.at("data").is_string()) {
                return std::unexpected("npm attachment is invalid");
            }
            const auto decoded = base64_decode(attachment.at("data").get<std::string>());
            if (!decoded) {
                return std::unexpected(decoded.error().message);
            }
            const auto saved = write(npm_tarball_path(name, filename), *decoded);
            if (!saved) {
                return std::unexpected(saved.error());
            }
            if (metadata.contains("versions") && metadata.at("versions").is_object()) {
                for (auto& [version, record] : metadata["versions"].items()) {
                    static_cast<void>(version);
                    if (record.is_object() && record.contains("dist") &&
                        record.at("dist").is_object()) {
                        record["dist"]["tarball"] =
                            std::string(origin) + "/" + std::string(name) + "/-/" + filename;
                    }
                }
            }
        }
        metadata.erase("_attachments");
        return write_text(npm_metadata_path(name), metadata.dump());
    }

    ArtifactResult<std::string> npm_metadata(std::string_view name) const {
        if (!safe_package(name)) {
            return std::unexpected("npm package name is invalid");
        }
        return read_text(npm_metadata_path(name));
    }

    ArtifactResult<std::vector<std::byte>> npm_tarball(std::string_view name,
                                                       std::string_view filename) const {
        if (!safe_package(name) || !safe_component(filename)) {
            return std::unexpected("npm tarball name is invalid");
        }
        return read(npm_tarball_path(name, filename));
    }

    ArtifactResult<void> put_python(std::string_view name,
                                    std::string_view version,
                                    std::string_view filename,
                                    std::span<const std::byte> bytes) {
        const std::string normalized = normalize_python_name(name);
        if (normalized.empty() || version.empty() || !safe_component(filename)) {
            return std::unexpected("Python package publication is invalid");
        }
        const auto package_saved = write(python_package_path(filename), bytes);
        if (!package_saved) {
            return std::unexpected(package_saved.error());
        }
        nlohmann::json files = nlohmann::json::array();
        const auto existing = read_text(python_index_path(normalized));
        if (existing) {
            files = nlohmann::json::parse(*existing, nullptr, false);
        }
        if (!files.is_array()) {
            return std::unexpected("stored Python package index is invalid");
        }
        const bool present = std::ranges::any_of(files, [&](const nlohmann::json& item) {
            return item.value("filename", "") == filename;
        });
        if (!present) {
            files.push_back(
                {{"filename", filename}, {"sha256", sha256_digest(bytes)}, {"version", version}});
        }
        return write_text(python_index_path(normalized), files.dump());
    }

    ArtifactResult<std::string> python_simple(std::string_view name,
                                              std::string_view origin) const {
        const std::string normalized = normalize_python_name(name);
        if (normalized.empty()) {
            return std::unexpected("Python package name is invalid");
        }
        const auto index = read_text(python_index_path(normalized));
        if (!index) {
            return std::unexpected(index.error());
        }
        const nlohmann::json files = nlohmann::json::parse(*index, nullptr, false);
        if (!files.is_array()) {
            return std::unexpected("stored Python package index is invalid");
        }
        std::string html = "<!doctype html><html><body>";
        for (const nlohmann::json& file : files) {
            const std::string filename = file.value("filename", "");
            const std::string digest = file.value("sha256", "");
            html += "<a href=\"" + std::string(origin) + "/packages/" + filename +
                    "#sha256=" + digest.substr(7) + "\">" + filename + "</a>\n";
        }
        html += "</body></html>";
        return html;
    }

    ArtifactResult<std::vector<std::byte>> python_package(std::string_view filename) const {
        if (!safe_component(filename)) {
            return std::unexpected("Python package filename is invalid");
        }
        return read(python_package_path(filename));
    }

    ArtifactResult<void> put_debug(std::string_view build_id,
                                   std::string_view kind,
                                   std::string_view source,
                                   std::span<const std::byte> bytes) {
        const auto path = debug_path(build_id, kind, source);
        if (!path) {
            return std::unexpected(path.error());
        }
        return write(*path, bytes);
    }

    ArtifactResult<std::vector<std::byte>>
    debug(std::string_view build_id, std::string_view kind, std::string_view source) const {
        const auto path = debug_path(build_id, kind, source);
        if (!path) {
            return std::unexpected(path.error());
        }
        return read(*path);
    }

    // The tags one OCI repository holds, sorted (a workspace deletion's confirmation, lake#514).
    std::vector<std::string> tags(std::string_view repository) const {
        std::vector<std::string> found;
        if (!available() || !safe_oci_repository(repository)) {
            return found;
        }
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(
                 root_ / "manifests" / std::string(repository), error)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                found.push_back(entry.path().stem().string());
            }
        }
        std::ranges::sort(found);
        return found;
    }

    // Removes one OCI repository's tags and manifest pointers, so nothing can pull it (a workspace
    // deletion, lake#514). Nested repositories below it (`<space>/cache/...` under `<space>/`) are
    // other repositories and stay. Blobs stay too: other manifests may reference them, and the
    // registry has no collection yet. Removing a repository that holds nothing is a success.
    ArtifactResult<void> remove_repository(std::string_view repository) {
        if (!available()) {
            return std::unexpected("artifact storage is not configured (LAKE_STORE_ROOT)");
        }
        if (!safe_oci_repository(repository)) {
            return std::unexpected("OCI repository name is invalid");
        }
        std::unique_lock lock(mutex_);
        const std::filesystem::path directory = root_ / "manifests" / std::string(repository);
        std::error_code error;
        std::vector<std::filesystem::path> pointers;
        for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                pointers.push_back(entry.path());
            }
        }
        if (error && error != std::errc::no_such_file_or_directory) {
            return std::unexpected("could not list OCI repository: " + error.message());
        }
        for (const std::filesystem::path& pointer : pointers) {
            std::error_code removed;
            std::filesystem::remove(pointer, removed);
            if (removed) {
                return std::unexpected("could not remove OCI tag: " + removed.message());
            }
        }
        std::error_code ignored;
        if (std::filesystem::is_empty(directory, ignored)) {
            std::filesystem::remove(directory, ignored);
        }
        return {};
    }

private:
    static bool safe_component(std::string_view value) {
        if (value.empty() || value == "." || value == "..") {
            return false;
        }
        return std::ranges::all_of(value, [](unsigned char character) {
            return std::isalnum(character) || character == '-' || character == '_' ||
                   character == '.' || character == ':';
        });
    }

    static bool safe_oci_repository(std::string_view value) {
        if (value.empty() || value.front() == '/' || value.back() == '/') {
            return false;
        }
        while (!value.empty()) {
            const std::size_t slash = value.find('/');
            const std::string_view component = value.substr(0, slash);
            if (!safe_component(component) || component.find(':') != std::string_view::npos) {
                return false;
            }
            if (slash == std::string_view::npos) {
                return true;
            }
            value.remove_prefix(slash + 1);
        }
        return false;
    }

    static bool safe_package(std::string_view value) {
        return safe_component(value) && value.find(':') == std::string_view::npos;
    }

    static bool valid_digest(std::string_view value) {
        if (!value.starts_with("sha256:") || value.size() != 71) {
            return false;
        }
        return std::ranges::all_of(value.substr(7), [](unsigned char character) {
            return std::isxdigit(character) != 0 && !std::isupper(character);
        });
    }

    static std::string sha256_digest(std::span<const std::byte> bytes) {
        std::array<unsigned char, 32> digest{};
        unsigned int size = 0;
        EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_sha256(), nullptr);
        std::string output = "sha256:";
        for (const unsigned char byte : digest) {
            output += std::format("{:02x}", byte);
        }
        return output;
    }

    static ArtifactResult<std::string> sha256_file(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return std::unexpected("could not open OCI upload for digest verification");
        }
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                        &EVP_MD_CTX_free);
        if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
            return std::unexpected("could not initialize OCI upload digest verification");
        }
        std::array<char, 64 * 1024> buffer{};
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize read = input.gcount();
            if (read > 0 &&
                EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(read)) !=
                    1) {
                return std::unexpected("could not update OCI upload digest verification");
            }
        }
        if (input.bad()) {
            return std::unexpected("could not read OCI upload for digest verification");
        }
        std::array<unsigned char, 32> digest{};
        unsigned int size = 0;
        if (EVP_DigestFinal_ex(context.get(), digest.data(), &size) != 1 || size != digest.size()) {
            return std::unexpected("could not finish OCI upload digest verification");
        }
        std::string output = "sha256:";
        for (const unsigned char byte : digest) {
            output += std::format("{:02x}", byte);
        }
        return output;
    }

    static ArtifactResult<std::vector<std::string>>
    manifest_references(const nlohmann::json& manifest) {
        std::vector<std::string> output;
        if (manifest.contains("config") && manifest.at("config").is_object()) {
            output.push_back(manifest.at("config").value("digest", ""));
        }
        if (!manifest.contains("layers") || !manifest.at("layers").is_array()) {
            return std::unexpected("OCI manifest has no layer array");
        }
        for (const nlohmann::json& layer : manifest.at("layers")) {
            output.push_back(layer.value("digest", ""));
        }
        if (std::ranges::any_of(output,
                                [](const std::string& digest) { return !valid_digest(digest); })) {
            return std::unexpected("OCI manifest contains an invalid digest");
        }
        return output;
    }

    static std::string normalize_python_name(std::string_view name) {
        std::string output;
        for (const unsigned char character : name) {
            if (std::isalnum(character)) {
                output += static_cast<char>(std::tolower(character));
            } else if (character == '-' || character == '_' || character == '.') {
                if (output.empty() || output.back() != '-') {
                    output += '-';
                }
            } else {
                return {};
            }
        }
        return output;
    }

    std::filesystem::path upload_path(std::string_view id) const {
        return root_ / "uploads" / std::string(id);
    }

    std::filesystem::path blob_path(std::string_view digest) const {
        return root_ / "blobs" / std::string(digest.substr(7));
    }

    std::filesystem::path manifest_path(std::string_view repository,
                                        std::string_view reference) const {
        return root_ / "manifests" / std::string(repository) / (std::string(reference) + ".json");
    }

    std::filesystem::path npm_metadata_path(std::string_view name) const {
        return root_ / "npm" / std::string(name) / "metadata.json";
    }

    std::filesystem::path npm_tarball_path(std::string_view name, std::string_view filename) const {
        return root_ / "npm" / std::string(name) / std::string(filename);
    }

    std::filesystem::path python_index_path(std::string_view name) const {
        return root_ / "python" / "simple" / (std::string(name) + ".json");
    }

    std::filesystem::path python_package_path(std::string_view filename) const {
        return root_ / "python" / "packages" / std::string(filename);
    }

    ArtifactResult<std::filesystem::path>
    debug_path(std::string_view build_id, std::string_view kind, std::string_view source) const {
        if (build_id.empty() || !std::ranges::all_of(build_id, [](unsigned char character) {
                return std::isxdigit(character) != 0;
            })) {
            return std::unexpected("debug build identifier is invalid");
        }
        if (kind == "executable" || kind == "debuginfo") {
            return root_ / "debug" / std::string(build_id) / std::string(kind);
        }
        if (kind != "source" || source.empty() || source.find("..") != std::string_view::npos) {
            return std::unexpected("debug artifact kind or source path is invalid");
        }
        return root_ / "debug" / std::string(build_id) / "source" /
               sha256_digest(std::as_bytes(std::span(source.data(), source.size()))).substr(7);
    }

    ArtifactResult<void> write_text(const std::filesystem::path& path, std::string_view text) {
        return write(path, std::as_bytes(std::span(text)));
    }

    ArtifactResult<std::string> read_text(const std::filesystem::path& path) const {
        const auto bytes = read(path);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    }

    ArtifactResult<void> write(const std::filesystem::path& path,
                               std::span<const std::byte> bytes) {
        if (!available()) {
            return std::unexpected("artifact storage is not configured (LAKE_STORE_ROOT)");
        }
        std::unique_lock lock(mutex_);
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return std::unexpected("could not create artifact directory: " + error.message());
        }
        const std::filesystem::path temporary = path.string() + ".tmp-" + control::new_uuid_v4();
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return std::unexpected("could not open artifact temporary file");
        }
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) {
            std::filesystem::remove(temporary, error);
            return std::unexpected("could not write artifact bytes");
        }
        std::filesystem::rename(temporary, path, error);
        if (error) {
            std::filesystem::remove(path, error);
            error.clear();
            std::filesystem::rename(temporary, path, error);
        }
        if (error) {
            std::filesystem::remove(temporary, error);
            return std::unexpected("could not publish artifact: " + error.message());
        }
        return {};
    }

    ArtifactResult<std::vector<std::byte>> read(const std::filesystem::path& path) const {
        if (!available()) {
            return std::unexpected("artifact storage is not configured (LAKE_STORE_ROOT)");
        }
        std::unique_lock lock(mutex_);
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return std::unexpected("artifact does not exist");
        }
        std::vector<char> contents{std::istreambuf_iterator<char>(input), {}};
        std::vector<std::byte> output(contents.size());
        std::ranges::transform(contents, output.begin(), [](char byte) {
            return static_cast<std::byte>(static_cast<unsigned char>(byte));
        });
        return output;
    }

    std::filesystem::path root_;
    mutable std::mutex mutex_;
};

struct OciUploadSink {
    ArtifactRegistry& artifacts;
    std::string upload;
    std::uint64_t offset;
    std::uint64_t limit;
    std::string storage_error;

    template <boost::capy::ConstBufferSequence CB>
    boost::capy::immediate<boost::capy::io_result<std::size_t>> write_some(const CB& buffers) {
        return write(buffers);
    }

    template <boost::capy::ConstBufferSequence CB>
    boost::capy::immediate<boost::capy::io_result<std::size_t>> write(const CB& buffers) {
        const std::size_t incoming = boost::capy::buffer_size(buffers);
        if (offset > limit || incoming > limit - offset) {
            return {{std::make_error_code(std::errc::file_too_large), 0}};
        }
        std::size_t written = 0;
        for (auto it = boost::capy::begin(buffers); it != boost::capy::end(buffers); ++it) {
            const boost::capy::const_buffer buffer(*it);
            const auto appended = artifacts.append_upload(
                upload, {static_cast<const std::byte*>(buffer.data()), buffer.size()}, offset);
            if (!appended) {
                storage_error = appended.error();
                return {{std::make_error_code(std::errc::io_error), written}};
            }
            offset = *appended;
            written += buffer.size();
        }
        return {{{}, written}};
    }

    boost::capy::immediate<boost::capy::io_result<>> write_eof() {
        return {{}};
    }
};

std::string disposition_parameter(std::string_view disposition, std::string_view name) {
    const std::string marker = std::string(name) + "=\"";
    const std::size_t start = disposition.find(marker);
    if (start == std::string_view::npos) {
        return {};
    }
    const std::size_t value_start = start + marker.size();
    const std::size_t end = disposition.find('"', value_start);
    return end == std::string_view::npos
               ? std::string{}
               : std::string(disposition.substr(value_start, end - value_start));
}

std::string uploaded_name(std::string name) {
    const std::size_t slash = name.find_last_of("/\\");
    return slash == std::string::npos ? name : name.substr(slash + 1);
}

MultipartForm parse_multipart(std::string_view body, std::string_view content_type) {
    MultipartForm output;
    const std::string marker = "boundary=";
    const std::size_t boundary_start = content_type.find(marker);
    if (boundary_start == std::string_view::npos) {
        output.error = "multipart boundary is missing";
        return output;
    }
    std::string boundary(content_type.substr(boundary_start + marker.size()));
    if (!boundary.empty() && boundary.front() == '"' && boundary.back() == '"') {
        boundary = boundary.substr(1, boundary.size() - 2);
    }
    const std::string delimiter = "--" + boundary;
    std::size_t position = 0;
    while ((position = body.find(delimiter, position)) != std::string_view::npos) {
        position += delimiter.size();
        if (body.substr(position, 2) == "--") {
            break;
        }
        if (body.substr(position, 2) != "\r\n") {
            output.error = "multipart delimiter is invalid";
            return output;
        }
        position += 2;
        const std::size_t header_end = body.find("\r\n\r\n", position);
        if (header_end == std::string_view::npos) {
            output.error = "multipart headers are incomplete";
            return output;
        }
        const std::string_view headers = body.substr(position, header_end - position);
        const std::size_t disposition_start = headers.find("Content-Disposition:");
        if (disposition_start == std::string_view::npos) {
            output.error = "multipart content disposition is missing";
            return output;
        }
        const std::size_t disposition_end = headers.find("\r\n", disposition_start);
        const std::string_view disposition = headers.substr(
            disposition_start,
            disposition_end == std::string_view::npos ? std::string_view::npos
                                                      : disposition_end - disposition_start);
        const std::string name = disposition_parameter(disposition, "name");
        const std::string filename = uploaded_name(disposition_parameter(disposition, "filename"));
        const std::size_t value_start = header_end + 4;
        const std::size_t next = body.find("\r\n" + delimiter, value_start);
        if (next == std::string_view::npos) {
            output.error = "multipart body is incomplete";
            return output;
        }
        const std::string_view value = body.substr(value_start, next - value_start);
        if (!filename.empty()) {
            std::vector<std::byte> contents(value.size());
            std::ranges::transform(
                value, contents.begin(), [](char byte) { return static_cast<std::byte>(byte); });
            output.files.push_back({filename, std::move(contents)});
        } else if (!name.empty()) {
            output.fields[name] = std::string(value);
        }
        position = next + 2;
    }
    if (output.files.empty()) {
        output.error = "at least one uploaded file is required";
    }
    return output;
}

std::string path_filename(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
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

std::string workspace_prefixed_local_path(std::string_view path) {
    if (!path.starts_with("/w/")) {
        return {};
    }
    const std::size_t end = path.find('/', 3);
    return end == std::string_view::npos ? std::string() : std::string(path.substr(end));
}

bool workspace_api_path(std::string_view path) {
    return path == "/mcp" || path == "/v1/openapi.json" || path.starts_with("/v1/");
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

std::string app_request_host(const boost::http::route_params& route) {
    std::string host;
    // ingress-nginx's auth subrequest sets Host to Lake's Service and sends no
    // X-Forwarded-Host; the app host arrives only in X-Original-URL
    // ($scheme://$http_host$request_uri).
    if (route.req.count("x-original-url") != 0) {
        const auto original =
            boost::urls::parse_uri_reference(std::string_view(route.req.at("x-original-url")));
        if (original && original->has_authority()) {
            host = std::string(original->encoded_host());
        }
    }
    if (host.empty() && route.req.count("x-forwarded-host") != 0) {
        host = std::string(route.req.at("x-forwarded-host"));
    } else if (host.empty() && route.req.count(boost::http::field::host) != 0) {
        host = std::string(route.req.at(boost::http::field::host));
    }
    if (const std::size_t comma = host.find(','); comma != std::string::npos) {
        host.resize(comma);
    }
    const std::size_t first = host.find_first_not_of(" \t\r\n");
    const std::size_t last = host.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    host = host.substr(first, last - first + 1);
    if (!host.empty() && host.front() == '[') {
        const std::size_t close = host.find(']');
        if (close != std::string::npos) {
            host = host.substr(1, close - 1);
        }
    } else if (const std::size_t colon = host.rfind(':'); colon != std::string::npos) {
        host.resize(colon);
    }
    std::ranges::transform(host, host.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return host;
}

std::string original_app_code(const boost::http::route_params& route) {
    if (route.req.count("x-original-url") == 0) {
        return {};
    }
    const auto original =
        boost::urls::parse_uri_reference(std::string_view(route.req.at("x-original-url")));
    if (!original) {
        return {};
    }
    for (const auto& parameter : original->params()) {
        if (parameter.key == "__lake_code") {
            return std::string(parameter.value);
        }
    }
    return {};
}

std::string app_authorize_path(std::string_view app_id, std::string_view redirect) {
    return "/apps/authorize?app=" + oauth_url_encode(app_id) + "&rd=" + oauth_url_encode(redirect);
}

// A Ship app return (`/apps/authorize?app=…&rd=…`) rebuilt from its parts, or empty when `path`
// is anything else; /apps/authorize itself then checks `rd` against the app.
std::string app_return_path(std::string_view path) {
    const auto parsed = boost::urls::parse_relative_ref(path);
    if (!parsed || parsed->encoded_path() != "/apps/authorize" || parsed->has_fragment()) {
        return {};
    }
    std::string app_id;
    std::string redirect;
    for (const auto& parameter : parsed->params()) {
        if (parameter.key == "app") {
            app_id = std::string(parameter.value);
        } else if (parameter.key == "rd") {
            redirect = std::string(parameter.value);
        }
    }
    if (app_id.empty() || redirect.empty()) {
        return {};
    }
    return app_authorize_path(app_id, redirect);
}

// The Ship app return /apps/authorize leaves in the lake_sign_in_return cookie for the sign-in
// routes that redirect after setting the session (the Clerk exchange, the OAuth callback).
std::string app_sign_in_return(const boost::http::route_params& route) {
    const std::string encoded = cookie_value(route, "lake_sign_in_return");
    if (encoded.empty()) {
        return {};
    }
    const auto decoded = decode_base64url(encoded);
    return decoded ? app_return_path(*decoded) : std::string();
}

// Where an account holder lands after signing in when nothing names a page: the home, which is Ask
// Orama for a signed-in person (lake#600). The operator's token sign-in still opens Repositories.
constexpr std::string_view account_landing = "/";

// Where a sign-in goes next when the browser names it (the web app posts `next`, lake#561): a
// same-origin path (one leading '/', never "//" or "/\", which a browser reads as another host),
// or a Ship app return. Empty: anything else.
std::string same_origin_next(std::string_view next) {
    if (const std::string app = app_return_path(next); !app.empty()) {
        return app;
    }
    if (!next.starts_with('/') || next.starts_with("//") || next.starts_with("/\\") ||
        std::ranges::any_of(next, [](unsigned char c) { return c < 0x20 || c == 0x7f; })) {
        return {};
    }
    const auto parsed = boost::urls::parse_relative_ref(next);
    if (!parsed || parsed->has_authority() || parsed->has_scheme()) {
        return {};
    }
    return std::string(next);
}

std::string app_sign_in_return_cookie(const boost::http::route_params& route,
                                      std::string_view path) {
    std::string cookie = "lake_sign_in_return=" + control::identity_crypto::base64url_encode(path) +
                         "; Path=/; Max-Age=120; HttpOnly; SameSite=Lax";
    if (request_is_https(route)) {
        cookie += "; Secure";
    }
    return cookie;
}

std::string clear_app_sign_in_return_cookie(const boost::http::route_params& route) {
    std::string cookie = "lake_sign_in_return=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax";
    if (request_is_https(route)) {
        cookie += "; Secure";
    }
    return cookie;
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

std::optional<std::uint64_t> decimal_value(std::string_view value) {
    std::uint64_t output = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return output;
}

struct OciContentRange {
    std::uint64_t start;
    std::uint64_t end;

    [[nodiscard]] std::uint64_t size() const {
        return end - start + 1;
    }
};

std::expected<std::optional<OciContentRange>, std::string>
oci_content_range(const boost::http::route_params& route) {
    if (route.req.count("Content-Range") == 0) {
        return std::optional<OciContentRange>{};
    }
    std::string_view value = route.req.at("Content-Range");
    if (value.starts_with("bytes ")) {
        value.remove_prefix(6);
    }
    if (const std::size_t slash = value.find('/'); slash != std::string_view::npos) {
        value = value.substr(0, slash);
    }
    const std::size_t dash = value.find('-');
    if (dash == std::string_view::npos || value.find('-', dash + 1) != std::string_view::npos) {
        return std::unexpected("Content-Range must be START-END for an OCI upload chunk");
    }
    const auto start = decimal_value(value.substr(0, dash));
    const auto end = decimal_value(value.substr(dash + 1));
    if (!start || !end || *end < *start ||
        (*start == 0 && *end == std::numeric_limits<std::uint64_t>::max())) {
        return std::unexpected("Content-Range is invalid for an OCI upload chunk");
    }
    return std::optional<OciContentRange>{{*start, *end}};
}

std::string browser_session_cookie(const boost::http::route_params& route,
                                   std::string_view secret) {
    std::string cookie = "lake_session=" + std::string(secret) + "; HttpOnly; SameSite=Lax; Path=/";
    if (request_is_https(route)) {
        cookie += "; Secure";
    }
    return cookie;
}

nlohmann::json to_nlohmann(const boost::json::value& value) {
    return nlohmann::json::parse(boost::json::serialize(value), nullptr, false);
};

// A request body as JSON, or a discarded value when it is not JSON: the parser throws on a body
// that ends inside a value (`{"name":"x`), which must be the caller's 400, not lake's 500
// (lake#18).
nlohmann::json body_json(JsonBodySink& sink) {
    try {
        return to_nlohmann(sink.release());
    } catch (const std::exception&) {
        return nlohmann::json(nlohmann::json::value_t::discarded);
    }
}

// RFC 6750's challenge for a refused bearer, naming why and where a new key is made (lake#450).
std::string bearer_challenge(const ApiResult& result) {
    std::string challenge = "Bearer realm=\"lake\"";
    if (result.error.reason == "invalid" || result.error.reason == "expired") {
        challenge += ", error=\"invalid_token\", error_description=\"" +
                     std::string(result.error.reason == "expired" ? "the credential has expired"
                                                                  : "the credential is not valid") +
                     "\"";
    }
    if (!result.error.help.empty()) {
        challenge += ", error_uri=\"" + result.error.help + "\"";
    }
    return challenge;
}

std::string error_body(std::string_view code, std::string_view message) {
    return nlohmann::json{{"ok", false},
                          {"error", {{"code", code}, {"message", message}, {"path", ""}}}}
        .dump();
}

boost::http::status scim_status(const ApiResult& result) {
    if (result.ok) {
        return boost::http::status::ok;
    }
    if (result.error.code == "unauthorized" || result.error.code == "unauthenticated") {
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

class LiquidCustomerStores final : public SyncStore {
public:
    LiquidCustomerStores(std::string root, std::chrono::milliseconds owner_wait)
        : root_(std::move(root)), owner_wait_(owner_wait) {}

    // lake#380: where a workspace's own store is and when a repository is served from it. Every
    // member is set by the server from the control store and the workspace service.
    struct WorkspaceStoreConfig {
        // The workspace whose own store holds the repository, or nothing while the repository is
        // served from its space's store (workspaces.store_moved_at is null).
        std::function<SyncStoreResult<std::optional<std::string>>(const std::string& customer_id,
                                                                  const std::string& repository_id)>
            route;
        // A space's switched workspaces, each with whether it is serving now (desired running),
        // and whether any of its workspaces is not switched yet.
        std::function<SyncStoreResult<std::pair<std::vector<std::pair<std::string, bool>>, bool>>(
            const std::string& customer_id)>
            switched;
        std::function<SyncStoreResult<control::WorkspaceEndpoint>(const std::string& customer_id,
                                                                  const std::string& workspace_id)>
            endpoint;
        // The bucket quota a workspace store is created with (the space's storage quota).
        std::function<std::uint64_t(const std::string& customer_id)> quota_bytes;
        std::shared_ptr<control::ObjectTransport> transport;
        std::string instance_kek;
        std::chrono::milliseconds lease_ttl{std::chrono::seconds(30)};
        // Each request to a workspace Zetta's object routes, for the process metrics (lake#527).
        control::ObjectRequestObserver observer;
    };

    // lake#915: whether a replication pull may reach private addresses, and how long one may take.
    // Set once by the server before it serves.
    void configure_replication(bool allow_private_addresses, std::chrono::milliseconds timeout) {
        replication_allow_private_ = allow_private_addresses;
        replication_timeout_ = timeout;
    }

    void configure_workspace_stores(WorkspaceStoreConfig config) {
        std::lock_guard maps(mutex_);
        workspace_config_ = std::move(config);
    }

    // The counters of an open workspace store, for tests and the latency measurement.
    std::shared_ptr<control::ObjectStoreCounters> workspace_store_counters(const std::string& workspace_id) {
        std::lock_guard maps(mutex_);
        const auto found = workspace_stores_.find(workspace_id);
        return found == workspace_stores_.end() ? nullptr : found->second->client->counters();
    }

    // Opens every existing customer store and repository/result index. The server runs it once
    // it owns the store root and before store-touching requests are admitted, so every existing
    // store's liquid owner lock is taken off the request path (lake#479).
    SyncStoreResult<void> warm_metering() try {
        if (root_.empty()) {
            return {};
        }
        std::error_code error;
        if (!std::filesystem::exists(root_, error) && !error) {
            return {};
        }
        if (error) {
            return std::unexpected(SyncStoreError{"io", error.message()});
        }
        for (const auto& customer : std::filesystem::directory_iterator(root_, error)) {
            if (std::filesystem::is_directory(customer.path() / "store", error)) {
                const std::string customer_id = customer.path().filename().string();
                auto space = hold("s:" + customer_id);
                const auto opened = open_customer(customer_id);
                if (!opened) {
                    return std::unexpected(opened.error());
                }
                // Seed the space store's size, which a write in a switched workspace counts when
                // the space's lane is busy, so a restart never counts it as 0.
                if (const auto measured = space_stored_bytes_locked(customer_id)) {
                    std::lock_guard maps(mutex_);
                    last_space_bytes_[customer_id] = *measured;
                }
            }
            error.clear();
            const auto repositories = customer.path() / "repositories";
            if (!std::filesystem::is_directory(repositories, error)) {
                error.clear();
                continue;
            }
            for (const auto& repository : std::filesystem::directory_iterator(repositories, error)) {
                if (!repository.is_directory()) {
                    continue;
                }
                // A repository served from its workspace's store is warmed when that store opens
                // on first use (lake#380), not at start.
                {
                    const auto route = workspace_route(customer.path().filename().string(),
                                                       repository.path().filename().string());
                    // A directory no live repository routes to is warmed on first use, if ever.
                    if (!route || *route) {
                        continue;
                    }
                }
                RepositoryArtifactFilter filter;
                filter.limit = 1000;
                for (;;) {
                    const auto page = repository_artifacts(customer.path().filename().string(),
                        repository.path().filename().string(), filter);
                    if (!page) {
                        return std::unexpected(page.error());
                    }
                    if (page->done || page->next_cursor.empty()) {
                        break;
                    }
                    filter.cursor = page->next_cursor;
                }
            }
        }
        if (error) {
            return std::unexpected(SyncStoreError{"io", error.message()});
        }
        return {};
    } catch (const std::filesystem::filesystem_error& error) {
        return std::unexpected(SyncStoreError{"io", error.what()});
    }

    SyncStoreResult<std::vector<std::string>>
    find_missing(const std::string& customer_id, const std::vector<std::string>& digests) override {
        if (!path_component_is_safe(customer_id)) {
            return std::unexpected(SyncStoreError{"invalid_input", "invalid customer location"});
        }
        auto space = admit("s:" + customer_id);
        if (!space) {
            return std::unexpected(space.error());
        }
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
        if (!path_component_is_safe(customer_id)) {
            return std::unexpected(SyncStoreError{"invalid_input", "invalid customer location"});
        }
        auto space = admit("s:" + customer_id);
        if (!space) {
            return std::unexpected(space.error());
        }
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
        auto stored_bytes = unique_stored_bytes_locked(customer_id, true);
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
        if (!path_component_is_safe(customer_id)) {
            return std::unexpected(SyncStoreError{"invalid_input", "invalid customer location"});
        }
        auto space = admit("s:" + customer_id);
        if (!space) {
            return std::unexpected(space.error());
        }
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
        if (!path_component_is_safe(customer_id)) {
            return std::unexpected(SyncStoreError{"invalid_input", "invalid customer location"});
        }
        auto space = admit("s:" + customer_id);
        if (!space) {
            return std::unexpected(space.error());
        }
        return unique_stored_bytes_locked(customer_id, true);
    }

    SyncStoreResult<std::string> repository_liquid_id(const std::string& customer_id,
                                                      const std::string& repository_id) override {
        auto entered_repository = enter_repository(customer_id, repository_id);
        auto& state = entered_repository.state;
        if (!state) {
            return std::unexpected(state.error());
        }
        const auto id = (*state)->subject->id();
        if (!id) {
            return std::unexpected(error_of(id.error()));
        }
        static constexpr std::string_view digits = "0123456789abcdef";
        std::string output;
        output.reserve(id->size() * 2);
        for (const std::byte octet : *id) {
            const auto value = std::to_integer<unsigned char>(octet);
            output += digits[value >> 4U];
            output += digits[value & 0x0fU];
        }
        return output;
    }

    SyncStoreResult<std::vector<std::pair<std::string, std::string>>>
    refs(const std::string& customer_id, const std::string& repository_id) override {
                auto entered_repository = enter_repository(customer_id, repository_id);        auto& state = entered_repository.state;
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
                auto entered_repository = enter_repository(customer_id, repository_id);        auto& state = entered_repository.state;
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
                auto entered_repository = enter_repository(customer_id, repository_id);        auto& state = entered_repository.state;
        if (!state) {
            return std::unexpected(state.error());
        }
        auto stored_bytes = unique_stored_bytes_locked(customer_id, (*state)->workspace.empty());
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
                .author = item.author.taap_id,
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
        auto pushed = from_liquid(liquid::runtime::block_on(server.handle_push(request)));
        if (!pushed) {
            return pushed;
        }
        if (auto durable = entered_repository.durable.now(); !durable) {
            return std::unexpected(durable.error());
        }
        return pushed;
    }

    SyncStoreResult<std::vector<std::byte>> pull(const std::string& customer_id,
                                                 const std::string& repository_id,
                                                 std::span<const std::byte> request) override {
                auto entered_repository = enter_repository(customer_id, repository_id);        auto& state = entered_repository.state;
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
                auto entered_repository = enter_repository(customer_id, repository_id);        auto& state = entered_repository.state;
        if (!state) {
            return std::unexpected(state.error());
        }
        auto stored_bytes = unique_stored_bytes_locked(customer_id, (*state)->workspace.empty());
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
        if (auto durable = entered_repository.durable.now(); !durable) {
            return std::unexpected(durable.error());
        }
        return unique_stored_bytes_locked(customer_id, (*state)->workspace.empty());
    }

    SyncStoreResult<std::vector<std::byte>> read_chunk(const std::string& customer_id,
                                                       const std::string& repository_id,
                                                       const std::string& digest_text,
                                                       std::uint64_t offset,
                                                       std::uint64_t length) override {
                if (length > std::numeric_limits<std::size_t>::max()) {
            return std::unexpected(SyncStoreError{
                "invalid_argument", "chunk range length is too large", "sync server"});
        }
        const auto digest = liquid::parse_digest(digest_text);
        if (!digest) {
            return std::unexpected(error_of(digest.error()));
        }
        auto entered_repository = enter_repository(customer_id, repository_id);        auto& state = entered_repository.state;
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

    SyncStoreResult<RepositoryReplicationReport>
    replicate_from(const std::string& customer_id,
                   const std::string& repository_id,
                   const std::string& source_instance,
                   const std::string& source_url,
                   const std::string& bearer) override {
        if (!peer_url_is_safe(source_url)) {
            return std::unexpected(SyncStoreError{
                "invalid_input", "replication peer URL must use HTTPS or loopback HTTP"});
        }
        // lake#915: only HTTPS to public addresses unless the operator allows private ones.
        const auto target = parse_mcp_url(source_url, replication_allow_private_);
        if (!target || (!replication_allow_private_ && target->scheme != "https")) {
            return std::unexpected(
                SyncStoreError{"invalid_input", "replication peer URL must use HTTPS"});
        }
        if (!path_component_is_safe(source_instance)) {
            return std::unexpected(
                SyncStoreError{"invalid_input", "source instance is not a safe identifier"});
        }
        auto entered_repository = enter_repository(customer_id, repository_id);        auto& state = entered_repository.state;
        if (!state) {
            return std::unexpected(state.error());
        }
        // The cursor is repository state in liquid (liquid#326), so it lives in whichever store
        // holds the repository. A cursor lake wrote beside the repository before is read once.
        liquid::sync_endpoint_options options;
        options.bearer_token = bearer;
        options.atomic = true;
        auto cursor = liquid::runtime::block_on((*state)->subject->replication_cursor(source_instance));
        if (!cursor) {
            return std::unexpected(error_of(cursor.error()));
        }
        if (*cursor) {
            options.cursor = std::move(**cursor);
        } else if ((*state)->workspace.empty()) {
            const std::filesystem::path legacy =
                (*state)->path / "replication" / (source_instance + ".cursor");
            std::ifstream cursor_input(legacy, std::ios::binary);
            if (cursor_input) {
                options.cursor.token.assign(std::istreambuf_iterator<char>(cursor_input),
                                            std::istreambuf_iterator<char>());
            } else if (std::filesystem::exists(legacy)) {
                return std::unexpected(
                    SyncStoreError{"io", "could not read repository replication cursor"});
            }
        }
        liquid::sync_endpoint endpoint(source_url, std::move(options));
        const auto pulled =
            liquid::runtime::block_on(bounded_replication_pull(endpoint,
                                                               *(*state)->subject,
                                                               target->host,
                                                               target->port,
                                                               !replication_allow_private_,
                                                               replication_timeout_));
        if (!pulled) {
            return std::unexpected(error_of(pulled.error()));
        }
        const auto saved = liquid::runtime::block_on(
            (*state)->subject->set_replication_cursor(source_instance, pulled->cursor));
        if (!saved) {
            return std::unexpected(error_of(saved.error()));
        }
        if (auto durable = entered_repository.durable.now(); !durable) {
            return std::unexpected(durable.error());
        }
        return RepositoryReplicationReport{
            .cursor = pulled->cursor.token,
            .metadata_items = pulled->metadata_items,
            .received_objects = pulled->received.objects,
            .received_bytes = pulled->received.bytes,
            .references = pulled->references.size(),
        };
    }

    SyncStoreResult<std::vector<RepositoryContentRef>>
    repository_refs(const std::string& customer_id, const std::string& repository_id) override {
                auto entered_repository = enter_repository(customer_id, repository_id);        auto& repository = entered_repository.state;
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
                auto entered_repository = enter_repository(customer_id, repository_id);        auto& repository = entered_repository.state;
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
                auto entered_repository = enter_repository(customer_id, repository_id);        auto& repository = entered_repository.state;
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

    SyncStoreResult<RepositoryFile> repository_blob(const std::string& customer_id,
                                                    const std::string& repository_id,
                                                    const std::string& path,
                                                    const std::string& blob) override {
                auto entered_repository = enter_repository(customer_id, repository_id);        auto& repository = entered_repository.state;
        if (!repository) {
            return std::unexpected(repository.error());
        }
        const auto parsed = liquid::parse_digest(blob);
        if (!parsed) {
            return std::unexpected(error_of(parsed.error()));
        }
        const auto contents = liquid::runtime::block_on((*repository)->subject->blob(*parsed));
        if (!contents) {
            return std::unexpected(error_of(contents.error()));
        }
        return RepositoryFile{path,
                              blob,
                              display_contents(*contents),
                              static_cast<std::uint64_t>(contents->size()),
                              *contents};
    }

    SyncStoreResult<std::vector<RepositoryChange>>
    repository_log(const std::string& customer_id, const std::string& repository_id) override {
                auto entered_repository = enter_repository(customer_id, repository_id);        auto& repository = entered_repository.state;
        if (!repository) {
            return std::unexpected(repository.error());
        }
        const auto changes = repository_changes(*(*repository)->subject);
        if (!changes) {
            return std::unexpected(changes.error());
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
                auto entered_repository = enter_repository(customer_id, repository_id);        auto& repository = entered_repository.state;
        if (!repository) {
            return std::unexpected(repository.error());
        }
        const auto changes = repository_changes(*(*repository)->subject);
        if (!changes) {
            return std::unexpected(changes.error());
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
        if (!path_component_is_safe(customer_id) || !path_component_is_safe(repository_id)) {
            return std::unexpected(SyncStoreError{"invalid_input", "invalid repository location"});
        }
        const auto route = workspace_route(customer_id, repository_id);
        if (!route) {
            return std::unexpected(route.error());
        }
        std::error_code error;
        const auto path = std::filesystem::path(root_) / customer_id / "repositories" / repository_id;
        if (!*route && !std::filesystem::exists(path, error) && !error) {
            return RepositoryArtifactPage{.done = true};
        }
        if (error) {
            return std::unexpected(SyncStoreError{"io", error.message()});
        }
        auto entered_repository = enter_repository(customer_id, repository_id);
        auto& repository = entered_repository.state;
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
            auto artifact = artifact_record_with_evidence(*(*repository)->subject, record);
            if (!artifact) {
                return std::unexpected(artifact.error());
            }
            output.items.push_back(std::move(*artifact));
        }
        if (page->next) {
            output.next_cursor = page->next->token;
        }
        return output;
    }

    SyncStoreResult<RepositoryArtifact> repository_artifact(const std::string& customer_id,
                                                            const std::string& repository_id,
                                                            const std::string& result_id) override {
                auto entered_repository = enter_repository(customer_id, repository_id);        auto& repository = entered_repository.state;
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
        return artifact_record_with_evidence(*(*repository)->subject, results->records.front());
    }

    SyncStoreResult<std::vector<RepositoryConflict>>
    repository_conflicts(const std::string& customer_id,
                         const std::string& repository_id) override {
                auto entered_repository = enter_repository(customer_id, repository_id);        auto& repository = entered_repository.state;
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
            RepositoryConflict mapped{
                .id = liquid::to_string(conflict.id),
                .base = liquid::to_string(conflict.base),
                .left = liquid::to_string(conflict.left),
                .right = liquid::to_string(conflict.right),
                .region = {.path = conflict.region.path},
                .kind = conflict_kind_name(conflict.kind),
                .explanation = conflict.explanation,
                .reasons = conflict.reasons,
                .node = conflict.node,
            };
            if (conflict.region.bytes) {
                mapped.region.bytes = RepositoryConflictByteRange{conflict.region.bytes->offset,
                                                                  conflict.region.bytes->length};
            }
            if (conflict.region.nodes) {
                mapped.region.nodes =
                    RepositoryConflictNodeRange{node_id_text(conflict.region.nodes->first),
                                                node_id_text(conflict.region.nodes->last)};
            }
            if (conflict.proposal) {
                mapped.proposal = liquid::to_string(*conflict.proposal);
            }
            for (const liquid::digest& value : {conflict.left, conflict.right}) {
                const auto contents =
                    liquid::runtime::block_on((*repository)->subject->blob(value));
                if (contents) {
                    mapped.version_blobs.push_back(liquid::to_string(value));
                    continue;
                }
                const auto tree = liquid::runtime::block_on((*repository)->subject->tree(value));
                if (!tree) {
                    mapped.version_blobs.push_back(liquid::to_string(value));
                    continue;
                }
                const auto entry = find_file(*(*repository)->subject, *tree, conflict.region.path);
                mapped.version_blobs.push_back(entry ? liquid::to_string(entry->ref)
                                                     : liquid::to_string(value));
            }
            output.push_back(std::move(mapped));
        }
        return output;
    }

    SyncStoreResult<RepositoryConflictResolution>
    resolve_repository_conflict(const std::string& customer_id,
                                const std::string& repository_id,
                                const std::string& conflict_id,
                                const std::string& resolution,
                                std::vector<std::byte> blob) override {
                auto entered_repository = enter_repository(customer_id, repository_id);        auto& repository = entered_repository.state;
        if (!repository) {
            return std::unexpected(repository.error());
        }
        const auto parsed = liquid::parse_digest(conflict_id);
        if (!parsed) {
            return std::unexpected(error_of(parsed.error()));
        }
        liquid::conflict_resolution_request request;
        if (resolution == "accept_proposal") {
            request.kind = liquid::conflict_resolution_kind::accept_proposal;
        } else if (resolution == "left") {
            request.kind = liquid::conflict_resolution_kind::left;
        } else if (resolution == "right") {
            request.kind = liquid::conflict_resolution_kind::right;
        } else if (resolution == "blob") {
            request.kind = liquid::conflict_resolution_kind::blob;
            request.blob = std::move(blob);
        } else {
            return std::unexpected(SyncStoreError{"invalid_input", "unknown conflict resolution"});
        }
        const auto resolved = liquid::runtime::block_on(
            (*repository)->subject->resolve_conflict(*parsed, std::move(request)));
        if (!resolved) {
            return std::unexpected(error_of(resolved.error()));
        }
        if (auto durable = entered_repository.durable.now(); !durable) {
            return std::unexpected(durable.error());
        }
        const std::string change_id = liquid::to_string(resolved->value);
        const auto history = liquid::runtime::block_on((*repository)->subject->log());
        if (!history) {
            return std::unexpected(error_of(history.error()));
        }
        const auto change = std::ranges::find_if(*history, [&](const liquid::change& candidate) {
            return liquid::to_string(candidate.id.value) == change_id;
        });
        if (change == history->end()) {
            return std::unexpected(
                SyncStoreError{"store_unavailable", "resolved change is absent from history"});
        }
        const auto recorded =
            std::ranges::find_if(change->resolutions, [&](const liquid::conflict_resolution& item) {
                return liquid::to_string(item.conflict) == conflict_id;
            });
        if (recorded == change->resolutions.end()) {
            return std::unexpected(SyncStoreError{
                "store_unavailable", "resolved change does not name the conflict value"});
        }
        return RepositoryConflictResolution{
            conflict_id, liquid::to_string(recorded->value), change_id};
    }

    SyncStoreResult<RepositoryStorageSummary>
    repository_storage(const std::string& customer_id, const std::string& repository_id) override {
                auto entered_repository = enter_repository(customer_id, repository_id);        auto& state = entered_repository.state;
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
                if (!valid_lfs_oid(oid)) {
            return std::unexpected(SyncStoreError{"invalid_input", "invalid LFS SHA-256 oid"});
        }
        auto entered_repository = enter_repository(customer_id, repository_id);        auto& state = entered_repository.state;
        if (!state) {
            return std::unexpected(state.error());
        }
        auto stored_bytes = unique_stored_bytes_locked(customer_id, (*state)->workspace.empty());
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
        if (auto durable = entered_repository.durable.now(); !durable) {
            return std::unexpected(durable.error());
        }
        return unique_stored_bytes_locked(customer_id, (*state)->workspace.empty());
    }

    SyncStoreResult<std::string>
    commit_repository_files(const std::string& customer_id,
                            const std::string& repository_id,
                            const std::string& revision,
                            const std::string& message,
                            std::vector<RepositoryWriteOperation> operations) override {
                auto entered_repository = enter_repository(customer_id, repository_id);        auto& state = entered_repository.state;
        if (!state) {
            return std::unexpected(state.error());
        }
        if (revision.empty() || message.empty()) {
            return std::unexpected(
                SyncStoreError{"invalid_input", "revision and commit message are required"});
        }
        if (revision != "main") {
            return std::unexpected(
                SyncStoreError{"conflict", "repository changes currently update main only"});
        }
        std::set<std::string> target_paths;
        std::set<std::string> staged_uploads;
        std::set<std::string> source_paths;
        for (const RepositoryWriteOperation& operation : operations) {
            // A folder at the top (an empty path) is the repository's own empty-folder marker:
            // create_drive writes it as a new repository's first change (docs/api/drives.md).
            const bool top_folder =
                operation.kind == RepositoryWriteKind::Folder && operation.path.empty();
            if (!top_folder && !valid_repository_path(operation.path)) {
                return std::unexpected(
                    SyncStoreError{"invalid_input", "invalid repository path", operation.path});
            }
            if (!target_paths.insert(operation.path).second) {
                return std::unexpected(SyncStoreError{
                    "invalid_input", "each commit operation must have a distinct path"});
            }
            if (operation.kind == RepositoryWriteKind::Moved &&
                (!valid_repository_path(operation.source_path) ||
                 !source_paths.insert(operation.source_path).second)) {
                return std::unexpected(SyncStoreError{
                    "invalid_input", "each move must name a distinct valid source path"});
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
            if (operation.kind == RepositoryWriteKind::Folder) {
                const auto written = liquid::runtime::block_on((*state)->subject->write(
                    operation.path.empty() ? std::string(".lake-folder")
                                           : operation.path + "/.lake-folder",
                    {}));
                if (!written) {
                    return std::unexpected(error_of(written.error()));
                }
                continue;
            }
            if (operation.kind == RepositoryWriteKind::Moved) {
                const auto moved = liquid::runtime::block_on(
                    (*state)->subject->move(operation.source_path, operation.path));
                if (!moved) {
                    return std::unexpected(error_of(moved.error()));
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
        // The checkpoint ends with the removal of LIQUID_JOURNAL and no barrier (liquid#343): it
        // is durable here, before the change is acknowledged (lake#535).
        if (auto durable = entered_repository.durable.now(); !durable) {
            return std::unexpected(durable.error());
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
                auto entered_repository = enter_repository(customer_id, repository_id);        auto& state = entered_repository.state;
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
        if (auto durable = entered_repository.durable.now(); !durable) {
            return std::unexpected(durable.error());
        }
        return provenance_record(
            *model_revision, dataset_repository_id, *model_id, *run_id, *model, *run);
    }

    SyncStoreResult<std::optional<RepositoryModelProvenance>>
    model_provenance(const std::string& customer_id,
                     const std::string& repository_id,
                     const std::string& revision) override {
                auto entered_repository = enter_repository(customer_id, repository_id);        auto& state = entered_repository.state;
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
                auto entered_repository = enter_repository(customer_id, repository_id);        auto& repository = entered_repository.state;
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
        if (auto durable = entered_repository.durable.now(); !durable) {
            return std::unexpected(durable.error());
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
                        auto entered_repository = enter_repository(customer_id, repository_id);            auto& state = entered_repository.state;
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
                        auto entered_repository = enter_repository(customer_id, repository_id);            auto& state = entered_repository.state;
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
                auto entered_repository = enter_repository(customer_id, repository_id);        auto& state = entered_repository.state;
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
        if (auto durable = entered_repository.durable.now(); !durable) {
            return std::unexpected(durable.error());
        }
        return {};
    }

    SyncStoreResult<RepositoryGitImport> import_git(const std::string& customer_id,
                                                    const std::string& repository_id,
                                                    const std::string& source_directory) override {
                auto entered_repository = enter_repository(customer_id, repository_id);        auto& state = entered_repository.state;
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
        if (auto durable = entered_repository.durable.now(); !durable) {
            return std::unexpected(durable.error());
        }
        return output;
    }

private:
    struct RepositoryState {
        std::unique_ptr<liquid::repository> subject;
        liquid::store* objects{};
        std::filesystem::path path;
        // The workspace whose own store holds it (lake#380); empty for the space's store.
        std::string workspace;
    };

    // One workspace's liquid store in its workspace Zetta (lake#380), opened on first use.
    struct WorkspaceStore {
        std::string customer_id;
        std::shared_ptr<control::ZettaObjectClient> client;
        std::shared_ptr<control::ZettaObjectBackend> backend;
        std::shared_ptr<control::ZettaStoreOwner> owner;
        std::unique_ptr<liquid::store> store;
        // The workspace's data key, as search seals its index objects with it (lake#381), read
        // once per open store.
        std::shared_ptr<const control::WorkspaceStoreCipher> search_cipher;
        // Set when Zetta refused a search index write as fenced: the lease is another holder's.
        // A failed search write never fails the store otherwise.
        std::shared_ptr<std::atomic<bool>> search_fenced = std::make_shared<std::atomic<bool>>(false);
        // Closed when this process stops writing the store's search index: at the drain's
        // invalidation and when the store closes (lake#606).
        std::shared_ptr<IndexWriteGate> index_gate = std::make_shared<IndexWriteGate>();

        // A store whose lease was lost, or one a write failed on, is closed and opened again.
        bool usable() const {
            return !owner->lost() && !backend->failed() && !search_fenced->load() &&
                   !client->restarted();
        }
    };

    // One lane per store: `s:<space>` for a space's store on lake's volume, `w:<workspace>` for a
    // workspace's own store. A request holds its store's lane for its whole call; mutex_ guards
    // only the maps below and is never held across store I/O, network calls or waits, so one
    // workspace's I/O, lease wait or switch never holds up another's. Lock order: a workspace lane,
    // then its space's lane.
    //
    // Admission (lake#380): a request is admitted to a lane only while fewer than
    // lane_admission_limit requests are in it (holding or waiting), so requests parked on one slow
    // store never hold more than that many of the blocking pool's threads; and never while the
    // lane is being moved (copy, verify or switch). Otherwise it is answered store_unavailable at
    // once, without waiting.
    static constexpr int lane_admission_limit = 4;
    struct Lane {
        std::mutex mutex;
        std::atomic<int> occupants{0};
        std::atomic<int> moving{0};
        // Image pulls in flight (lake#383 phase B), counted apart from `occupants`, so pulls never
        // take the admissions repository and git requests need.
        std::atomic<int> streams{0};
    };
    static constexpr int lane_stream_limit = 8;
    class LaneLock {
    public:
        LaneLock() = default;
        LaneLock(std::shared_ptr<Lane> lane, std::unique_lock<std::mutex> lock, bool admitted)
            : lane_(std::move(lane)), lock_(std::move(lock)), admitted_(admitted) {}
        LaneLock(LaneLock&& other) noexcept
            : lane_(std::move(other.lane_)), lock_(std::move(other.lock_)),
              admitted_(std::exchange(other.admitted_, false)) {}
        LaneLock& operator=(LaneLock&& other) noexcept {
            if (this != &other) {
                release();
                lane_ = std::move(other.lane_);
                lock_ = std::move(other.lock_);
                admitted_ = std::exchange(other.admitted_, false);
            }
            return *this;
        }
        LaneLock(const LaneLock&) = delete;
        LaneLock& operator=(const LaneLock&) = delete;
        ~LaneLock() {
            release();
        }

    private:
        void release() {
            if (lock_.owns_lock()) {
                lock_.unlock();
            }
            if (admitted_ && lane_) {
                lane_->occupants.fetch_sub(1);
            }
            admitted_ = false;
        }
        std::shared_ptr<Lane> lane_;
        std::unique_lock<std::mutex> lock_;
        bool admitted_ = false;
    };
    std::shared_ptr<Lane> lane_of(const std::string& name) {
        std::lock_guard maps(mutex_);
        auto& slot = lanes_[name];
        if (!slot) {
            slot = std::make_shared<Lane>();
        }
        return slot;
    }
    // A request's way into a lane (see Admission). Once lake drains (lake#536), a workspace's
    // lane admits no new request: the workspace store is about to release its lease, and the
    // retry lands on the successor that takes it.
    SyncStoreResult<LaneLock> admit(const std::string& name) {
        if (name.starts_with("w:") && draining_.load()) {
            return std::unexpected(stopping_error());
        }
        auto lane = lane_of(name);
        if (lane->moving.load() != 0) {
            return std::unexpected(SyncStoreError{
                "store_unavailable", "the store is being moved to its workspace; retry shortly"});
        }
        if (lane->occupants.fetch_add(1) >= lane_admission_limit) {
            lane->occupants.fetch_sub(1);
            return std::unexpected(SyncStoreError{
                "store_unavailable", "the store is busy with other requests; retry shortly"});
        }
        std::unique_lock<std::mutex> lock(lane->mutex);
        return LaneLock(lane, std::move(lock), true);
    }
    // Marks a lane as being moved while a copy, verify or switch holds it.
    class Moving {
    public:
        explicit Moving(std::shared_ptr<Lane> lane) : lane_(std::move(lane)) {
            lane_->moving.fetch_add(1);
        }
        ~Moving() {
            lane_->moving.fetch_sub(1);
        }
        Moving(const Moving&) = delete;
        Moving& operator=(const Moving&) = delete;

    private:
        std::shared_ptr<Lane> lane_;
    };
    // Lake's own work in a lane (warm-up, the move, closing): not admitted, not capped.
    LaneLock hold(const std::string& name) {
        auto lane = lane_of(name);
        std::unique_lock<std::mutex> lock(lane->mutex);
        return LaneLock(lane, std::move(lock), false);
    }

    // The requests a workspace store sends while one call holds its lane, added to the call's
    // ObjectRequestScope when it leaves the lane (lake#527, the request's Server-Timing).
    class StoreRequests {
    public:
        StoreRequests() = default;
        explicit StoreRequests(std::shared_ptr<control::ObjectStoreCounters> counters)
            : sink_(control::ObjectRequestScope::current()),
              counters_(sink_ != nullptr ? std::move(counters) : nullptr) {
            if (counters_) {
                snapshot();
            }
        }
        StoreRequests(StoreRequests&& other) noexcept
            : sink_(std::exchange(other.sink_, nullptr)), counters_(std::move(other.counters_)),
              count_(other.count_), nanoseconds_(other.nanoseconds_) {}
        StoreRequests& operator=(StoreRequests&& other) noexcept {
            if (this != &other) {
                settle();
                sink_ = std::exchange(other.sink_, nullptr);
                counters_ = std::move(other.counters_);
                count_ = other.count_;
                nanoseconds_ = other.nanoseconds_;
            }
            return *this;
        }
        StoreRequests(const StoreRequests&) = delete;
        StoreRequests& operator=(const StoreRequests&) = delete;
        ~StoreRequests() {
            settle();
        }
        // The store the call uses once it is open. One this call opened (none before, or a
        // failed one replaced) is counted from zero: everything it sent so far was this call's.
        void opened(std::shared_ptr<control::ObjectStoreCounters> counters) {
            if (sink_ != nullptr && counters && counters != counters_) {
                if (counters_ && sink_ != nullptr) {
                    sink_->add_difference(counters_->by_kind, count_, nanoseconds_);
                }
                counters_ = std::move(counters);
                count_.fill(0);
                nanoseconds_.fill(0);
            }
        }

    private:
        void snapshot() {
            for (std::size_t index = 0; index < control::object_request_kinds; ++index) {
                count_[index] = counters_->by_kind.count[index].load(std::memory_order_relaxed);
                nanoseconds_[index] = counters_->by_kind.nanoseconds[index].load(std::memory_order_relaxed);
            }
        }
        void settle() {
            if (sink_ != nullptr && counters_) {
                sink_->add_difference(counters_->by_kind, count_, nanoseconds_);
            }
            sink_ = nullptr;
            counters_.reset();
        }
        control::ObjectRequestTally* sink_ = nullptr;
        std::shared_ptr<control::ObjectStoreCounters> counters_;
        std::array<std::uint64_t, control::object_request_kinds> count_{};
        std::array<std::uint64_t, control::object_request_kinds> nanoseconds_{};
    };

    // What a call wrote to a workspace store is durable before the call leaves the store's lane
    // (lake#535): the backend sends what it holds and one Zetta :barrier. A call that writes makes
    // it durable with now() before it answers, so a failure answers the call; the destructor
    // covers every other way out, and a failed sync there leaves the backend failed, so the store
    // is closed and opened again before its next use.
    class StoreDurability {
    public:
        StoreDurability() = default;
        explicit StoreDurability(std::shared_ptr<control::ZettaObjectBackend> backend)
            : backend_(std::move(backend)) {}
        StoreDurability(StoreDurability&& other) noexcept : backend_(std::move(other.backend_)) {}
        StoreDurability& operator=(StoreDurability&& other) noexcept {
            if (this != &other) {
                settle();
                backend_ = std::move(other.backend_);
            }
            return *this;
        }
        StoreDurability(const StoreDurability&) = delete;
        StoreDurability& operator=(const StoreDurability&) = delete;
        ~StoreDurability() {
            settle();
        }
        SyncStoreResult<void> now() {
            if (!backend_) {
                return {};
            }
            auto durable = liquid::runtime::block_on(backend_->make_durable());
            if (!durable) {
                return std::unexpected(error_of(durable.error()));
            }
            return {};
        }

    private:
        void settle() noexcept {
            if (!backend_) {
                return;
            }
            try {
                static_cast<void>(liquid::runtime::block_on(backend_->make_durable()));
            } catch (...) {
            }
            backend_.reset();
        }
        std::shared_ptr<control::ZettaObjectBackend> backend_;
    };

    struct Entered {
        LaneLock lane;
        SyncStoreResult<RepositoryState*> state;
        // Declared after the lane: it settles while the lane is still held.
        StoreRequests requests;
        // Declared last: it syncs first, while the lane is held and the requests still count.
        StoreDurability durable;
    };

    // The open workspace store's backend, or none.
    std::shared_ptr<control::ZettaObjectBackend> workspace_backend(const std::string& workspace_id) {
        std::lock_guard maps(mutex_);
        const auto found = workspace_stores_.find(workspace_id);
        return found != workspace_stores_.end() ? found->second->backend : nullptr;
    }

    // A workspace's lane for one blob operation (lake#383 phase B): admitted for a request,
    // held (waiting, never refused) for lake's own work and a pull's later reads.
    SyncStoreResult<LaneLock> enter_workspace_lane(const std::string& workspace_id, bool admitted) {
        if (admitted) {
            return admit("w:" + workspace_id);
        }
        return hold("w:" + workspace_id);
    }

    static SyncStoreError error_of(const liquid::error& error) {
        // A store whose owner lock or bucket lease another process holds, or whose lease was lost
        // (lake#479, lake#380): the request can succeed once the store is owned again.
        if (error.code == liquid::errc::locked &&
            (error.message.find("owner grant was lost") != std::string::npos ||
             error.message.find("workspace store") != std::string::npos ||
             error.message.find("store root is owned by") != std::string::npos)) {
            return SyncStoreError{"store_unavailable", error.message, error.context};
        }
        // A workspace Zetta lake will not use (lake#543): without the durability format, or of
        // several nodes. Refused until it is upgraded.
        if (control::workspace_zetta_unsupported(error)) {
            return SyncStoreError{"store_unavailable", error.message, error.context};
        }
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
    space_stored_bytes_locked(const std::string& customer_id) const {
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

    static SyncStoreResult<void> append_result_attestations(liquid::repository& repository,
                                                            const liquid::repository_result& record,
                                                            RepositoryArtifact& artifact) {
        if (record.value.provenance) {
            const auto provenance =
                liquid::runtime::block_on(liquid::build::verify_provenance(repository, record.id));
            if (!provenance) {
                return std::unexpected(error_of(provenance.error()));
            }
            append_attestation(artifact,
                               provenance->attestation,
                               provenance->statement,
                               provenance->verified,
                               provenance->reason);
        }
        if (record.verification) {
            const auto verification = liquid::runtime::block_on(
                repository.attestations({.id = record.verification, .page = {.limit = 1}}));
            if (!verification) {
                return std::unexpected(error_of(verification.error()));
            }
            for (const liquid::repository_attestation& attestation : verification->records) {
                append_verified_attestation(artifact, attestation.id, attestation.value);
            }
        }
        auto attestation_page = liquid::record_page_options{.limit = 1'000};
        for (;;) {
            const auto attestations = liquid::runtime::block_on(
                repository.attestations({.subject = record.id.value, .page = attestation_page}));
            if (!attestations) {
                return std::unexpected(error_of(attestations.error()));
            }
            for (const liquid::repository_attestation& attestation : attestations->records) {
                append_verified_attestation(artifact, attestation.id, attestation.value);
            }
            if (attestations->done || !attestations->next) {
                break;
            }
            attestation_page.cursor = attestations->next;
        }
        auto sbom_page = liquid::record_page_options{.limit = 1'000};
        for (;;) {
            const auto sboms = liquid::runtime::block_on(repository.sboms(record.id, sbom_page));
            if (!sboms) {
                return std::unexpected(error_of(sboms.error()));
            }
            for (const liquid::sbom_report& sbom : sboms->records) {
                append_attestation(
                    artifact, sbom.attestation, sbom.statement, sbom.verified, sbom.reason);
            }
            if (sboms->done || !sboms->next) {
                break;
            }
            sbom_page.cursor = sboms->next;
        }
        return {};
    }

    static SyncStoreResult<std::optional<RepositoryArtifactTrace>>
    result_trace(liquid::repository& repository, const std::string& result_id) {
        auto trace_page = liquid::record_page_options{.limit = 1'000};
        for (;;) {
            const auto traces = liquid::runtime::block_on(repository.traces(trace_page));
            if (!traces) {
                return std::unexpected(error_of(traces.error()));
            }
            for (const liquid::build_trace& trace : traces->records) {
                const auto action = std::ranges::find_if(
                    trace.actions, [&](const liquid::build_trace_action& value) {
                        return liquid::to_string(value.result.value) == result_id;
                    });
                if (action != trace.actions.end()) {
                    return RepositoryArtifactTrace{
                        .id = liquid::to_string(trace.id),
                        .wall_ms = milliseconds(trace.wall_time),
                        .actions = static_cast<std::uint64_t>(trace.actions.size()),
                        .dependencies = static_cast<std::uint64_t>(trace.edges.size()),
                        .cache_hits = trace.cache.hits,
                        .cache_misses = trace.cache.misses,
                    };
                }
            }
            if (traces->done || !traces->next) {
                return std::optional<RepositoryArtifactTrace>{};
            }
            trace_page.cursor = traces->next;
        }
    }

    static SyncStoreResult<RepositoryArtifact>
    artifact_record_with_evidence(liquid::repository& repository,
                                  const liquid::repository_result& record) {
        RepositoryArtifact output = artifact_record(record);
        const auto attestations = append_result_attestations(repository, record, output);
        if (!attestations) {
            return std::unexpected(attestations.error());
        }
        const auto trace = result_trace(repository, output.id);
        if (!trace) {
            return std::unexpected(trace.error());
        }
        output.trace = *trace;
        return output;
    }

    SyncStoreResult<liquid::store*> open_customer(const std::string& customer_id) {
        if (root_.empty()) {
            return std::unexpected(
                SyncStoreError{"store_unavailable", "LAKE_STORE_ROOT is not configured"});
        }
        {
            std::lock_guard maps(mutex_);
            const auto existing = stores_.find(customer_id);
            if (existing != stores_.end()) {
                return existing->second.get();
            }
        }
        // The caller holds the space's lane, so no one else opens this store meanwhile.
        const std::filesystem::path path = std::filesystem::path(root_) / customer_id / "store";
        // Exclusive: the store holds liquid's owner lock on <store>/store.lock until close()
        // (liquid#321). The server's root lock already serializes processes, so this wait is
        // the second line of defense against any other writer of this one store.
        liquid::store_options options;
        options.exclusive_owner = true;
        options.owner_wait = owner_wait_;
        auto opened = liquid::runtime::block_on(liquid::store::open(path, std::move(options)));
        if (!opened) {
            return std::unexpected(error_of(opened.error()));
        }
        std::lock_guard maps(mutex_);
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
            path.starts_with(".lake-hub-upload/") || path == ".lake-folder" ||
            path.ends_with("/.lake-folder")) {
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

    static std::string conflict_kind_name(liquid::conflict_kind kind) {
        switch (kind) {
        case liquid::conflict_kind::text:
            return "text";
        case liquid::conflict_kind::structural:
            return "structural";
        case liquid::conflict_kind::crdt_suspicious:
            return "crdt-suspicious";
        case liquid::conflict_kind::suspicious_merge:
            return "suspicious-merge";
        }
        return "text";
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

    static SyncStoreResult<std::vector<liquid::change>>
    repository_changes(liquid::repository& repository) {
        auto changes = liquid::runtime::block_on(repository.log());
        if (!changes) {
            return std::unexpected(error_of(changes.error()));
        }
        auto identifiers = std::set<std::string>{};
        for (const liquid::change& change : *changes) {
            identifiers.insert(liquid::to_string(change.id.value));
        }
        const auto refs = liquid::runtime::block_on(repository.refs());
        if (!refs) {
            return std::unexpected(error_of(refs.error()));
        }
        for (const liquid::ref& ref : *refs) {
            const auto referenced = liquid::runtime::block_on(repository.log({.from = ref.id}));
            if (!referenced) {
                return std::unexpected(error_of(referenced.error()));
            }
            for (const liquid::change& change : *referenced) {
                if (identifiers.insert(liquid::to_string(change.id.value)).second) {
                    changes->push_back(change);
                }
            }
        }
        return *changes;
    }

    // The repository's identifier in liquid: its path under the space's store directory. A
    // workspace store keeps the same identifier, so liquid#328's transfer keeps its state keys.
    std::filesystem::path repository_identifier(const std::string& customer_id,
                                                const std::string& repository_id) const {
        return std::filesystem::path(root_) / customer_id / "repositories" / repository_id;
    }

    std::uint64_t epoch_of(const std::string& customer_id) {
        std::lock_guard maps(mutex_);
        return epochs_[customer_id];
    }

    // The workspace whose own store serves the repository, or nothing for the space's store. The
    // answer is cached until the space's next switch; one computed across a switch is not kept.
    SyncStoreResult<std::optional<std::string>> workspace_route(const std::string& customer_id,
                                                                const std::string& repository_id) {
        decltype(workspace_config_.route) route_of;
        const std::string key = customer_id + "/" + repository_id;
        std::uint64_t epoch = 0;
        {
            std::lock_guard maps(mutex_);
            if (!workspace_config_.route) {
                return std::optional<std::string>{};
            }
            if (const auto cached = routes_.find(key); cached != routes_.end()) {
                return cached->second;
            }
            route_of = workspace_config_.route;
            epoch = epochs_[customer_id];
        }
        auto route = route_of(customer_id, repository_id);
        if (route) {
            std::lock_guard maps(mutex_);
            if (epochs_[customer_id] == epoch) {
                routes_.insert_or_assign(key, *route);
            }
        }
        return route;
    }

    // The workspace's data key for its search index (lake#381): unwrapped from the store's key
    // object under LAKE_INSTANCE_KEK, once per open store. The caller holds the workspace's lane.
    SyncStoreResult<std::shared_ptr<const control::WorkspaceStoreCipher>>
    search_cipher_of(WorkspaceStore& store, const std::string& workspace_id) {
        {
            std::lock_guard maps(mutex_);
            if (store.search_cipher) {
                return store.search_cipher;
            }
        }
        std::string kek;
        {
            std::lock_guard maps(mutex_);
            kek = workspace_config_.instance_kek;
        }
        auto document = liquid::runtime::block_on(
            store.client->get_all(std::string(control::workspace_store_key_object)));
        if (!document) {
            return std::unexpected(error_of(document.error()));
        }
        auto wrapping = control::WorkspaceStoreCipher::wrapping_key(kek, workspace_id);
        if (!wrapping) {
            return std::unexpected(error_of(wrapping.error()));
        }
        auto unwrapped = control::WorkspaceStoreCipher::unwrap(*wrapping, workspace_id, *document);
        std::ranges::fill(*wrapping, 0);
        if (!unwrapped) {
            return std::unexpected(error_of(unwrapped.error()));
        }
        auto cipher = std::make_shared<const control::WorkspaceStoreCipher>(
            workspace_id, unwrapped->first, unwrapped->second);
        std::ranges::fill(unwrapped->second, 0);
        std::lock_guard maps(mutex_);
        store.search_cipher = cipher;
        return store.search_cipher;
    }

    // The index objects of an open workspace store, sealed with its data key and fenced by its
    // lease. The caller holds the workspace's lane.
    SyncStoreResult<std::shared_ptr<search::IndexObjects>>
    search_objects_of(WorkspaceStore& store, const std::string& workspace_id) {
        auto cipher = search_cipher_of(store, workspace_id);
        if (!cipher) {
            return std::unexpected(cipher.error());
        }
        std::shared_ptr<std::atomic<bool>> fenced = store.search_fenced;
        return std::make_shared<GatedIndexObjects>(
            search::make_sealed_index_objects(
                store.client, *cipher, store.owner->fence(), [fenced] { fenced->store(true); }),
            store.index_gate);
    }

    // Takes the lane of the store that serves the repository and opens it there. A switch of the
    // repository's space between the route and the lane starts it again, so a request held by a
    // switch is answered from the store the switch chose.
    // `lake_work`: lake's own work (a search index rebuild, lake#381) waits for the lane instead of
    // being refused admission when requests fill it.
    Entered enter_repository(const std::string& customer_id, const std::string& repository_id,
                             bool lake_work = false) {
        if (!path_component_is_safe(customer_id) || !path_component_is_safe(repository_id)) {
            return {{}, std::unexpected(SyncStoreError{"invalid_input", "invalid repository location"})};
        }
        for (;;) {
            const std::uint64_t epoch = epoch_of(customer_id);
            auto route = workspace_route(customer_id, repository_id);
            if (!route) {
                return {{}, std::unexpected(route.error())};
            }
            const std::string name = *route ? "w:" + **route : "s:" + customer_id;
            // Lake's own work stays out of a workspace's lane once lake drains (lake#536), so it
            // never holds off the store's release.
            if (lake_work && *route && draining_.load()) {
                return {{}, std::unexpected(stopping_error())};
            }
            SyncStoreResult<LaneLock> lane = lake_work ? SyncStoreResult<LaneLock>(hold(name)) : admit(name);
            if (!lane) {
                return {{}, std::unexpected(lane.error())};
            }
            if (epoch_of(customer_id) != epoch) {
                continue;
            }
            StoreRequests requests(*route ? workspace_store_counters(**route) : nullptr);
            auto state = open_repository(customer_id, repository_id, *route);
            StoreDurability durable;
            if (*route) {
                requests.opened(workspace_store_counters(**route));
                durable = StoreDurability(workspace_backend(**route));
            }
            return {std::move(*lane), std::move(state), std::move(requests), std::move(durable)};
        }
    }

    // Closes a workspace store, releasing its lease unless the lease was lost, after evicting its
    // repositories. The caller holds the workspace's lane; mutex_ is not held while it closes.
    void discard_workspace(const std::string& workspace_id) {
        std::unique_ptr<WorkspaceStore> store;
        std::vector<std::unique_ptr<RepositoryState>> evicted;
        {
            std::lock_guard maps(mutex_);
            for (auto it = repositories_.begin(); it != repositories_.end();) {
                if (it->second->workspace == workspace_id) {
                    evicted.push_back(std::move(it->second));
                    it = repositories_.erase(it);
                } else {
                    ++it;
                }
            }
            const auto found = workspace_stores_.find(workspace_id);
            if (found != workspace_stores_.end()) {
                store = std::move(found->second);
                workspace_stores_.erase(found);
            }
        }
        evicted.clear();
        if (store) {
            store->index_gate->close();
            static_cast<void>(liquid::runtime::block_on(store->store->close()));
        }
    }

    // Opens the workspace's store on first use (lake#380); the caller holds the workspace's lane,
    // so requests for this workspace wait here and others do not. `create` is set only while lake
    // creates the store, before the workspace is switched to it: then the bucket is created with
    // the space's quota and a data key may be created in it while it is empty. A switched
    // workspace's store must already exist with its key; anything else is refused, never served
    // as an empty store. The open acquires the bucket's lease, waiting (bounded) for a
    // predecessor to release it, as lake#479 waits for the root. `wait` replaces that bound (the
    // pre-open waits in short attempts). Once lake drains, no store is opened again (lake#536).
    SyncStoreResult<WorkspaceStore*> open_workspace(const std::string& customer_id,
                                                    const std::string& workspace_id, bool create,
                                                    std::optional<std::chrono::milliseconds> wait = {}) {
        WorkspaceStoreConfig config;
        bool reacquire = false;
        {
            std::lock_guard maps(mutex_);
            config = workspace_config_;
            reacquire = lost_workspaces_.contains(workspace_id);
        }
        if (!config.endpoint || !config.transport) {
            return std::unexpected(SyncStoreError{
                "store_unavailable", "workspace stores are not configured on this Lake"});
        }
        if (root_.empty()) {
            return std::unexpected(
                SyncStoreError{"store_unavailable", "LAKE_STORE_ROOT is not configured"});
        }
        WorkspaceStore* current = nullptr;
        {
            std::lock_guard maps(mutex_);
            const auto found = workspace_stores_.find(workspace_id);
            if (found != workspace_stores_.end()) {
                current = found->second.get();
            }
        }
        if (current != nullptr) {
            if (current->customer_id != customer_id) {
                return std::unexpected(SyncStoreError{
                    "invalid_input", "the workspace is not in the repository's account"});
            }
            if (current->usable()) {
                return current;
            }
            // A lost lease is tried again without waiting until the store is owned again, so the
            // workspace's requests answer 503 at once while another process holds it. A store a
            // write failed on releases its lease when it closes and is opened again at once.
            if (current->owner->lost()) {
                reacquire = true;
                std::lock_guard maps(mutex_);
                lost_workspaces_.insert(workspace_id);
            }
            discard_workspace(workspace_id);
        }
        // Counted before the drain check, so the drain's announcement (wait_for_opens) never
        // misses an open that passed the check: either this sees the drain, or the drain sees it.
        opening_.fetch_add(1);
        struct Opening {
            std::atomic<int>& count;
            ~Opening() {
                count.fetch_sub(1);
            }
        } opening{opening_};
        if (draining_.load()) {
            return std::unexpected(stopping_error());
        }
        const auto endpoint = config.endpoint(customer_id, workspace_id);
        if (!endpoint) {
            return std::unexpected(endpoint.error());
        }
        auto output = std::make_unique<WorkspaceStore>();
        output->customer_id = customer_id;
        output->client = std::make_shared<control::ZettaObjectClient>(
            config.transport, control::ObjectEndpoint{endpoint->url, endpoint->token},
            std::string(control::workspace_store_bucket), nullptr, config.observer);
        if (create) {
            const std::uint64_t quota = config.quota_bytes ? config.quota_bytes(customer_id) : 0;
            const auto bucket = liquid::runtime::block_on(output->client->create_bucket(quota));
            if (!bucket) {
                return std::unexpected(error_of(bucket.error()));
            }
        }
        output->owner = std::make_shared<control::ZettaStoreOwner>(output->client, config.lease_ttl);
        output->backend = std::make_shared<control::ZettaObjectBackend>(
            output->client, control::ZettaObjectBackendOptions{.workspace_id = workspace_id,
                                                               .instance_kek = config.instance_kek,
                                                               .create_key = create});
        liquid::store_options options;
        options.backend = output->backend;
        options.owner = output->owner;
        options.owner_wait = reacquire ? std::chrono::milliseconds(0) : wait.value_or(owner_wait_);
        auto store = liquid::runtime::block_on(liquid::store::open(
            std::filesystem::path(root_) / "workspaces" / workspace_id, std::move(options)));
        if (!store) {
            auto error = error_of(store.error());
            if (store.error().code == liquid::errc::not_found && !create) {
                // A switched workspace whose bucket is gone (a recreated Zetta, a wrong endpoint).
                error = SyncStoreError{"corrupt",
                                       "the workspace store is missing from its workspace Zetta: "
                                       "the workspace was switched to it, so Lake will not create "
                                       "an empty one: " +
                                           store.error().message,
                                       store.error().context};
            }
            return std::unexpected(error);
        }
        output->store = std::make_unique<liquid::store>(std::move(*store));
        // A new view starts unsynced (lake#535): what a predecessor sent may not be durable, and
        // liquid's open may have written. One :barrier before anything is served from the store;
        // a store whose :barrier fails is not served.
        if (auto durable = liquid::runtime::block_on(output->backend->make_durable()); !durable) {
            static_cast<void>(liquid::runtime::block_on(output->store->close()));
            return std::unexpected(error_of(durable.error()));
        }
        bool seeded = false;
        {
            std::lock_guard maps(mutex_);
            seeded = last_usage_.contains(workspace_id);
        }
        if (!seeded) {
            // The first open of a workspace store after a start seeds its usage for quota checks.
            if (const auto usage = liquid::runtime::block_on(output->client->usage(std::chrono::seconds(5)))) {
                std::lock_guard maps(mutex_);
                last_usage_.emplace(workspace_id, usage->bytes);
                usage_targets_[workspace_id] = customer_id;
            }
        }
        std::lock_guard maps(mutex_);
        lost_workspaces_.erase(workspace_id);
        auto inserted = workspace_stores_.insert_or_assign(workspace_id, std::move(output));
        return inserted.first->second.get();
    }

    // Opens the repository in the store `route` names; the caller holds that store's lane. A
    // space-store repository is created on first use, as before. A repository in a workspace store
    // is only opened here: it is created explicitly (create_repository_content, or the switch),
    // so a store that lost it answers corrupt instead of serving an empty repository.
    SyncStoreResult<RepositoryState*> open_repository(const std::string& customer_id,
                                                      const std::string& repository_id,
                                                      const std::optional<std::string>& route) {
        const std::string key = customer_id + "/" + repository_id;
        {
            std::lock_guard maps(mutex_);
            if (const auto existing = repositories_.find(key); existing != repositories_.end()) {
                const std::string workspace = existing->second->workspace;
                bool usable = workspace.empty();
                if (!usable) {
                    const auto store = workspace_stores_.find(workspace);
                    usable = store != workspace_stores_.end() && store->second->usable();
                }
                if (usable && workspace == route.value_or(std::string())) {
                    return existing->second.get();
                }
            }
        }
        if (route) {
            const std::string& workspace_id = *route;
            auto workspace = open_workspace(customer_id, workspace_id, false);
            if (!workspace) {
                return std::unexpected(workspace.error());
            }
            const std::filesystem::path identifier = repository_identifier(customer_id, repository_id);
            liquid::repository_options options;
            options.store = (*workspace)->store.get();
            auto opened = liquid::runtime::block_on(liquid::repository::open(identifier, options));
            if (!opened) {
                if (opened.error().code == liquid::errc::not_found) {
                    return std::unexpected(SyncStoreError{
                        "corrupt",
                        "repository " + repository_id +
                            " is missing from its workspace store; "
                            "the workspace was switched, so Lake will not create it empty",
                        key,
                        "missing_repository"});
                }
                return std::unexpected(error_of(opened.error()));
            }
            // Liquid's open may recover the repository's state (a journal replayed, a checkpoint
            // finished): durable before the repository serves anything, also when nothing else
            // syncs after it (the pre-open, lake#536).
            if (auto durable = liquid::runtime::block_on((*workspace)->backend->make_durable()); !durable) {
                return std::unexpected(error_of(durable.error()));
            }
            auto state = std::make_unique<RepositoryState>();
            state->subject = std::make_unique<liquid::repository>(std::move(*opened));
            state->objects = (*workspace)->store.get();
            state->path = identifier;
            state->workspace = workspace_id;
            std::lock_guard maps(mutex_);
            auto inserted = repositories_.insert_or_assign(key, std::move(state));
            return inserted.first->second.get();
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
        state->path = path;
        std::lock_guard maps(mutex_);
        auto inserted = repositories_.insert_or_assign(key, std::move(state));
        return inserted.first->second.get();
    }

    // The bytes a space stores: its store on lake's volume while any of its workspaces is not
    // switched yet, plus each switched workspace's bucket usage (docs/api/metering.md). A
    // workspace that is not serving, or whose Zetta does not answer in time, counts with its last
    // known usage, so it never refuses another workspace's write. `space_held`: the caller holds
    // the space's lane; otherwise it is taken to read the space store.
    SyncStoreResult<std::uint64_t> unique_stored_bytes_locked(const std::string& customer_id,
                                                              bool space_held) {
        WorkspaceStoreConfig config;
        {
            std::lock_guard maps(mutex_);
            config = workspace_config_;
        }
        std::vector<std::pair<std::string, bool>> switched;
        bool any_unswitched = true;
        if (config.switched) {
            auto listed = config.switched(customer_id);
            if (!listed) {
                return std::unexpected(listed.error());
            }
            switched = std::move(listed->first);
            any_unswitched = listed->second;
        }
        std::uint64_t total = 0;
        if (any_unswitched) {
            // Measured under the space's lane. A write in a switched workspace does not wait for
            // that lane (a switch of another workspace may hold it): it counts the space store's
            // last measurement instead.
            std::unique_lock<std::mutex> space;
            if (!space_held) {
                space = std::unique_lock<std::mutex>(lane_of("s:" + customer_id)->mutex, std::try_to_lock);
            }
            if (space_held || space.owns_lock()) {
                auto measured = space_stored_bytes_locked(customer_id);
                if (!measured) {
                    return std::unexpected(measured.error());
                }
                total = *measured;
                std::lock_guard maps(mutex_);
                last_space_bytes_[customer_id] = total;
            } else {
                std::lock_guard maps(mutex_);
                total = last_space_bytes_[customer_id];
            }
        }
        // Never a network call here: a write must not wait on another workspace's Zetta. Each
        // switched workspace counts with the usage the owner loop last read
        // (refresh_workspace_usage).
        std::lock_guard maps(mutex_);
        for (const auto& [workspace_id, serving] : switched) {
            usage_targets_[workspace_id] = customer_id;
            total += last_usage_[workspace_id];
        }
        return total;
    }

    std::string root_;
    std::chrono::milliseconds owner_wait_;
    bool replication_allow_private_ = false;
    std::chrono::milliseconds replication_timeout_{std::chrono::minutes(10)};
    // Guards the maps below and nothing else (see Lane).
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Lane>> lanes_;
    std::map<std::string, std::uint64_t> epochs_;
    std::set<std::string> lost_workspaces_;
    std::map<std::string, std::uint64_t> last_usage_;
    // Bytes search indexes wrote since each workspace's usage was last read (lake#381).
    std::map<std::string, std::uint64_t> search_spent_;
    std::map<std::string, std::uint64_t> last_space_bytes_;
    // Switched workspaces whose usage a write has counted: workspace -> its space.
    std::map<std::string, std::string> usage_targets_;
    WorkspaceStoreConfig workspace_config_;
    std::map<std::string, std::unique_ptr<liquid::store>> stores_;
    std::map<std::string, std::unique_ptr<WorkspaceStore>> workspace_stores_;
    std::map<std::string, std::unique_ptr<RepositoryState>> repositories_;
    std::map<std::string, std::optional<std::string>> routes_;

    // Whether the space's store holds the repository: decided from the source alone, so a
    // not_found from the workspace store is never taken for "nothing to copy". The caller holds
    // the space's lane.
    SyncStoreResult<bool> held_by_space(const std::string& customer_id, const std::string& repository_id) {
        // Read without opening the repository, which would write lease records into the source:
        // the space store keeps a repository's entry point at repositories/<blake3 of its
        // identifier>/LIQUID_REPO (liquid#326) on lake's volume.
        const std::string identifier = repository_identifier(customer_id, repository_id).generic_string();
        const auto id = liquid::hash(
            liquid::algorithm::blake3,
            std::span(reinterpret_cast<const std::byte*>(identifier.data()), identifier.size()));
        if (!id) {
            return std::unexpected(error_of(id.error()));
        }
        const std::string encoded = liquid::to_string(*id);
        const std::filesystem::path marker = std::filesystem::path(root_) / customer_id / "store" /
                                             "repositories" /
                                             encoded.substr(encoded.find(':') + 1) / "LIQUID_REPO";
        std::error_code error;
        const bool present = std::filesystem::exists(marker, error);
        if (error) {
            return std::unexpected(SyncStoreError{"io", "could not inspect the account store: " + error.message()});
        }
        // Lake makes a repository's directory beside the space store when it first opens it, and
        // liquid writes LIQUID_REPO at create, so a directory without the entry point means the
        // space store lost the repository: the move refuses rather than create it empty.
        const bool opened_before =
            std::filesystem::exists(repository_identifier(customer_id, repository_id), error);
        if (error) {
            return std::unexpected(SyncStoreError{"io", "could not inspect the account store: " + error.message()});
        }
        if (!present && opened_before) {
            return std::unexpected(SyncStoreError{
                "corrupt", "repository " + repository_id +
                               " was opened in the account store but its entry point is missing there; "
                               "nothing is copied or created for it"});
        }
        return present;
    }

    // Copies (or compares) one repository; the caller holds the workspace's lane and then the
    // space's lane. A repository the space store never held is `absent`, decided from the source.
    SyncStoreResult<WorkspaceStoreTransfer> transfer_locked(const std::string& customer_id,
                                                            const std::string& repository_id,
                                                            const std::string& workspace_id,
                                                            bool verify,
                                                            bool create) {
        if (!path_component_is_safe(customer_id) || !path_component_is_safe(repository_id)) {
            return std::unexpected(SyncStoreError{"invalid_input", "invalid repository location"});
        }
        WorkspaceStoreTransfer output;
        output.repository_id = repository_id;
        auto held = held_by_space(customer_id, repository_id);
        if (!held) {
            return std::unexpected(held.error());
        }
        if (!*held) {
            output.absent = true;
            output.complete = true;
            return output;
        }
        auto source = open_customer(customer_id);
        if (!source) {
            return std::unexpected(source.error());
        }
        auto destination = open_workspace(customer_id, workspace_id, create);
        if (!destination) {
            return std::unexpected(destination.error());
        }
        liquid::repository_transfer_options options;
        options.mode = verify ? liquid::repository_transfer_mode::verify
                              : liquid::repository_transfer_mode::copy;
        auto report = liquid::runtime::block_on(liquid::repository::transfer(
            **source, *(*destination)->store, repository_identifier(customer_id, repository_id),
            options));
        if (!report) {
            return std::unexpected(error_of(report.error()));
        }
        // What the copy wrote is durable before the switch is recorded (lake#535).
        if (auto durable = StoreDurability((*destination)->backend).now(); !durable) {
            return std::unexpected(durable.error());
        }
        output.complete = report->complete;
        output.objects = report->objects;
        output.objects_present = report->objects_present;
        output.objects_written = report->objects_written;
        output.bytes_written = report->bytes_written;
        output.state_written = report->state_written;
        output.leases_written = report->leases_written;
        for (const liquid::digest& digest : report->missing) {
            output.missing.push_back(liquid::to_string(digest));
        }
        for (const liquid::repository_transfer_difference& difference : report->differences) {
            output.differences.emplace_back(difference.subject, difference.reason);
        }
        return output;
    }

    // Creates an empty repository in a workspace store; the caller holds the workspace's lane.
    SyncStoreResult<void> create_in_workspace(const std::string& customer_id,
                                              const std::string& repository_id,
                                              const std::string& workspace_id,
                                              bool create_store) {
        auto workspace = open_workspace(customer_id, workspace_id, create_store);
        if (!workspace) {
            return std::unexpected(workspace.error());
        }
        liquid::repository_options options;
        options.store = (*workspace)->store.get();
        auto created = liquid::runtime::block_on(liquid::repository::create(
            repository_identifier(customer_id, repository_id), options));
        if (!created && created.error().code != liquid::errc::already_exists) {
            return std::unexpected(error_of(created.error()));
        }
        // Durable before the creation or the switch is answered (lake#535). The repository's row
        // was written before this (create_repository_content): a crash between the two leaves a
        // row whose repository is missing, which is answered `missing_repository` and tombstoned
        // by the next creation of its name (create_repository_row).
        return StoreDurability((*workspace)->backend).now();
    }

public:
    // Registers a space's switched workspaces for refresh_workspace_usage; at start-up the server
    // calls it for every space, then refreshes, so quota checks never count them as 0.
    void track_space_usage(const std::string& customer_id) {
        WorkspaceStoreConfig config;
        {
            std::lock_guard maps(mutex_);
            config = workspace_config_;
        }
        if (!config.switched) {
            return;
        }
        const auto listed = config.switched(customer_id);
        if (!listed) {
            return;
        }
        std::lock_guard maps(mutex_);
        for (const auto& [workspace_id, serving] : listed->first) {
            usage_targets_[workspace_id] = customer_id;
        }
    }

    // Reads each switched workspace's bucket usage for the space quota (docs/api/metering.md). The
    // owner loop calls it, off every request path; a workspace that does not answer within 5 s,
    // or is not serving, keeps its last known usage.
    void refresh_workspace_usage() {
        WorkspaceStoreConfig config;
        std::map<std::string, std::string> targets;
        {
            std::lock_guard maps(mutex_);
            config = workspace_config_;
            targets = usage_targets_;
        }
        for (const auto& [workspace_id, customer_id] : targets) {
            std::shared_ptr<control::ZettaObjectClient> client;
            {
                std::lock_guard maps(mutex_);
                if (const auto open = workspace_stores_.find(workspace_id); open != workspace_stores_.end()) {
                    client = open->second->client;
                }
            }
            if (!client && config.endpoint && config.transport) {
                const auto endpoint = config.endpoint(customer_id, workspace_id);
                if (!endpoint) {
                    continue;
                }
                client = std::make_shared<control::ZettaObjectClient>(
                    config.transport, control::ObjectEndpoint{endpoint->url, endpoint->token});
            }
            if (!client) {
                continue;
            }
            const auto usage = liquid::runtime::block_on(client->usage(std::chrono::seconds(5)));
            if (usage) {
                std::lock_guard maps(mutex_);
                last_usage_[workspace_id] = usage->bytes;
                // The reading includes what search wrote before it.
                search_spent_.erase(workspace_id);
            }
        }
    }

public:
    SyncStoreResult<bool> repository_missing_from_workspace_store(const std::string& customer_id,
                                                                  const std::string& repository_id) override {
        const auto route = workspace_route(customer_id, repository_id);
        if (!route) {
            return std::unexpected(route.error());
        }
        if (!*route) {
            return false;
        }
        auto entered = enter_repository(customer_id, repository_id);
        if (entered.state) {
            return false;
        }
        if (entered.state.error().rule == "missing_repository") {
            return true;
        }
        return std::unexpected(entered.state.error());
    }

    // The workspace whose own store serves the repository, or nothing for its space's store.
    SyncStoreResult<std::optional<std::string>> route_of(const std::string& customer_id,
                                                         const std::string& repository_id) {
        return workspace_route(customer_id, repository_id);
    }

    // What search borrows: the repository, and for one served by a workspace store, its index
    // objects and the store's fence (lake#381). `hold` keeps the store's lane.
    struct LentRepository {
        std::shared_ptr<void> hold;
        liquid::repository* repository = nullptr;
        std::string workspace;
        std::shared_ptr<search::IndexObjects> search_objects;
        std::uint64_t fence = 0;
    };

    // Lends search the repository from the store that serves it, under that store's lane and on
    // the one open instance (lake#380).
    SyncStoreResult<LentRepository> lend_repository(const std::string& customer_id,
                                                    const std::string& repository_id,
                                                    bool lake_work = false) {
        auto entered =
            std::make_shared<Entered>(enter_repository(customer_id, repository_id, lake_work));
        if (!entered->state) {
            return std::unexpected(entered->state.error());
        }
        LentRepository output;
        output.repository = (*entered->state)->subject.get();
        output.workspace = (*entered->state)->workspace;
        if (!output.workspace.empty()) {
            WorkspaceStore* store = nullptr;
            {
                std::lock_guard maps(mutex_);
                const auto found = workspace_stores_.find(output.workspace);
                if (found != workspace_stores_.end() && found->second->usable()) {
                    store = found->second.get();
                }
            }
            if (store == nullptr) {
                return std::unexpected(SyncStoreError{
                    "store_unavailable", "the workspace store closed while search borrowed it; retry"});
            }
            auto objects = search_objects_of(*store, output.workspace);
            if (!objects) {
                return std::unexpected(objects.error());
            }
            output.search_objects = *objects;
            output.fence = store->owner->fence();
        }
        output.hold = std::move(entered);
        return output;
    }

    struct LentWorkspace {
        std::shared_ptr<void> hold;
        std::string customer_id;
        std::shared_ptr<search::IndexObjects> search_objects;
        std::uint64_t fence = 0;
    };

    // Lends search a workspace's index objects under the workspace store's lane, opening the
    // store on first use (lake#381). With `only_if_open` (a workspace deletion) nothing is opened:
    // a store lake does not have open lends nothing.
    SyncStoreResult<std::optional<LentWorkspace>> lend_workspace(const std::string& customer_id,
                                                                 const std::string& workspace_id,
                                                                 bool only_if_open,
                                                                 bool lake_work = false) {
        if (!path_component_is_safe(workspace_id) ||
            (!customer_id.empty() && !path_component_is_safe(customer_id))) {
            return std::unexpected(SyncStoreError{"invalid_input", "invalid workspace location"});
        }
        LaneLock lane;
        if (lake_work && draining_.load()) {
            return std::unexpected(stopping_error());
        }
        if (only_if_open || lake_work) {
            lane = hold("w:" + workspace_id);
        } else {
            auto admitted = admit("w:" + workspace_id);
            if (!admitted) {
                return std::unexpected(admitted.error());
            }
            lane = std::move(*admitted);
        }
        WorkspaceStore* store = nullptr;
        if (only_if_open) {
            std::lock_guard maps(mutex_);
            const auto found = workspace_stores_.find(workspace_id);
            if (found == workspace_stores_.end() || !found->second->usable()) {
                return std::optional<LentWorkspace>{};
            }
            store = found->second.get();
        } else {
            auto opened = open_workspace(customer_id, workspace_id, false);
            if (!opened) {
                return std::unexpected(opened.error());
            }
            store = *opened;
        }
        LentWorkspace output;
        output.customer_id = store->customer_id;
        auto objects = search_objects_of(*store, workspace_id);
        if (!objects) {
            return std::unexpected(objects.error());
        }
        output.search_objects = *objects;
        output.fence = store->owner->fence();
        output.hold = std::make_shared<LaneLock>(std::move(lane));
        return std::optional<LentWorkspace>{std::move(output)};
    }

    // How many more bytes a workspace's search index may add (lake#381): the space's quota less
    // the space's stored bytes as the push quota check counts them (its store on lake's volume while
    // any workspace is not switched, plus every switched workspace's last bucket usage), less what
    // search wrote since those readings, less a tenth of the quota kept for liquid's own writes. A
    // usage not read yet counts as no room. Nothing when there is no quota. Never a network call.
    std::optional<std::uint64_t> search_index_budget(const std::string& customer_id,
                                                     const std::string&) {
        WorkspaceStoreConfig config;
        {
            std::lock_guard maps(mutex_);
            config = workspace_config_;
        }
        const std::uint64_t quota = config.quota_bytes ? config.quota_bytes(customer_id) : 0;
        if (quota == 0) {
            return std::nullopt;
        }
        if (!config.switched) {
            return 0;
        }
        const auto listed = config.switched(customer_id);
        if (!listed) {
            return 0;
        }
        std::uint64_t total = 0;
        std::lock_guard maps(mutex_);
        if (listed->second) {
            const auto space = last_space_bytes_.find(customer_id);
            if (space == last_space_bytes_.end()) {
                return 0;
            }
            total += space->second;
        }
        for (const auto& [workspace_id, serving] : listed->first) {
            const auto usage = last_usage_.find(workspace_id);
            if (usage == last_usage_.end()) {
                return 0;
            }
            total += usage->second;
            if (const auto spent = search_spent_.find(workspace_id); spent != search_spent_.end()) {
                total += spent->second;
            }
        }
        const std::uint64_t reserved = total + quota / 10;
        return quota > reserved ? quota - reserved : 0;
    }

    // Bytes a workspace's search index wrote since its bucket usage was last read.
    void search_index_spent(const std::string& workspace_id, std::uint64_t bytes) {
        std::lock_guard maps(mutex_);
        search_spent_[workspace_id] += bytes;
    }

    SyncStoreResult<WorkspaceStoreTransfer>
    transfer_to_workspace_store(const std::string& customer_id,
                                const std::string& repository_id,
                                const std::string& workspace_id,
                                bool verify) override {
        if (!path_component_is_safe(customer_id) || !path_component_is_safe(workspace_id)) {
            return std::unexpected(SyncStoreError{"invalid_input", "invalid workspace location"});
        }
        // The operator's copy and verify are lake work: refused once lake drains (lake#536).
        if (draining_.load()) {
            return std::unexpected(stopping_error());
        }
        // Copy and verify mark only the workspace's lane as moving. The space's lane is taken for
        // this one repository and not marked, so the space's other workspaces wait at most one
        // repository's copy instead of being refused; only the switch holds it throughout.
        Moving moving_workspace(lane_of("w:" + workspace_id));
        auto workspace_lane = hold("w:" + workspace_id);
        auto space_lane = hold("s:" + customer_id);
        // Copying creates the store; verifying only opens it.
        return transfer_locked(customer_id, repository_id, workspace_id, verify, !verify);
    }

    SyncStoreResult<std::vector<WorkspaceStoreTransfer>>
    switch_workspace_store(const std::string& customer_id,
                           const std::string& workspace_id,
                           const std::function<SyncStoreResult<std::vector<std::string>>()>& list,
                           const std::function<SyncStoreResult<void>()>& record) override {
        // Every request for this workspace's store, every request for the space's store (which
        // serves the workspace's repositories until the switch) and every repository created in
        // the workspace (create_repository_content) waits until the switch is recorded. The lanes
        // are held throughout and never given up.
        if (!path_component_is_safe(customer_id) || !path_component_is_safe(workspace_id)) {
            return std::unexpected(SyncStoreError{"invalid_input", "invalid workspace location"});
        }
        if (draining_.load()) {
            return std::unexpected(stopping_error());
        }
        Moving moving_workspace(lane_of("w:" + workspace_id));
        Moving moving_space(lane_of("s:" + customer_id));
        auto workspace_lane = hold("w:" + workspace_id);
        auto space_lane = hold("s:" + customer_id);
        auto listed = list();
        if (!listed) {
            return std::unexpected(listed.error());
        }
        std::vector<WorkspaceStoreTransfer> output;
        for (const std::string& repository_id : *listed) {
            auto copied = transfer_locked(customer_id, repository_id, workspace_id, false, true);
            if (!copied) {
                return std::unexpected(copied.error());
            }
            auto verified = transfer_locked(customer_id, repository_id, workspace_id, true, false);
            if (!verified) {
                return std::unexpected(verified.error());
            }
            if (!verified->complete) {
                return std::unexpected(SyncStoreError{
                    "conflict", "repository " + repository_id +
                                    " does not verify in the workspace store; nothing was switched"});
            }
            verified->objects_written = copied->objects_written;
            verified->bytes_written = copied->bytes_written;
            verified->state_written = copied->state_written;
            verified->leases_written = copied->leases_written;
            output.push_back(std::move(*verified));
        }
        // A repository the space store never held is created empty in the workspace store, so
        // every repository of the workspace exists there once it is switched.
        for (const WorkspaceStoreTransfer& transfer : output) {
            if (transfer.absent) {
                if (auto created = create_in_workspace(customer_id, transfer.repository_id,
                                                       workspace_id, true);
                    !created) {
                    return std::unexpected(created.error());
                }
            }
        }
        // A workspace with no repository still gets its store (and data key) before it is
        // switched: a switched workspace's store is never created later, and its tenant image
        // blobs live there (lake#383 phase B).
        if (listed->empty()) {
            if (auto created = open_workspace(customer_id, workspace_id, true); !created) {
                return std::unexpected(created.error());
            }
        }
        if (auto recorded = record(); !recorded) {
            return std::unexpected(recorded.error());
        }
        // Every route of the space is recomputed and its space-store repositories reopened, so no
        // request is answered from a route chosen before the switch.
        std::vector<std::unique_ptr<RepositoryState>> evicted;
        {
            std::lock_guard maps(mutex_);
            ++epochs_[customer_id];
            const std::string prefix = customer_id + "/";
            for (auto it = routes_.lower_bound(prefix);
                 it != routes_.end() && it->first.starts_with(prefix);) {
                it = routes_.erase(it);
            }
            for (auto it = repositories_.lower_bound(prefix);
                 it != repositories_.end() && it->first.starts_with(prefix);) {
                if (it->second->workspace.empty()) {
                    evicted.push_back(std::move(it->second));
                    it = repositories_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        return output;
    }

    SyncStoreResult<void> create_repository_content(const std::string& customer_id,
                                                    const std::string& workspace_id,
                                                    const std::string& repository_id,
                                                    const std::function<SyncStoreResult<void>()>& create_row) override {
        // Under the workspace's lane: a switch either finished before (and the repository is
        // created in the workspace store here) or starts after (and finds the row in its list).
        auto workspace_lane = admit("w:" + workspace_id);
        if (!workspace_lane) {
            return std::unexpected(workspace_lane.error());
        }
        if (auto created = create_row(); !created) {
            return std::unexpected(created.error());
        }
        auto route = workspace_route(customer_id, repository_id);
        if (!route) {
            return std::unexpected(route.error());
        }
        if (!*route) {
            return {};
        }
        return create_in_workspace(customer_id, repository_id, **route, false);
    }

    SyncStoreResult<SpaceStoreRemoval>
    remove_space_store(const std::string& customer_id, bool dry_run,
                       const std::function<SyncStoreResult<void>()>& check) override try {
        // Only <root>/<space>/store and <root>/<space>/repositories, never the instance's own
        // directory (<root>/<node name>), <root>/workspaces or anything above the space.
        if (!path_component_is_safe(customer_id) || customer_id == "workspaces") {
            return std::unexpected(SyncStoreError{"invalid_input", "invalid account store location"});
        }
        if (root_.empty()) {
            return std::unexpected(
                SyncStoreError{"store_unavailable", "LAKE_STORE_ROOT is not configured"});
        }
        // A real removal refuses new requests for the space's store (503 with Retry-After) and
        // waits for the admitted ones to finish, so nothing uses the store when it is closed.
        std::optional<Moving> moving;
        LaneLock space_lane;
        if (!dry_run) {
            moving.emplace(lane_of("s:" + customer_id));
            space_lane = hold("s:" + customer_id);
        }
        if (auto checked = check(); !checked) {
            return std::unexpected(checked.error());
        }
        const std::filesystem::path space = std::filesystem::path(root_) / customer_id;
        {
            // The space's own directory must be a directory and not a link: a link would take the
            // removal outside the store root.
            std::error_code error;
            const auto status = std::filesystem::symlink_status(space, error);
            if (error && error != std::errc::no_such_file_or_directory) {
                return std::unexpected(SyncStoreError{
                    "io", "could not inspect " + space.string() + ": " + error.message()});
            }
            if (std::filesystem::exists(status) && (std::filesystem::is_symlink(status) ||
                                                    !std::filesystem::is_directory(status))) {
                return std::unexpected(SyncStoreError{
                    "conflict", space.string() + " is not a directory; nothing was removed"});
            }
        }
        SpaceStoreRemoval output;
        for (const char* name : {"store", "repositories"}) {
            SpaceStoreDirectory directory;
            const std::filesystem::path path = space / name;
            directory.path = path.string();
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            if (error && error != std::errc::no_such_file_or_directory) {
                return std::unexpected(SyncStoreError{"io", "could not inspect " + directory.path + ": " +
                                                                error.message()});
            }
            if (std::filesystem::exists(status)) {
                // A link would take the removal outside the space's directory.
                if (!std::filesystem::is_directory(status) || std::filesystem::is_symlink(status)) {
                    return std::unexpected(SyncStoreError{
                        "conflict", directory.path + " is not a directory; nothing was removed"});
                }
                directory.exists = true;
                // A dry run holds nothing, so files may come and go while they are counted (an
                // unswitched workspace's request writing the store): an entry that vanishes or
                // cannot be read is skipped, and the count is what was there at that moment.
                std::error_code walk;
                auto it = std::filesystem::recursive_directory_iterator(
                    path, std::filesystem::directory_options::skip_permission_denied, walk);
                for (; !walk && it != std::filesystem::recursive_directory_iterator();
                     it.increment(walk)) {
                    std::error_code entry_error;
                    const auto entry = it->symlink_status(entry_error);
                    if (entry_error || !std::filesystem::is_regular_file(entry)) {
                        continue;
                    }
                    const auto size = it->file_size(entry_error);
                    if (entry_error) {
                        continue;
                    }
                    directory.bytes += size;
                    ++directory.files;
                }
                if (walk && walk != std::errc::no_such_file_or_directory) {
                    if (!dry_run) {
                        return std::unexpected(SyncStoreError{
                            "io", "could not measure " + directory.path + ": " + walk.message()});
                    }
                }
            }
            output.bytes += directory.bytes;
            output.directories.push_back(std::move(directory));
        }
        if (dry_run) {
            return output;
        }
        // Close the store and forget every space-store repository and route of the space; the
        // next request for one opens (and liquid creates) the space store afresh.
        std::unique_ptr<liquid::store> store;
        std::vector<std::unique_ptr<RepositoryState>> evicted;
        {
            std::lock_guard maps(mutex_);
            ++epochs_[customer_id];
            const std::string prefix = customer_id + "/";
            for (auto it = routes_.lower_bound(prefix);
                 it != routes_.end() && it->first.starts_with(prefix);) {
                it = routes_.erase(it);
            }
            for (auto it = repositories_.lower_bound(prefix);
                 it != repositories_.end() && it->first.starts_with(prefix);) {
                if (it->second->workspace.empty()) {
                    evicted.push_back(std::move(it->second));
                    it = repositories_.erase(it);
                } else {
                    ++it;
                }
            }
            if (const auto found = stores_.find(customer_id); found != stores_.end()) {
                store = std::move(found->second);
                stores_.erase(found);
            }
            last_space_bytes_.erase(customer_id);
        }
        evicted.clear();
        if (store) {
            const auto closed = liquid::runtime::block_on(store->close());
            if (!closed) {
                return std::unexpected(error_of(closed.error()));
            }
            store.reset();
        }
        for (const SpaceStoreDirectory& directory : output.directories) {
            if (!directory.exists) {
                continue;
            }
            std::error_code error;
            std::filesystem::remove_all(directory.path, error);
            if (error) {
                return std::unexpected(SyncStoreError{"io", "could not remove " + directory.path +
                                                                ": " + error.message()});
            }
        }
        output.removed = true;
        return output;
    } catch (const std::filesystem::filesystem_error& error) {
        return std::unexpected(SyncStoreError{"io", error.what()});
    }

    SyncStoreResult<void> close_workspace_store(const std::string& workspace_id) override {
        // The lane waits for an open in progress (one waiting for the bucket lease included), so
        // a deletion that races an open still releases the lease the open took.
        auto workspace_lane = hold("w:" + workspace_id);
        auto closed = close_workspace_store_locked(workspace_id);
        if (!closed) {
            return std::unexpected(closed.error());
        }
        return {};
    }

    // Closes the workspace's store, settling it and releasing its lease (unless the lease was
    // lost), after evicting its repositories; the caller holds the workspace's lane. True when a
    // store was open.
    SyncStoreResult<bool> close_workspace_store_locked(const std::string& workspace_id) {
        std::unique_ptr<WorkspaceStore> store;
        std::vector<std::unique_ptr<RepositoryState>> evicted;
        {
            std::lock_guard maps(mutex_);
            for (auto it = repositories_.begin(); it != repositories_.end();) {
                if (it->second->workspace == workspace_id) {
                    evicted.push_back(std::move(it->second));
                    it = repositories_.erase(it);
                } else {
                    ++it;
                }
            }
            const auto found = workspace_stores_.find(workspace_id);
            if (found == workspace_stores_.end()) {
                return false;
            }
            store = std::move(found->second);
            workspace_stores_.erase(found);
        }
        store->index_gate->close();
        evicted.clear();
        const auto closed = liquid::runtime::block_on(store->store->close());
        if (!closed) {
            return std::unexpected(error_of(closed.error()));
        }
        return true;
    }

    SyncStoreResult<WorkspaceStoreProvision>
    provision_workspace_store(const std::string& customer_id,
                              const std::string& workspace_id) override {
        if (!path_component_is_safe(customer_id) || !path_component_is_safe(workspace_id)) {
            return std::unexpected(SyncStoreError{"invalid_input", "invalid workspace location"});
        }
        WorkspaceStoreConfig config;
        {
            std::lock_guard maps(mutex_);
            config = workspace_config_;
        }
        if (!config.endpoint || !config.transport) {
            return std::unexpected(SyncStoreError{
                "store_unavailable", "workspace stores are not configured on this Lake"});
        }
        // Under the workspace's lane: no request opens the store meanwhile.
        auto lane = hold("w:" + workspace_id);
        const auto endpoint = config.endpoint(customer_id, workspace_id);
        if (!endpoint) {
            return std::unexpected(endpoint.error());
        }
        control::ZettaObjectClient client(config.transport,
                                          control::ObjectEndpoint{endpoint->url, endpoint->token});
        std::uint64_t objects = 0;
        if (auto listed = liquid::runtime::block_on(client.list("")); listed) {
            objects = listed->size();
        } else if (listed.error().code != liquid::errc::not_found) {
            return std::unexpected(error_of(listed.error()));
        }
        if (objects != 0) {
            // A store this lake already provisioned (or that opens with its key) is left as it is.
            if (auto opened = open_workspace(customer_id, workspace_id, false); opened) {
                return WorkspaceStoreProvision{false, objects};
            }
            return std::unexpected(SyncStoreError{
                "conflict",
                "the workspace's bucket lake_store holds " + std::to_string(objects) +
                    " objects but no store that opens; Lake will not create a store over them"});
        }
        auto created = open_workspace(customer_id, workspace_id, true);
        if (!created) {
            return std::unexpected(created.error());
        }
        return WorkspaceStoreProvision{true, 0};
    }

    SyncStoreResult<WorkspaceBlob> put_workspace_blob(const std::string& customer_id,
                                                      const std::string& workspace_id,
                                                      const std::filesystem::path& source,
                                                      bool admitted,
                                                      const std::string& known_object,
                                                      bool known_rooted) override {
        if (!path_component_is_safe(customer_id) || !path_component_is_safe(workspace_id)) {
            return std::unexpected(SyncStoreError{"invalid_input", "invalid workspace location"});
        }
        std::error_code filesystem_error;
        const std::uintmax_t size = std::filesystem::file_size(source, filesystem_error);
        if (filesystem_error) {
            return std::unexpected(SyncStoreError{
                "io", "could not inspect the staged blob: " + filesystem_error.message()});
        }
        auto lane = enter_workspace_lane(workspace_id, admitted);
        if (!lane) {
            return std::unexpected(lane.error());
        }
        auto workspace = open_workspace(customer_id, workspace_id, false);
        if (!workspace) {
            return std::unexpected(workspace.error());
        }
        liquid::store& store = *(*workspace)->store;
        bool known = false;
        if (const auto id = liquid::parse_digest(known_object); !known_object.empty() && id) {
            const auto present = liquid::runtime::block_on(store.contains(*id));
            if (!present) {
                return std::unexpected(error_of(present.error()));
            }
            known = *present;
        }
        std::function<std::uint64_t(const std::string&)> quota_of;
        {
            std::lock_guard maps(mutex_);
            quota_of = workspace_config_.quota_bytes;
        }
        // A re-push of an object the store holds adds nothing, so only new bytes meet the quota.
        if (const std::uint64_t quota = quota_of ? quota_of(customer_id) : 0; quota != 0 && !known) {
            auto stored = unique_stored_bytes_locked(customer_id, false);
            if (!stored) {
                return std::unexpected(stored.error());
            }
            if (exceeds_quota(*stored, size, quota)) {
                return std::unexpected(quota_error(*stored, quota));
            }
        }
        // liquid reads the file in windows as it chunks it, so a layer is never held whole.
        auto id = liquid::runtime::block_on(store.put_file(source));
        if (!id) {
            return std::unexpected(error_of(id.error()));
        }
        const std::string object = liquid::to_string(*id);
        // Only its lake_oci_blobs record names an image blob; nothing in liquid refers to it. A
        // lease (a liquid root) for a century keeps any liquid collection from removing it. One
        // per object and record: a re-push of an object its record names as rooted adds none.
        if (!(known && known_rooted && object == known_object)) {
            auto rooted = liquid::runtime::block_on(store.lease_root(*id, image_blob_root_ttl));
            if (!rooted) {
                return std::unexpected(error_of(rooted.error()));
            }
        }
        // Durable before the blob's record names it (lake#535).
        if (auto durable = StoreDurability((*workspace)->backend).now(); !durable) {
            return std::unexpected(durable.error());
        }
        return WorkspaceBlob{object, static_cast<std::uint64_t>(size), true};
    }

    SyncStoreResult<void> root_workspace_blob(const std::string& customer_id,
                                              const std::string& workspace_id,
                                              const std::string& object) override {
        if (!path_component_is_safe(customer_id) || !path_component_is_safe(workspace_id)) {
            return std::unexpected(SyncStoreError{"invalid_input", "invalid workspace location"});
        }
        const auto id = liquid::parse_digest(object);
        if (!id) {
            return std::unexpected(SyncStoreError{"corrupt", "not a store object", object});
        }
        // A request's blob operation, though it holds the lane: once lake drains it is refused
        // like an admission, so it never waits on, or reopens, a store being handed off.
        if (draining_.load()) {
            return std::unexpected(stopping_error());
        }
        auto lane = hold("w:" + workspace_id);
        auto workspace = open_workspace(customer_id, workspace_id, false);
        if (!workspace) {
            return std::unexpected(workspace.error());
        }
        auto rooted = liquid::runtime::block_on(
            (*workspace)->store->lease_root(*id, image_blob_root_ttl));
        if (!rooted) {
            return std::unexpected(error_of(rooted.error()));
        }
        return StoreDurability((*workspace)->backend).now();
    }

    static constexpr std::chrono::seconds image_blob_root_ttl{std::chrono::years(100)};

    SyncStoreResult<std::shared_ptr<void>> admit_workspace_stream(const std::string& workspace_id) override {
        if (!path_component_is_safe(workspace_id)) {
            return std::unexpected(SyncStoreError{"invalid_input", "invalid workspace location"});
        }
        if (draining_.load()) {
            return std::unexpected(stopping_error());
        }
        auto lane = lane_of("w:" + workspace_id);
        if (lane->moving.load() != 0) {
            return std::unexpected(SyncStoreError{
                "store_unavailable", "the store is being moved to its workspace; retry shortly"});
        }
        if (lane->streams.fetch_add(1) >= lane_stream_limit) {
            lane->streams.fetch_sub(1);
            return std::unexpected(SyncStoreError{
                "store_unavailable", "the workspace has too many image pulls in progress; retry shortly"});
        }
        return std::shared_ptr<void>(nullptr, [lane](void*) { lane->streams.fetch_sub(1); });
    }

    SyncStoreResult<bool> workspace_blob_present(const std::string& customer_id,
                                                 const std::string& workspace_id,
                                                 const std::string& object) override {
        if (!path_component_is_safe(customer_id) || !path_component_is_safe(workspace_id)) {
            return std::unexpected(SyncStoreError{"invalid_input", "invalid workspace location"});
        }
        const auto id = liquid::parse_digest(object);
        if (!id) {
            return false;
        }
        // A request's blob operation, though it holds the lane: once lake drains it is refused
        // like an admission, so it never waits on, or reopens, a store being handed off.
        if (draining_.load()) {
            return std::unexpected(stopping_error());
        }
        auto lane = hold("w:" + workspace_id);
        auto workspace = open_workspace(customer_id, workspace_id, false);
        if (!workspace) {
            return std::unexpected(workspace.error());
        }
        return from_liquid(liquid::runtime::block_on((*workspace)->store->contains(*id)));
    }

    SyncStoreResult<std::uint64_t> read_workspace_blob(const std::string& customer_id,
                                                       const std::string& workspace_id,
                                                       const std::string& object,
                                                       std::uint64_t offset,
                                                       std::span<std::byte> into,
                                                       bool admitted) override {
        if (!path_component_is_safe(customer_id) || !path_component_is_safe(workspace_id)) {
            return std::unexpected(SyncStoreError{"invalid_input", "invalid workspace location"});
        }
        const auto id = liquid::parse_digest(object);
        if (!id) {
            return std::unexpected(
                SyncStoreError{"corrupt", "the image blob record names no valid store object", object});
        }
        auto lane = enter_workspace_lane(workspace_id, admitted);
        if (!lane) {
            return std::unexpected(lane.error());
        }
        auto workspace = open_workspace(customer_id, workspace_id, false);
        if (!workspace) {
            return std::unexpected(workspace.error());
        }
        return from_liquid(liquid::runtime::block_on((*workspace)->store->read(*id, offset, into)));
    }

    // Destroys every open repository, closes every workspace store (releasing its bucket lease),
    // then every space store, which releases their liquid owner locks. The server calls it after
    // the last request has finished; each store is closed under its own lane.
    void close() {
        std::vector<std::string> workspaces;
        std::vector<std::string> spaces;
        {
            std::lock_guard maps(mutex_);
            for (const auto& [workspace_id, store] : workspace_stores_) {
                workspaces.push_back(workspace_id);
            }
            for (const auto& [customer_id, store] : stores_) {
                spaces.push_back(customer_id);
            }
        }
        for (const std::string& workspace_id : workspaces) {
            static_cast<void>(close_workspace_store(workspace_id));
        }
        for (const std::string& customer_id : spaces) {
            auto space_lane = hold("s:" + customer_id);
            std::unique_ptr<liquid::store> store;
            std::vector<std::unique_ptr<RepositoryState>> evicted;
            {
                std::lock_guard maps(mutex_);
                const std::string prefix = customer_id + "/";
                for (auto it = repositories_.lower_bound(prefix);
                     it != repositories_.end() && it->first.starts_with(prefix);) {
                    evicted.push_back(std::move(it->second));
                    it = repositories_.erase(it);
                }
                const auto found = stores_.find(customer_id);
                if (found != stores_.end()) {
                    store = std::move(found->second);
                    stores_.erase(found);
                }
            }
        }
    }

    // ---- The rollout hand-off of workspace stores (lake#536) ----------------------------------

    // The drain has begun: a workspace's lane admits no new request, no image pull starts, and
    // no workspace store is opened again, so a lease once released stays released.
    void begin_drain() {
        draining_.store(true);
    }
    // A new server's store starts undrained (a server object is started once, but say so).
    void reset_drain() {
        draining_.store(false);
    }

    // Waits until no open_workspace is in progress, or `deadline`. After begin_drain no new one
    // starts, so once this answers true no store of this process takes a lease again. True when
    // none is in progress.
    bool wait_for_opens(std::chrono::steady_clock::time_point deadline) {
        while (opening_.load() != 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }

    // Runs `job(0..count-1)` on at most `width` threads at once: bounded however many workspaces
    // an instance has, since each liquid block_on starts runners of its own.
    static void run_bounded(std::size_t count, std::size_t width, const std::function<void(std::size_t)>& job) {
        std::atomic<std::size_t> next{0};
        std::vector<lake::Thread> workers;
        const std::size_t threads = std::min(count, width);
        workers.reserve(threads);
        for (std::size_t worker = 0; worker < threads; ++worker) {
            workers.emplace_back([&next, count, &job] {
                for (std::size_t index = next.fetch_add(1); index < count; index = next.fetch_add(1)) {
                    job(index);
                }
            });
        }
    }
    static constexpr std::size_t handoff_width = 8;

    struct WorkspaceRelease {
        std::string workspace_id;
        // A store was open and is closed now; its lease is released unless it had been lost.
        bool closed = false;
        // Why it is still open (requests still in its lane at the deadline), or why closing failed.
        std::string error;
        // Its search index manifest was removed for the next process to rebuild.
        bool index_invalidated = false;
        // From the call until its lane was free, and how long the settle and release took.
        std::chrono::milliseconds waited{0};
        std::chrono::milliseconds closing{0};
    };

    // Closes every open workspace store as soon as the requests admitted to its lane before the
    // drain (and the image pulls started before it) have left, eight stores at a time, so one slow
    // settle (bounded by the owner's budget) never holds up the others. It sweeps again until no
    // store is left, so a store an open already in progress inserts after a sweep is released
    // too. A store whose lane is still in use at `deadline` stays open for close().
    //
    // `stale_indexes`: workspaces whose search index is behind what this process knew (a rebuild
    // or a retry the drain dropped). Their index manifest is removed under the lane and the lease
    // before the store closes, so the next process rebuilds the index instead of serving it stale.
    std::vector<WorkspaceRelease> release_workspace_stores(std::chrono::steady_clock::time_point deadline,
                                                           const std::set<std::string>& stale_indexes = {}) {
        std::vector<WorkspaceRelease> output;
        std::set<std::string> left_open;
        for (;;) {
            std::vector<std::string> open;
            {
                std::lock_guard maps(mutex_);
                for (const auto& [workspace_id, store] : workspace_stores_) {
                    if (!left_open.contains(workspace_id)) {
                        open.push_back(workspace_id);
                    }
                }
            }
            if (open.empty()) {
                break;
            }
            std::vector<WorkspaceRelease> sweep(open.size());
            run_bounded(open.size(), handoff_width, [this, &open, &sweep, deadline, &stale_indexes](std::size_t index) {
                sweep[index] = release_workspace_store(open[index], deadline, stale_indexes.contains(open[index]));
            });
            for (WorkspaceRelease& release : sweep) {
                if (!release.closed && !release.error.empty()) {
                    left_open.insert(release.workspace_id);
                }
                output.push_back(std::move(release));
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
        }
        return output;
    }

    // Opens a switched workspace's store ahead of its first request (lake#536), waiting at most
    // `wait` for its lease. Lake's own work: it holds the lane without admission, so a request
    // that arrives meanwhile waits for this open and then uses it. False when it was open.
    SyncStoreResult<bool> preopen_workspace(const std::string& customer_id,
                                            const std::string& workspace_id,
                                            std::chrono::milliseconds wait) {
        if (!path_component_is_safe(customer_id) || !path_component_is_safe(workspace_id)) {
            return std::unexpected(SyncStoreError{"invalid_input", "invalid workspace location"});
        }
        if (draining_.load()) {
            return std::unexpected(stopping_error());
        }
        auto lane = hold("w:" + workspace_id);
        {
            std::lock_guard maps(mutex_);
            const auto found = workspace_stores_.find(workspace_id);
            if (found != workspace_stores_.end() && found->second->usable()) {
                return false;
            }
        }
        // Checked again under the lane before every attempt: a workspace deleted, stopped or not
        // switched since the pass listed it is not opened (a deletion closes under this lane).
        const auto serving = serving_workspaces(customer_id);
        if (!serving) {
            return std::unexpected(serving.error());
        }
        if (std::ranges::find(*serving, workspace_id) == serving->end()) {
            return std::unexpected(SyncStoreError{
                "not_found", "the workspace is no longer a running switched workspace"});
        }
        auto opened = open_workspace(customer_id, workspace_id, false, wait);
        if (!opened) {
            return std::unexpected(opened.error());
        }
        // lake#541: the open may have waited up to `wait` for the lease, and a stop meanwhile
        // takes no lane. Checked again under the lane now that the lease is held: a workspace that
        // is no longer a running switched one has its store closed and its lease released at once,
        // before any request or renewal uses it. (A deletion closes under this lane, so it waited
        // for this open and finds nothing open.)
        const auto still_serving = serving_workspaces(customer_id);
        if (!still_serving || std::ranges::find(*still_serving, workspace_id) == still_serving->end()) {
            static_cast<void>(close_workspace_store_locked(workspace_id));
            if (!still_serving) {
                return std::unexpected(still_serving.error());
            }
            return std::unexpected(SyncStoreError{
                "not_found", "the workspace was stopped, deleted or unswitched while the pre-open waited for its lease"});
        }
        return true;
    }

    // Opens a pre-opened workspace store's repositories (lake#536), each under the lane on its
    // own, so the first request to one does not pay for reading its state, and a request for
    // another waits for one repository at most. The caller owns the store root (a repository is
    // named by its path under it). A repository that does not open is left to its first request.
    // Answers how many are open.
    std::size_t preopen_repositories(const std::string& customer_id, const std::string& workspace_id,
                                     const std::vector<std::string>& repository_ids) {
        std::size_t opened = 0;
        for (const std::string& repository_id : repository_ids) {
            if (draining_.load()) {
                break;
            }
            if (!path_component_is_safe(customer_id) || !path_component_is_safe(repository_id)) {
                continue;
            }
            auto lane = hold("w:" + workspace_id);
            {
                // Never opens the store itself (lake#541): a store closed since the pre-open
                // opened it (the workspace deleted, its lease lost) is left to a request.
                std::lock_guard maps(mutex_);
                const auto found = workspace_stores_.find(workspace_id);
                if (found == workspace_stores_.end() || !found->second->usable()) {
                    break;
                }
            }
            const auto route = workspace_route(customer_id, repository_id);
            if (!route || !*route || **route != workspace_id) {
                continue;
            }
            if (open_repository(customer_id, repository_id, *route)) {
                ++opened;
            }
        }
        return opened;
    }

    // A space's switched workspaces that are running, the ones the pre-open opens.
    SyncStoreResult<std::vector<std::string>> serving_workspaces(const std::string& customer_id) {
        WorkspaceStoreConfig config;
        {
            std::lock_guard maps(mutex_);
            config = workspace_config_;
        }
        std::vector<std::string> output;
        if (!config.switched || !config.endpoint || !config.transport) {
            return output;
        }
        auto listed = config.switched(customer_id);
        if (!listed) {
            return std::unexpected(listed.error());
        }
        for (const auto& [workspace_id, serving] : listed->first) {
            if (serving) {
                output.push_back(workspace_id);
            }
        }
        return output;
    }

private:
    static SyncStoreError stopping_error() {
        return SyncStoreError{
            "store_unavailable",
            "Lake is stopping and hands the workspace store to its successor; retry shortly"};
    }

    WorkspaceRelease release_workspace_store(const std::string& workspace_id,
                                             std::chrono::steady_clock::time_point deadline,
                                             bool stale_index) {
        WorkspaceRelease output;
        output.workspace_id = workspace_id;
        const auto started = std::chrono::steady_clock::now();
        const auto milliseconds = [](auto duration) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(duration);
        };
        auto lane = lane_of("w:" + workspace_id);
        const auto idle = [&lane] { return lane->occupants.load() == 0 && lane->streams.load() == 0; };
        for (;;) {
            if (idle()) {
                // Lake's own work in the lane (a search job, the deletion pass) is waited for by
                // trying again, so the deadline holds.
                std::unique_lock<std::mutex> lock(lane->mutex, std::try_to_lock);
                if (lock.owns_lock() && idle()) {
                    LaneLock held(lane, std::move(lock), false);
                    const auto free = std::chrono::steady_clock::now();
                    output.waited = milliseconds(free - started);
                    if (stale_index) {
                        output.index_invalidated = invalidate_search_index_locked(workspace_id);
                    }
                    auto closed = close_workspace_store_locked(workspace_id);
                    output.closing = milliseconds(std::chrono::steady_clock::now() - free);
                    if (!closed) {
                        output.error = closed.error().message;
                    } else {
                        output.closed = *closed;
                    }
                    return output;
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                output.waited = milliseconds(std::chrono::steady_clock::now() - started);
                output.error = "requests were still in its lane at the drain deadline";
                return output;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    // Removes the workspace's search index manifest with the store's client and lease fence; the
    // caller holds the lane. True when the manifest is gone.
    bool invalidate_search_index_locked(const std::string& workspace_id) {
        std::shared_ptr<control::ZettaObjectClient> client;
        std::shared_ptr<IndexWriteGate> gate;
        std::uint64_t fence = 0;
        {
            std::lock_guard maps(mutex_);
            const auto found = workspace_stores_.find(workspace_id);
            if (found == workspace_stores_.end() || !found->second->usable()) {
                return false;
            }
            client = found->second->client;
            gate = found->second->index_gate;
            fence = found->second->owner->fence();
        }
        // No index write of this process lands after the manifest is removed (lake#606).
        gate->close();
        const auto removed = liquid::runtime::block_on(
            client->remove(std::string(search::workspace_index_manifest), fence, true));
        return removed || removed.error().code == liquid::errc::not_found;
    }

    // lake#536: set once when lake begins to drain; never cleared (a drained process exits).
    std::atomic<bool> draining_{false};
    // open_workspace calls in progress.
    std::atomic<int> opening_{0};
};

struct OutboundRequest {
    std::string method = "GET";
    std::string url;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    // The whole exchange after the TLS handshake; speech-to-text uploads take longer.
    std::chrono::seconds timeout{30};
    // The largest response body read; a model's answer or event stream can be larger (lake#248).
    std::size_t body_limit = 2 * 1024 * 1024;
    // When set, the response body goes to it piece by piece as it arrives, with the response's
    // status, and is not kept in OutboundResponse::body (lake#665, a model's event stream relayed
    // as it arrives). Called on the client's io thread, so it must not block; false stops reading.
    std::function<bool(int status, std::string_view piece)> on_body;
    // Plain http to a cluster Service (`<name>.<namespace>.svc.cluster.local`): a served
    // methodology's model (lake#690), whose Service only Lake's pods can reach.
    bool cluster_http = false;
    // Only HTTPS to public addresses (lake#876): every address the host resolves to is checked at
    // connect time with mcp_public_address, as MCP connections are, so a webhook a tenant points at
    // a private, loopback or link-local address never reaches Lake's own network.
    bool public_only = false;
};

// An identity provider's discovery document, token endpoint and keys (lake#837) answer within this.
constexpr std::chrono::seconds oidc_timeout{10};

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

bool parse_outbound_url(std::string_view url, OutboundTarget& target, bool cluster_http = false) {
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
    const bool cluster = cluster_http && target.host.ends_with(".svc.cluster.local");
    return !target.host.empty() && !target.port.empty() && (target.secure || loopback || cluster);
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
    limits.body_limit = request.body_limit;
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
        bool more = true;
        for (const boost::capy::const_buffer& buffer : buffers) {
            const std::string_view piece(static_cast<const char*>(buffer.data()), buffer.size());
            if (request.on_body) {
                more = more && request.on_body(response.status, piece);
            } else {
                response.body.append(piece);
            }
            consumed += buffer.size();
        }
        body.consume(consumed);
        if (!more) {
            co_return {{}, "the response body's reader stopped"};
        }
    }
    co_return {{}, std::string()};
}

class OutboundHttpClient {
public:
    OutboundHttpClient() {
        boost::capy::run_async(io_context_.get_executor())(keepalive());
        worker_ = lake::Thread([this] { io_context_.run(); });
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
        if (!parse_outbound_url(pending->request.url, target, pending->request.cluster_http)) {
            pending->response.error = "outbound URL must use HTTPS, except for loopback tests";
            finish(pending);
            co_return;
        }
        if (pending->request.public_only && !target.secure) {
            pending->response.error = "outbound URL must use HTTPS";
            finish(pending);
            co_return;
        }
        // Every step is bounded by the request's timeout (lake#772): the name lookup, the
        // connection and the TLS handshake, as well as the exchange. An endpoint that accepts a
        // connection and never answers cannot hold the request.
        const auto deadline = pending->request.timeout;
        boost::corosio::resolver resolver(io_context_);
        auto [resolve_error, results] =
            co_await boost::capy::timeout(resolver.resolve(target.host, target.port), deadline);
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
            const auto endpoint = entry.get_endpoint();
            if (pending->request.public_only) {
                std::vector<std::uint8_t> bytes;
                if (endpoint.is_v4()) {
                    const auto raw = endpoint.v4_address().to_bytes();
                    bytes.assign(raw.begin(), raw.end());
                } else {
                    const auto raw = endpoint.v6_address().to_bytes();
                    bytes.assign(raw.begin(), raw.end());
                }
                if (!mcp_public_address(bytes)) {
                    connect_error = "the outbound host's address is not a public address";
                    continue;
                }
            }
            auto [error] = co_await boost::capy::timeout(socket.connect(endpoint), deadline);
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
            auto [handshake_error] = co_await boost::capy::timeout(
                secure.handshake(boost::corosio::tls_stream::client), deadline);
            if (handshake_error) {
                pending->response.error =
                    "outbound TLS handshake failed: " + handshake_error.message();
                finish(pending);
                co_return;
            }
            auto [timeout_error, reason] = co_await boost::capy::timeout(
                exchange_outbound(secure, pending->request, target, pending->response),
                pending->request.timeout);
            exchange_error = timeout_error ? "outbound request timed out" : std::move(reason);
            [[maybe_unused]] auto shutdown = co_await secure.shutdown();
        } else {
            auto [timeout_error, reason] = co_await boost::capy::timeout(
                exchange_outbound(socket, pending->request, target, pending->response),
                pending->request.timeout);
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
    lake::Thread worker_;
    std::map<std::string, std::shared_ptr<const boost::corosio::tls_context>> tls_contexts_;
};

// The MCP connections' transport (decision 60, lake#652, docs/api/mcp-connections.md#the-client):
// McpHttp over the same capy/corosio stack as OutboundHttpClient, on its own io thread so a slow
// MCP server never holds up Stripe or speech-to-text. Every address a name resolves to must be
// public unless the transport allows private addresses (tests, LAKE_MCP_ALLOW_PRIVATE_ADDRESSES),
// so a connection cannot reach lake's own network: its Postgres, the workspace Zettas, the cloud
// metadata address. A response is read until `enough`, the byte limit or the deadline.
struct McpExchangeState {
    McpHttpRequest request;
    McpHttpResponse response;
    McpEnough enough;
    bool streaming = false;
    bool allow_private = false;
    std::mutex mutex;
    std::condition_variable changed;
    bool head_ready = false;
    bool done = false;
    std::string queued;
    bool closed = false;
    std::string stream_error;
    std::atomic<bool> cancelled{false};
    boost::corosio::tcp_socket* socket = nullptr; // the io thread's, while it is open
};

template <class Stream>
boost::capy::io_task<std::string> mcp_write_and_read_head(Stream& stream,
                                                          McpExchangeState& state,
                                                          const McpUrl& target,
                                                          boost::http::response_parser& parser) {
    namespace http = boost::http;
    const http::method method = http::string_to_method(state.request.method);
    if (method == http::method::unknown) {
        co_return {{}, "unsupported HTTP method"};
    }
    http::request message(method, target.path, http::version::http_1_1);
    boost::system::error_code ignored;
    const bool standard = (target.scheme == "https" && target.port == "443") ||
                          (target.scheme == "http" && target.port == "80");
    const std::string host =
        target.host.find(':') != std::string::npos ? "[" + target.host + "]" : target.host;
    message.set(http::field::host, standard ? host : host + ":" + target.port, ignored);
    message.set(http::field::connection, "close", ignored);
    for (const auto& [name, value] : state.request.headers) {
        message.set(name, value, ignored);
    }
    message.set_content_length(state.request.body.size());
    http::serializer serializer(http::make_serializer_config(http::serializer_config{}));
    serializer.set_message(message);
    auto sink = serializer.sink_for(stream);
    auto [write_error, written] =
        co_await sink.write_eof(boost::capy::make_buffer(std::string_view(state.request.body)));
    if (write_error) {
        co_return {{}, "could not send the request: " + write_error.message()};
    }
    state.response.sent_bytes = state.request.body.size();
    parser.reset();
    parser.start();
    auto [header_error] = co_await parser.read_header(stream);
    if (header_error) {
        co_return {{}, "could not read the response: " + header_error.message()};
    }
    std::lock_guard lock(state.mutex);
    state.response.status = static_cast<int>(parser.get().status_int());
    for (const auto& field : parser.get()) {
        std::string name(field.name);
        std::ranges::transform(
            name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        state.response.headers[name] = std::string(field.value);
    }
    co_return {{}, std::string()};
}

template <class Stream>
boost::capy::io_task<std::string>
mcp_read_body(Stream& stream, McpExchangeState& state, boost::http::response_parser& parser) {
    auto body = parser.source_for(stream);
    boost::capy::const_buffer chunks[8];
    std::size_t received = 0;
    for (;;) {
        if (state.cancelled.load()) {
            co_return {{}, std::string()};
        }
        auto [read_error, buffers] = co_await body.pull(chunks);
        if (read_error == boost::capy::error::eof) {
            break;
        }
        if (read_error) {
            co_return {{},
                       state.cancelled.load()
                           ? std::string()
                           : "could not read the response: " + read_error.message()};
        }
        std::size_t consumed = 0;
        std::string arrived;
        for (const boost::capy::const_buffer& buffer : buffers) {
            arrived.append(static_cast<const char*>(buffer.data()), buffer.size());
            consumed += buffer.size();
        }
        body.consume(consumed);
        received += arrived.size();
        if (state.streaming) {
            std::lock_guard lock(state.mutex);
            if (state.queued.size() + arrived.size() > 8 * 1024 * 1024) {
                co_return {{}, "the stream outpaced its reader"};
            }
            state.queued += arrived;
            state.changed.notify_all();
            continue;
        }
        const std::size_t room = state.request.max_response_bytes > state.response.body.size()
                                     ? state.request.max_response_bytes - state.response.body.size()
                                     : 0;
        if (arrived.size() > room) {
            state.response.body.append(arrived, 0, room);
            state.response.truncated = true;
            co_return {{}, std::string()};
        }
        state.response.body += arrived;
        if (state.enough && state.enough(state.response.body)) {
            co_return {{}, std::string()};
        }
    }
    co_return {{}, std::string()};
}

template <class Stream>
boost::capy::io_task<std::string> mcp_exchange_on(Stream& stream,
                                                  McpExchangeState& state,
                                                  const McpUrl& target,
                                                  boost::http::response_parser& parser) {
    auto [head_error, head] = co_await mcp_write_and_read_head(stream, state, target, parser);
    if (head_error || !head.empty()) {
        co_return {head_error, head};
    }
    co_return co_await mcp_read_body(stream, state, parser);
}

class McpOutboundHttp final : public McpHttp, public std::enable_shared_from_this<McpOutboundHttp> {
public:
    explicit McpOutboundHttp(bool allow_private) : allow_private_(allow_private) {
        boost::capy::run_async(io_context_.get_executor())(keepalive());
        worker_ = lake::Thread([this] { io_context_.run(); });
    }

    ~McpOutboundHttp() override {
        io_context_.stop();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    McpHttpResponse exchange(const McpHttpRequest& request, const McpEnough& enough) override {
        auto state = std::make_shared<McpExchangeState>();
        state->request = request;
        state->enough = enough;
        state->allow_private = allow_private_;
        boost::capy::run_async(io_context_.get_executor())(perform(state));
        std::unique_lock lock(state->mutex);
        state->changed.wait(lock, [&] { return state->done; });
        return std::move(state->response);
    }

    std::unique_ptr<McpEventStream> open(const McpHttpRequest& request) override {
        auto state = std::make_shared<McpExchangeState>();
        state->request = request;
        state->streaming = true;
        state->allow_private = allow_private_;
        boost::capy::run_async(io_context_.get_executor())(perform(state));
        {
            std::unique_lock lock(state->mutex);
            state->changed.wait(lock, [&] { return state->head_ready || state->done; });
        }
        return std::make_unique<Stream>(state, shared_from_this());
    }

private:
    class Stream final : public McpEventStream {
    public:
        Stream(std::shared_ptr<McpExchangeState> state, std::shared_ptr<McpOutboundHttp> owner)
            : state_(std::move(state)), owner_(std::move(owner)) {
            std::lock_guard lock(state_->mutex);
            head_ = state_->response;
        }
        ~Stream() override {
            state_->cancelled.store(true);
            owner_->cancel(state_);
        }
        const McpHttpResponse& head() const override {
            return head_;
        }
        Read read(std::chrono::milliseconds wait) override {
            std::unique_lock lock(state_->mutex);
            state_->changed.wait_for(
                lock, wait, [&] { return !state_->queued.empty() || state_->closed; });
            Read out;
            out.bytes.swap(state_->queued);
            if (out.bytes.empty() && state_->closed) {
                out.closed = true;
                out.error = state_->stream_error;
            }
            return out;
        }

    private:
        std::shared_ptr<McpExchangeState> state_;
        std::shared_ptr<McpOutboundHttp> owner_;
        McpHttpResponse head_;
    };

    void cancel(std::shared_ptr<McpExchangeState> state) {
        boost::capy::run_async(io_context_.get_executor())(cancel_socket(std::move(state)));
    }

    boost::capy::task<> cancel_socket(std::shared_ptr<McpExchangeState> state) {
        if (state->socket != nullptr) {
            state->socket->cancel();
        }
        co_return;
    }

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

    static void finish(const std::shared_ptr<McpExchangeState>& state, std::string error) {
        std::lock_guard lock(state->mutex);
        if (!error.empty() && state->response.error.empty() && !state->head_ready) {
            state->response.error = error;
        }
        state->stream_error = std::move(error);
        state->done = true;
        state->closed = true;
        state->head_ready = true;
        state->changed.notify_all();
    }

    boost::capy::task<> perform(std::shared_ptr<McpExchangeState> state) {
        auto target = parse_mcp_url(
            state->request.url, state->allow_private, state->request.cluster_service);
        if (!target) {
            finish(state, target.error());
            co_return;
        }
        // A workspace App Lake runs for a stdio server (lake#659) is reached at the cluster Service
        // name Lake made, whose address is private; nothing else private is.
        const bool cluster_service =
            state->request.cluster_service && mcp_cluster_service_host(target->host);
        boost::corosio::resolver resolver(io_context_);
        auto [resolve_error, results] = co_await boost::capy::timeout(
            resolver.resolve(target->host, target->port), state->request.timeout);
        if (resolve_error) {
            finish(state, "could not resolve the server's name: " + resolve_error.message());
            co_return;
        }
        boost::corosio::tcp_socket socket(io_context_);
        socket.open();
        bool connected = false;
        bool refused_address = false;
        std::string connect_error = "the server's name resolved to no address";
        for (const auto& entry : results) {
            const auto endpoint = entry.get_endpoint();
            std::vector<std::uint8_t> bytes;
            if (endpoint.is_v4()) {
                const auto raw = endpoint.v4_address().to_bytes();
                bytes.assign(raw.begin(), raw.end());
            } else {
                const auto raw = endpoint.v6_address().to_bytes();
                bytes.assign(raw.begin(), raw.end());
            }
            if (!state->allow_private && !cluster_service && !mcp_public_address(bytes)) {
                refused_address = true;
                continue;
            }
            auto [error] =
                co_await boost::capy::timeout(socket.connect(endpoint), state->request.timeout);
            if (!error) {
                connected = true;
                break;
            }
            connect_error = "could not connect to the server: " + error.message();
        }
        if (!connected) {
            finish(state,
                   refused_address && connect_error.starts_with("the server's name")
                       ? "the server's address is not a public address; Lake connects "
                         "only to public MCP servers"
                       : connect_error);
            co_return;
        }
        state->socket = &socket;
        namespace http = boost::http;
        http::parser_config limits(false);
        // Lake's own byte limit applies as the body arrives (mcp_read_body), so a response that
        // passes it is cut and marked truncated rather than failed by the parser.
        limits.body_limit = std::uint64_t{1} << 50;
        http::response_parser parser(http::make_parser_config(limits));
        std::string error;
        bool timed_out = false;
        const auto run = [&](auto& stream) -> boost::capy::task<> {
            if (state->streaming) {
                auto [timeout_error, reason] = co_await boost::capy::timeout(
                    mcp_write_and_read_head(stream, *state, *target, parser),
                    state->request.timeout);
                if (timeout_error || !reason.empty()) {
                    timed_out = static_cast<bool>(timeout_error);
                    error = timeout_error ? "the server did not answer in time" : reason;
                    co_return;
                }
                {
                    std::lock_guard lock(state->mutex);
                    state->head_ready = true;
                    state->changed.notify_all();
                }
                auto [read_error, read_reason] = co_await mcp_read_body(stream, *state, parser);
                error = read_reason;
                co_return;
            }
            auto [timeout_error, reason] = co_await boost::capy::timeout(
                mcp_exchange_on(stream, *state, *target, parser), state->request.timeout);
            timed_out = static_cast<bool>(timeout_error);
            error = timeout_error ? "the server did not answer in time" : reason;
        };
        if (target->scheme == "https") {
            std::string context_error;
            const auto context = tls_context_for(target->host, context_error);
            if (!context) {
                state->socket = nullptr;
                finish(state, context_error);
                co_return;
            }
            boost::corosio::openssl_stream secure(&socket, *context);
            auto [handshake_error] = co_await boost::capy::timeout(
                secure.handshake(boost::corosio::tls_stream::client), state->request.timeout);
            if (handshake_error) {
                state->socket = nullptr;
                finish(state, "the TLS handshake with the server failed");
                co_return;
            }
            co_await run(secure);
            [[maybe_unused]] auto shutdown = co_await secure.shutdown();
        } else {
            co_await run(socket);
        }
        state->socket = nullptr;
        {
            std::lock_guard lock(state->mutex);
            state->response.timed_out = timed_out;
        }
        // An exchange that read an answer keeps it; its transport failure is only kept for the
        // caller when no status arrived.
        finish(state, state->response.status == 0 || state->streaming ? error : std::string());
    }

    bool allow_private_ = false;
    boost::corosio::io_context io_context_;
    lake::Thread worker_;
    std::map<std::string, std::shared_ptr<const boost::corosio::tls_context>> tls_contexts_;
};

// Where the store root stands (lake#479). `opening`: the owner thread is taking the root's lock
// or opening its stores; `waiting`: another process holds the root and this one waits for it to
// drain; `open`: this process owns the root and its stores are open (or it has no root);
// `failed`: the wait expired or the open failed, and the process exits.
enum class StoreRootState : int { opening, waiting, open, failed };

std::string_view to_string(StoreRootState state) {
    switch (state) {
    case StoreRootState::opening:
        return "opening";
    case StoreRootState::waiting:
        return "waiting";
    case StoreRootState::open:
        return "open";
    case StoreRootState::failed:
        return "failed";
    }
    return "unknown";
}

// Requests that never read or write the store root, and so are answered while the store root is
// still being handed over: the probes, metrics and the embedded static assets. Every other
// request waits for the open, because a capability can reach the root indirectly (the audit
// mirror into the transparency log, search, registry indexes) and a missed route would let two
// processes write the root at once.
bool store_root_exempt(std::string_view path) {
    return path == "/healthz" || path == "/readyz" || path == "/metrics" || path == "/style.css" ||
           path == web_ui_stylesheet_url() || path.starts_with("/fonts/");
}

// Counts a route handler as in flight from its first step until it completes, so stop() can
// wait for the requests it has already accepted.
boost::http::route_task count_in_flight(std::atomic<int>& counter, boost::http::route_task task) {
    counter.fetch_add(1);
    struct Finished {
        std::atomic<int>& counter;
        ~Finished() {
            counter.fetch_sub(1);
        }
    } finished{counter};
    co_return co_await std::move(task);
}

} // namespace

std::shared_ptr<McpHttp> make_mcp_http(bool allow_private_addresses) {
    return std::make_shared<McpOutboundHttp>(allow_private_addresses);
}

struct ApiServer::Impl {
    ServerOptions options;
    telemetry::Metrics metrics;
    std::chrono::steady_clock::time_point next_topic_reap{};
    std::chrono::steady_clock::time_point next_secret_lease_sweep{};
    std::size_t reported_bus_reaps = 0;
    // Repository usage on its way to the workspace Zettas (lake#348); null without workspaces.
    std::unique_ptr<MeteringRecorder> metering_recorder;
    std::chrono::steady_clock::time_point next_metering_flush{};
    MeteringRecorder::Totals reported_metering;
    // Events about a workspace's data on their way to its audit (decision 52); null without
    // workspaces, when every event is central.
    std::unique_ptr<AuditRecorder> audit_recorder;
    std::chrono::steady_clock::time_point next_audit_flush{};
    AuditRecorder::Totals reported_audit;
    // Each workspace's `lake_requests` series on its way to its Zetta (lake#384); null without
    // workspaces.
    std::unique_ptr<RequestRecorder> request_recorder;
    std::chrono::steady_clock::time_point next_request_series_flush{};
    RequestRecorder::Totals reported_request_series;
    // Usage over time on its way to each workspace Zetta (lake#564); null without workspaces.
    std::unique_ptr<UsageRecorder> usage_recorder;
    std::chrono::steady_clock::time_point next_usage_flush{};
    std::chrono::steady_clock::time_point next_usage_sample{};
    // The MCP connections' streams (lake#652), on the maintenance thread of the store root's owner.
    McpStreamWorker mcp_streams;
    AgentStreamWorker agent_streams;
    std::chrono::steady_clock::time_point next_mcp_stream_pass{};
    std::chrono::steady_clock::time_point last_usage_sample{};
    UsageRecorder::Totals reported_usage;
    mutable LiquidCustomerStores sync_store;
    // The last platform poll's problem as logged, so an unchanged one is not logged every poll.
    std::string last_platform_sync_problem;
    ArtifactRegistry artifacts;
    // The instance registry's tenant images, for the workspace deletion cascade (lake#514).
    WorkspaceContent workspace_content{
        .image_tags = [this](const std::string& repository) { return artifacts.tags(repository); },
        // Only the store root's owner changes the registry; a successor still waiting for it
        // leaves the deletion pending, and its owner loop finishes it once it owns the root.
        .remove_image_repository = [this](const std::string& repository)
            -> std::expected<void, std::string> {
            if (store_root_state.load() != StoreRootState::open) {
                return std::unexpected("this Lake does not own the store root yet");
            }
            return artifacts.remove_repository(repository);
        },
        .legacy_image_tags = [this](const std::string& after, int limit) {
            return artifacts.legacy_tenant_tags(after, limit);
        },
        .read_blob = [this](const std::string& digest) -> std::expected<std::string, std::string> {
            return artifacts.blob_text(digest);
        },
        .blob_size = [this](const std::string& digest) { return artifacts.blob_size(digest); },
        .blob_file = [this](const std::string& digest) { return artifacts.blob_file(digest); },
        .instance_digests = [this] { return artifacts.instance_digests(); },
        .drop_repository_index = [this](const std::string& customer_id,
                                        const std::string& repository_id)
            -> std::expected<void, std::string> {
            if (!options.search) {
                return {};
            }
            if (!options.store_root.empty() && store_root_state.load() != StoreRootState::open) {
                return std::unexpected("this Lake does not own the store root yet");
            }
            const auto dropped = options.search->drop_repository(customer_id, repository_id);
            if (!dropped) {
                return std::unexpected(dropped.error().message);
            }
            return {};
        },
        .drop_workspace_index = [this](const std::string& workspace_id)
            -> std::expected<void, std::string> {
            if (!options.search) {
                return {};
            }
            const auto dropped = options.search->drop_workspace(workspace_id);
            if (!dropped) {
                return std::unexpected(dropped.error().message);
            }
            return {};
        },
        // lake#380: the workspace's own store is closed and its bucket lease released before the
        // deletion is reported; the workspace Zetta's deletion destroys its bytes.
        .close_workspace_store = [this](const std::string& workspace_id)
            -> std::expected<void, std::string> {
            const auto closed = sync_store.close_workspace_store(workspace_id);
            if (!closed) {
                return std::unexpected(closed.error().message);
            }
            return {};
        },
    };
    std::chrono::steady_clock::time_point next_workspace_deletion_pass{};
    // Switched workspaces' bucket usage is read here, never on a write path (lake#380).
    std::chrono::steady_clock::time_point next_workspace_usage_refresh{};
    std::chrono::steady_clock::time_point next_upload_sweep{};
    Registry registry;
    mutable control::pki::Service pki;
    mutable control::agents::Service agents;
    std::unique_ptr<transparency::Service> transparency;
    std::unique_ptr<transparency::CheckpointService> checkpoints;
    std::unique_ptr<transparency::TiledLog> transparency_log;
    std::unique_ptr<transparency::TimestampService> timestamping;
    std::unique_ptr<control::callers::StoreFederationIssuerPolicy> store_federation_policy;
    std::unique_ptr<control::callers::Service> callers;
    std::string openapi_body;
    std::string agent_openapi_body;
    // The registry is fixed for the life of the process, so its MCP tool list is too (lake#441).
    std::string mcp_tool_digest;
    // What each MCP session last listed (lake#684).
    McpSessionLists mcp_session_lists;
    mutable OutboundHttpClient outbound_http;
    // How invoices are paid where billing is on (docs/api/billing.md#payment-providers).
    std::shared_ptr<PaymentProviders> payments;
    // The apex order sheet (lake#831, docs/api/orders.md): the anonymous form's limit, the key its
    // status links are signed with (LAKE_INSTANCE_KEK, else one made at start), when the order
    // pass runs next, and whether an order asked for it sooner.
    std::unique_ptr<OrderRateLimiter> order_limiter;
    std::string order_status_key;
    std::chrono::steady_clock::time_point next_order_pass{};
    std::atomic<bool> order_pass_requested{false};
    bool order_email_waiting_logged = false;
    // Speech-to-text (docs/api/transcripts.md): the injected one, else OpenAI over outbound_http
    // when a key is configured, else empty.
    Transcriber transcriber;
    // MCP connections' transport (docs/api/mcp-connections.md#the-client): the injected one, else
    // the server's own, which refuses private addresses unless the options allow them.
    std::shared_ptr<McpHttp> mcp_http;
    // OpenID Connect discovery (lake#837), over outbound_http.
    std::shared_ptr<OidcDiscovery> oidc_discovery;
    // A customer's own buckets (lake#664): their transport, public addresses only, and the
    // browser over it.
    std::shared_ptr<McpHttp> store_transport;
    control::StoreBrowser store_browser;
    // The model gateway's provider (docs/api/model-gateway.md): the injected one, else OpenAI's
    // API over outbound_http when LAKE_OPENAI_API_KEY is set (lake#248).
    MethodologyServer methodology_server;
    ModelProvider model_provider;
    // The workspace Zettas' object transport (configure_workspace_stores), for workspace files;
    // null without workspaces.
    std::shared_ptr<control::ObjectTransport> object_transport;
    struct CachedJwks {
        std::string document;
        std::chrono::steady_clock::time_point fetched_at;
    };
    std::map<std::string, CachedJwks> jwks_cache;
    std::optional<ClerkConfig> clerk;
    boost::corosio::io_context io_context;
    // Lake#534: connections apart from the request workers (src/api/http_connections.hpp).
    std::unique_ptr<HttpConnections> server;
    lake::Thread io_thread;
    std::atomic<bool> is_running{false};
    unsigned short bound_port = 0;
    std::string failure_reason;
    std::vector<std::string> get_routes;
    // Exclusive ownership of options.store_root while the server runs (lake#467). The owner
    // thread takes it, waiting for a predecessor to release it, then opens the customer stores;
    // store_root_state tells requests and /readyz where that stands (lake#479).
    StoreOwnerLock store_owner;
    lake::Thread owner_thread;
    // Work that waits on workspace Zettas or on a store's lane (usage refresh, the deletion pass's
    // store closing, the start-up usage priming) runs on this thread, never on the owner loop, so
    // the loop's flushes and renewals are not delayed by a Zetta timeout or a switch (lake#380).
    // A job is dropped when one of its name is already queued.
    std::mutex maintenance_mutex;
    std::condition_variable maintenance_cv;
    std::deque<std::pair<std::string, std::function<void()>>> maintenance_queue;
    bool maintenance_stopping = false;
    lake::Thread maintenance_thread;

    void schedule_maintenance(std::string name, std::function<void()> job) {
        std::lock_guard lock(maintenance_mutex);
        if (maintenance_stopping ||
            std::ranges::any_of(maintenance_queue, [&](const auto& queued) { return queued.first == name; })) {
            return;
        }
        maintenance_queue.emplace_back(std::move(name), std::move(job));
        maintenance_cv.notify_all();
    }

    void run_maintenance() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(maintenance_mutex);
                maintenance_cv.wait(lock, [this] { return maintenance_stopping || !maintenance_queue.empty(); });
                if (maintenance_stopping) {
                    return;
                }
                job = std::move(maintenance_queue.front().second);
                maintenance_queue.pop_front();
            }
            job();
        }
    }

    // lake#381: search index rebuilds and their retries run on this thread of their own. A job is
    // named; one whose name is queued already is not queued again; a job may be due later.
    std::mutex search_jobs_mutex;
    std::condition_variable search_jobs_cv;
    std::deque<std::tuple<std::string, std::chrono::steady_clock::time_point, std::function<void()>>>
        search_jobs;
    bool search_jobs_stopping = false;
    lake::Thread search_jobs_thread;

    void schedule_search_job(std::string name, std::chrono::milliseconds delay, std::function<void()> job) {
        std::lock_guard lock(search_jobs_mutex);
        if (search_jobs_stopping ||
            std::ranges::any_of(search_jobs, [&](const auto& queued) { return std::get<0>(queued) == name; })) {
            return;
        }
        search_jobs.emplace_back(std::move(name), std::chrono::steady_clock::now() + delay, std::move(job));
        search_jobs_cv.notify_all();
    }

    void run_search_jobs() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(search_jobs_mutex);
                for (;;) {
                    if (search_jobs_stopping) {
                        return;
                    }
                    if (search_jobs.empty()) {
                        search_jobs_cv.wait(lock);
                        continue;
                    }
                    const auto next = std::ranges::min_element(
                        search_jobs, {}, [](const auto& queued) { return std::get<1>(queued); });
                    if (std::get<1>(*next) <= std::chrono::steady_clock::now()) {
                        job = std::move(std::get<2>(*next));
                        search_jobs.erase(next);
                        break;
                    }
                    search_jobs_cv.wait_until(lock, std::get<1>(*next));
                }
            }
            job();
        }
    }

    void stop_search_jobs() {
        if (options.search) {
            options.search->shutdown();
        }
        {
            std::lock_guard lock(search_jobs_mutex);
            search_jobs_stopping = true;
            search_jobs.clear();
        }
        search_jobs_cv.notify_all();
        if (search_jobs_thread.joinable()) {
            search_jobs_thread.join();
        }
    }

    // lake#536: no new search or maintenance job starts, and search stops what it is doing at
    // its next check; the threads are joined by stop_search_jobs and stop_maintenance.
    // Answers the workspaces whose index jobs it dropped, or whose index search knows to be behind
    // (Service::unsettled_workspaces): the release makes the next process rebuild them.
    std::set<std::string> pause_lake_work() {
        std::set<std::string> stale;
        if (options.search) {
            options.search->shutdown();
        }
        {
            std::lock_guard lock(search_jobs_mutex);
            search_jobs_stopping = true;
            constexpr std::string_view job_prefix = "search index ";
            for (const auto& queued : search_jobs) {
                if (std::get<0>(queued).starts_with(job_prefix)) {
                    stale.insert(std::get<0>(queued).substr(job_prefix.size()));
                }
            }
            search_jobs.clear();
        }
        search_jobs_cv.notify_all();
        {
            std::lock_guard lock(maintenance_mutex);
            maintenance_stopping = true;
            maintenance_queue.clear();
        }
        maintenance_cv.notify_all();
        if (options.search) {
            for (std::string& workspace : options.search->unsettled_workspaces()) {
                stale.insert(std::move(workspace));
            }
        }
        return stale;
    }

    void stop_maintenance() {
        {
            std::lock_guard lock(maintenance_mutex);
            maintenance_stopping = true;
            maintenance_queue.clear();
        }
        maintenance_cv.notify_all();
        if (maintenance_thread.joinable()) {
            maintenance_thread.join();
        }
    }

    // lake#381: each space's search index on lake's volume drops what it holds of repositories now
    // served by a workspace store (their index is rebuilt beside that store), once at start.
    void purge_space_search_indexes() {
        if (!options.search || options.store_root.empty()) {
            return;
        }
        std::error_code error;
        const std::filesystem::path root(options.store_root);
        if (!std::filesystem::exists(root, error)) {
            return;
        }
        for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
            if (!entry.is_directory() || !std::filesystem::exists(entry.path() / "index", error)) {
                continue;
            }
            const std::string customer_id = entry.path().filename().string();
            const auto dropped = options.search->drop_switched(customer_id);
            if (!dropped) {
                std::println(stderr, "search index purge {}: {}", customer_id, dropped.error().message);
            } else if (*dropped > 0 || !std::filesystem::exists(entry.path() / "index", error)) {
                std::println(stderr, "search index purge {}: dropped {} repositories of switched workspaces{}",
                             customer_id, *dropped,
                             std::filesystem::exists(entry.path() / "index", error)
                                 ? ""
                                 : "; the account's index is empty and was removed");
            }
        }
    }

    // lake#536: the drain's first step, at SIGTERM. Stops taking new work (as stop() did), then
    // hands every workspace store to the successor: no new request is admitted to a workspace's
    // lane (503 with Retry-After), and each store is settled and its lease released as soon as
    // the requests admitted before have finished, without waiting for unrelated requests or the
    // drain deadline. stop() finishes the drain.
    void begin_drain() {
        // Called from main's SIGTERM watcher and from stop(); the second waits for the first.
        std::lock_guard lock(drain_mutex);
        if (drain_begun.exchange(true)) {
            return;
        }
        drain_started = std::chrono::steady_clock::now();
        drain_deadline = drain_started + options.drain_timeout;
        draining.store(true);
        // Connections waiting for their next request close now (lake#534).
        if (server) {
            server->begin_drain();
        }
        owner_wait_abandoned.store(true);
        sync_store.begin_drain();
        workspace_release_thread = lake::Thread([this] {
            // Lake's own work leaves the workspace lanes first: search stops (a rebuild stops at
            // its next repository) and the search and maintenance queues take no new job, and a
            // workspace lane refuses lake work from now on (LiquidCustomerStores::begin_drain).
            // Nothing is joined here, so a release waits for the lane's current holder only, never
            // for the rest of a job; stop() joins both threads.
            const std::set<std::string> stale_indexes = pause_lake_work();
            // The successor may take a free workspace lease before it owns the root only once no
            // workspace store here is opened again: no new open starts after begin_drain, and the
            // opens already in progress finish (bounded by the drain deadline). A deadline passed
            // with one still in progress announces nothing; the successor then waits for the root.
            // store_root_state turns open only after the owner thread stored the lock.
            const bool settled = sync_store.wait_for_opens(drain_deadline);
            if (settled && !options.store_root.empty() && store_root_state.load() == StoreRootState::open &&
                store_owner.held()) {
                const bool announced = store_owner.announce_draining(options.store_root);
                log_store_owner({{"state", announced ? "draining_announced" : "draining_not_announced"}});
            }
            const auto released = sync_store.release_workspace_stores(drain_deadline, stale_indexes);
            for (const auto& release : released) {
                nlohmann::json event{{"scope", "workspace"},
                                     {"workspace_id", release.workspace_id},
                                     {"state", release.closed ? "released"
                                                               : (release.error.empty() ? "not_open" : "still_open")},
                                     {"waited_ms", release.waited.count()},
                                     {"closing_ms", release.closing.count()},
                                     {"index_invalidated", release.index_invalidated}};
                if (!release.error.empty()) {
                    event["error"] = release.error;
                }
                log_store_owner(std::move(event));
            }
        });
    }

    // lake#536: opens the store of every running switched workspace in the background from
    // start, so the first request after a rollout does not pay for the open.
    //
    // Before this process owns the store root it takes no free lease: a predecessor that still
    // serves may need any workspace's store (one it never opened, one it reopens after a failed
    // write). It attempts an open only once the root's owner has announced that it is draining
    // (StoreOwnerLock::announce_draining, written once no open of its own is in progress and none
    // can start), or once it owns the root. A predecessor from before lake#536 never announces, so
    // then the opens start with the root. One poller reads the announcement every 250 ms for all
    // the openers. Each attempt waits up to 5 s for the lease (Zetta's acquire wakes on the
    // release); attempts go on until store_owner_wait has passed, the drain begins, or the root
    // could not be owned. At most 8 stores open at once.
    //
    // The repositories of each opened store are opened by a second set of workers, which start
    // when the root is owned: stores opened before then are taken in turn, and a store that opens
    // later is taken as soon as it opens, so one workspace that is slow to open (an unready
    // workspace Zetta, retried until store_owner_wait) holds up no other workspace's repositories.
    void preopen_workspace_stores() {
        if (!options.store || !options.workspaces || options.store_root.empty()) {
            return;
        }
        const auto deadline = std::chrono::steady_clock::now() + options.store_owner_wait;
        const auto customers = options.store->list_customers(100000);
        if (!customers) {
            log_store_owner({{"scope", "workspace"}, {"state", "preopen_failed"},
                             {"error", customers.error().message}});
            return;
        }
        std::vector<std::pair<std::string, std::string>> targets;
        for (const control::Customer& customer : *customers) {
            const auto serving = sync_store.serving_workspaces(customer.id);
            if (!serving) {
                continue;
            }
            for (const std::string& workspace_id : *serving) {
                targets.emplace_back(customer.id, workspace_id);
            }
        }
        if (targets.empty()) {
            return;
        }
        PreopenQueue queue;
        lake::Thread poller([this, &queue] {
            for (;;) {
                {
                    std::lock_guard lock(queue.mutex);
                    if (queue.stores_done) {
                        return;
                    }
                }
                if (preopen_stopped()) {
                    return;
                }
                if (store_root_state.load() == StoreRootState::open ||
                    StoreOwnerLock::owner_is_draining(options.store_root)) {
                    leases_free_to_take.store(true);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        });
        lake::Thread repositories([this, &queue, &targets] {
            // Waits for the root, then opens each opened store's repositories as it arrives.
            while (!preopen_stopped() && store_root_state.load() != StoreRootState::open) {
                std::unique_lock lock(queue.mutex);
                queue.changed.wait_for(lock, std::chrono::milliseconds(50));
            }
            LiquidCustomerStores::run_bounded(
                LiquidCustomerStores::handoff_width, LiquidCustomerStores::handoff_width,
                [this, &queue, &targets](std::size_t) {
                    for (;;) {
                        std::size_t index = 0;
                        {
                            std::unique_lock lock(queue.mutex);
                            queue.changed.wait(lock, [&] {
                                return !queue.opened.empty() || queue.stores_done || preopen_stopped();
                            });
                            if (preopen_stopped() || queue.opened.empty()) {
                                return;
                            }
                            index = queue.opened.front();
                            queue.opened.pop_front();
                        }
                        preopen_repositories_of(targets[index].first, targets[index].second);
                    }
                });
        });
        LiquidCustomerStores::run_bounded(
            targets.size(), LiquidCustomerStores::handoff_width,
            [this, &targets, &queue, deadline](std::size_t index) {
                if (preopen_workspace_store(targets[index].first, targets[index].second, deadline)) {
                    std::lock_guard lock(queue.mutex);
                    queue.opened.push_back(index);
                    queue.changed.notify_all();
                }
            });
        {
            std::lock_guard lock(queue.mutex);
            queue.stores_done = true;
        }
        queue.changed.notify_all();
        // The repository workers finish the opened stores (or stop at the drain) and are joined
        // here; the poller has stopped.
    }

    struct PreopenQueue {
        std::mutex mutex;
        std::condition_variable changed;
        // Indexes of the opened stores whose repositories are not opened yet.
        std::deque<std::size_t> opened;
        bool stores_done = false;
    };

    void preopen_repositories_of(const std::string& customer_id, const std::string& workspace_id) {
        const auto started = std::chrono::steady_clock::now();
        std::vector<std::string> ids;
        if (const auto listed = options.store->list_repositories(customer_id, 100000, workspace_id)) {
            for (const control::Repository& repository : *listed) {
                if (repository.deleted_at.empty()) {
                    ids.push_back(repository.id);
                }
            }
        }
        const std::size_t repositories = sync_store.preopen_repositories(customer_id, workspace_id, ids);
        log_store_owner({{"scope", "workspace"},
                         {"workspace_id", workspace_id},
                         {"state", "repositories_preopened"},
                         {"repositories", repositories},
                         {"repositories_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now() - started)
                                                 .count()}});
    }

    // Set by the pre-open's one poller: the root is owned, or its owner announced its drain.
    std::atomic<bool> leases_free_to_take{false};

    bool preopen_stopped() const {
        return draining.load() || store_root_state.load() == StoreRootState::failed;
    }

    // Whether this process may take a free workspace lease now (see preopen_workspace_stores).
    bool may_take_workspace_leases() const {
        return leases_free_to_take.load() || store_root_state.load() == StoreRootState::open;
    }

    // True when the store is open (by this pass or already).
    bool preopen_workspace_store(const std::string& customer_id, const std::string& workspace_id,
                                 std::chrono::steady_clock::time_point deadline) {
        const auto started = std::chrono::steady_clock::now();
        const auto milliseconds = [](auto duration) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        };
        int attempts = 0;
        while (!preopen_stopped()) {
            if (!may_take_workspace_leases()) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            const auto attempt = std::chrono::steady_clock::now();
            ++attempts;
            const auto opened = sync_store.preopen_workspace(customer_id, workspace_id, std::chrono::seconds(5));
            if (opened) {
                log_store_owner({{"scope", "workspace"},
                                 {"workspace_id", workspace_id},
                                 {"state", *opened ? "preopened" : "already_open"},
                                 {"attempts", attempts},
                                 {"store_ms", milliseconds(std::chrono::steady_clock::now() - started)}});
                return true;
            }
            // Unavailable is the lease still held elsewhere (after the attempt's wait) or the
            // workspace Zetta not ready (at once); anything else (the workspace deleted, stopped
            // or no longer switched) is left to the first request.
            if (opened.error().code != "store_unavailable" || std::chrono::steady_clock::now() >= deadline) {
                log_store_owner({{"scope", "workspace"},
                                 {"workspace_id", workspace_id},
                                 {"state", "preopen_failed"},
                                 {"attempts", attempts},
                                 {"error", opened.error().message}});
                return false;
            }
            while (!preopen_stopped() &&
                   std::chrono::steady_clock::now() < attempt + std::chrono::seconds(1)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        return false;
    }

    void prime_workspace_usage() {
        if (!options.store) {
            return;
        }
        const auto customers = options.store->list_customers(100000);
        if (!customers) {
            return;
        }
        for (const control::Customer& customer : *customers) {
            sync_store.track_space_usage(customer.id);
        }
        sync_store.refresh_workspace_usage();
    }
    std::atomic<StoreRootState> store_root_state{StoreRootState::opening};
    // Set once another process was found holding the root: this server reported Ready while it
    // waited, so it stays Ready through the open that follows.
    std::atomic<bool> store_root_handoff{false};
    std::atomic<bool> owner_wait_abandoned{false};
    // The drain has begun (begin_drain, at SIGTERM or from stop()): new requests close their
    // connection, /readyz answers 503, and workspace stores are handed off (lake#536).
    std::atomic<bool> draining{false};
    std::atomic<bool> drain_begun{false};
    std::mutex drain_mutex;
    std::chrono::steady_clock::time_point drain_started;
    std::chrono::steady_clock::time_point drain_deadline;
    // Releases every workspace store's lease as soon as its admitted requests are done.
    lake::Thread workspace_release_thread;
    // Opens running switched workspaces' stores ahead of their first request (lake#536).
    lake::Thread preopen_thread;
    // Route handlers running now (every guarded handler counts itself).
    std::atomic<int> in_flight{0};
    mutable std::mutex store_root_mutex;
    std::string store_root_holder;
    std::string store_root_failure;
    std::mutex work_mutex;
    std::condition_variable work_cv;
    std::chrono::steady_clock::time_point next_timestamp_renewal_check =
        std::chrono::steady_clock::now();
    std::mutex app_auth_audit_mutex;
    std::map<std::string, std::chrono::steady_clock::time_point> app_auth_denials;

    // lake#380: a switched workspace's repositories are served from its own store in its
    // workspace Zetta, reached with the workspace's mounted credential.
    void configure_workspace_stores() {
        if (!options.store || !options.workspaces) {
            return;
        }
        LiquidCustomerStores::WorkspaceStoreConfig config;
        std::shared_ptr<control::Store> store = options.store;
        std::shared_ptr<control::WorkspaceService> workspaces = options.workspaces;
        // Fails closed: a repository or workspace that cannot be read is an error, never the space
        // store, so a switched workspace's repositories are never answered from the old store.
        config.route = [store](const std::string& customer_id, const std::string& repository_id)
            -> SyncStoreResult<std::optional<std::string>> {
            const auto repository = store->get_repository(repository_id);
            if (!repository) {
                if (repository.error().code == "not_found" ||
                    repository.error().code == "invalid_input") {
                    return std::unexpected(SyncStoreError{"not_found", "no repository " + repository_id});
                }
                return std::unexpected(SyncStoreError{"store_unavailable", repository.error().message});
            }
            if (repository->customer_id != customer_id) {
                return std::unexpected(SyncStoreError{
                    "invalid_input", "the repository is not in this account"});
            }
            if (repository->workspace_id.empty()) {
                // Not yet assigned (lake#513): it can only be in the space's store.
                return std::optional<std::string>{};
            }
            const auto workspace = store->get_workspace(repository->workspace_id);
            if (!workspace) {
                return std::unexpected(SyncStoreError{"store_unavailable", workspace.error().message});
            }
            if (workspace->desired == "deleted") {
                return std::unexpected(SyncStoreError{"not_found", "the repository's workspace is deleted"});
            }
            if (workspace->store_moved_at.empty()) {
                return std::optional<std::string>{};
            }
            return std::optional<std::string>{workspace->id};
        };
        config.switched = [store](const std::string& customer_id)
            -> SyncStoreResult<std::pair<std::vector<std::pair<std::string, bool>>, bool>> {
            const auto rows = store->list_workspaces(customer_id);
            if (!rows) {
                return std::unexpected(SyncStoreError{"store_unavailable", rows.error().message});
            }
            std::pair<std::vector<std::pair<std::string, bool>>, bool> output{{}, false};
            for (const control::Workspace& workspace : *rows) {
                if (!workspace.store_moved_at.empty()) {
                    if (workspace.desired != "deleted") {
                        output.first.emplace_back(workspace.id, workspace.desired == "running");
                    }
                } else if (workspace.desired != "deleted") {
                    output.second = true;
                }
            }
            return output;
        };
        config.endpoint = [workspaces](const std::string& customer_id, const std::string& workspace_id)
            -> SyncStoreResult<control::WorkspaceEndpoint> {
            auto endpoint = workspaces->endpoint(customer_id, workspace_id);
            if (!endpoint) {
                return std::unexpected(SyncStoreError{
                    endpoint.error().code == "workspace_not_ready" ? "store_unavailable"
                                                                   : endpoint.error().code,
                    endpoint.error().message});
            }
            return *endpoint;
        };
        const std::uint64_t default_quota = options.metering.storage_quota_bytes;
        config.quota_bytes = [store, default_quota](const std::string& customer_id) {
            const auto configured = store->storage_quota(customer_id);
            return configured ? configured->value_or(default_quota) : default_quota;
        };
        config.transport =
            options.object_transport ? options.object_transport : control::make_zetta_object_transport();
        // Workspace files (docs/api/drives.md) reach the same Zettas over the same connections.
        object_transport = config.transport;
        config.instance_kek = options.instance_kek;
        config.lease_ttl = options.workspace_store_lease_ttl;
        // Bounded labels only (lake#527): the request's kind and its outcome class, never a
        // workspace, key or bucket.
        config.observer = [this](control::ObjectRequestKind kind, int status,
                                 std::chrono::nanoseconds elapsed) {
            const std::string name(control::object_request_kind_name(kind));
            const std::string outcome = status == 0     ? "no_answer"
                                        : status < 300  ? "ok"
                                        : status < 500  ? "client_error"
                                                        : "server_error";
            metrics.increment_counter("lake_workspace_store_requests_total",
                                      {{"kind", name}, {"outcome", outcome}});
            metrics.observe_histogram("lake_workspace_store_request_duration_seconds",
                                      {{"kind", name}},
                                      std::chrono::duration<double>(elapsed).count());
        };
        sync_store.configure_workspace_stores(std::move(config));
    }

    // Search reads repositories only through the repository store (lake#380), never by opening a
    // second instance of a store.
    void lend_repositories_to_search() {
        if (!options.search || options.store_root.empty()) {
            return;
        }
        options.search->set_repository_access(
            [this](const std::string& customer_id, const std::string& repository_id, bool lake_work)
                -> search::Result<search::BorrowedRepository> {
                auto lent = sync_store.lend_repository(customer_id, repository_id, lake_work);
                if (!lent) {
                    return std::unexpected(search::Error{lent.error().code, lent.error().message});
                }
                search::BorrowedRepository output{lent->hold, lent->repository, std::nullopt};
                if (!lent->workspace.empty()) {
                    output.workspace = workspace_index(customer_id, lent->workspace,
                                                       lent->search_objects, lent->fence);
                }
                return output;
            });
        // lake#381: a switched workspace's index lives beside its store, in its workspace Zetta.
        search::WorkspaceAccess access;
        access.lend = [this](const std::string& customer_id, const std::string& workspace_id,
                             bool lake_work) -> search::Result<search::BorrowedWorkspace> {
            std::string customer = customer_id;
            if (options.store) {
                const auto workspace = options.store->get_workspace(workspace_id);
                if (!workspace) {
                    return std::unexpected(
                        search::Error{workspace.error().code, workspace.error().message});
                }
                if (!customer.empty() && workspace->customer_id != customer) {
                    return std::unexpected(
                        search::Error{"invalid_input", "the workspace is not in this account"});
                }
                if (workspace->store_moved_at.empty() || workspace->desired == "deleted") {
                    return std::unexpected(search::Error{
                        "invalid_input", "the workspace's repositories are not served from its "
                                         "own store, so it has no index of its own"});
                }
                customer = workspace->customer_id;
            }
            auto lent = sync_store.lend_workspace(customer, workspace_id, false, lake_work);
            if (!lent) {
                return std::unexpected(search::Error{lent.error().code, lent.error().message});
            }
            return search::BorrowedWorkspace{
                (*lent)->hold,
                workspace_index(customer, workspace_id, (*lent)->search_objects, (*lent)->fence)};
        };
        access.lend_if_open = [this](const std::string& workspace_id)
            -> search::Result<std::optional<search::BorrowedWorkspace>> {
            auto lent = sync_store.lend_workspace("", workspace_id, true);
            if (!lent) {
                return std::unexpected(search::Error{lent.error().code, lent.error().message});
            }
            if (!*lent) {
                return std::optional<search::BorrowedWorkspace>{};
            }
            return std::optional<search::BorrowedWorkspace>{search::BorrowedWorkspace{
                (*lent)->hold, workspace_index((*lent)->customer_id, workspace_id,
                                               (*lent)->search_objects, (*lent)->fence)}};
        };
        access.switched = [this](const std::string& customer_id)
            -> search::Result<std::vector<std::pair<std::string, std::string>>> {
            std::vector<std::pair<std::string, std::string>> output;
            if (!options.store) {
                return output;
            }
            std::vector<std::string> customers;
            if (customer_id.empty()) {
                // No cursor: a full page is refused rather than taken for every space.
                constexpr int page = 100'000;
                const auto listed = options.store->list_customers(page);
                if (!listed) {
                    return std::unexpected(search::Error{listed.error().code, listed.error().message});
                }
                if (listed->size() >= static_cast<std::size_t>(page)) {
                    return std::unexpected(search::Error{
                        "unsupported", "too many accounts to search them all; name a customer_id"});
                }
                for (const control::Customer& customer : *listed) {
                    customers.push_back(customer.id);
                }
            } else {
                customers.push_back(customer_id);
            }
            for (const std::string& customer : customers) {
                const auto rows = options.store->list_workspaces(customer);
                if (!rows) {
                    return std::unexpected(search::Error{rows.error().code, rows.error().message});
                }
                for (const control::Workspace& workspace : *rows) {
                    if (!workspace.store_moved_at.empty() && workspace.desired != "deleted") {
                        output.emplace_back(customer, workspace.id);
                    }
                }
            }
            return output;
        };
        access.route = [this](const std::string& customer_id, const std::string& repository_id)
            -> search::Result<std::optional<std::string>> {
            std::string customer = customer_id;
            if (customer.empty() && options.store) {
                const auto repository = options.store->get_repository(repository_id);
                if (!repository) {
                    return std::unexpected(
                        search::Error{repository.error().code, repository.error().message});
                }
                customer = repository->customer_id;
            }
            auto route = sync_store.route_of(customer, repository_id);
            if (!route) {
                return std::unexpected(search::Error{route.error().code, route.error().message});
            }
            return *route;
        };
        // A rebuild never runs inside a request: it runs on search's own worker, never on the
        // maintenance thread, so the usage refresh and the deletion pass never wait on one.
        access.schedule = [this](const std::string& customer_id, const std::string& workspace_id,
                                 std::chrono::milliseconds delay) {
            schedule_search_job("search index " + workspace_id, delay, [this, customer_id, workspace_id] {
                const auto rebuilt = options.search->rebuild_workspace(customer_id, workspace_id);
                if (!rebuilt) {
                    metrics.increment_counter("lake_search_rebuild_errors_total");
                    std::println(stderr, "lake: search index {}: {}", workspace_id,
                                 rebuilt.error().message);
                }
            });
        };
        options.search->set_workspace_access(std::move(access));
        options.search->set_memory_bound(options.search_memory_bytes);
    }

    // One workspace's index as search sees it.
    search::WorkspaceIndex workspace_index(const std::string& customer_id,
                                           const std::string& workspace_id,
                                           std::shared_ptr<search::IndexObjects> objects,
                                           std::uint64_t fence) {
        search::WorkspaceIndex index;
        index.customer = customer_id;
        index.workspace = workspace_id;
        index.generation = fence;
        index.objects = std::move(objects);
        index.repositories = [this, customer_id, workspace_id]()
            -> search::Result<std::vector<std::string>> {
            if (!options.store) {
                return std::vector<std::string>{};
            }
            // The store has no cursor for this list: a full page is refused rather than taken
            // for the whole, so a rebuild never sweeps away the objects of repositories past it.
            constexpr int page = 100'000;
            const auto rows = options.store->list_repositories(customer_id, page, workspace_id);
            if (!rows) {
                return std::unexpected(search::Error{rows.error().code, rows.error().message});
            }
            if (rows->size() >= static_cast<std::size_t>(page)) {
                return std::unexpected(search::Error{
                    "unsupported", "the workspace has too many repositories to rebuild its index in one listing"});
            }
            std::vector<std::string> output;
            for (const control::Repository& repository : *rows) {
                if (repository.deleted_at.empty() && repository.workspace_id == workspace_id) {
                    output.push_back(repository.id);
                }
            }
            return output;
        };
        // Releases are forge records in the workspace's Zetta (lake#341); a rebuild reads them
        // on the maintenance thread.
        index.tags = [this](const std::string& repository_id)
            -> search::Result<std::vector<std::pair<std::string, std::string>>> {
            std::vector<std::pair<std::string, std::string>> output;
            if (!options.store || !options.workspaces) {
                return output;
            }
            const auto repository = options.store->get_repository(repository_id);
            if (!repository) {
                return std::unexpected(search::Error{repository.error().code, repository.error().message});
            }
            ApiContext context;
            context.store = options.store.get();
            context.workspaces = options.workspaces.get();
            context.instance_kek = options.instance_kek;
            auto forge = RepositoryForge::open(context, *repository);
            if (!forge) {
                return std::unexpected(search::Error{forge.error().code, forge.error().message});
            }
            auto releases = control::settled(liquid::runtime::block_on(forge->list("release", 0)));
            if (!releases) {
                return std::unexpected(search::Error{releases.error().code, releases.error().message});
            }
            for (const control::ForgeRecord& release : *releases) {
                const std::string tag = release.value.value("tag", std::string());
                const std::string change = release.value.value("change_id", std::string());
                if (!tag.empty() && !change.empty()) {
                    output.emplace_back(tag, change);
                }
            }
            return output;
        };
        index.write_budget = [this, customer_id, workspace_id]() {
            return sync_store.search_index_budget(customer_id, workspace_id);
        };
        index.spent = [this, workspace_id](std::uint64_t bytes) {
            sync_store.search_index_spent(workspace_id, bytes);
        };
        return index;
    }

    explicit Impl(ServerOptions opts)
        : options(std::move(opts)), sync_store(options.store_root, options.store_owner_wait),
          artifacts(options.store_root, options.node_name) {
        sync_store.configure_replication(options.replication_allow_private_addresses,
                                         options.replication_timeout);
        configure_workspace_stores();
        lend_repositories_to_search();
        if (web_ui_stylesheet_url().empty()) {
            failure_reason = "could not calculate the stylesheet asset hash";
        }
        for (const std::string_view font : web_ui_font_names()) {
            if (web_ui_font_url(font).empty()) {
                failure_reason = "could not calculate the font asset hash for " + std::string(font);
                break;
            }
        }
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
        clerk = clerk_config(options.clerk_publishable_key);
        transcriber = options.transcriber;
        if (!transcriber && !options.openai_api_key.empty()) {
            transcriber = openai_transcriber(
                options.openai_api_key,
                options.transcribe_model,
                [this](const std::string& url,
                       std::string body,
                       std::vector<std::pair<std::string, std::string>> headers,
                       std::chrono::seconds timeout) {
                    OutboundResponse response = outbound_http.request(
                        {"POST", url, std::move(body), std::move(headers), timeout});
                    return TranscriptPostResponse{
                        response.status, std::move(response.body), std::move(response.error)};
                });
        }
        mcp_http = options.mcp_http ? options.mcp_http
                                    : make_mcp_http(options.mcp_allow_private_addresses);
        // Discovery documents come over the outbound client, HTTPS to public addresses only unless
        // LAKE_OIDC_ALLOW_PRIVATE_ADDRESSES is set (lake#837, lake#876), within 10 s.
        oidc_discovery = std::make_shared<OidcDiscovery>(
            [this](const std::string& url) {
                OutboundRequest request{
                    "GET",
                    url,
                    "",
                    {{"Accept", "application/json"}, {"User-Agent", "lake-oauth/1"}},
                    oidc_timeout};
                request.body_limit = 256 * 1024;
                request.public_only = !options.oidc_allow_private_addresses;
                OutboundResponse response = outbound_http.request(request);
                return OidcFetchResponse{
                    response.status, std::move(response.body), std::move(response.error)};
            },
            options.oidc_allow_private_addresses);
        store_browser = options.store_browser;
        if (!store_browser) {
            // Its own transport, on its own io thread, so a slow bucket never holds up an MCP
            // server; each exchange runs on the blocking pool.
            store_transport = make_mcp_http(options.store_allow_private_addresses);
            store_browser = control::cloud_store_browser(
                [transport = store_transport](control::StoreHttpRequest request)
                    -> liquid::runtime::task<control::StoreHttpResponse> {
                    auto ran = co_await run_blocking([transport, request = std::move(request)] {
                        McpHttpRequest out;
                        out.method = request.method;
                        out.url = request.url;
                        out.headers = request.headers;
                        out.body = request.body;
                        out.timeout = request.timeout;
                        out.max_response_bytes = request.max_response_bytes;
                        McpHttpResponse answer = transport->exchange(out, {});
                        control::StoreHttpResponse response;
                        response.status = answer.status;
                        response.headers = std::move(answer.headers);
                        response.body = std::move(answer.body);
                        response.error = answer.timed_out ? std::string("the bucket timed out")
                                         : answer.truncated
                                             ? std::string("the bucket's answer was too large")
                                             : std::move(answer.error);
                        return response;
                    });
                    if (!ran) {
                        control::StoreHttpResponse failed;
                        failed.error = ran.error().message;
                        co_return failed;
                    }
                    co_return std::move(*ran);
                });
        }
        model_provider = options.model_provider;
        if (!model_provider && !options.openai_api_key.empty()) {
            model_provider = openai_model_provider(
                options.openai_api_key,
                options.model_provider_url,
                [this](const std::string& url,
                       std::string body,
                       std::vector<std::pair<std::string, std::string>> headers,
                       std::chrono::seconds timeout,
                       ModelBodySink on_body) {
                    OutboundRequest request{
                        "POST", url, std::move(body), std::move(headers), timeout};
                    request.body_limit = 64 * 1024 * 1024;
                    // A 2xx answer goes on as it arrives (lake#665); a refusal is kept whole,
                    // for its message.
                    auto refusal = std::make_shared<std::string>();
                    if (on_body) {
                        request.on_body = [on_body, refusal](int status, std::string_view piece) {
                            if (status >= 200 && status < 300) {
                                return on_body(piece);
                            }
                            if (refusal->size() < 64 * 1024) {
                                refusal->append(piece);
                            }
                            return true;
                        };
                    }
                    OutboundResponse response = outbound_http.request(std::move(request));
                    if (on_body && (response.status < 200 || response.status >= 300)) {
                        response.body = std::move(*refusal);
                    }
                    return ModelProviderResponse{
                        response.status, std::move(response.body), std::move(response.error)};
                });
        }
        register_core_capabilities(registry, version_string());
        // Billing only where it is on (LAKE_BILLING, lake#564): off, its capabilities are absent.
        if (options.billing) {
            payments = options.payment_providers;
            if (!payments) {
                // Stripe over the server's outbound client with the keys lake was started with,
                // else the manual provider and why (docs/api/billing.md#where-the-keys-live).
                const std::string base = options.stripe_api_base;
                payments = environment_payment_providers(
                    options.stripe_keys,
                    options.billing_instructions,
                    [this, base](const StripeKeys& keys) -> std::shared_ptr<PaymentProvider> {
                        return stripe_payment_provider(
                            keys,
                            options.public_host,
                            [this](const StripeRequest& request) {
                                OutboundResponse response = outbound_http.request(
                                    {request.method, request.url, request.body, request.headers});
                                return StripeResponse{response.status,
                                                      std::move(response.body),
                                                      std::move(response.error)};
                            },
                            base);
                    });
            }
            register_billing_capabilities(registry, payments);
        }
        order_limiter = std::make_unique<OrderRateLimiter>(
            options.order_sheet.orders_per_client_per_hour, options.order_sheet.orders_per_hour);
        order_status_key = options.instance_kek;
        if (order_status_key.empty()) {
            // No instance key (a local Lake): status links work until this process stops.
            std::array<unsigned char, 32> random{};
            if (RAND_bytes(random.data(), static_cast<int>(random.size())) == 1) {
                for (const unsigned char byte : random) {
                    order_status_key += std::format("{:02x}", byte);
                }
            }
        }
        if (options.order_sheet.enabled && options.regions.order_sheet_url.empty() &&
            !options.regions.apex_host.empty()) {
            options.regions.order_sheet_url = "https://" + options.regions.apex_host + "/";
        }
        openapi_body = openapi_document(registry, version_string()).dump();
        agent_openapi_body =
            openapi_document(registry, version_string(), SchemaAudience::AgentTask).dump();
        mcp_tool_digest = mcp_tool_list_digest(registry);
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

    std::expected<nlohmann::json, ApiError>
    request_replication_batch(std::string source_url, std::uint64_t after_tree_size) const {
        while (source_url.ends_with('/')) {
            source_url.pop_back();
        }
        std::vector<std::pair<std::string, std::string>> headers{
            {"Accept", "application/json"},
            {"Content-Type", "application/json"},
            {"User-Agent", "lake-replication/1"},
        };
        if (!options.replication_token.empty()) {
            headers.emplace_back("Authorization", "Bearer " + options.replication_token);
        }
        // lake#915: the capability has already refused a source that is not a configured peer;
        // the request also reaches only HTTPS on public addresses, and waits a bounded time.
        OutboundRequest request{"POST",
                                source_url + "/v1/export_transparency_replication_batch",
                                nlohmann::json{{"after_tree_size", after_tree_size}}.dump(),
                                std::move(headers)};
        request.timeout =
            std::min<std::chrono::seconds>(options.replication_timeout, std::chrono::seconds(60));
        request.public_only = !options.replication_allow_private_addresses;
        const OutboundResponse response = outbound_http.request(std::move(request));
        if (!response.error.empty()) {
            return std::unexpected(ApiError{
                "store_unavailable", "replication peer request failed: " + response.error, {}});
        }
        const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            return std::unexpected(
                ApiError{"unrecoverable", "replication peer returned invalid JSON", {}});
        }
        if (response.status < 200 || response.status >= 300 || !body.value("ok", false)) {
            const nlohmann::json error = body.value("error", nlohmann::json::object());
            return std::unexpected(
                ApiError{error.value("code", "store_unavailable"),
                         error.value("message", "replication peer refused request"),
                         error.value("path", "")});
        }
        if (!body.contains("value")) {
            return std::unexpected(
                ApiError{"unrecoverable", "replication peer returned no value", {}});
        }
        return body.at("value");
    }

    void record_request(const boost::http::route_params& route,
                        std::string_view route_pattern,
                        std::chrono::steady_clock::time_point start) {
        const double duration_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const std::string method(route.req.method_text());
        const int status_code = route.res.status_int();
        const std::string status = std::to_string(status_code);
        const telemetry::Labels labels{
            {"method", method}, {"route", std::string(route_pattern)}, {"status", status}};
        metrics.increment_counter("lake_http_requests_total", labels);
        metrics.observe_histogram("lake_http_request_duration_seconds", labels, duration_seconds);

        // What the request's capability calls resolved (lake#384). A request that resolved no
        // workspace is not counted in any workspace's series.
        const auto timing = request_timing(route);
        const RequestAttribution::Snapshot resolved =
            timing ? timing->attribution->snapshot() : RequestAttribution::Snapshot{};
        const std::string capability =
            request_log_capability_route(route_pattern) ? resolved.capability : std::string();
        if (request_recorder && !resolved.workspace_id.empty()) {
            request_recorder->add(
                resolved.workspace_id, std::string(route_pattern), capability, status_code);
        }
        // The same attribution counts the workspace's API and MCP calls over time (lake#564).
        if (usage_recorder && !resolved.workspace_id.empty() && !capability.empty()) {
            usage_recorder->add(resolved.workspace_id,
                                "calls",
                                1,
                                {{"kind", route_pattern.ends_with("/mcp") ? "mcp" : "api"}});
        }

        // Route patterns and identifiers only, never the raw path (decision 54 S9).
        RequestLogEntry entry{.ts = log_timestamp(),
                              .method = method,
                              .route = std::string(route_pattern),
                              .status = status_code,
                              .duration_ms = duration_seconds * 1000.0,
                              .node = options.node_name,
                              .request_id = resolved.request_id,
                              .capability = capability,
                              .ids = request_log_ids(route.params)};
        if (entry.request_id.empty() && route.req.count("x-request-id") != 0) {
            entry.request_id = std::string(route.req.at("x-request-id"));
        }
        if (!resolved.workspace_id.empty()) {
            entry.ids["workspace_id"] = resolved.workspace_id;
        }
        std::println("{}", request_log_line(entry));
    }

    boost::http::route_task observe_request(boost::http::route_params& route,
                                            std::string route_pattern,
                                            boost::http::route_task task) {
        const auto timing = request_timing(route);
        const auto start = timing ? timing->started : std::chrono::steady_clock::now();
        const RequestTimingScope timing_scope(route, start);
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
            "/deployments",
            "/design/projects",
            "/design/project",
            "/design/projects/create",
            "/design/project/compile",
            "/design/project/approve",
            "/design/project/run",
            "/train/data",
            "/train/runs",
            "/train/run",
            "/ship/models",
            "/ship/models/publish",
            "/ship/indexes",
            "/ship/packages",
            "/workspace/start",
            "/workspace/delete",
            "/design/pipeline",
            "/ship/index",
            "/train/dataset",
            "/train/dataset/promote",
            "/train/query",
            "/train/data/generate",
            "/ship/index/publish",
            "/ship/indexes/create",
            "/design/pipeline/edit",
            "/design/pipeline/approve",
            "/design/pipeline/gate",
            "/design/pipeline/run",
            "/design/pipeline/deploy",
            "/design/pipeline/ask",
            "/design/opportunities",
            "/design/agent-library",
            "/design/opportunities/scan",
            "/design/opportunities/create",
            "/design/opportunities/dismiss",
            "/design/solutions/start",
            "/design/pipeline/apply",
            "/design/project/new-pipeline",
            "/design/project/n8n",
            "/design/project/python",
            "/services",
            "/services/create",
            "/services/delete",
            "/services/connection",
            "/train/data/upload",
            "/design/project/delete",
            "/train/data/connect",
            "/train/data/connect-service",
            "/train/data/disconnect",
            "/train/data/pull",
            "/train/data/store-create",
            "/train/data/store-delete",
            "/train/data/store-connection",
            "/environments",
            "/environment",
            "/logs",
            "/platform/env/set",
            "/platform/env/unset",
            "/platform/domain/create",
            "/platform/domain/delete",
            "/file",
            "/changes",
            "/change",
            "/issues",
            "/issue",
            "/releases",
            "/webhooks",
            "/webhooks/retry",
            "/protection",
            "/conflicts",
            "/conflicts/resolve",
            "/artifact",
            "/drive",
            "/drive/preview",
            "/drive/download",
            "/drive/history",
            "/drive/history/diff",
            "/drive/conflict",
            "/drive/conflict/resolve",
            "/drive/folder",
            "/drive/upload",
            "/drive/rename",
            "/drive/move",
            "/drive/delete",
            "/drive/share",
            "/drive/share/link",
            "/shared-with-me",
            "/search",
            "/metering",
            "/transparency",
            "/secret-mounts",
            "/secrets",
            "/encryption-keys",
            "/audit",
            "/activity",
            "/sign-up",
            "/sign-in",
            "/apps/authorize",
            "/v1/apps/auth",
            "/mfa",
            "/passkeys",
            "/passkey-sign-in",
            "/sessions",
            "/tokens",
            "/developer/api",
            "/developer/mcp",
            "/docs",
            "/docs/:page",
            "/developer/mcp/key",
            "/developer/mcp/skill",
            "/skills/lake/SKILL.md",
            "/llms.txt",
            "/llms-full.txt",
            "/identity/sign-up",
            "/identity/sign-in",
            "/identity/clerk-session",
            "/identity/clerk-webhook",
            "/billing/stripe/webhook",
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
        const std::string workspace_local = workspace_prefixed_local_path(path);
        if (!workspace_local.empty() && !workspace_path_id(path).empty()) {
            if (workspace_local == "/v1/apps/auth") {
                return "/w/:workspace_id/v1/apps/auth";
            }
            if (workspace_local == "/v1/openapi.json" || workspace_local == "/mcp") {
                return "/w/:workspace_id" + workspace_local;
            }
            for (const auto& capability : registry.list()) {
                const std::string pattern = rest_path(*capability);
                if (workspace_local == pattern) {
                    return "/w/:workspace_id" + pattern;
                }
            }
        }
        if (path == web_ui_stylesheet_url()) {
            return path;
        }
        for (const std::string_view font : web_ui_font_names()) {
            if (path == web_ui_font_url(font)) {
                return path;
            }
        }
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
        if (path.starts_with("/v1/saml/")) {
            for (const std::string_view leaf : {"/metadata", "/start", "/acs"}) {
                if (path.ends_with(leaf)) {
                    return "/v1/saml/:provider" + std::string(leaf);
                }
            }
        }
        if (path.starts_with("/scim/v2/Users/")) {
            return "/scim/v2/Users/:id";
        }
        if (path.starts_with("/scim/v2/Groups/")) {
            return "/scim/v2/Groups/:id";
        }
        return "unmatched";
    }

    // The library agent a bus credential belongs to, or empty. Every token is compared, so the
    // time taken does not say which one was close.
    std::string bus_agent_for(const std::string& bearer) const {
        std::string found;
        if (bearer.empty() || !options.workspaces) {
            return found;
        }
        for (const auto& [agent, token] : options.workspaces->config().agent_bus_tokens) {
            if (constant_time_equal(bearer, token)) {
                found = agent;
            }
        }
        return found;
    }

    ApiContext context_for(const boost::http::route_params& route) const {
        const auto timing = request_timing(route);
        const bool page_read = route.req.method() == boost::http::method::get;
        if (page_read && timing && timing->context) {
            return *timing->context;
        }
        ApiContext ctx;
        // A capability that long-polls gives its HTTP worker back (lake#534).
        if (server) {
            ctx.leave_worker = [connections = server.get(), token = server->request_token(route)](
                                   const std::string& principal) {
                return connections->leave_worker(token, principal);
            };
        }
        ctx.request_path = std::string(route.url.path());
        ctx.request_query = std::string(route.url.encoded_query());
        ctx.path_workspace_id = workspace_path_id(ctx.request_path);
        ctx.workspace_id = ctx.path_workspace_id;
        ctx.store = options.store.get();
        ctx.search = options.search.get();
        ctx.pki = &pki;
        ctx.network_records = options.network_records.get();
        ctx.agents = &agents;
        ctx.platform = options.platform.get();
        ctx.workspaces = options.workspaces.get();
        if (timing) {
            ctx.workspace_call_timing = timing->workspace_calls;
            ctx.request_attribution = timing->attribution;
        }
        ctx.non_expiring_credentials_allowed = options.allow_non_expiring_credentials;
        ctx.agent_caller = options.agent_caller.get();
        ctx.agent_bus = options.agent_bus.get();
        ctx.apps_wildcard = options.apps_wildcard;
        ctx.apps_runtime_image = options.apps_runtime_image;
        ctx.methodology_runtime_image = options.methodology_runtime_image;
        ctx.mcp_runner_image = options.mcp_runner_image;
        ctx.tunnel_clients_dir = options.tunnel_clients_dir;
        ctx.regions = &options.regions;
        // Build registry targets use the in-cluster authority (LAKE_INTERNAL_URL).
        ctx.internal_url = options.internal_url;
        ctx.apps_reserved_names = options.apps_reserved_names;
        ctx.apps_per_customer = options.apps_per_customer;
        ctx.app_replicas_per_customer = options.app_replicas_per_customer;
        ctx.callers = callers.get();
        ctx.transparency = transparency.get();
        ctx.checkpoints = checkpoints.get();
        ctx.transparency_log = transparency_log.get();
        ctx.transparency_submitter = [this](const std::string& endpoint,
                                            const nlohmann::json& request) {
            return submit_transparency(endpoint, request);
        };
        configure_timestamp_context(ctx);
        ctx.replication_requester = [this](const std::string& source_url,
                                           std::uint64_t after_tree_size) {
            return request_replication_batch(source_url, after_tree_size);
        };
        ctx.replication_token = options.replication_token;
        ctx.replication_peers = options.replication_peers;
        ctx.node_name = options.node_name;
        ctx.identity_seed = options.identity_seed;
        ctx.instance_kek = options.instance_kek;
        ctx.sync_store = &sync_store;
        ctx.metering = options.metering;
        ctx.metering_recorder = metering_recorder.get();
        ctx.audit_recorder = audit_recorder.get();
        ctx.request_recorder = request_recorder.get();
        ctx.usage_recorder = usage_recorder.get();
        if (artifacts.available()) {
            ctx.workspace_content = &workspace_content;
        }
        ctx.public_host = options.public_host;
        ctx.external_origin = options.external_origin;
        ctx.clerk_publishable_key = options.clerk_publishable_key;
        ctx.sealing_public_key = options.sealing_public_key;
        ctx.transcriber = transcriber;
        ctx.mcp_http = mcp_http;
        ctx.mcp_allow_private_addresses = options.mcp_allow_private_addresses;
        ctx.oidc_discovery = oidc_discovery;
        ctx.store_browser = store_browser;
        ctx.model_provider = model_provider;
        ctx.payments = payments;
        ctx.object_transport = object_transport;
        ctx.artifact_catalog = [this] {
            nlohmann::json catalog = artifacts.catalog();
            catalog["available"] = artifacts.available();
            return catalog;
        };
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
            // A delivery waits at most 10 s in all (lake#772), and reaches only HTTPS on public
            // addresses (lake#876). The capability calls this through run_blocking, never on the
            // server's own thread.
            OutboundRequest request{"POST", url, payload, std::move(outbound_headers)};
            request.timeout = std::chrono::seconds(10);
            request.public_only = !options.webhook_allow_private_addresses;
            const OutboundResponse response = outbound_http.request(std::move(request));
            return WebhookDeliveryResult{response.status, response.error};
        };
        if (route.req.count(boost::http::field::user_agent) != 0) {
            ctx.device = std::string(route.req.at(boost::http::field::user_agent));
        }
        // Authorization: the bearer wins; else the HttpOnly session cookie (its
        // value IS the credential secret, validated identically), so the web app
        // authenticates with the cookie a sign-in route set and never sees the secret.
        std::string bearer = bearer_of(route);
        if (!bearer.empty()) {
            ctx.authentication_method = "bearer";
        } else {
            bearer = cookie_value(route, "lake_session");
            if (!bearer.empty()) {
                ctx.authentication_method = "session";
            }
        }
        if (!options.admin_token.empty() && !bearer.empty() &&
            constant_time_equal(bearer, options.admin_token)) {
            ctx.authorized = true;
            ctx.acting_identity = "admin";
            ctx.operator_credential = true;
        } else if (const std::string agent = bus_agent_for(bearer); !agent.empty()) {
            // A library agent on the agent bus (docs/api/agent-bus.md): it may only reach the
            // bus's capabilities, which check for this kind and this agent themselves.
            ctx.authorized = true;
            ctx.acting_identity = "agent-service:" + agent;
            ctx.credential_kind = "agent_service";
            ctx.agent_service = agent;
        } else if (!bearer.empty() && options.store) {
            auto principal = options.store->context_for(bearer);
            if (principal && principal->pool_id != "lake") {
                // An identity application's session (lake#912). Grants name a bare TAAP identity,
                // and an application pool may hold a user whose identity is a Lake account's, so
                // this session is no credential here: not for the API, MCP, the cookie routes,
                // downloads or /metrics. The registry admits it only to the capabilities that act
                // on the user's own sign-in in its pool (Capability::serves_application_pools).
                ctx.application_session = *principal;
            } else if (principal) {
                ctx.authorized = true;
                ctx.session_credential = true;
                ctx.acting_user_id = principal->user_id;
                ctx.acting_identity = principal->taap_id;
                ctx.credential_device_id = principal->device_id;
                ctx.credential_session_id = principal->session_id;
                ctx.credential_kind = principal->kind;
                ctx.credential_workload_id = principal->workload_id;
                ctx.credential_customer_id = principal->workload_customer_id;
                ctx.credential_repository_id = principal->workload_repository_id;
                ctx.credential_revision = principal->workload_revision;
                ctx.credential_token_hash = principal->workload_token_hash;
                ctx.credential_expires_at_unix = principal->expires_at_unix;
                ctx.pool_id = principal->pool_id;
                ctx.scopes = principal->scopes;
            } else {
                // Said apart so a caller knows to renew rather than fix its key (lake#450).
                ctx.credential_rejection =
                    principal.error().code == "expired" ? "expired" : "invalid";
            }
        } else if (!bearer.empty()) {
            ctx.credential_rejection = "invalid";
        }
        if (route.req.count("x-request-id") != 0) {
            ctx.request_id = std::string(route.req.at("x-request-id"));
        } else {
            ctx.request_id = control::new_uuid_v4();
        }
        if (timing) {
            std::lock_guard lock(timing->attribution->mutex);
            if (timing->attribution->request_id.empty()) {
                timing->attribution->request_id = ctx.request_id;
            }
        }
        if (route.req.count("liquid-offline-grant") != 0) {
            ctx.offline_grant_id = std::string(route.req.at("liquid-offline-grant"));
        }
        if (page_read && timing) {
            timing->context = ctx;
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

    ApiContext app_auth_audit_context(const boost::http::route_params& route,
                                      const control::WebApp* app,
                                      std::string_view user_id = {}) const {
        ApiContext context;
        context.store = options.store.get();
        context.identity_seed = options.identity_seed;
        context.instance_kek = options.instance_kek;
        context.authentication_method = "app_forward_auth";
        context.acting_user_id = std::string(user_id);
        context.acting_identity = user_id.empty() ? "anonymous" : "user/" + std::string(user_id);
        context.request_id = route.req.count("x-request-id") == 0
                                 ? control::new_uuid_v4()
                                 : std::string(route.req.at("x-request-id"));
        if (app != nullptr) {
            context.credential_customer_id = app->customer_id;
        }
        return context;
    }

    void audit_app_auth_denial(const boost::http::route_params& route,
                               std::string_view host,
                               std::string_view reason,
                               const control::WebApp* app = nullptr) {
        const auto now = std::chrono::steady_clock::now();
        const std::string key = std::string(host) + "\n" + std::string(reason);
        {
            std::lock_guard lock(app_auth_audit_mutex);
            const auto found = app_auth_denials.find(key);
            if (found != app_auth_denials.end() && now - found->second < std::chrono::minutes(1)) {
                return;
            }
            app_auth_denials[key] = now;
            if (app_auth_denials.size() > 1'024) {
                std::erase_if(app_auth_denials, [&](const auto& entry) {
                    return now - entry.second >= std::chrono::minutes(1);
                });
            }
        }
        ApiContext context = app_auth_audit_context(route, app);
        (void)record_direct_event(context,
                                  audit::EventDraft{
                                      .customer_id = app == nullptr ? "" : app->customer_id,
                                      .kind = "authentication",
                                      .object = "app/" + std::string(host),
                                      .action = "forward_auth",
                                      .outcome = "denied",
                                      .method = "app_forward_auth",
                                      .details = {{"reason", reason}},
                                  });
    }

    boost::http::route_task send_app_auth_success(boost::http::route_params& route,
                                                  const control::WebApp& app,
                                                  std::string_view user_id,
                                                  std::string_view cookie = {}) {
        route.res.set("X-Orama-User", user_id);
        route.res.set("X-Orama-Space", app.workspace_id);
        if (!cookie.empty()) {
            route.res.set(boost::http::field::set_cookie, cookie);
        }
        return send_text(
            route, boost::http::status::ok, "", "text/plain; charset=utf-8", "no-store");
    }

    boost::http::route_task serve_app_forward_auth(boost::http::route_params& route) {
        namespace http = boost::http;
        const std::string host = app_request_host(route);
        const std::string workspace_id = workspace_path_id(std::string(route.url.path()));
        if (!options.store || host.empty()) {
            audit_app_auth_denial(route, host, "app_host_missing");
            co_return co_await send_text(
                route, http::status::unauthorized, "", "text/plain", "no-store");
        }

        const std::string session = cookie_value(route, "lake_app_session");
        if (!session.empty()) {
            const auto claims = control::verify_web_app_session(options.instance_kek, session);
            if (claims) {
                const auto app = options.store->authorize_web_app_session(
                    host, claims->workspace_id, claims->app_id, claims->user_id);
                if (app && (workspace_id.empty() || workspace_id == app->workspace_id)) {
                    co_return co_await send_app_auth_success(route, *app, claims->user_id);
                }
            }
        }

        const std::string code = original_app_code(route);
        if (!code.empty()) {
            const auto authorization = options.store->consume_web_app_authorization_code(
                host, workspace_id, control::identity_crypto::sha256_hex(code));
            if (authorization) {
                const auto token = control::issue_web_app_session(options.instance_kek,
                                                                  authorization->app.id,
                                                                  authorization->user_id,
                                                                  authorization->app.workspace_id);
                if (token) {
                    const std::string cookie =
                        "lake_app_session=" + *token + "; Path=/; Secure; HttpOnly; SameSite=Lax";
                    ApiContext context =
                        app_auth_audit_context(route, &authorization->app, authorization->user_id);
                    (void)record_direct_event(context,
                                              audit::EventDraft{
                                                  .customer_id = authorization->app.customer_id,
                                                  .kind = "authentication",
                                                  .principal = "user/" + authorization->user_id,
                                                  .object = "app/" + authorization->app.id,
                                                  .action = "exchange_code",
                                                  .outcome = "success",
                                                  .method = "app_forward_auth",
                                              });
                    co_return co_await send_app_auth_success(
                        route, authorization->app, authorization->user_id, cookie);
                }
            }
        }

        const auto app = options.store->resolve_web_app_auth_host(host, workspace_id);
        audit_app_auth_denial(route,
                              host,
                              code.empty() ? "credential_missing_or_invalid" : "code_invalid",
                              app ? &*app : nullptr);
        co_return co_await send_text(
            route, http::status::unauthorized, "", "text/plain", "no-store");
    }

    boost::http::route_task serve_app_authorize(boost::http::route_params& route) {
        namespace http = boost::http;
        const std::string app_id = query_value(route, "app");
        const std::string redirect_url = query_value(route, "rd");
        if (app_id.empty() || redirect_url.empty()) {
            co_return co_await send_text(route,
                                         http::status::bad_request,
                                         "app and rd are required",
                                         "text/plain; charset=utf-8",
                                         "no-store");
        }
        const std::string return_path = app_authorize_path(app_id, redirect_url);
        const ApiContext context = context_for(route);
        if (!context.authorized) {
            const std::string sign_in = "/sign-in?return_to=" + oauth_url_encode(return_path);
            co_return co_await redirect(
                route, sign_in, app_sign_in_return_cookie(route, return_path));
        }
        if (!options.store || context.acting_user_id.empty()) {
            co_return co_await send_text(route,
                                         http::status::forbidden,
                                         "You do not have access to this app.",
                                         "text/plain; charset=utf-8",
                                         "no-store");
        }
        const auto app = options.store->authorize_web_app(app_id, context.acting_user_id);
        if (!app) {
            const http::status status = app.error().code == "not_found" ? http::status::bad_request
                                                                        : http::status::forbidden;
            audit_app_auth_denial(route, app_id, app.error().code);
            co_return co_await send_text(route,
                                         status,
                                         status == http::status::forbidden
                                             ? "You do not have access to this app."
                                             : "The requested app does not exist.",
                                         "text/plain; charset=utf-8",
                                         "no-store");
        }
        const auto parsed = boost::urls::parse_uri(redirect_url);
        if (!parsed || parsed->scheme_id() != boost::urls::scheme::https ||
            !parsed->has_authority() || parsed->has_userinfo() || parsed->has_port() ||
            parsed->has_fragment() || parsed->host() != app->host) {
            audit_app_auth_denial(route, app->host, "redirect_invalid", &*app);
            co_return co_await send_text(route,
                                         http::status::bad_request,
                                         "rd must be an HTTPS URL on the app host.",
                                         "text/plain; charset=utf-8",
                                         "no-store");
        }
        const std::string code = control::identity_crypto::random_secret();
        const auto stored = options.store->create_web_app_authorization_code(
            control::WebAppAuthorizationCode{app->id,
                                             context.acting_user_id,
                                             app->host,
                                             control::identity_crypto::sha256_hex(code)});
        if (!stored) {
            co_return co_await send_text(route,
                                         http::status::service_unavailable,
                                         "Lake could not authorize this app.",
                                         "text/plain; charset=utf-8",
                                         "no-store");
        }
        boost::urls::url destination(*parsed);
        destination.params().set("__lake_code", code);
        co_return co_await redirect(
            route, destination.buffer(), clear_app_sign_in_return_cookie(route));
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
            // Back to the sign-in page, which says why (the web app reads `error` too, lake#561).
            co_return co_await redirect(route, "/login?error=That%20token%20did%20not%20match.");
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
        // End the session on the server too, so a copied cookie stops working (lake#159). An
        // operator credential is not a session and is left alone.
        if (context.authorized && !context.operator_credential && options.store &&
            !context.credential_session_id.empty()) {
            (void)options.store->revoke_session(context.acting_user_id,
                                                context.credential_session_id);
        }
        // An identity application's session signs out too, though it is no credential here
        // (lake#912). It is named by its user id, never by a TAAP identity a Lake account may hold.
        const auto& application = context.application_session;
        if (application && options.store) {
            (void)options.store->revoke_session(application->user_id, application->session_id);
        }
        if (!record_direct_event(
                context,
                audit::EventDraft{
                    .kind = "session",
                    .principal = application ? "user/" + application->user_id : std::string(),
                    .object = "lake_session",
                    .action = "revoke",
                    .outcome = context.authorized || application ? "revoked" : "denied",
                    .method = context.authentication_method,
                    .device = context.device,
                })) {
            return send_text(route,
                             boost::http::status::service_unavailable,
                             error_body("store_error", "could not record session revocation"),
                             "application/json; charset=utf-8",
                             "no-store");
        }
        // With Clerk, the browser also holds a Clerk session; the sign-in page ends it too, or the
        // next visit would sign the person straight back in.
        return redirect(route,
                        clerk ? "/sign-in?signed_out=1" : "/",
                        "lake_session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
    }

    boost::http::route_task serve_page_error(boost::http::route_params& route,
                                             const ApiResult& result) {
        return send_text(route,
                         boost::http::int_to_status(static_cast<unsigned>(rest_status(result))),
                         rest_envelope(result).dump(),
                         "application/json; charset=utf-8",
                         "no-store");
    }

    // A download's failure: without a credential, the sign-in page; else the API's own answer.
    boost::http::route_task serve_download_error(boost::http::route_params& route,
                                                 const ApiContext& context,
                                                 const ApiResult& result) {
        if (!context.authorized &&
            (result.error.code == "unauthorized" || result.error.code == "unauthenticated")) {
            return redirect(route, "/sign-in");
        }
        return serve_page_error(route, result);
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

    // The application a routing read found, with its settings from the record in its workspace
    // (lake#351): what a hosted page renders (name, theme) and where it sends a user back to
    // (redirect URIs). One get_record.
    liquid::runtime::task<std::expected<control::Application, ApiResult>>
    configured_for(control::Result<control::Application> routing,
                   const boost::http::route_params& route) {
        ApiContext context = context_for(route);
        co_return configured(co_await configured_application(std::move(routing), context));
    }

    static std::expected<control::Application, ApiResult> configured_result(
        std::expected<std::expected<control::Application, ApiResult>, liquid::error> completed) {
        return configured(std::move(completed));
    }

    boost::http::route_task send_application_error(boost::http::route_params& route,
                                                   const ApiResult& failure) {
        return send_text(route,
                         boost::http::int_to_status(static_cast<unsigned>(rest_status(failure))),
                         error_body(failure.error.code, failure.error.message),
                         "application/json; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task send_application_identity_page(boost::http::route_params& route,
                                                           const control::Application& application,
                                                           std::string_view page,
                                                           std::string_view error = {}) {
        std::string body;
        if (page == "sign-up") {
            body = web_ui_application_sign_up(application, error);
        } else if (page == "sign-in") {
            body = web_ui_application_sign_in(application, error);
        } else if (page == "mfa") {
            body = web_ui_application_mfa(application, error);
        } else {
            body = web_ui_application_passkey_sign_in(application, error);
        }
        return send_text(
            route, boost::http::status::ok, body, "text/html; charset=utf-8", "no-store");
    }

    // A hosted identity page renders the application's name and theme, its settings in its
    // workspace (lake#351).
    boost::http::route_task serve_application_identity_page(boost::http::route_params& route,
                                                            std::string page,
                                                            std::string error = {}) {
        auto application = co_await configured_for(application_for_route(route), route);
        const auto found = configured_result(std::move(application));
        if (!found) {
            co_return co_await send_application_error(route, found.error());
        }
        co_return co_await send_application_identity_page(route, *found, page, error);
    }

    boost::http::route_task serve_application_password(boost::http::route_params& route,
                                                       std::string_view capability) {
        namespace http = boost::http;
        // Read with its settings once: the page an error renders and the redirect URI the user
        // returns to are both the application's settings (lake#351).
        const auto application =
            configured_result(co_await configured_for(application_for_route(route), route));
        if (!application) {
            co_return co_await send_application_error(route, application.error());
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
        const ApiResult result =
            unwrap(co_await registry.call_async(std::string(capability), input, context));
        if (!result.ok && result.error.code == "mfa_required") {
            co_return co_await redirect(route, "/apps/" + application->id + "/mfa");
        }
        if (!result.ok) {
            const std::string page = capability == "sign_up_password"
                                         ? "sign-up"
                                         : (capability == "verify_mfa" ? "mfa" : "sign-in");
            co_return co_await send_application_identity_page(
                route, *application, page, result.error.message);
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
            JsonBodySink sink;
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
        const ApiResult result =
            unwrap(co_await registry.call_async(std::string(capability), input, context));
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
        const ApiResult result = unwrap(co_await registry.call_async(
            "start_oauth",
            {{"pool_id", application->pool_id},
             {"provider", form_field(body, "provider")},
             {"device_name", form_field(body, "device_name")},
             {"identity_document", form_field(body, "identity_document")}},
            context));
        if (!result.ok) {
            co_return co_await serve_application_identity_page(
                route, "sign-in", result.error.message);
        }
        co_return co_await redirect(
            route,
            result.value.value("authorization_url", "/apps/" + application->id + "/sign-in"));
    }

    // The public keys are read from the application's workspace Zetta (lake#352).
    boost::http::route_task serve_application_jwks(boost::http::route_params& route) {
        ApiContext context = context_for(route);
        auto completed = co_await registry.call_async(
            "application_jwks", {{"application_id", route_value(route, "id")}}, context);
        const ApiResult result = completed
                                     ? std::move(*completed)
                                     : ApiResult::failure("store_error", completed.error().message);
        co_return co_await send_text(
            route,
            boost::http::int_to_status(static_cast<unsigned>(rest_status(result))),
            result.ok ? result.value.dump() : rest_envelope(result).dump(),
            "application/json; charset=utf-8",
            result.ok ? "public, max-age=300" : "no-store");
    }

    // GET /sign-up, /sign-in, /mfa and /passkey-sign-in on an identity application's custom
    // sign-in host: the application's hosted page, with its settings read from its workspace
    // (lake#351). On lake's own host the web app answers these paths (docs/ui/web-app.md), so a
    // request that is not a browser's falls through to the 404.
    boost::http::route_task serve_custom_domain_page(boost::http::route_params& route,
                                                     std::string_view page) {
        auto routing = application_for_domain(route);
        if (!routing) {
            co_return boost::http::route_next;
        }
        const auto application =
            configured_result(co_await configured_for(std::move(routing), route));
        if (!application) {
            co_return co_await send_application_error(route, application.error());
        }
        co_return co_await send_application_identity_page(route, *application, page);
    }

    // lake's public origin for copy-ready instructions: the configured external origin, else the
    // public host, else the host this request came to.
    std::string public_origin(const boost::http::route_params& route) const {
        if (!options.external_origin.empty()) {
            return options.external_origin;
        }
        if (!options.public_host.empty()) {
            return "https://" + options.public_host;
        }
        const auto host = route.req.find(boost::http::field::host);
        return host == route.req.end() ? std::string() : "http://" + std::string(host->value);
    }

    // Public, like llms.txt, which links it: the skill holds no one's data, only how to use lake.
    boost::http::route_task serve_mcp_skill(boost::http::route_params& route) {
        route.res.set("content-disposition", "attachment; filename=SKILL.md");
        return send_text(route,
                         boost::http::status::ok,
                         agent_docs::skill_md(registry, public_origin(route)),
                         "text/markdown; charset=utf-8",
                         "public, max-age=300");
    }

    // Public: an agent learns lake from one URL (llmstxt.org).
    boost::http::route_task serve_llms(boost::http::route_params& route, bool full) {
        return send_text(route,
                         boost::http::status::ok,
                         full ? agent_docs::llms_full_txt(registry, public_origin(route))
                              : agent_docs::llms_txt(registry, public_origin(route)),
                         "text/plain; charset=utf-8",
                         "public, max-age=300");
    }

    // `public_only` for an identity provider's keys (lake#837): HTTPS to public addresses only,
    // unless LAKE_OIDC_ALLOW_PRIVATE_ADDRESSES is set. Clerk's, which the operator configures, is
    // not limited.
    std::optional<std::string>
    jwks_for(const std::string& url, bool refresh, bool public_only = false) {
        const auto found = jwks_cache.find(url);
        if (!refresh && found != jwks_cache.end() &&
            std::chrono::steady_clock::now() - found->second.fetched_at < std::chrono::hours(1)) {
            return found->second.document;
        }
        OutboundRequest request{
            "GET", url, "", {{"Accept", "application/json"}, {"User-Agent", "lake-oauth/1"}}};
        if (public_only) {
            request.timeout = oidc_timeout;
            request.public_only = true;
        }
        OutboundResponse response = outbound_http.request(request);
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

    // POST /identity/clerk-session (docs/api/clerk.md): the sign-in page posts Clerk's `lake`
    // template token once. It is verified against the instance JWKS (RS256, iss, aud = lake,
    // exp/nbf/iat), then the same session, audit and cookie path as OAuth sign-in runs. Failures
    // return to the sign-in page with a message rather than an API error.
    // POST /billing/stripe/webhook (docs/api/billing.md#webhooks, lake#620): Stripe's payment
    // notices. Unauthenticated: the Stripe-Signature HMAC is the credential, checked with
    // LAKE_STRIPE_WEBHOOK_SECRET. 404 where this lake does not bill or takes no notices.
    boost::http::route_task serve_payment_notice(boost::http::route_params& route) {
        namespace http = boost::http;
        if (!payments) {
            co_return co_await send_text(route,
                                         http::status::not_found,
                                         error_body("not_found", "this Lake does not bill"),
                                         "application/json; charset=utf-8",
                                         "no-store");
        }
        auto [read_error, body] = co_await read_string_body(route, std::size_t{1024} * 1024);
        if (read_error) {
            if (body_is_too_large(read_error)) {
                co_return co_await send_text(
                    route,
                    http::status::payload_too_large,
                    error_body("size_invalid", "a notice is at most 1 MiB"),
                    "application/json; charset=utf-8",
                    "no-store");
            }
            co_return http::route_error(read_error);
        }
        const std::string signature = route.req.count("stripe-signature") == 0
                                          ? std::string()
                                          : std::string(route.req.at("stripe-signature"));
        ApiContext context = context_for(route);
        context.registry = &registry;
        auto outcome = co_await receive_payment_notice(*payments, context, body, signature);
        if (!outcome) {
            co_return co_await send_text(route,
                                         http::status::service_unavailable,
                                         error_body("store_error", outcome.error().message),
                                         "application/json; charset=utf-8",
                                         "no-store");
        }
        co_return co_await send_text(route,
                                     static_cast<http::status>(outcome->status),
                                     outcome->body.dump(),
                                     "application/json; charset=utf-8",
                                     "no-store");
    }

    // POST /identity/clerk-webhook (docs/api/clerk.md#webhooks, lake#765): Clerk's user events,
    // delivered by Svix. Unauthenticated: the svix-signature HMAC with LAKE_CLERK_WEBHOOK_SECRET is
    // the credential. 404 without the secret.
    boost::http::route_task serve_clerk_webhook(boost::http::route_params& route) {
        namespace http = boost::http;
        if (options.clerk_webhook_secret.empty() || !options.store) {
            co_return co_await send_text(
                route,
                http::status::not_found,
                error_body("not_found", "Clerk webhooks are not configured"),
                "application/json; charset=utf-8",
                "no-store");
        }
        auto [read_error, body] = co_await read_string_body(route, std::size_t{1024} * 1024);
        if (read_error) {
            if (body_is_too_large(read_error)) {
                co_return co_await send_text(
                    route,
                    http::status::payload_too_large,
                    error_body("size_invalid", "an event is at most 1 MiB"),
                    "application/json; charset=utf-8",
                    "no-store");
            }
            co_return http::route_error(read_error);
        }
        const auto header = [&](std::string_view name) {
            return route.req.count(name) == 0 ? std::string() : std::string(route.req.at(name));
        };
        ApiContext context = context_for(route);
        context.authentication_method = "clerk_webhook";
        const auto outcome = clerk_webhooks::receive(
            context,
            options.clerk_webhook_secret,
            {header("svix-id"), header("svix-timestamp"), header("svix-signature"), body});
        co_return co_await send_text(route,
                                     static_cast<http::status>(outcome.status),
                                     outcome.body.dump(),
                                     "application/json; charset=utf-8",
                                     "no-store");
    }

    boost::http::route_task serve_clerk_session(boost::http::route_params& route) {
        namespace http = boost::http;
        // redirect() takes views and runs when awaited, so the location lives in this frame.
        std::string failure;
        const auto back = [&](std::string_view message) {
            failure = "/sign-in?error=" + oauth_url_encode(std::string(message));
            return redirect(route, failure);
        };
        if (!clerk || !options.store) {
            co_return co_await send_text(route,
                                         http::status::not_found,
                                         error_body("not_found", "Clerk sign-in is not configured"),
                                         "application/json; charset=utf-8",
                                         "no-store");
        }
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return http::route_error(read_error);
        }
        const std::string token = form_field(body, "token");
        std::string device_name = form_field(body, "device_name");
        if (device_name.empty()) {
            device_name = "browser";
        }
        const std::string audience(clerk_token_template);
        std::optional<control::identity_crypto::OidcClaims> claims;
        if (!token.empty()) {
            for (const bool refresh : {false, true}) {
                const std::optional<std::string> jwks = jwks_for(clerk->jwks_url, refresh);
                claims = jwks ? control::identity_crypto::verify_oidc_id_token(
                                    token, *jwks, clerk->issuer, audience, "")
                              : std::nullopt;
                if (claims) {
                    break;
                }
            }
        }
        ApiContext context = context_for(route);
        context.authentication_method = "clerk";
        if (!claims) {
            (void)record_direct_event(context,
                                      audit::EventDraft{
                                          .kind = "authentication",
                                          .object = "clerk",
                                          .action = "sign_in",
                                          .outcome = "denied",
                                          .method = "clerk",
                                          .device = device_name,
                                      });
            co_return co_await back("Your sign-in could not be verified. Please try again.");
        }
        const auto minted = audit::mint_custodial_identity(options.instance_kek);
        if (!minted) {
            co_return co_await back("Lake cannot create an identity right now: " +
                                    minted.error().message);
        }
        control::Result<control::IssuedSession> issued =
            options.store->sign_in_clerk({claims->subject,
                                          claims->email,
                                          claims->display_name,
                                          device_name,
                                          minted->taap_id,
                                          minted->public_key,
                                          minted->wrapped_seed});
        if (!issued) {
            co_return co_await back(issued.error().message);
        }
        if (const auto principal = options.store->context_for(issued->secret)) {
            context.authorized = true;
            context.acting_user_id = principal->user_id;
            context.acting_identity = principal->taap_id;
        }
        const ApiResult session_result = unwrap(
            co_await application_session_result(std::move(issued), context, "session.started"));
        if (!session_result.ok) {
            co_return co_await back(session_result.error.message);
        }
        if (!record_direct_event(context,
                                 audit::EventDraft{
                                     .kind = "authentication",
                                     .object = "clerk",
                                     .action = "sign_in",
                                     .outcome = "success",
                                     .method = "clerk",
                                     .device = device_name,
                                 })) {
            co_return co_await back("Lake could not record the sign-in. Please try again.");
        }
        std::string cookie = "lake_session=" + session_result.value.value("secret", "") +
                             "; HttpOnly; SameSite=Lax; Path=/";
        if (request_is_https(route)) {
            cookie += "; Secure";
        }
        // The page that sent the person here (`next`, lake#561), else the Ship app return.
        std::string destination = same_origin_next(form_field(body, "next"));
        if (destination.empty()) {
            destination = app_sign_in_return(route);
        }
        co_return co_await redirect(
            route, destination.empty() ? std::string(account_landing) : destination, cookie);
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
        // The endpoints are the provider's discovery document's (lake#837), read on the blocking
        // pool: an issuer that does not answer holds up only this request.
        auto discovered = co_await run_blocking([discovery = oidc_discovery, provider = *provider] {
            return oauth_endpoints(provider, discovery.get());
        });
        if (!discovered || !*discovered ||
            !oauth_endpoints_allowed(**discovered, options.oidc_allow_private_addresses)) {
            co_return co_await oauth_error(route,
                                           http::status::service_unavailable,
                                           !discovered    ? discovered.error().message
                                           : !*discovered ? (*discovered).error().message
                                                          : "OAuth endpoints must use HTTPS");
        }
        const OAuthEndpoints endpoints = std::move(**discovered);
        const bool public_only = !options.oidc_allow_private_addresses;
        const std::string callback = oauth_callback_url(options.public_host, provider_name);
        std::string token_body = "grant_type=authorization_code";
        token_body += "&code=" + oauth_url_encode(code);
        token_body += "&redirect_uri=" + oauth_url_encode(callback);
        token_body += "&client_id=" + oauth_url_encode(provider->client_id);
        token_body += "&client_secret=" + oauth_url_encode(provider->client_secret);
        token_body += "&code_verifier=" + oauth_url_encode(flow->code_verifier);
        OutboundRequest token_request{"POST",
                                      endpoints.token,
                                      token_body,
                                      {{"Accept", "application/json"},
                                       {"Content-Type", "application/x-www-form-urlencoded"},
                                       {"User-Agent", "lake-oauth/1"}},
                                      oidc_timeout};
        token_request.public_only = public_only;
        const OutboundResponse token_response = outbound_http.request(token_request);
        if (!token_response.error.empty() || token_response.status < 200 ||
            token_response.status >= 300) {
            co_return co_await oauth_error(
                route, http::status::service_unavailable, "OAuth token exchange failed");
        }
        const nlohmann::json token = nlohmann::json::parse(token_response.body, nullptr, false);
        if (token.is_discarded()) {
            co_return co_await oauth_error(
                route, http::status::service_unavailable, "OAuth token response was invalid");
        }

        control::OAuthIdentity identity;
        identity.pool_id = flow->pool_id;
        identity.provider = provider_name;
        identity.device_name = flow->device_name;
        identity.identity_document = flow->identity_document;
        if (provider_name == "github") {
            const std::string access_token = token.value("access_token", "");
            if (access_token.empty()) {
                co_return co_await oauth_error(route,
                                               http::status::service_unavailable,
                                               "GitHub did not return an access token");
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
                    route, http::status::service_unavailable, "GitHub user verification failed");
            }
            identity.subject = user["id"].is_string() ? user["id"].get<std::string>()
                                                      : std::to_string(user["id"].get<long long>());
            identity.email = user.value("email", "");
            identity.display_name = user.value("name", user.value("login", std::string()));
        } else {
            const std::string id_token = token.value("id_token", "");
            std::optional<std::string> jwks = jwks_for(endpoints.jwks, false, public_only);
            auto claims =
                jwks ? control::identity_crypto::verify_oidc_id_token(
                           id_token, *jwks, provider->issuer, provider->client_id, flow->nonce)
                     : std::nullopt;
            if (!claims) {
                jwks = jwks_for(endpoints.jwks, true, public_only);
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
        if (const auto principal = options.store->context_for(issued->secret);
            principal && principal->pool_id == "lake") {
            context.authorized = true;
            context.acting_user_id = principal->user_id;
            context.acting_identity = principal->taap_id;
        } else if (principal) {
            // An application pool's user acts as no Lake identity (lake#912); the audit names
            // it by its user id.
            context.authorized = false;
            context.acting_user_id = principal->user_id;
            context.acting_identity = "user/" + principal->user_id;
        }
        const std::string issued_pool = issued->session.pool_id;
        const ApiResult application_result = unwrap(
            co_await application_session_result(std::move(issued), context, "session.started"));
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
        std::string destination(account_landing);
        if (issued_pool != "lake") {
            // The redirect URI is the application's settings in its workspace (lake#351); when
            // they cannot be read, the user returns to the application's hosted sign-in.
            if (auto routing = options.store->get_application_by_pool(issued_pool)) {
                const std::string fallback = "/apps/" + routing->id + "/sign-in";
                const auto application =
                    configured_result(co_await configured_for(std::move(routing), route));
                destination = application
                                  ? application_redirect_location(*application, application_result)
                                  : fallback;
            }
        } else if (const std::string app_return = app_sign_in_return(route); !app_return.empty()) {
            destination = app_return;
        }
        co_return co_await redirect(route, destination, cookie);
    }

    // SAML sign-in (docs/api/sso.md#saml-sign-in, lake#836). The provider id comes from the
    // route; these three routes need no credential.
    static std::string saml_provider(const boost::http::route_params& route) {
        for (const auto& [name, value] : route.params) {
            if (name == "provider") {
                return value;
            }
        }
        return {};
    }

    static boost::http::status saml_status(std::string_view code) {
        namespace http = boost::http;
        if (code == "invalid_input") {
            return http::status::bad_request;
        }
        if (code == "not_found") {
            return http::status::not_found;
        }
        if (code == "unauthorized" || code == "conflict") {
            return http::status::forbidden;
        }
        return http::status::service_unavailable;
    }

    boost::http::route_task saml_error(boost::http::route_params& route,
                                       const std::string& provider_id,
                                       const ApiError& error) {
        ApiContext context = context_for(route);
        context.authentication_method = "saml";
        if (!record_direct_event(context,
                                 audit::EventDraft{
                                     .kind = "authentication",
                                     .object = "saml/" + provider_id,
                                     .action = "sign_in",
                                     .outcome = error.code == "unauthorized" ? "denied" : "failure",
                                     .method = "saml",
                                     .device = context.device,
                                     .details = {{"message", error.message}},
                                 })) {
            return send_text(route,
                             boost::http::status::service_unavailable,
                             error_body("store_error", "could not record the SAML failure"),
                             "application/json; charset=utf-8",
                             "no-store");
        }
        return send_text(route,
                         saml_status(error.code),
                         error_body(error.code, error.message),
                         "application/json; charset=utf-8",
                         "no-store");
    }

    // GET /v1/saml/<provider id>/metadata: Lake's service-provider metadata; its URL is the SP
    // entity id.
    boost::http::route_task serve_saml_metadata(boost::http::route_params& route) {
        namespace http = boost::http;
        const std::string provider_id = saml_provider(route);
        const auto provider = options.store && !options.public_host.empty()
                                  ? options.store->get_sso_provider(provider_id)
                                  : control::Result<control::SsoProvider>(std::unexpected(
                                        control::StoreError{"not_found", "no SAML provider"}));
        if (!provider || provider->protocol != "saml" || !provider->enabled) {
            co_return co_await send_text(route,
                                         http::status::not_found,
                                         error_body("not_found", "SAML provider is unavailable"),
                                         "application/json; charset=utf-8",
                                         "no-store");
        }
        co_return co_await send_text(route,
                                     http::status::ok,
                                     control::saml::sp_metadata(options.public_host, provider->id),
                                     "application/samlmetadata+xml; charset=utf-8",
                                     "public, max-age=300");
    }

    // GET /v1/saml/<provider id>/start[?return_to=/path]: records a one-use AuthnRequest and
    // redirects to the identity provider. The provider's sign-on URL for its app tile.
    boost::http::route_task serve_saml_start(boost::http::route_params& route) {
        const std::string provider_id = saml_provider(route);
        if (!options.store) {
            co_return co_await saml_error(
                route, provider_id, ApiError{.code = "store_unavailable", .message = "no store"});
        }
        const auto started = start_saml_sign_in(*options.store,
                                                options.public_host,
                                                provider_id,
                                                "browser",
                                                "",
                                                same_origin_next(query_value(route, "return_to")));
        if (!started) {
            co_return co_await saml_error(route, provider_id, started.error());
        }
        co_return co_await redirect(route, *started);
    }

    // POST /v1/saml/<provider id>/acs: the Assertion Consumer Service (HTTP-POST binding). The
    // form's SAMLResponse is verified like sign_in_saml; RelayState is not used. It sets
    // lake_session and redirects to the page the request named, else the home.
    boost::http::route_task serve_saml_acs(boost::http::route_params& route) {
        namespace http = boost::http;
        const std::string provider_id = saml_provider(route);
        if (!options.store) {
            co_return co_await saml_error(
                route, provider_id, ApiError{.code = "store_unavailable", .message = "no store"});
        }
        auto [read_error, body] =
            co_await read_string_body(route, control::saml::maximum_document_bytes * 2);
        if (read_error) {
            if (body_is_too_large(read_error)) {
                co_return co_await saml_error(
                    route,
                    provider_id,
                    ApiError{.code = "invalid_input", .message = "the SAML response is too large"});
            }
            co_return http::route_error(read_error);
        }
        auto accepted = accept_saml_response(*options.store,
                                             options.public_host,
                                             provider_id,
                                             form_field(body, "SAMLResponse"),
                                             "",
                                             "browser",
                                             saml_now());
        if (!accepted) {
            co_return co_await saml_error(route, provider_id, accepted.error());
        }
        control::Result<control::IssuedSession> issued =
            options.store->sign_in_sso(accepted->identity);
        if (!issued) {
            co_return co_await saml_error(route,
                                          provider_id,
                                          ApiError{.code = issued.error().code == "unauthorized"
                                                               ? "unauthorized"
                                                               : "store_error",
                                                   .message = issued.error().message});
        }
        ApiContext context = context_for(route);
        context.authentication_method = "saml";
        if (const auto principal = options.store->context_for(issued->secret)) {
            context.authorized = true;
            context.acting_user_id = principal->user_id;
            context.acting_identity = principal->taap_id;
        }
        const std::string issued_pool = issued->session.pool_id;
        const ApiResult session_result = unwrap(
            co_await application_session_result(std::move(issued), context, "session.started"));
        if (!session_result.ok) {
            co_return co_await saml_error(route, provider_id, session_result.error);
        }
        if (!record_direct_event(context,
                                 audit::EventDraft{
                                     .kind = "authentication",
                                     .object = "saml/" + provider_id,
                                     .action = "sign_in",
                                     .outcome = "success",
                                     .method = "saml",
                                     .device = accepted->identity.device_name,
                                 })) {
            co_return co_await send_text(route,
                                         http::status::service_unavailable,
                                         error_body("store_error", "SAML audit recording failed"),
                                         "application/json; charset=utf-8",
                                         "no-store");
        }
        std::string cookie = "lake_session=" + session_result.value.value("secret", "") +
                             "; HttpOnly; SameSite=Lax; Path=/";
        if (request_is_https(route)) {
            cookie += "; Secure";
        }
        std::string destination = same_origin_next(accepted->return_to);
        if (destination.empty()) {
            destination = std::string(account_landing);
        }
        if (issued_pool != "lake") {
            // An application's pool returns to the application, as the OIDC callback does.
            if (auto routing = options.store->get_application_by_pool(issued_pool)) {
                const std::string fallback = "/apps/" + routing->id + "/sign-in";
                const auto application =
                    configured_result(co_await configured_for(std::move(routing), route));
                destination = application
                                  ? application_redirect_location(*application, session_result)
                                  : fallback;
            }
        }
        co_return co_await redirect(route, destination, cookie);
    }

    // A workspace-scoped server route names a live workspace, or answers 404: /w/<id>/v1/...,
    // /w/<id>/mcp, /w/<id>/v1/openapi.json, the model gateway under /w/<id>/openai/v1/, and a
    // workspace's Zetta under
    // /w/<id>/workspaces/<space>/zetta/. Every other /w/<id>/ path belongs to the web app
    // (docs/ui/web-app.md), which a browser already has; anything else falls through to the 404.
    boost::http::route_task workspace_route(boost::http::route_params& route) {
        namespace http = boost::http;
        const std::string path(route.url.path());
        const std::string local = workspace_prefixed_local_path(path);
        if ((!workspace_api_path(local) && !local.starts_with("/workspaces/") &&
             !local.starts_with("/openai/v1/")) ||
            local == "/v1/apps/auth") {
            co_return http::route_next;
        }
        const std::string selected = workspace_path_id(path);
        if (selected.empty() || !options.store) {
            co_return co_await send_text(route,
                                         http::status::not_found,
                                         error_body("not_found", "no such workspace"),
                                         "application/json; charset=utf-8",
                                         "no-store");
        }
        const auto workspace = options.store->get_workspace(selected);
        if (!workspace || workspace->desired == "deleted") {
            co_return co_await send_text(route,
                                         http::status::not_found,
                                         error_body("not_found", "no such workspace"),
                                         "application/json; charset=utf-8",
                                         "no-store");
        }
        co_return http::route_next;
    }

    static ApiResult unwrap(std::expected<ApiResult, liquid::error> completed) {
        return completed ? std::move(*completed)
                         : ApiResult::failure("store_error", completed.error().message);
    }

    // ---- Downloads: the routes that answer bytes rather than a capability's JSON ----

    boost::http::route_task serve_export_download(boost::http::route_params& route) {
        ApiContext ctx = context_for(route);
        const std::string repository_id = query_value(route, "repository_id");
        if (repository_id.empty()) {
            co_return co_await redirect(route, "/repositories");
        }
        const ApiResult exported = unwrap(co_await registry.call_async(
            "export_repository",
            {{"repository_id", repository_id},
             {"include_git", query_value(route, "include_git") == "true"}},
            ctx));
        if (!exported.ok) {
            co_return co_await serve_download_error(route, ctx, exported);
        }
        route.res.set("content-disposition", "attachment; filename=lake-export.json");
        co_return co_await send_text(route,
                                     boost::http::status::ok,
                                     exported.value.at("archive").dump(2),
                                     "application/vnd.lake.export+json",
                                     "no-store");
    }

    boost::http::route_task serve_repository_file_content(boost::http::route_params& route,
                                                          bool download) {
        ApiContext context = context_for(route);
        const std::string repository_id = query_value(route, "repository_id");
        const std::string ref = query_value(route, "ref");
        const std::string path = query_value(route, "path");
        if (repository_id.empty() || ref.empty() || path.empty()) {
            return redirect(route, "/repositories");
        }
        const ApiResult result = registry.call(
            "repository_file", {{"id", repository_id}, {"ref", ref}, {"path", path}}, context);
        if (!result.ok) {
            return serve_download_error(route, context, result);
        }
        if (download) {
            route.res.set("content-disposition", "attachment; filename=download");
        }
        return send_text(route,
                         boost::http::status::ok,
                         json_string(result.value, "contents"),
                         "text/plain; charset=utf-8",
                         "no-store");
    }

    // The media types a browser shows as data and never runs as a page: the only ones a download
    // is served as (inline on request for a workspace file); anything else is
    // application/octet-stream.
    static bool displayable_media_type(std::string_view type) {
        static constexpr std::array types = std::to_array<std::string_view>({"image/png",
                                                                             "image/jpeg",
                                                                             "image/gif",
                                                                             "image/webp",
                                                                             "application/pdf",
                                                                             "text/plain",
                                                                             "text/csv",
                                                                             "text/markdown",
                                                                             "audio/mpeg",
                                                                             "audio/mp4",
                                                                             "audio/wav",
                                                                             "video/mp4",
                                                                             "video/webm"});
        return std::ranges::find(types, type) != types.end();
    }

    boost::http::route_task serve_drive_download(boost::http::route_params& route) {
        ApiContext context = context_for(route);
        const std::string repository_id = query_value(route, "repository_id");
        const std::string ref =
            query_value(route, "ref").empty() ? "main" : query_value(route, "ref");
        const std::string path = query_value(route, "path");
        const ApiResult result =
            registry.call("download_drive_file",
                          {{"repository_id", repository_id}, {"ref", ref}, {"path", path}},
                          context);
        if (!result.ok) {
            co_return co_await serve_download_error(route, context, result);
        }
        std::string filename = path_filename(path);
        std::ranges::replace(filename, '"', '_');
        std::ranges::replace(filename, '\\', '_');
        std::ranges::replace(filename, '\r', '_');
        std::ranges::replace(filename, '\n', '_');
        route.res.set("content-disposition", "attachment; filename=\"" + filename + "\"");
        // The file's own type when a browser only shows it (a CSV is text/csv, lake#587).
        const std::string media_type = json_string(result.value, "media_type");
        co_return co_await send_bytes(route,
                                      boost::http::status::ok,
                                      displayable_media_type(media_type)
                                          ? media_type
                                          : std::string("application/octet-stream"),
                                      context.sync_response,
                                      "\"" + json_string(result.value, "blob") + "\"");
    }

    // GET /tunnel-clients/<platform>?expires=&signature= (lake#745, docs/api/tunnel-clients.md):
    // one tunnel client, for a link tunnel_client_downloads signed. The link is the credential, so
    // a plain curl follows it without a session. The platform is checked against the allow-list
    // before anything is read, so it never names a path.
    boost::http::route_task serve_tunnel_client(boost::http::route_params& route) {
        const auto refuse = [&route](ApiResult refused) -> boost::http::route_task {
            return send_text(route,
                             boost::http::int_to_status(static_cast<unsigned>(rest_status(refused))),
                             refused.envelope().dump(),
                             "application/json; charset=utf-8",
                             "no-store");
        };
        const std::string platform = route_value(route, "platform");
        if (find_tunnel_client_platform(platform) == nullptr) {
            co_return co_await refuse(
                ApiResult::failure("not_found", "no tunnel client for platform " + platform));
        }
        const auto clients = read_tunnel_clients(options.tunnel_clients_dir);
        if (!clients) {
            co_return co_await refuse(
                ApiResult::failure("not_found", "this Lake has no tunnel clients"));
        }
        const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
        const TunnelClientLinkCheck check = check_tunnel_client_link(options.instance_kek,
                                                                     platform,
                                                                     clients->version,
                                                                     query_value(route, "expires"),
                                                                     query_value(route, "signature"),
                                                                     now);
        if (check != TunnelClientLinkCheck::valid) {
            ApiResult refused = ApiResult::failure(
                "unauthenticated",
                check == TunnelClientLinkCheck::expired
                    ? "this download link has expired; open Get credentials again for a new one"
                    : "this download link is not valid; open Get credentials again for a new one");
            refused.error.reason = check == TunnelClientLinkCheck::expired ? "expired" : "invalid";
            co_return co_await refuse(std::move(refused));
        }
        const auto client = std::ranges::find(clients->clients, platform, &TunnelClient::platform);
        if (client == clients->clients.end()) {
            co_return co_await refuse(
                ApiResult::failure("not_found", "no tunnel client for platform " + platform));
        }
        auto read = co_await run_blocking([path = client->path, size = client->size] {
            std::vector<std::byte> bytes(static_cast<std::size_t>(size));
            std::ifstream in(path, std::ios::binary);
            if (!in || !in.read(reinterpret_cast<char*>(bytes.data()),
                                static_cast<std::streamsize>(bytes.size()))) {
                bytes.clear();
            }
            return bytes;
        });
        if (!read || read->empty()) {
            co_return co_await refuse(
                ApiResult::failure("store_error", "the tunnel client could not be read"));
        }
        route.res.set("content-disposition", "attachment; filename=\"" + client->file_name + "\"");
        route.res.set("x-content-type-options", "nosniff");
        co_return co_await send_bytes(
            route, boost::http::status::ok, "application/octet-stream", *read,
            "\"" + client->sha256 + "\"");
    }

    // GET /drive/files/download?customer_id=&workspace_id=&drive_id=&id=[&inline=1]: a workspace
    // file (docs/api/drives.md). get_drive_entry authorizes and audits the read; the bytes stream
    // from the workspace Zetta's `lake_files` bucket one 1 MiB block at a time, opened as they go,
    // so a file is never held whole. `inline=1` asks to show it in the browser, which is honoured
    // only for media types a browser cannot run as a page.
    boost::http::route_task serve_workspace_file_download(boost::http::route_params& route) {
        ApiContext context = context_for(route);
        const std::string customer_id = query_value(route, "customer_id");
        const std::string workspace_id = query_value(route, "workspace_id");
        const std::string drive_id = query_value(route, "drive_id");
        const std::string id = query_value(route, "id");
        const ApiResult entry = unwrap(co_await registry.call_async("get_drive_entry",
                                                                    {{"customer_id", customer_id},
                                                                     {"workspace_id", workspace_id},
                                                                     {"drive_id", drive_id},
                                                                     {"id", id}},
                                                                    context));
        if (!entry.ok) {
            co_return co_await serve_download_error(route, context, entry);
        }
        if (entry.value.value("kind", "") != "file" || entry.value.value("state", "") != "ready") {
            co_return co_await serve_download_error(
                route,
                context,
                ApiResult::failure("not_found", "no such file in this drive", "/id"));
        }
        auto found = control::settled(
            co_await find_workspace_file(context, customer_id, workspace_id, drive_id, id));
        if (!found) {
            co_return co_await serve_download_error(
                route, context, ApiResult::failure(found.error().code, found.error().message));
        }
        const bool show =
            query_value(route, "inline") == "1" && displayable_media_type(found->content_type);
        std::string filename = found->name;
        for (char& c : filename) {
            if (c == '"' || c == '\\' || c == '\r' || c == '\n') {
                c = '_';
            }
        }
        route.res.set("content-disposition",
                      std::string(show ? "inline" : "attachment") + "; filename=\"" + filename +
                          "\"");
        // Only the file's own bytes, never a page: it is shown in the browser as data at most.
        route.res.set("Content-Security-Policy", "sandbox; default-src 'none'");
        auto shared = std::make_shared<ApiContext>(context);
        const std::string key = found->object_key;
        const std::uint64_t size = found->size;
        const std::string store_drive_id = found->store_drive_id;
        const std::string etag = !found->sha256.empty() ? found->sha256
                                 : !found->etag.empty() ? found->etag
                                                        : "size-" + std::to_string(size);
        co_return co_await stream_blob(
            route,
            "\"" + etag + "\"",
            size,
            [shared, customer_id, workspace_id, key, size, store_drive_id](
                std::uint64_t offset, std::span<std::byte> into) -> SyncStoreResult<std::uint64_t> {
                auto window = control::settled(liquid::runtime::block_on(read_workspace_file(
                    *shared, customer_id, workspace_id, key, size, offset, into.size(),
                    store_drive_id)));
                if (!window) {
                    return std::unexpected(
                        SyncStoreError{window.error().code, window.error().message});
                }
                std::memcpy(into.data(), window->data(), window->size());
                return static_cast<std::uint64_t>(window->size());
            },
            false,
            StreamedFile{show ? found->content_type : std::string("application/octet-stream")},
            workspace_id);
    }

    liquid::runtime::task<ApiResult>
    page_call(std::string capability, nlohmann::json input, ApiContext& context) {
        co_return unwrap(co_await registry.call_async(capability, input, context));
    }

    nlohmann::json drive_share_input(const boost::http::route_params& route) {
        nlohmann::json input{
            {"token", route_value(route, "token")},
            {"ref", query_value(route, "ref").empty() ? "main" : query_value(route, "ref")}};
        for (const auto& parameter : route.url.params()) {
            if (parameter.key == "path") {
                input["path"] = std::string(parameter.value);
            }
        }
        return input;
    }

    boost::http::route_task serve_drive_share_download(boost::http::route_params& route) {
        ApiContext context = context_for(route);
        const ApiResult result = unwrap(
            co_await page_call("download_drive_share_link", drive_share_input(route), context));
        if (!result.ok) {
            co_return co_await serve_page_error(route, result);
        }
        std::string filename = path_filename(json_string(result.value, "path"));
        std::ranges::replace(filename, '"', '_');
        std::ranges::replace(filename, '\\', '_');
        std::ranges::replace(filename, '\r', '_');
        std::ranges::replace(filename, '\n', '_');
        route.res.set("content-disposition", "attachment; filename=\"" + filename + "\"");
        co_return co_await send_bytes(route,
                                      boost::http::status::ok,
                                      json_string(result.value, "media_type"),
                                      context.sync_response);
    }

    control::Result<control::Repository> hub_repository(std::string_view hub_id) {
        if (!options.store) {
            return std::unexpected(
                control::StoreError{"store_unavailable", "no control-plane store is configured"});
        }
        return find_hub_repository(*options.store, hub_id);
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

    enum class RegistryAccess { Allowed, Missing, Denied };

    // What a registry credential may do, and for a workload credential the workspace it acts in:
    // a build pushes only into its own workspace's repositories and routes a repository on its
    // first push from there; an app pulls only its own workspace's image (lake#383).
    struct RegistryGrant {
        RegistryAccess access = RegistryAccess::Denied;
        std::string customer_id;
        std::string workspace_id;
        bool may_route = false;
        std::string build_image; // a build may route only `<image>` and `cache/<image>`
    };

    // A refusal in the Distribution API's shape.
    struct RegistryRefusal {
        boost::http::status status;
        std::string message;
        std::string code;
    };

    // A tenant repository resolved through its route: the records of `workspace_id` hold its
    // manifests and tags.
    struct TenantImage {
        std::string customer_id;
        std::string name;
        std::string workspace_id;
        ApiContext context;
    };

    bool registry_write_authorized(const boost::http::route_params& route) const {
        const std::string token = registry_token_of(route);
        return !options.admin_token.empty() && !token.empty() &&
               constant_time_equal(token, options.admin_token);
    }

    std::string oci_repository(const boost::http::route_params& route) const {
        const std::string repository = route_value(route, "repository");
        const std::string space = route_value(route, "space");
        const std::string category = route_value(route, "category");
        if (space.empty()) {
            return repository;
        }
        return space + "/" + (category.empty() ? "" : category + "/") + repository;
    }

    RegistryGrant registry_access(const boost::http::route_params& route,
                                  std::string_view repository,
                                  bool write) const {
        if (!write && repository.find('/') == std::string_view::npos) {
            return {RegistryAccess::Allowed};
        }
        const RegistryCredential credential = registry_credential_of(route);
        if (credential.token.empty()) {
            return {RegistryAccess::Missing};
        }
        if (!options.admin_token.empty() &&
            constant_time_equal(credential.token, options.admin_token)) {
            return {RegistryAccess::Allowed};
        }
        if (!options.store) {
            return {RegistryAccess::Denied};
        }
        const auto principal = options.store->context_for(credential.token);
        // An identity application's session pulls nothing (lake#912): its TAAP identity may be a
        // Lake account's, and pulls are authorized by that account's grants.
        if (!principal || principal->pool_id != "lake") {
            return {RegistryAccess::Denied};
        }
        if (principal->kind == "build_push") {
            if (!credential.username.empty() && credential.username != "lake") {
                return {RegistryAccess::Denied};
            }
            const auto customer = options.store->get_customer(principal->workload_customer_id);
            const auto build = options.store->get_build(principal->workload_id);
            if (!customer || !build || build->customer_id != customer->id ||
                build->workspace_id.empty()) {
                return {RegistryAccess::Denied};
            }
            const std::string prefix = customer->name + "/";
            if (!repository.starts_with(prefix)) {
                return {RegistryAccess::Denied};
            }
            return {RegistryAccess::Allowed,
                    customer->id,
                    build->workspace_id,
                    true,
                    build_target_image(build->target, customer->name)};
        }
        if (principal->kind == "app_pull") {
            if (write || credential.username != "app") {
                return {RegistryAccess::Denied};
            }
            const auto app = options.store->get_web_app(principal->workload_id);
            const auto customer = options.store->get_customer(principal->workload_customer_id);
            // An app without a workspace has no image to pull: its route could not be checked.
            if (!app || !customer || app->customer_id != customer->id ||
                app->workspace_id.empty()) {
                return {RegistryAccess::Denied};
            }
            if (repository != customer->name + "/" + app->name) {
                return {RegistryAccess::Denied};
            }
            return {RegistryAccess::Allowed, customer->id, app->workspace_id, false};
        }
        if (principal->kind == "methodology_run") {
            // A methodology run's pods pull its built image with the run's own credential
            // (orama-operator#291, lake#655): that one image repository, read only, while the run
            // is live.
            if (write || credential.username != "methodology-run") {
                return {RegistryAccess::Denied};
            }
            const auto run = options.store->get_methodology_run(principal->workload_id);
            const auto customer = options.store->get_customer(principal->workload_customer_id);
            if (!run || !customer || run->customer_id != customer->id || run->build_id.empty()) {
                return {RegistryAccess::Denied};
            }
            const auto build = options.store->get_build(run->build_id);
            const std::string image =
                build ? build_target_image(build->target, customer->name) : std::string();
            if (!build || build->customer_id != customer->id || image.empty() ||
                repository != customer->name + "/" + image) {
                return {RegistryAccess::Denied};
            }
            return {RegistryAccess::Allowed, customer->id, run->workspace_id, false};
        }
        if (principal->kind == "methodology_deployment") {
            // A served methodology model's serve pods pull its image with the deployment's own
            // credential (lake#690) whenever one (re)starts: that one image repository, read only,
            // while the deployment is present.
            if (write || credential.username != "methodology-deployment") {
                return {RegistryAccess::Denied};
            }
            const auto deployment =
                options.store->get_methodology_deployment(principal->workload_id);
            const auto customer = options.store->get_customer(principal->workload_customer_id);
            if (!deployment || !customer || deployment->customer_id != customer->id ||
                deployment->build_id.empty()) {
                return {RegistryAccess::Denied};
            }
            const auto build = options.store->get_build(deployment->build_id);
            const std::string image =
                build ? build_target_image(build->target, customer->name) : std::string();
            if (!build || build->customer_id != customer->id || image.empty() ||
                repository != customer->name + "/" + image) {
                return {RegistryAccess::Denied};
            }
            return {RegistryAccess::Allowed, customer->id, deployment->workspace_id, false};
        }
        if (credential.username == "app" || credential.username == "methodology-run" ||
            credential.username == "methodology-deployment") {
            return {RegistryAccess::Denied};
        }
        if (write || principal->taap_id.empty() ||
            (std::ranges::find(principal->scopes, "*") == principal->scopes.end() &&
             std::ranges::find(principal->scopes, "registry_pull") == principal->scopes.end())) {
            return {RegistryAccess::Denied};
        }
        const std::string space = std::string(repository.substr(0, repository.find('/')));
        const auto customers = options.store->list_customers(10000);
        if (!customers) {
            return {RegistryAccess::Denied};
        }
        const auto customer = std::ranges::find(*customers, space, &control::Customer::name);
        if (customer == customers->end()) {
            return {RegistryAccess::Denied};
        }
        if (!principal->user_id.empty() && customer->user_id == principal->user_id) {
            return {RegistryAccess::Allowed, customer->id};
        }
        const auto object = options.store->find_policy_object("customer", customer->id);
        if (!object) {
            return {RegistryAccess::Denied};
        }
        control::AuthorizationCache cache;
        const auto decision =
            control::authorize(*options.store, principal->taap_id, object->id, "read", cache);
        return decision && decision->allowed ? RegistryGrant{RegistryAccess::Allowed, customer->id}
                                             : RegistryGrant{RegistryAccess::Denied};
    }

    // Resolves a tenant repository to its workspace through oci_repository_routes, before any
    // workspace is read (lake#383, decision 54 S6). A build's first push routes it from the
    // build's workspace; anything else needs a route that exists (create_image_repository names
    // the workspace explicitly). A workload credential reaches only its own workspace's routes.
    std::expected<TenantImage, RegistryRefusal> tenant_image(const boost::http::route_params& route,
                                                             std::string_view repository,
                                                             const RegistryGrant& grant,
                                                             bool write) const {
        const auto tenant = tenant_repository(repository);
        if (!tenant) {
            return std::unexpected(RegistryRefusal{boost::http::status::bad_request,
                                                   "OCI repository name is invalid",
                                                   "NAME_INVALID"});
        }
        if (!options.store || !options.workspaces) {
            return std::unexpected(
                RegistryRefusal{boost::http::status::not_found,
                                "tenant image repositories need a control store and workspaces",
                                "NAME_UNKNOWN"});
        }
        std::optional<control::Customer> customer;
        if (!grant.customer_id.empty()) {
            if (const auto found = options.store->get_customer(grant.customer_id)) {
                customer = *found;
            }
        } else {
            customer = space_customer(*options.store, tenant->space);
        }
        if (!customer || customer->name != tenant->space) {
            return std::unexpected(RegistryRefusal{
                boost::http::status::not_found, "image repository is unknown", "NAME_UNKNOWN"});
        }
        const auto unavailable = [](const control::StoreError& error) {
            return RegistryRefusal{boost::http::status::service_unavailable,
                                   "image routes cannot be read: " + error.message,
                                   "UNAVAILABLE"};
        };
        auto routed = options.store->get_oci_repository_route(customer->id, tenant->name);
        if (!routed && routed.error().code != "not_found") {
            return std::unexpected(unavailable(routed.error()));
        }
        // A build's first push routes only its own target, `<image>` and `cache/<image>`: a
        // route never moves, so a build cannot claim another name for its workspace.
        const bool own_target =
            !grant.build_image.empty() &&
            (tenant->name == grant.build_image || tenant->name == "cache/" + grant.build_image);
        if (!routed && write && grant.may_route && own_target) {
            routed = options.store->create_oci_repository_route(
                customer->id, tenant->name, grant.workspace_id);
            if (!routed && routed.error().code == "invalid_input") {
                return std::unexpected(RegistryRefusal{
                    boost::http::status::bad_request, routed.error().message, "NAME_INVALID"});
            }
            if (!routed && routed.error().code == "conflict") {
                return std::unexpected(RegistryRefusal{
                    boost::http::status::forbidden, routed.error().message, "DENIED"});
            }
            if (!routed) {
                return std::unexpected(unavailable(routed.error()));
            }
        }
        if (!routed && write && grant.may_route) {
            return std::unexpected(
                RegistryRefusal{boost::http::status::forbidden,
                                "a build routes only its own target image and its cache",
                                "DENIED"});
        }
        if (!routed) {
            return std::unexpected(
                RegistryRefusal{boost::http::status::not_found,
                                "image repository " + std::string(repository) +
                                    " has no workspace; route it with create_image_repository",
                                "NAME_UNKNOWN"});
        }
        if (!grant.workspace_id.empty() && routed->workspace_id != grant.workspace_id) {
            return std::unexpected(RegistryRefusal{boost::http::status::forbidden,
                                                   "image repository " + std::string(repository) +
                                                       " belongs to another workspace",
                                                   "DENIED"});
        }
        ApiContext context = context_for(route);
        context.workspace_id = routed->workspace_id;
        return TenantImage{customer->id, tenant->name, routed->workspace_id, std::move(context)};
    }

    boost::http::route_task registry_refusal(boost::http::route_params& route,
                                             const RegistryRefusal& refusal) {
        return registry_error(route, refusal.status, refusal.message, true, refusal.code);
    }

    // A workspace that cannot be read: the pull or push may be retried.
    boost::http::route_task registry_workspace_error(boost::http::route_params& route,
                                                     const control::ZettaControlError& error) {
        return registry_error(route,
                              boost::http::status::service_unavailable,
                              "the image repository's workspace cannot be reached: " + error.code +
                                  ": " + error.message,
                              true,
                              "UNAVAILABLE");
    }

    boost::http::route_task registry_auth_error(boost::http::route_params& route,
                                                RegistryAccess access) {
        const boost::http::status status = access == RegistryAccess::Missing
                                               ? boost::http::status::unauthorized
                                               : boost::http::status::forbidden;
        if (status == boost::http::status::unauthorized) {
            route.res.set(boost::http::field::www_authenticate, "Basic realm=\"lake registry\"");
        }
        return registry_error(
            route, status, "registry credential does not grant this repository", true);
    }

    boost::http::route_task registry_error(boost::http::route_params& route,
                                           boost::http::status status,
                                           std::string message,
                                           bool oci = false,
                                           std::string_view oci_code = {}) {
        nlohmann::json body;
        if (oci) {
            const std::string code =
                oci_code.empty()
                    ? (status == boost::http::status::not_found ? "BLOB_UNKNOWN" : "DENIED")
                    : std::string(oci_code);
            body = {{"errors",
                     nlohmann::json::array(
                         {{{"code", std::move(code)}, {"message", std::move(message)}}})}};
        } else {
            body = {{"ok", false}, {"error", std::move(message)}};
        }
        return send_text(route, status, body.dump(), "application/json; charset=utf-8", "no-store");
    }

    boost::http::route_task serve_oci_ping(boost::http::route_params& route) {
        route.res.set("Docker-Distribution-Api-Version", "registry/2.0");
        return send_text(
            route, boost::http::status::ok, "{}", "application/json; charset=utf-8", "no-store");
    }

    // Authorizes a blob upload request and, for a tenant repository, resolves (or for a build's
    // first push of its own target, writes) its route. Blobs stay in the instance registry until
    // lake#383 phase B; the workspace records each blob its repositories received.
    struct UploadRefusal {
        RegistryAccess access;
        std::optional<RegistryRefusal> refusal;
    };
    std::expected<std::optional<TenantImage>, UploadRefusal>
    authorize_oci_upload(const boost::http::route_params& route, const std::string& repository) {
        const RegistryGrant grant = registry_access(route, repository, true);
        if (grant.access != RegistryAccess::Allowed) {
            return std::unexpected(UploadRefusal{grant.access, std::nullopt});
        }
        if (!repository.contains('/')) {
            return std::optional<TenantImage>{};
        }
        auto tenant = tenant_image(route, repository, grant, true);
        if (!tenant) {
            return std::unexpected(UploadRefusal{grant.access, tenant.error()});
        }
        return std::optional<TenantImage>{std::move(*tenant)};
    }

    boost::http::route_task serve_oci_upload_start(boost::http::route_params& route) {
        const std::string repository = oci_repository(route);
        auto authorized = authorize_oci_upload(route, repository);
        if (!authorized) {
            if (authorized.error().refusal) {
                co_return co_await registry_refusal(route, *authorized.error().refusal);
            }
            co_return co_await registry_auth_error(route, authorized.error().access);
        }
        const auto started = artifacts.begin_upload(repository);
        if (!started) {
            co_return co_await registry_error(
                route, boost::http::status::bad_request, started.error(), true);
        }
        const std::string location =
            "/v2/" + repository + "/blobs/uploads/" + std::string(*started);
        route.res.set(boost::http::field::location, location);
        route.res.set("Docker-Upload-UUID", *started);
        route.res.set(boost::http::field::range, "0-0");
        co_return co_await send_text(
            route, boost::http::status::accepted, {}, "application/octet-stream", "no-store");
    }

    boost::http::route_task serve_oci_upload(boost::http::route_params& route, bool finish) {
        const std::string repository = oci_repository(route);
        auto authorized = authorize_oci_upload(route, repository);
        if (!authorized) {
            if (authorized.error().refusal) {
                co_return co_await registry_refusal(route, *authorized.error().refusal);
            }
            co_return co_await registry_auth_error(route, authorized.error().access);
        }
        const std::string upload = route_value(route, "upload");
        const auto initial_size = artifacts.upload_size(upload);
        if (!initial_size) {
            co_return co_await registry_error(route,
                                              boost::http::status::bad_request,
                                              initial_size.error(),
                                              true,
                                              "BLOB_UPLOAD_INVALID");
        }
        if (*initial_size > options.max_registry_blob_bytes) {
            co_return co_await registry_error(
                route,
                boost::http::status::payload_too_large,
                "OCI upload exceeds LAKE_MAX_REGISTRY_BLOB_BYTES limit of " +
                    std::to_string(options.max_registry_blob_bytes) + " bytes",
                true,
                "BLOB_UPLOAD_INVALID");
        }
        const auto content_range = oci_content_range(route);
        if (!content_range) {
            co_return co_await registry_error(route,
                                              boost::http::status::range_not_satisfiable,
                                              content_range.error(),
                                              true,
                                              "BLOB_UPLOAD_INVALID");
        }
        if (*content_range && (*content_range)->start != *initial_size) {
            co_return co_await registry_error(
                route,
                boost::http::status::range_not_satisfiable,
                "Content-Range starts at " + std::to_string((*content_range)->start) +
                    " but the OCI upload is at " + std::to_string(*initial_size),
                true,
                "BLOB_UPLOAD_INVALID");
        }
        OciUploadSink sink{artifacts, upload, *initial_size, options.max_registry_blob_bytes, {}};
        auto [read_error, bytes_read] = co_await boost::capy::push_to(route.req_body, sink);
        if (read_error) {
            const auto restored = artifacts.truncate_upload(upload, *initial_size);
            if (body_is_too_large(read_error)) {
                co_return co_await registry_error(
                    route,
                    boost::http::status::payload_too_large,
                    "OCI upload exceeds LAKE_MAX_REGISTRY_BLOB_BYTES limit of " +
                        std::to_string(options.max_registry_blob_bytes) + " bytes",
                    true,
                    "BLOB_UPLOAD_INVALID");
            }
            std::string message = sink.storage_error.empty()
                                      ? "could not read OCI upload body: " + read_error.message()
                                      : "could not store OCI upload body: " + sink.storage_error;
            if (!restored) {
                message += "; " + restored.error();
            }
            co_return co_await registry_error(route,
                                              boost::http::status::bad_request,
                                              std::move(message),
                                              true,
                                              "BLOB_UPLOAD_INVALID");
        }
        if (*content_range && bytes_read != (*content_range)->size()) {
            const auto restored = artifacts.truncate_upload(upload, *initial_size);
            std::string message =
                "Content-Range describes " + std::to_string((*content_range)->size()) +
                " bytes but the OCI upload chunk contained " + std::to_string(bytes_read);
            if (!restored) {
                message += "; " + restored.error();
            }
            co_return co_await registry_error(route,
                                              boost::http::status::range_not_satisfiable,
                                              std::move(message),
                                              true,
                                              "BLOB_UPLOAD_INVALID");
        }
        // A tenant push's bytes are its workspace's transfer in (lake#658), whether or not the
        // blob is finished; instance-registry names belong to no workspace (lake#662).
        if (*authorized && usage_recorder) {
            usage_recorder->add_transfer((**authorized).workspace_id, bytes_read, 0);
        }
        if (!finish) {
            route.res.set(boost::http::field::location,
                          "/v2/" + repository + "/blobs/uploads/" + upload);
            route.res.set("Docker-Upload-UUID", upload);
            route.res.set(boost::http::field::range,
                          sink.offset == 0 ? "0-0" : "0-" + std::to_string(sink.offset - 1));
            co_return co_await send_text(
                route, boost::http::status::accepted, {}, "application/octet-stream", "no-store");
        }
        if (*authorized) {
            co_return co_await finish_tenant_upload(
                route, std::move(**authorized), repository, upload, query_value(route, "digest"));
        }
        const auto completed = artifacts.finish_upload(upload, query_value(route, "digest"));
        if (!completed) {
            co_return co_await registry_error(route,
                                              boost::http::status::bad_request,
                                              completed.error(),
                                              true,
                                              "BLOB_UPLOAD_INVALID");
        }
        route.res.set(boost::http::field::location,
                      "/v2/" + repository + "/blobs/" + std::string(*completed));
        route.res.set("Docker-Content-Digest", *completed);
        co_return co_await send_text(
            route, boost::http::status::created, {}, "application/octet-stream", "no-store");
    }

    // Completes a tenant blob upload (lake#383). In a switched workspace (lake#380) the staged
    // file is first moved, under the registry's lock, to a name only this completion knows, so no
    // PATCH to the same upload can append to the bytes being checked and stored. Off the server
    // thread, those bytes are checked against the digest and streamed into the workspace's liquid
    // store, the stored object must have the size that was hashed, and the blob record maps the
    // digest to the object (decision 54 S5). The claimed file is removed once the record is
    // written; if the record cannot be written it goes back under its upload id, so a retry of the
    // same completion works; a store that is unavailable or over quota removes it. A workspace
    // still served from its space's store keeps phase A: the bytes go into the instance registry
    // until the copy moves them. Either way the workspace records that its space pushed the blob,
    // so its manifests may reference it; until it does, the push is refused and the client retries.
    boost::http::route_task finish_tenant_upload(boost::http::route_params& route,
                                                 TenantImage tenant,
                                                 std::string repository,
                                                 std::string upload,
                                                 std::string digest) {
        const auto workspace = options.store->get_workspace(tenant.workspace_id);
        if (!workspace) {
            co_return co_await registry_error(route,
                                              boost::http::status::service_unavailable,
                                              "the image repository's workspace cannot be read: " +
                                                  workspace.error().message,
                                              true,
                                              "UNAVAILABLE");
        }
        auto records = workspace_records(tenant.context, tenant.customer_id, tenant.workspace_id);
        if (!records) {
            co_return co_await registry_workspace_error(route, records.error());
        }
        control::OciBlob blob{.customer_id = tenant.customer_id, .digest = digest};
        if (workspace->store_moved_at.empty()) {
            const auto completed = artifacts.finish_upload(upload, digest);
            if (!completed) {
                co_return co_await registry_error(route,
                                                  boost::http::status::bad_request,
                                                  completed.error(),
                                                  true,
                                                  "BLOB_UPLOAD_INVALID");
            }
            blob.size = artifacts.blob_size(*completed).value_or(0);
            auto recorded = control::settled(co_await control::record_oci_blob(*records, blob));
            if (!recorded) {
                co_return co_await registry_workspace_error(route, recorded.error());
            }
            co_return co_await blob_created(route, repository, digest);
        }
        if (!ArtifactRegistry::digest_name(digest)) {
            co_return co_await registry_error(route,
                                              boost::http::status::bad_request,
                                              "OCI upload or digest is invalid",
                                              true,
                                              "BLOB_UPLOAD_INVALID");
        }
        // The object the workspace already names for this digest: a re-push of a blob it holds
        // adds nothing, so the quota is not checked for it.
        auto existing =
            control::settled(co_await control::get_oci_blob(*records, tenant.customer_id, digest));
        if (!existing) {
            co_return co_await registry_workspace_error(route, existing.error());
        }
        const std::string known = *existing ? (*existing)->object : std::string();
        const bool known_rooted = *existing && (*existing)->rooted;
        const auto claimed = artifacts.claim_upload(upload);
        if (!claimed) {
            co_return co_await registry_error(route,
                                              boost::http::status::bad_request,
                                              claimed.error(),
                                              true,
                                              "BLOB_UPLOAD_INVALID");
        }
        const std::filesystem::path source = *claimed;
        auto stored = co_await blocking_call(
            [&]() -> SyncStoreResult<WorkspaceBlob> {
                const auto actual = ArtifactRegistry::file_digest(source);
                if (!actual) {
                    return std::unexpected(SyncStoreError{"io", actual.error()});
                }
                if (*actual != digest) {
                    return std::unexpected(SyncStoreError{
                        "invalid_digest", "uploaded bytes do not match the requested digest"});
                }
                std::error_code error;
                const std::uintmax_t hashed = std::filesystem::file_size(source, error);
                if (error) {
                    return std::unexpected(SyncStoreError{"io", error.message()});
                }
                auto put = sync_store.put_workspace_blob(
                    tenant.customer_id, tenant.workspace_id, source, true, known, known_rooted);
                if (put && put->size != hashed) {
                    return std::unexpected(SyncStoreError{
                        "corrupt", "the stored blob's size differs from the bytes hashed"});
                }
                return put;
            },
            [](std::string message) -> SyncStoreResult<WorkspaceBlob> {
                return std::unexpected(SyncStoreError{"store_unavailable", std::move(message)});
            });
        if (!stored) {
            // Bytes that are not their digest, or that the space's quota refuses, are dropped;
            // after any other failure (an unavailable or busy store, a local read error) they go
            // back under the upload id, so the client's retry of the same completion works.
            const bool drop =
                stored.error().code == "invalid_digest" || stored.error().rule == "storage_quota";
            if (drop) {
                ArtifactRegistry::remove_claimed(source);
            } else {
                static_cast<void>(artifacts.release_upload(upload, source));
            }
            if (stored.error().code == "invalid_digest" || stored.error().code == "io") {
                co_return co_await registry_error(route,
                                                  boost::http::status::bad_request,
                                                  stored.error().message,
                                                  true,
                                                  "BLOB_UPLOAD_INVALID");
            }
            co_return co_await registry_store_error(route, stored.error());
        }
        blob.size = stored->size;
        blob.object = stored->object;
        blob.rooted = stored->rooted;
        auto recorded = control::settled(co_await control::store_oci_blob(*records, blob));
        if (!recorded) {
            static_cast<void>(artifacts.release_upload(upload, source));
            co_return co_await registry_workspace_error(route, recorded.error());
        }
        ArtifactRegistry::remove_claimed(source);
        co_return co_await blob_created(route, repository, digest);
    }

    boost::http::route_task blob_created(boost::http::route_params& route,
                                         const std::string& repository,
                                         const std::string& digest) {
        route.res.set(boost::http::field::location, "/v2/" + repository + "/blobs/" + digest);
        route.res.set("Docker-Content-Digest", digest);
        return send_text(
            route, boost::http::status::created, {}, "application/octet-stream", "no-store");
    }

    // A workspace store that refused a blob read or write (lake#383 phase B): unavailable stores
    // (a lease held elsewhere, a full or moving lane, too many pulls) may be retried; the space's
    // storage quota is 507; a store that lost what a record names is never answered from anywhere
    // else.
    boost::http::route_task registry_store_error(boost::http::route_params& route,
                                                 const SyncStoreError& error) {
        if (error.rule == "storage_quota") {
            return registry_error(
                route, boost::http::status::insufficient_storage, error.message, true, "DENIED");
        }
        if (error.code == "store_unavailable" || error.code == "busy" || error.code == "locked") {
            route.res.set(boost::http::field::retry_after, "1");
            // A refused client does not keep its connection (and a server worker) while it waits
            // to retry (lake#534).
            route.res.set_keep_alive(false);
            return registry_error(route,
                                  boost::http::status::service_unavailable,
                                  "the image repository's workspace store is unavailable: " +
                                      error.message,
                                  true,
                                  "UNAVAILABLE");
        }
        if (error.code == "not_found") {
            return registry_error(route,
                                  boost::http::status::internal_server_error,
                                  "the workspace store does not hold a blob its record names; push "
                                  "the blob again",
                                  true,
                                  "UNKNOWN");
        }
        return registry_error(route,
                              boost::http::status::internal_server_error,
                              "the image repository's workspace store failed: " + error.code +
                                  ": " + error.message,
                              true,
                              "UNKNOWN");
    }

    // At most this many blob streams run at once in this process, each with one window of
    // buffer (at most 24 MiB of layer bytes), fewer than the server's 32 worker coroutines, so
    // pulls never take every worker (lake#383 phase B).
    static constexpr int blob_stream_limit = 24;
    static constexpr std::uint64_t blob_window = 1024 * 1024;
    std::atomic<int> blob_streams{0};

    // Streams a tenant blob one window at a time, so a layer is never held whole: from the
    // workspace's store (`read` reads a window there) or from the instance registry's file for a
    // record not copied yet. The first window is read before any header is sent, so a refusal is
    // a clean status; a later read that fails ends the connection, which the client sees as a
    // short body and retries.
    using BlobReader =
        std::function<SyncStoreResult<std::uint64_t>(std::uint64_t, std::span<std::byte>)>;
    //
    // A workspace file (GET /drive/files/download, docs/api/drives.md) streams the same way with
    // `file` set: its media type and ETag are the file's, no registry header is sent, and a refusal
    // is lake's JSON error rather than the registry's.
    struct StreamedFile {
        std::string content_type;
    };
    //
    // `metered_workspace` is the workspace whose transfer the bytes are (lake#658): what was sent,
    // whole or not, is added to its `transfer` `out` when the stream ends.
    boost::http::route_task stream_blob(boost::http::route_params& route,
                                        std::string digest,
                                        std::uint64_t size,
                                        BlobReader read,
                                        bool head,
                                        std::optional<StreamedFile> file = std::nullopt,
                                        std::string metered_workspace = {}) {
        namespace http = boost::http;
        struct Metered {
            UsageRecorder* recorder;
            std::string workspace_id;
            std::uint64_t sent = 0;
            ~Metered() {
                if (recorder != nullptr) {
                    recorder->add_transfer(workspace_id, 0, sent);
                }
            }
        } metered{metered_workspace.empty() ? nullptr : usage_recorder.get(),
                  std::move(metered_workspace)};
        const auto refuse = [&](const SyncStoreError& error) -> http::route_task {
            if (!file) {
                return registry_store_error(route, error);
            }
            const auto status = error.code == "not_found" ? http::status::not_found
                                : error.code == "store_unavailable" || error.code == "busy"
                                    ? http::status::service_unavailable
                                    : http::status::internal_server_error;
            if (status == http::status::service_unavailable) {
                route.res.set(http::field::retry_after, "1");
            }
            return send_text(route,
                             status,
                             error_body(error.code, error.message),
                             "application/json; charset=utf-8",
                             "no-store");
        };
        struct Slot {
            std::atomic<int>& count;
            ~Slot() {
                count.fetch_sub(1);
            }
        };
        if (blob_streams.fetch_add(1) >= blob_stream_limit) {
            blob_streams.fetch_sub(1);
            co_return co_await refuse(SyncStoreError{"store_unavailable",
                                                     file ? "too many downloads in progress"
                                                          : "too many image pulls in progress"});
        }
        Slot slot{blob_streams};
        const auto window_read = [&](std::uint64_t offset, std::span<std::byte> into) {
            return blocking_call([&, offset, into] { return read(offset, into); },
                                 [](std::string message) -> SyncStoreResult<std::uint64_t> {
                                     return std::unexpected(
                                         SyncStoreError{"store_unavailable", std::move(message)});
                                 });
        };
        std::vector<std::byte> buffer;
        std::uint64_t filled = 0;
        if (!head && size != 0) {
            buffer.resize(static_cast<std::size_t>(std::min(blob_window, size)));
            auto first = co_await window_read(0, buffer);
            if (!first) {
                co_return co_await refuse(first.error());
            }
            if (*first == 0) {
                co_return co_await refuse(
                    SyncStoreError{"not_found", "the file's bytes are missing"});
            }
            filled = *first;
        }
        route.status(http::status::ok);
        route.res.set(http::field::content_type,
                      file ? file->content_type : std::string("application/octet-stream"));
        route.res.set(http::field::cache_control, "no-store");
        route.res.erase(http::field::etag);
        route.res.erase(http::field::content_length);
        route.res.set(http::field::etag, digest);
        if (file) {
            route.res.set("X-Content-Type-Options", "nosniff");
        } else {
            route.res.set("Docker-Content-Digest", digest);
        }
        route.res.set_payload_size(size);
        if (head || size == 0) {
            auto [send_error] = co_await route.res_body.commit_eof(0);
            if (send_error) {
                co_return http::route_error(send_error);
            }
            co_return http::route_done;
        }
        // A pull streams for as long as its client reads: it leaves the short-request workers
        // (lake#534); its own stream slots and the long-request caps bound it.
        if (server && !server->leave_worker(route)) {
            route.res.set(http::field::retry_after, "1");
            co_return co_await registry_error(route,
                                              http::status::too_many_requests,
                                              "too many long requests are waiting; retry",
                                              true,
                                              "TOOMANYREQUESTS");
        }
        std::uint64_t sent = 0;
        for (;;) {
            std::size_t offset = 0;
            while (offset < filled) {
                auto buffers = std::array<boost::capy::mutable_buffer, 16>{};
                const auto output = route.res_body.prepare(buffers);
                const auto copied = boost::capy::buffer_copy(
                    output, boost::capy::const_buffer(buffer.data() + offset, filled - offset));
                offset += copied;
                // A client that takes no bytes for the idle timeout loses its stream, and with it
                // its stream slot and the connection.
                const auto idle = options.registry_stream_idle_timeout;
                if (sent + offset == size) {
                    auto [send_error] =
                        co_await boost::capy::timeout(route.res_body.commit_eof(copied), idle);
                    if (send_error) {
                        co_return http::route_error(send_error);
                    }
                    metered.sent = size;
                    co_return http::route_done;
                }
                auto [send_error] =
                    co_await boost::capy::timeout(route.res_body.commit(copied), idle);
                metered.sent = sent + offset;
                if (send_error) {
                    if (send_error == boost::capy::error::timeout) {
                        std::cerr << nlohmann::json{{"event", "oci_blob_stream_idle"},
                                                    {"sent", sent + offset},
                                                    {"size", size}}
                                         .dump()
                                  << '\n';
                    }
                    co_return http::route_error(send_error);
                }
            }
            sent += filled;
            const auto wanted = static_cast<std::size_t>(std::min(blob_window, size - sent));
            auto next = co_await window_read(sent, std::span(buffer).first(wanted));
            if (!next || *next == 0) {
                std::cerr << nlohmann::json{{"event", "oci_blob_stream_failed"},
                                            {"sent", sent},
                                            {"size", size},
                                            {"error",
                                             next
                                                 ? std::string("short read")
                                                 : next.error().code + ": " + next.error().message}}
                                 .dump()
                          << '\n';
                co_return http::route_error(std::make_error_code(std::errc::io_error));
            }
            filled = *next;
        }
    }

    // A tenant blob in its workspace's store. The pull takes one of the workspace's eight stream
    // slots for its whole stream (apart from the admissions of pushes, git and repository
    // requests; refused at once with 503 and `Connection: close` when they are taken or the lane is
    // moving), and each window takes the lane only for its one read. HEAD checks that the store
    // holds the object.
    boost::http::route_task serve_workspace_blob(boost::http::route_params& route,
                                                 std::string customer_id,
                                                 std::string workspace_id,
                                                 control::OciBlob blob,
                                                 bool head) {
        auto admission = sync_store.admit_workspace_stream(workspace_id);
        if (!admission) {
            co_return co_await registry_store_error(route, admission.error());
        }
        if (head) {
            auto present = co_await blocking_call(
                [&] {
                    return sync_store.workspace_blob_present(
                        customer_id, workspace_id, blob.object);
                },
                [](std::string message) -> SyncStoreResult<bool> {
                    return std::unexpected(SyncStoreError{"store_unavailable", std::move(message)});
                });
            if (!present) {
                co_return co_await registry_store_error(route, present.error());
            }
            if (!*present) {
                co_return co_await registry_store_error(route, SyncStoreError{"not_found", ""});
            }
        }
        const std::string object = blob.object;
        co_return co_await stream_blob(
            route,
            blob.digest,
            blob.size,
            [this, customer_id, workspace_id, object](std::uint64_t offset,
                                                      std::span<std::byte> into) {
                return sync_store.read_workspace_blob(
                    customer_id, workspace_id, object, offset, into, false);
            },
            head,
            std::nullopt,
            workspace_id);
    }

    // A tenant blob whose record names no store object yet: streamed from the instance registry's
    // file, never read whole.
    boost::http::route_task serve_instance_blob(boost::http::route_params& route,
                                                std::string digest,
                                                bool head,
                                                std::string metered_workspace) {
        const auto file = artifacts.blob_file(digest);
        std::error_code error;
        const std::uintmax_t size = file ? std::filesystem::file_size(*file, error) : 0;
        if (!file || error) {
            co_return co_await registry_error(
                route, boost::http::status::not_found, "blob is unknown in this repository", true);
        }
        const std::filesystem::path path = *file;
        co_return co_await stream_blob(
            route,
            digest,
            size,
            [path](std::uint64_t offset,
                   std::span<std::byte> into) -> SyncStoreResult<std::uint64_t> {
                std::ifstream input(path, std::ios::binary);
                input.seekg(static_cast<std::streamoff>(offset));
                input.read(reinterpret_cast<char*>(into.data()),
                           static_cast<std::streamsize>(into.size()));
                if (input.bad() || (!input && input.gcount() == 0 && !into.empty())) {
                    return std::unexpected(SyncStoreError{"io", "could not read " + path.string()});
                }
                return static_cast<std::uint64_t>(input.gcount());
            },
            head,
            std::nullopt,
            std::move(metered_workspace));
    }

    boost::http::route_task serve_oci_blob(boost::http::route_params& route, bool head) {
        const std::string repository = oci_repository(route);
        const std::string digest = route_value(route, "digest");
        const RegistryGrant grant = registry_access(route, repository, false);
        if (grant.access != RegistryAccess::Allowed) {
            co_return co_await registry_auth_error(route, grant.access);
        }
        if (repository.contains('/')) {
            // A tenant blob is readable through a repository only when one of its tags names it
            // or references it (lake#383: one list of the repository's tags in its workspace).
            auto tenant = tenant_image(route, repository, grant, false);
            if (!tenant) {
                co_return co_await registry_refusal(route, tenant.error());
            }
            auto records =
                workspace_records(tenant->context, tenant->customer_id, tenant->workspace_id);
            if (!records) {
                co_return co_await registry_workspace_error(route, records.error());
            }
            auto tags = control::settled(
                co_await control::list_oci_tags(*records, tenant->customer_id, tenant->name));
            if (!tags) {
                co_return co_await registry_workspace_error(route, tags.error());
            }
            bool referenced = false;
            bool manifest = false;
            for (const control::OciTag& tag : *tags) {
                manifest = manifest || tag.digest == digest;
                referenced =
                    referenced || std::ranges::any_of(tag.references, [&](const auto& item) {
                        return item.digest == digest;
                    });
            }
            if (manifest && !referenced) {
                auto stored = control::settled(
                    co_await control::get_oci_manifest(*records, tenant->customer_id, digest));
                if (!stored) {
                    co_return co_await registry_workspace_error(route, stored.error());
                }
                if (*stored) {
                    route.res.set("Docker-Content-Digest", digest);
                    const std::span<const char> characters((*stored)->document.data(),
                                                           (*stored)->document.size());
                    const auto bytes = std::as_bytes(characters);
                    co_return co_await send_bytes(route,
                                                  boost::http::status::ok,
                                                  "application/octet-stream",
                                                  bytes,
                                                  digest,
                                                  {},
                                                  head);
                }
            }
            if (!referenced) {
                co_return co_await registry_error(route,
                                                  boost::http::status::not_found,
                                                  "blob is unknown in this repository",
                                                  true);
            }
            // ...and only when the workspace records that its space pushed it. A record that names
            // a store object is read only from the workspace's store (lake#383 phase B); one that
            // names none was written before its workspace's blobs moved, and its bytes are still
            // in the instance registry until the copy names its object.
            auto recorded = control::settled(
                co_await control::get_oci_blob(*records, tenant->customer_id, digest));
            if (!recorded) {
                co_return co_await registry_workspace_error(route, recorded.error());
            }
            if (!*recorded) {
                co_return co_await registry_error(route,
                                                  boost::http::status::not_found,
                                                  "blob is unknown in this repository",
                                                  true);
            }
            if (!(*recorded)->object.empty()) {
                co_return co_await serve_workspace_blob(
                    route, tenant->customer_id, tenant->workspace_id, std::move(**recorded), head);
            }
            co_return co_await serve_instance_blob(route, digest, head, tenant->workspace_id);
        } else if (!artifacts.repository_has_blob(repository, digest)) {
            co_return co_await registry_error(
                route, boost::http::status::not_found, "blob is unknown in this repository", true);
        }
        const auto object = artifacts.blob(digest);
        if (!object) {
            co_return co_await registry_error(
                route, boost::http::status::not_found, object.error(), true);
        }
        route.res.set("Docker-Content-Digest", object->digest);
        co_return co_await send_bytes(route,
                                      boost::http::status::ok,
                                      object->content_type,
                                      object->bytes,
                                      object->digest,
                                      {},
                                      head);
    }

    // A tenant manifest read (lake#383): by tag, its tag record and then the manifest it names;
    // by digest, only a manifest one of the repository's tags names.
    boost::http::route_task serve_tenant_manifest_read(boost::http::route_params& route,
                                                       TenantImage tenant,
                                                       std::string reference,
                                                       bool head) {
        auto records = workspace_records(tenant.context, tenant.customer_id, tenant.workspace_id);
        if (!records) {
            co_return co_await registry_workspace_error(route, records.error());
        }
        std::string digest;
        std::string content_type;
        if (ArtifactRegistry::digest_name(reference)) {
            auto tags = control::settled(
                co_await control::list_oci_tags(*records, tenant.customer_id, tenant.name));
            if (!tags) {
                co_return co_await registry_workspace_error(route, tags.error());
            }
            const auto named = std::ranges::find(*tags, reference, &control::OciTag::digest);
            if (named != tags->end()) {
                digest = named->digest;
                content_type = named->content_type;
            }
        } else if (ArtifactRegistry::tag_name(reference)) {
            auto tag = control::settled(co_await control::get_oci_tag(
                *records, tenant.customer_id, tenant.name, reference));
            if (!tag) {
                co_return co_await registry_workspace_error(route, tag.error());
            }
            if (*tag) {
                digest = (*tag)->digest;
                content_type = (*tag)->content_type;
            }
        }
        if (digest.empty()) {
            co_return co_await registry_error(route,
                                              boost::http::status::not_found,
                                              "OCI manifest is unknown in this repository",
                                              true,
                                              "MANIFEST_UNKNOWN");
        }
        auto manifest = control::settled(
            co_await control::get_oci_manifest(*records, tenant.customer_id, digest));
        if (!manifest) {
            co_return co_await registry_workspace_error(route, manifest.error());
        }
        if (!*manifest) {
            co_return co_await registry_error(route,
                                              boost::http::status::not_found,
                                              "OCI manifest is unknown in this repository",
                                              true,
                                              "MANIFEST_UNKNOWN");
        }
        route.res.set("Docker-Content-Digest", digest);
        const std::span<const char> characters((*manifest)->document.data(),
                                               (*manifest)->document.size());
        const auto bytes = std::as_bytes(characters);
        co_return co_await send_bytes(
            route,
            boost::http::status::ok,
            content_type.empty() ? "application/vnd.oci.image.manifest.v1+json" : content_type,
            bytes,
            digest,
            {},
            head);
    }

    // A tenant manifest push (lake#383): the manifest is stored once in the workspace (`if_absent`,
    // keyed by digest) and the tag moved to it at the revision read. Its config and layers must
    // already be in the instance registry, where blobs stay until phase B.
    boost::http::route_task serve_tenant_manifest_write(boost::http::route_params& route,
                                                        TenantImage tenant,
                                                        std::string repository,
                                                        std::string reference,
                                                        std::string content_type,
                                                        std::string document) {
        if (!ArtifactRegistry::tag_name(reference)) {
            co_return co_await registry_error(route,
                                              boost::http::status::bad_request,
                                              "OCI manifest name is invalid",
                                              true,
                                              "TAG_INVALID");
        }
        const std::string digest = ArtifactRegistry::digest_of(document);
        if (ArtifactRegistry::digest_name(reference) && reference != digest) {
            co_return co_await registry_error(route,
                                              boost::http::status::bad_request,
                                              "OCI manifest bytes do not match their digest",
                                              true,
                                              "DIGEST_INVALID");
        }
        const nlohmann::json parsed = nlohmann::json::parse(document, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object() || parsed.value("schemaVersion", 0) != 2) {
            co_return co_await registry_error(route,
                                              boost::http::status::bad_request,
                                              "OCI manifest is not valid schema version 2 JSON",
                                              true,
                                              "MANIFEST_INVALID");
        }
        const auto digests = manifest_digests(parsed);
        if (!digests) {
            co_return co_await registry_error(
                route, boost::http::status::bad_request, digests.error(), true, "MANIFEST_INVALID");
        }
        auto records = workspace_records(tenant.context, tenant.customer_id, tenant.workspace_id);
        if (!records) {
            co_return co_await registry_workspace_error(route, records.error());
        }
        // Only blobs this space pushed into this workspace: knowing another space's digest is not
        // enough to publish its layer. Each size is the one the blob's record holds, wherever its
        // bytes are (the workspace store, or the instance registry before the copy).
        std::vector<control::OciReference> references;
        for (const std::string& referenced : *digests) {
            auto pushed = control::settled(
                co_await control::get_oci_blob(*records, tenant.customer_id, referenced));
            if (!pushed) {
                co_return co_await registry_workspace_error(route, pushed.error());
            }
            if (!*pushed) {
                co_return co_await registry_error(
                    route,
                    boost::http::status::bad_request,
                    "OCI manifest references a blob not pushed to this workspace: " + referenced,
                    true,
                    "MANIFEST_BLOB_UNKNOWN");
            }
            references.push_back({referenced, (*pushed)->size});
        }
        auto stored =
            control::settled(co_await control::put_oci_manifest(*records,
                                                                {.customer_id = tenant.customer_id,
                                                                 .digest = digest,
                                                                 .content_type = content_type,
                                                                 .document = document,
                                                                 .references = references}));
        if (!stored) {
            co_return co_await registry_workspace_error(route, stored.error());
        }
        auto tagged =
            control::settled(co_await control::set_oci_tag(*records,
                                                           {.customer_id = tenant.customer_id,
                                                            .repository = tenant.name,
                                                            .tag = reference,
                                                            .digest = digest,
                                                            .content_type = content_type,
                                                            .manifest_size = document.size(),
                                                            .references = std::move(references)}));
        if (!tagged) {
            co_return co_await registry_workspace_error(route, tagged.error());
        }
        route.res.set(boost::http::field::location,
                      "/v2/" + repository + "/manifests/" + reference);
        route.res.set("Docker-Content-Digest", digest);
        co_return co_await send_text(
            route, boost::http::status::created, {}, "application/json; charset=utf-8", "no-store");
    }

    boost::http::route_task serve_oci_manifest(boost::http::route_params& route, bool head) {
        const std::string repository = oci_repository(route);
        const std::string reference = route_value(route, "reference");
        const bool tenant_path = repository.contains('/');
        if (route.req.method() != boost::http::method::put) {
            const RegistryGrant grant = registry_access(route, repository, false);
            if (grant.access != RegistryAccess::Allowed) {
                co_return co_await registry_auth_error(route, grant.access);
            }
            if (tenant_path) {
                auto tenant = tenant_image(route, repository, grant, false);
                if (!tenant) {
                    co_return co_await registry_refusal(route, tenant.error());
                }
                co_return co_await serve_tenant_manifest_read(
                    route, std::move(*tenant), reference, head);
            }
            const auto object = artifacts.manifest(repository, reference);
            if (!object) {
                co_return co_await registry_error(
                    route, boost::http::status::not_found, object.error(), true);
            }
            route.res.set("Docker-Content-Digest", object->digest);
            co_return co_await send_bytes(route,
                                          boost::http::status::ok,
                                          object->content_type,
                                          object->bytes,
                                          object->digest,
                                          {},
                                          head);
        }
        const RegistryGrant grant = registry_access(route, repository, true);
        if (grant.access != RegistryAccess::Allowed) {
            co_return co_await registry_auth_error(route, grant.access);
        }
        std::optional<TenantImage> tenant;
        if (tenant_path) {
            auto resolved = tenant_image(route, repository, grant, true);
            if (!resolved) {
                co_return co_await registry_refusal(route, resolved.error());
            }
            tenant = std::move(*resolved);
        }
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error || body.empty()) {
            co_return co_await registry_error(
                route, boost::http::status::bad_request, "OCI manifest body is required", true);
        }
        const std::string content_type =
            route.req.count(boost::http::field::content_type) == 0
                ? "application/vnd.oci.image.manifest.v1+json"
                : std::string(route.req.at(boost::http::field::content_type));
        if (tenant) {
            co_return co_await serve_tenant_manifest_write(
                route, std::move(*tenant), repository, reference, content_type, std::move(body));
        }
        const std::span<const char> characters(body.data(), body.size());
        const auto published =
            artifacts.put_manifest(repository, reference, content_type, std::as_bytes(characters));
        if (!published) {
            co_return co_await registry_error(
                route, boost::http::status::bad_request, published.error(), true);
        }
        route.res.set(boost::http::field::location,
                      "/v2/" + repository + "/manifests/" + reference);
        route.res.set("Docker-Content-Digest", *published);
        co_return co_await send_text(
            route, boost::http::status::created, {}, "application/json; charset=utf-8", "no-store");
    }

    boost::http::route_task serve_npm_metadata(boost::http::route_params& route) {
        const std::string package = route_value(route, "package");
        if (route.req.method() == boost::http::method::get) {
            const auto metadata = artifacts.npm_metadata(package);
            if (!metadata) {
                co_return co_await registry_error(
                    route, boost::http::status::not_found, metadata.error());
            }
            co_return co_await send_text(route,
                                         boost::http::status::ok,
                                         *metadata,
                                         "application/json; charset=utf-8",
                                         "no-store");
        }
        if (!registry_write_authorized(route)) {
            co_return co_await registry_error(
                route, boost::http::status::unauthorized, "registry bearer is required");
        }
        auto [read_error, body] = co_await read_string_body(route);
        const nlohmann::json metadata = nlohmann::json::parse(body, nullptr, false);
        if (read_error || metadata.is_discarded() || !metadata.is_object()) {
            co_return co_await registry_error(
                route, boost::http::status::bad_request, "npm publication is invalid JSON");
        }
        const auto published = artifacts.put_npm(package, metadata, hub_origin(route));
        if (!published) {
            co_return co_await registry_error(
                route, boost::http::status::bad_request, published.error());
        }
        co_return co_await send_text(route,
                                     boost::http::status::created,
                                     nlohmann::json{{"ok", true}, {"id", package}}.dump(),
                                     "application/json; charset=utf-8",
                                     "no-store");
    }

    boost::http::route_task serve_npm_tarball(boost::http::route_params& route) {
        const auto tarball =
            artifacts.npm_tarball(route_value(route, "package"), route_value(route, "filename"));
        if (!tarball) {
            co_return co_await registry_error(
                route, boost::http::status::not_found, tarball.error());
        }
        co_return co_await send_bytes(
            route, boost::http::status::ok, "application/octet-stream", *tarball);
    }

    boost::http::route_task serve_python_publish(boost::http::route_params& route) {
        if (!registry_write_authorized(route)) {
            co_return co_await registry_error(
                route, boost::http::status::unauthorized, "registry bearer is required");
        }
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return co_await registry_error(
                route, boost::http::status::bad_request, "could not read Python publication");
        }
        const std::string content_type =
            route.req.count(boost::http::field::content_type) == 0
                ? ""
                : std::string(route.req.at(boost::http::field::content_type));
        MultipartForm form = parse_multipart(body, content_type);
        if (!form.error.empty()) {
            co_return co_await registry_error(route, boost::http::status::bad_request, form.error);
        }
        const auto published = artifacts.put_python(form.fields["name"],
                                                    form.fields["version"],
                                                    form.files.front().name,
                                                    form.files.front().contents);
        if (!published) {
            co_return co_await registry_error(
                route, boost::http::status::bad_request, published.error());
        }
        co_return co_await send_text(
            route, boost::http::status::created, {}, "text/plain; charset=utf-8", "no-store");
    }

    boost::http::route_task serve_python_simple(boost::http::route_params& route) {
        const auto page = artifacts.python_simple(route_value(route, "package"), hub_origin(route));
        if (!page) {
            return registry_error(route, boost::http::status::not_found, page.error());
        }
        return send_text(
            route, boost::http::status::ok, *page, "text/html; charset=utf-8", "no-store");
    }

    boost::http::route_task serve_python_package(boost::http::route_params& route, bool head) {
        const auto package = artifacts.python_package(route_value(route, "filename"));
        if (!package) {
            co_return co_await registry_error(
                route, boost::http::status::not_found, package.error());
        }
        co_return co_await send_bytes(
            route, boost::http::status::ok, "application/octet-stream", *package, {}, {}, head);
    }

    boost::http::route_task serve_debug_publish(boost::http::route_params& route) {
        if (!registry_write_authorized(route)) {
            co_return co_await registry_error(
                route, boost::http::status::unauthorized, "registry bearer is required");
        }
        auto [read_error, body] = co_await read_string_body(route);
        if (read_error) {
            co_return co_await registry_error(
                route, boost::http::status::bad_request, "could not read debug artifact");
        }
        const std::span<const char> characters(body.data(), body.size());
        const auto published = artifacts.put_debug(route_value(route, "build_id"),
                                                   route_value(route, "kind"),
                                                   query_value(route, "source"),
                                                   std::as_bytes(characters));
        if (!published) {
            co_return co_await registry_error(
                route, boost::http::status::bad_request, published.error());
        }
        co_return co_await send_text(
            route, boost::http::status::created, {}, "application/octet-stream", "no-store");
    }

    boost::http::route_task serve_debug_artifact(boost::http::route_params& route,
                                                 std::string kind) {
        const auto artifact =
            artifacts.debug(route_value(route, "build_id"), kind, route_value(route, "source"));
        if (!artifact) {
            co_return co_await registry_error(
                route, boost::http::status::not_found, artifact.error());
        }
        co_return co_await send_bytes(route,
                                      boost::http::status::ok,
                                      kind == "source" ? "text/plain; charset=utf-8"
                                                       : "application/octet-stream",
                                      *artifact);
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
        // Only what the capability reads: huggingface_hub adds fields of its own (`gitIgnore` when
        // a commit adds a .gitignore, 0.33.4 to 1.24.0), and the repository write schemas are
        // strict (lake#534).
        nlohmann::json input{{"id", repository->id}, {"revision", route_value(route, "revision")}};
        if (request.contains("files")) {
            input["files"] = request.at("files");
        }
        ApiContext context = context_for(route);
        const ApiResult result = registry.call("prepare_repository_upload", input, context);
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
        // The commit and its search index update (lake#381) wait on the repository's store, so
        // they run on the asynchronous registry path, off the server thread.
        const ApiResult result =
            unwrap(co_await registry.call_async("commit_repository_files",
                                                {{"id", repository->id},
                                                 {"revision", route_value(route, "revision")},
                                                 {"message", message},
                                                 {"operations", std::move(operations)}},
                                                context));
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
        // A repository belongs to one workspace (lake#315). Hub clients cannot add a field to
        // create_repo's body, so the workspace may also come in the X-Lake-Workspace-Id header
        // (huggingface_hub: HfApi(headers=...)). There is no default.
        std::string workspace_id =
            request.contains("workspace_id") && request.at("workspace_id").is_string()
                ? request.at("workspace_id").get<std::string>()
                : std::string();
        if (workspace_id.empty() && route.req.count("x-lake-workspace-id") != 0) {
            workspace_id = std::string(route.req.at("x-lake-workspace-id"));
        }
        const ApiResult result = registry.call("create_repository",
                                               {{"customer_id", owner->id},
                                                {"workspace_id", workspace_id},
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
        return signed_link_signature(options.instance_kek, payload);
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

    // Whether the request is for the apex host (LAKE_APEX_HOST, lake#757): every path there is the
    // region picker, apart from the stylesheet and fonts the picker links and the probes.
    bool region_picker_serves(const boost::http::route_params& route) const {
        if (options.regions.apex_host.empty()) {
            return false;
        }
        std::string host = request_host(route);
        std::ranges::transform(
            host, host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (host != options.regions.apex_host) {
            return false;
        }
        const std::string_view path = route.url.encoded_path();
        if (route.req.method() == boost::http::method::get &&
            (path == web_ui_stylesheet_url() || path == "/style.css" ||
             path.starts_with("/fonts/") || path == "/healthz" || path == "/readyz")) {
            return false;
        }
        return true;
    }

    // The region picker (docs/ui/regions.md). GET (any method but POST): the page, preselecting
    // the region chosen before (the `lake_region` cookie), else the closest to Cloudflare's
    // CF-IPCountry, else the first. POST with `region`: remember it and continue to the same path
    // and query on that region's host.
    boost::http::route_task serve_region_picker(boost::http::route_params& route) {
        namespace http = boost::http;
        const Regions& regions = options.regions;
        // The order sheet's own paths (lake#831, docs/api/orders.md#routes): /order (the form's
        // POST), /order/<offer> and /order/price-list, /orders/<id>. Every other path is the sheet.
        if (options.order_sheet.enabled && options.store) {
            const std::string_view path = route.url.encoded_path();
            const bool post = route.req.method() == http::method::post;
            if (post && path == "/order") {
                co_return co_await serve_order_submit(route);
            }
            if (!post && path.starts_with("/order/") &&
                (path == "/order/price-list" || !offer_from_slug(path.substr(7)).empty())) {
                co_return co_await serve_order_page(route, std::string(path.substr(7)));
            }
            if (!post && path.starts_with("/orders/") && path.size() > 8) {
                co_return co_await serve_order_status(route, std::string(path.substr(8)));
            }
        }
        std::string target(route.url.encoded_path());
        if (target.empty()) {
            target = "/";
        }
        if (route.url.has_query()) {
            target += "?" + std::string(route.url.encoded_query());
        }
        std::string error;
        if (route.req.method() == http::method::post) {
            auto [read_error, body] = co_await read_string_body(route);
            if (read_error) {
                co_return http::route_error(read_error);
            }
            if (const Region* chosen = regions.find(form_field(body, "region"))) {
                co_return co_await redirect(
                    route,
                    region_continue_url(*chosen, target),
                    region_preference_cookie(chosen->name, request_is_https(route)));
            }
            error = "Choose one of the regions below.";
        }
        const std::string country(route.req.value_or("cf-ipcountry", ""));
        const Region* selected =
            preselected_region(regions, cookie_value(route, region_cookie_name), country);
        co_return co_await send_text(
            route,
            error.empty() ? http::status::ok : http::status::bad_request,
            options.order_sheet.enabled
                ? web_ui_order_sheet(options.order_sheet, regions, *selected, target, error)
                : web_ui_region_picker(regions, *selected, target, error),
            "text/html; charset=utf-8",
            "no-store");
    }

    // ---------------------------------------------------------------------------------------
    // The apex order sheet's orders (lake#831, docs/api/orders.md).

    boost::http::route_task send_order_html(boost::http::route_params& route,
                                            boost::http::status status,
                                            std::string html) {
        co_return co_await send_text(
            route, status, std::move(html), "text/html; charset=utf-8", "no-store");
    }

    // The address an anonymous order is counted against (lake#838): X-Real-IP, which the ingress
    // (ingress-nginx) always sets itself, overwriting any the sender sent; else the last
    // X-Forwarded-For hop, the one the nearest proxy appended; else one bucket for every direct
    // client. Never CF-Connecting-IP or the first X-Forwarded-For hop: a sender that reaches the
    // ingress without passing Cloudflare can write either, and would land in a fresh bucket with
    // each request. The ingress decides which client address to trust (Cloudflare's ranges), and
    // lake counts what it decided.
    static std::string order_client_address(const boost::http::route_params& route) {
        if (route.req.count("x-real-ip") != 0) {
            return std::string(route.req.at("x-real-ip")).substr(0, 64);
        }
        if (route.req.count("x-forwarded-for") != 0) {
            std::string forwarded(route.req.at("x-forwarded-for"));
            const auto comma = forwarded.rfind(',');
            forwarded = comma == std::string::npos ? forwarded : forwarded.substr(comma + 1);
            const auto first = forwarded.find_first_not_of(' ');
            return first == std::string::npos ? "direct" : forwarded.substr(first, 64);
        }
        return "direct";
    }

    // Where order links point: https://<apex host>, else the order sheet's configured URL.
    std::string order_origin() const {
        if (!options.regions.apex_host.empty()) {
            return "https://" + options.regions.apex_host;
        }
        std::string url = options.regions.order_sheet_url;
        while (url.ends_with('/')) {
            url.pop_back();
        }
        return url;
    }

    std::string order_status_url(const control::CloudOrder& order) const {
        const std::string origin = order_origin();
        if (origin.empty() || order_status_key.empty()) {
            return {};
        }
        return origin + "/orders/" + order.id +
               "?token=" + order_status_token(order_status_key, order.id);
    }

    bool order_checkout_available() const {
        return payments != nullptr && payments->status().online_payment;
    }

    // GET /order/<offer> (the offer's form; `location` preselects) and GET /order/price-list.
    boost::http::route_task serve_order_page(boost::http::route_params& route, std::string rest) {
        namespace http = boost::http;
        const OrderSheet& sheet = options.order_sheet;
        if (rest == "price-list") {
            std::vector<PriceListRow> rows;
            std::string effective_from;
            if (const auto plans = options.store->billing_plans(); plans) {
                const std::chrono::year_month_day today{
                    std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
                const std::string month = std::format("{:04}-{:02}",
                                                      static_cast<int>(today.year()),
                                                      static_cast<unsigned>(today.month()));
                if (const auto list = price_list_for(price_lists(*plans), month); list) {
                    rows = price_list_rows(list->prices);
                    effective_from = list->effective_from;
                }
            }
            co_return co_await send_order_html(
                route, http::status::ok, web_ui_price_list(rows, effective_from));
        }
        const OrderOffer* offer = sheet.find_offer(offer_from_slug(rest));
        OrderFormInput values;
        values.offer = offer->id;
        values.location = query_value(route, "location");
        co_return co_await send_order_html(
            route,
            http::status::ok,
            web_ui_order_form(
                sheet, options.regions, *offer, values, {}, order_checkout_available()));
    }

    // POST /order: an order form, sent or to pay. Recorded in the control plane first; a checkout
    // then continues to Stripe, an order form to its confirmation.
    boost::http::route_task serve_order_submit(boost::http::route_params& route) {
        namespace http = boost::http;
        const OrderSheet& sheet = options.order_sheet;
        auto [read_error, body] = co_await read_string_body(route, std::size_t{32} * 1024);
        if (read_error) {
            if (body_is_too_large(read_error)) {
                metrics.increment_counter("lake_cloud_orders_refused_total", {{"reason", "size"}});
                co_return co_await send_order_html(
                    route,
                    http::status::payload_too_large,
                    web_ui_order_message("Order Too Large",
                                         "An order form is at most 32 KiB. Shorten the details "
                                         "and send it again."));
            }
            co_return http::route_error(read_error);
        }
        OrderFormInput input{form_field(body, "offer"),
                             form_field(body, "path"),
                             form_field(body, "location"),
                             form_field(body, "contact_name"),
                             form_field(body, "contact_email"),
                             form_field(body, "company"),
                             form_field(body, "details"),
                             form_field(body, "quantity")};
        const OrderOffer* offer = sheet.find_offer(input.offer);
        if (offer == nullptr) {
            co_return co_await send_order_html(
                route,
                http::status::bad_request,
                web_ui_order_message("Choose an Offer",
                                     "Choose one of the offers on the order sheet."));
        }
        // The field people never see: only a bot fills it. It is answered as if received, so the
        // bot learns nothing, and nothing is recorded or sent.
        if (!form_field(body, "website").empty()) {
            metrics.increment_counter("lake_cloud_orders_refused_total", {{"reason", "honeypot"}});
            co_return co_await send_order_html(
                route,
                http::status::ok,
                web_ui_order_message("Order Received", "Thank you. We will be in touch."));
        }
        const bool checkout_available = order_checkout_available();
        auto order = validate_order_form(sheet, options.regions, input, checkout_available);
        if (!order) {
            co_return co_await send_order_html(
                route,
                http::status::bad_request,
                web_ui_order_form(
                    sheet, options.regions, *offer, input, order.error(), checkout_available));
        }
        if (!order_limiter->allow(
                order_client_address(route), order->path, std::chrono::steady_clock::now())) {
            metrics.increment_counter("lake_cloud_orders_refused_total", {{"reason", "rate"}});
            co_return co_await send_order_html(
                route,
                http::status::too_many_requests,
                web_ui_order_message("Too Many Orders",
                                     "We have received many orders from here in the last hour. "
                                     "Please try again later."));
        }
        order->email_error = options.resend.unavailable_reason();
        auto created = options.store->create_cloud_order(*order);
        if (!created) {
            std::cerr << nlohmann::json{{"event", "cloud_order_not_recorded"},
                                        {"code", created.error().code},
                                        {"message", created.error().message}}
                             .dump()
                      << '\n';
            co_return co_await send_order_html(
                route,
                http::status::service_unavailable,
                web_ui_order_message("Not Recorded",
                                     "We could not record your order just now. Please try again "
                                     "in a minute."));
        }
        metrics.increment_counter("lake_cloud_orders_total",
                                  {{"offer", created->offer}, {"path", created->path}});
        if (!created->email_error.empty()) {
            // The order is kept; its email goes once the key is set (docs/api/orders.md#email).
            std::cerr << nlohmann::json{{"event", "cloud_order_email_waiting"},
                                        {"order_id", created->id},
                                        {"reason", created->email_error}}
                             .dump()
                      << '\n';
        }
        if (created->path == "checkout") {
            ApiContext context = context_for(route);
            context.registry = &registry;
            auto provider = co_await payments->current(context);
            const std::string origin = order_origin();
            const std::string status = order_status_url(*created);
            CheckoutRequest request{created->id,
                                    created->offer,
                                    order_location_label(sheet, options.regions, *created),
                                    offer->monthly_cents,
                                    created->quantity,
                                    "usd",
                                    created->contact_email,
                                    status + "&checkout=done",
                                    status + "&checkout=cancelled"};
            std::shared_ptr<PaymentProvider> payment_provider =
                provider && *provider ? **provider : nullptr;
            std::expected<CheckoutSession, std::string> session =
                std::unexpected(std::string("the payment provider is not available"));
            if (status.empty()) {
                session = std::unexpected(std::string("this Lake has no order sheet URL"));
            } else if (payment_provider) {
                session = payment_provider->create_checkout(request);
            }
            if (session) {
                const auto recorded = options.store->transition_cloud_order(
                    {created->id, "pending_payment", "pending_payment", "lake", "", session->id});
                if (recorded) {
                    co_return co_await redirect(route, session->url);
                }
                session = std::unexpected("the checkout could not be recorded: " +
                                          recorded.error().message);
            }
            // Nothing was paid: the order is cancelled, never provisioned, and the person can
            // send it as an order form instead.
            static_cast<void>(options.store->transition_cloud_order(
                {created->id,
                 "pending_payment",
                 "cancelled",
                 "lake",
                 "the checkout could not start: " + session.error()}));
            std::cerr << nlohmann::json{{"event", "cloud_order_checkout_failed"},
                                        {"order_id", created->id},
                                        {"message", session.error()}}
                             .dump()
                      << '\n';
            co_return co_await send_order_html(
                route,
                http::status::service_unavailable,
                web_ui_order_form(sheet,
                                  options.regions,
                                  *offer,
                                  input,
                                  {{"path",
                                    "Card payment could not start, and nothing was charged. Send "
                                    "it as an order form, or try again in a minute."}},
                                  checkout_available));
        }
        order_pass_requested.store(true);
        co_return co_await send_order_html(
            route,
            http::status::ok,
            web_ui_order_received(sheet, options.regions, *created, order_status_url(*created)));
    }

    // GET /orders/<id>?token=...: the order's status, for whoever holds its link.
    boost::http::route_task serve_order_status(boost::http::route_params& route, std::string id) {
        namespace http = boost::http;
        const std::string token = query_value(route, "token");
        const auto not_found = [&] {
            return web_ui_order_message("Order Not Found",
                                        "This order link is not valid. Use the link in the email "
                                        "we sent you.");
        };
        if (order_status_key.empty() || !order_status_token_matches(order_status_key, id, token)) {
            co_return co_await send_order_html(route, http::status::not_found, not_found());
        }
        const auto order = options.store->get_cloud_order(id);
        if (!order) {
            co_return co_await send_order_html(
                route,
                order.error().code == "not_found" ? http::status::not_found
                                                  : http::status::service_unavailable,
                order.error().code == "not_found"
                    ? not_found()
                    : web_ui_order_message("Try Again", "We could not read your order just now."));
        }
        co_return co_await send_order_html(
            route,
            http::status::ok,
            web_ui_order_status(
                options.order_sheet, options.regions, *order, query_value(route, "checkout")));
    }

    // The order pass (docs/api/orders.md#email, #audit), on the maintenance thread: every order
    // event not yet in the central audit log is recorded there, and every email due is sent
    // through Resend, retried with backoff when it fails.
    void run_order_pass() {
        control::Store& store = *options.store;
        if (auto events = store.unaudited_cloud_order_events(200); events && !events->empty()) {
            std::vector<std::int64_t> recorded;
            const audit::IdentityOptions identity{.configured_seed = options.identity_seed,
                                                  .instance_kek = options.instance_kek};
            for (const control::CloudOrderEvent& event : *events) {
                audit::EventDraft draft{
                    .customer_id = {},
                    .kind = "cloud_order." + event.to_state,
                    .principal = event.actor == "stripe" ? "payment-provider" : event.actor,
                    .object = "cloud_order/" + event.order_id,
                    .action = "cloud_order_state",
                    .outcome = "success",
                    .request_id = {},
                    .method = "order",
                    .device = {},
                    .details = {{"event_id", event.id},
                                {"at", event.at},
                                {"from_state",
                                 event.from_state.empty() ? nlohmann::json(nullptr)
                                                          : nlohmann::json(event.from_state)},
                                {"to_state", event.to_state},
                                {"actor", event.actor},
                                {"detail", event.detail}}};
                if (!audit::record(store, draft, identity)) {
                    break;
                }
                recorded.push_back(event.id);
            }
            static_cast<void>(store.mark_cloud_order_events_audited(recorded));
        }
        if (options.resend.api_key.empty()) {
            if (!order_email_waiting_logged && !options.regions.apex_host.empty() &&
                options.order_sheet.enabled) {
                order_email_waiting_logged = true;
                std::cerr << nlohmann::json{{"event", "cloud_order_email_disabled"},
                                            {"reason", options.resend.unavailable_reason()}}
                                 .dump()
                          << '\n';
            }
            return;
        }
        const ResendHttp http =
            options.resend_http
                ? options.resend_http
                : ResendHttp([this](const ResendRequest& request) {
                      OutboundResponse response = outbound_http.request(
                          {request.method, request.url, request.body, request.headers});
                      return ResendResponse{
                          response.status, std::move(response.body), std::move(response.error)};
                  });
        const auto orders = store.claim_cloud_order_emails(20, 300);
        if (!orders) {
            return;
        }
        for (const control::CloudOrder& order : *orders) {
            for (const std::string& recipient : order_email_recipients_due(order)) {
                const Email email =
                    order_email(options.order_sheet,
                                options.regions,
                                options.resend,
                                order,
                                recipient,
                                recipient == "customer" ? order_status_url(order) : "");
                const auto sent = send_resend_email(options.resend,
                                                    email,
                                                    "lake-order-" + order.id + "-" + order.state +
                                                        "-" + recipient,
                                                    http,
                                                    options.resend_api_base);
                static_cast<void>(store.record_cloud_order_email(
                    {order.id,
                     recipient,
                     order.state,
                     sent.has_value(),
                     sent ? std::string() : sent.error(),
                     order_email_retry_seconds(order.email_attempts + 1)}));
                metrics.increment_counter(
                    "lake_cloud_order_emails_total",
                    {{"recipient", recipient}, {"outcome", sent ? "sent" : "failed"}});
                if (!sent) {
                    std::cerr << nlohmann::json{{"event", "cloud_order_email_failed"},
                                                {"order_id", order.id},
                                                {"recipient", recipient},
                                                {"message", sent.error()}}
                                     .dump()
                              << '\n';
                    break;
                }
            }
        }
    }

    // A file of the embedded web app (docs/ui/web-app.md); 404 when the build has no such file.
    // The app's index.html with the `viewer` answer for this request embedded in it
    // (docs/ui/web-app.md#the-first-paint): the same capability the app would call first, so the
    // first paint needs no round trip. Personal, so never stored by a cache.
    boost::http::route_task serve_web_app_index(boost::http::route_params& route,
                                                const std::string& path) {
        std::string html(web_app_index());
        if (html.empty()) {
            return serve_web_app_file(route, "/index.html");
        }
        ApiContext context = context_for(route);
        nlohmann::json input = nlohmann::json::object();
        const std::string workspace = workspace_path_id(path);
        if (!workspace.empty()) {
            input["workspace_id"] = workspace;
        }
        const ApiResult viewer = registry.call("viewer", input, context);
        if (viewer.ok) {
            std::string data = viewer.envelope().dump();
            // Inside <script>, "<" is the only character that could end the element early.
            std::string escaped;
            escaped.reserve(data.size());
            for (const char c : data) {
                escaped += c == '<' ? std::string("\\u003c") : std::string(1, c);
            }
            const std::string tag =
                "<script id=\"lake-viewer\" type=\"application/json\">" + escaped + "</script>";
            if (const std::size_t head = html.find("</head>"); head != std::string::npos) {
                html.insert(head, tag);
            }
        }
        return send_text(route,
                         boost::http::status::ok,
                         std::move(html),
                         "text/html; charset=utf-8",
                         "private, no-store");
    }

    boost::http::route_task serve_web_app_file(boost::http::route_params& route, std::string path) {
        const WebAppAsset* asset = web_app_asset(path);
        if (asset == nullptr) {
            return send_text(route,
                             boost::http::status::not_found,
                             error_body("not_found", "no such route"),
                             "application/json; charset=utf-8",
                             "no-store");
        }
        return send_text(route,
                         boost::http::status::ok,
                         std::string(asset->body),
                         asset->content_type,
                         web_app_cache_control(asset->path));
    }

    boost::http::route_task serve_style(boost::http::route_params& route) {
        const std::string path(route.url.path());
        return send_text(route,
                         boost::http::status::ok,
                         std::string(web_ui_stylesheet()),
                         "text/css; charset=utf-8",
                         web_ui_asset_cache_control(path));
    }

    boost::http::route_task serve_font(boost::http::route_params& route, std::string_view name) {
        const std::string path(route.url.path());
        return send_text(route,
                         boost::http::status::ok,
                         std::string(web_ui_font(name)),
                         "font/woff2",
                         web_ui_asset_cache_control(path));
    }

    boost::http::route_task serve_openapi(boost::http::route_params& route) {
        const ApiContext context = context_for(route);
        return send_text(route,
                         boost::http::status::ok,
                         context.credential_kind == "agent_task" ? agent_openapi_body
                                                                 : openapi_body,
                         "application/json; charset=utf-8",
                         "no-store");
    }

    boost::http::route_task serve_healthz(boost::http::route_params& route) {
        nlohmann::json body{{"ok", true}};
        if (options.platform) {
            body["platform"] = options.platform->degraded() ? "degraded" : "ready";
            // Rows naming no lake repository are not shown; the count says so (lake#805).
            if (const auto unresolved = options.platform->unresolved_repositories().size();
                unresolved != 0) {
                body["platform_unresolved_repositories"] = unresolved;
            }
        }
        return send_text(route,
                         boost::http::status::ok,
                         body.dump(),
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
        // The store-root half of readiness (lake#479, docs/deployment/store-volume.md). Ready
        // when the root is open, and also while a predecessor holds it: requests sent here wait
        // for the hand-off instead of failing, and a surge rollout stops the predecessor only
        // after this pod is Ready, so withholding readiness here would deadlock the rollout. Not
        // ready during an uncontended open, after a failed one, or once stop() has begun.
        const StoreRootState state = store_root_state.load();
        if (draining.load()) {
            return send_text(route,
                             boost::http::status::service_unavailable,
                             error_body("store_unavailable", "Lake is stopping"),
                             "application/json; charset=utf-8",
                             "no-store");
        }
        const bool ready = state == StoreRootState::open || state == StoreRootState::waiting ||
                           (state == StoreRootState::opening && store_root_handoff.load());
        if (!ready) {
            std::string message = "the store root is " + std::string(to_string(state));
            if (state == StoreRootState::failed) {
                std::lock_guard lock(store_root_mutex);
                message += ": " + store_root_failure;
            }
            return send_text(route,
                             boost::http::status::service_unavailable,
                             error_body("store_unavailable", message),
                             "application/json; charset=utf-8",
                             "no-store");
        }
        nlohmann::json body{{"ok", true}, {"store", options.store->kind()}};
        if (!options.store_root.empty()) {
            body["store_root"] = std::string(to_string(state));
            if (state != StoreRootState::open) {
                std::lock_guard lock(store_root_mutex);
                body["store_root_holder"] = store_root_holder;
            }
        }
        return send_text(route,
                         boost::http::status::ok,
                         body.dump(),
                         "application/json; charset=utf-8",
                         "no-store");
    }

    // Settles the dynamic-secret leases that are due (lake#362) and counts what it did.
    void expire_secret_leases() {
        ApiContext context;
        context.store = options.store.get();
        context.workspaces = options.workspaces.get();
        context.instance_kek = options.instance_kek;
        const auto sweep = liquid::runtime::block_on(api::expire_secret_leases(context));
        if (!sweep) {
            metrics.increment_counter("lake_secret_lease_sweep_errors_total");
            return;
        }
        const auto count = [this](const char* outcome, std::size_t value) {
            if (value > 0) {
                metrics.increment_counter("lake_secret_leases_settled_total",
                                          {{"outcome", outcome}},
                                          static_cast<double>(value));
            }
        };
        count("expired", sweep->expired);
        count("settled", sweep->settled);
        count("rescheduled", sweep->rescheduled);
        count("removed", sweep->removed);
        count("failed", sweep->failed);
    }

    // Finishes the workspace deletions a crash or an unreachable dependency left incomplete
    // (lake#514): their tenant image tags, search segments and central audit events.
    void resume_workspace_deletions() {
        ApiContext context;
        context.store = options.store.get();
        context.search = options.search.get();
        context.identity_seed = options.identity_seed;
        context.instance_kek = options.instance_kek;
        if (artifacts.available()) {
            context.workspace_content = &workspace_content;
        }
        const auto resumed = api::resume_workspace_deletions(context);
        if (!resumed) {
            metrics.increment_counter("lake_workspace_deletion_errors_total");
            std::println(stderr, "lake: workspace deletion pending: {}", resumed.error());
        } else if (*resumed > 0) {
            metrics.increment_counter(
                "lake_workspace_deletions_completed_total", {}, static_cast<double>(*resumed));
        }
    }

    // Writes the repository usage counted since the last flush to the workspace Zettas, off the
    // request path (lake#348), and reports what was written, kept or dropped as counters.
    void flush_metering() {
        if (!metering_recorder) {
            return;
        }
        static_cast<void>(liquid::runtime::block_on(metering_recorder->flush()));
        const MeteringRecorder::Totals now = metering_recorder->totals();
        const auto report = [this](const char* name,
                                   const telemetry::Labels& labels,
                                   std::uint64_t current,
                                   std::uint64_t& reported) {
            if (current > reported) {
                metrics.increment_counter(name, labels, static_cast<double>(current - reported));
                reported = current;
            }
        };
        report("lake_metering_records_written_total",
               {{"collection", "metering_usage"}},
               now.flushed_transfers,
               reported_metering.flushed_transfers);
        report("lake_metering_records_written_total",
               {{"collection", "metering_build_records"}},
               now.flushed_builds,
               reported_metering.flushed_builds);
        report("lake_metering_flush_errors_total",
               {},
               now.flush_errors,
               reported_metering.flush_errors);
        report("lake_metering_dropped_bytes_total",
               {{"reason", "dropped"}},
               now.dropped_bytes,
               reported_metering.dropped_bytes);
        report("lake_metering_dropped_bytes_total",
               {{"reason", "no_workspace"}},
               now.unattributed_bytes,
               reported_metering.unattributed_bytes);
        report("lake_metering_dropped_builds_total",
               {},
               now.dropped_builds,
               reported_metering.dropped_builds);
        metrics.set_gauge(
            "lake_metering_pending", {}, static_cast<double>(metering_recorder->pending()));
    }

    // Appends the queued workspace events to their workspaces' audits, off the request path
    // (decision 52), and reports what was written, kept or dropped as counters.
    void flush_audit() {
        if (!audit_recorder) {
            return;
        }
        static_cast<void>(liquid::runtime::block_on(audit_recorder->flush()));
        const AuditRecorder::Totals now = audit_recorder->totals();
        const auto report = [this](const char* name,
                                   const telemetry::Labels& labels,
                                   std::uint64_t current,
                                   std::uint64_t& reported) {
            if (current > reported) {
                metrics.increment_counter(name, labels, static_cast<double>(current - reported));
                reported = current;
            }
        };
        report(
            "lake_audit_workspace_events_written_total", {}, now.written, reported_audit.written);
        report("lake_audit_workspace_flush_errors_total",
               {},
               now.flush_errors,
               reported_audit.flush_errors);
        report("lake_audit_workspace_conflicts_total", {}, now.conflicts, reported_audit.conflicts);
        report("lake_audit_workspace_dropped_total",
               {{"reason", "bound"}},
               now.dropped,
               reported_audit.dropped);
        report("lake_audit_workspace_dropped_total",
               {{"reason", "workspace_deleted"}},
               now.dropped_gone,
               reported_audit.dropped_gone);
        metrics.set_gauge(
            "lake_audit_workspace_pending", {}, static_cast<double>(audit_recorder->pending()));
    }

    // Writes the requests counted since the last flush to their workspaces' `lake_requests`
    // series, off the request path (lake#384), and reports what was written, kept or dropped.
    void flush_request_series() {
        if (!request_recorder) {
            return;
        }
        static_cast<void>(liquid::runtime::block_on(request_recorder->flush()));
        const RequestRecorder::Totals now = request_recorder->totals();
        const auto report = [this](const char* name,
                                   const telemetry::Labels& labels,
                                   std::uint64_t current,
                                   std::uint64_t& reported) {
            if (current > reported) {
                metrics.increment_counter(name, labels, static_cast<double>(current - reported));
                reported = current;
            }
        };
        report("lake_request_series_requests_written_total",
               {},
               now.recorded,
               reported_request_series.recorded);
        report("lake_request_series_model_calls_written_total",
               {},
               now.model_calls,
               reported_request_series.model_calls);
        report("lake_request_series_flush_errors_total",
               {},
               now.flush_errors,
               reported_request_series.flush_errors);
        report("lake_request_series_dropped_total",
               {{"reason", "bound"}},
               now.dropped_bound,
               reported_request_series.dropped_bound);
        report("lake_request_series_dropped_total",
               {{"reason", "workspace_deleted"}},
               now.dropped_workspace_deleted,
               reported_request_series.dropped_workspace_deleted);
        report("lake_request_series_dropped_total",
               {{"reason", "refused"}},
               now.dropped_refused,
               reported_request_series.dropped_refused);
        metrics.set_gauge(
            "lake_request_series_pending", {}, static_cast<double>(request_recorder->pending()));
    }

    // Writes the usage counted since the last flush to each workspace Zetta's `lake_usage` and
    // `lake_storage` series, off the request path (lake#564), and reports what was written, kept
    // or dropped.
    void flush_usage() {
        if (!usage_recorder) {
            return;
        }
        static_cast<void>(liquid::runtime::block_on(usage_recorder->flush()));
        const UsageRecorder::Totals now = usage_recorder->totals();
        const auto report = [this](const char* name,
                                   const telemetry::Labels& labels,
                                   std::uint64_t current,
                                   std::uint64_t& reported) {
            if (current > reported) {
                metrics.increment_counter(name, labels, static_cast<double>(current - reported));
                reported = current;
            }
        };
        report("lake_usage_points_written_total", {}, now.points, reported_usage.points);
        report("lake_usage_flush_errors_total", {}, now.flush_errors, reported_usage.flush_errors);
        report("lake_usage_dropped_total",
               {{"reason", "bound"}},
               now.dropped_bound,
               reported_usage.dropped_bound);
        report("lake_usage_dropped_total",
               {{"reason", "workspace_deleted"}},
               now.dropped_workspace_deleted,
               reported_usage.dropped_workspace_deleted);
        report("lake_usage_dropped_total",
               {{"reason", "refused"}},
               now.dropped_refused,
               reported_usage.dropped_refused);
        metrics.set_gauge("lake_usage_pending", {}, static_cast<double>(usage_recorder->pending()));
    }

    // The store root's owner samples every workspace's stored bytes and the instance and volume
    // time of its services, stores and apps since the last sample (lake#564). Runs on the
    // maintenance thread: it reads repository stores and workspace Zettas.
    void sample_usage() {
        if (!usage_recorder) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto interval = std::max<std::chrono::steady_clock::duration>(
            options.usage_sample_interval, std::chrono::seconds(1));
        // The time since the last sample, at most two intervals: a gap while no lake owned the
        // store root is not counted as if nothing had changed.
        auto elapsed = last_usage_sample == std::chrono::steady_clock::time_point{}
                           ? interval
                           : std::min(now - last_usage_sample, 2 * interval);
        last_usage_sample = now;
        ApiContext context;
        context.store = options.store.get();
        context.workspaces = options.workspaces.get();
        context.sync_store = &sync_store;
        context.instance_kek = options.instance_kek;
        context.metering_recorder = metering_recorder.get();
        context.usage_recorder = usage_recorder.get();
        const auto sampled = liquid::runtime::block_on(
            api::sample_usage(context, std::chrono::duration_cast<std::chrono::seconds>(elapsed)));
        if (!sampled) {
            metrics.increment_counter("lake_usage_sample_errors_total");
            return;
        }
        metrics.increment_counter("lake_usage_samples_total");
        if (!sampled->unmeasured.empty()) {
            metrics.increment_counter("lake_usage_sample_errors_total",
                                      {},
                                      static_cast<double>(sampled->unmeasured.size()));
        }
    }

    void run_mcp_streams() {
        ApiContext context;
        context.store = options.store.get();
        context.workspaces = options.workspaces.get();
        context.instance_kek = options.instance_kek;
        context.registry = &registry;
        context.mcp_http = mcp_http;
        context.mcp_allow_private_addresses = options.mcp_allow_private_addresses;
        context.usage_recorder = usage_recorder.get();
        context.audit_recorder = audit_recorder.get();
        context.external_origin = options.external_origin;
        const McpStreamPass done = mcp_streams.pass(context);
        metrics.increment_counter("lake_mcp_stream_passes_total");
        if (done.notifications > 0) {
            metrics.increment_counter("lake_mcp_notifications_mirrored_total",
                                      {},
                                      static_cast<double>(done.notifications));
        }
        if (done.turns > 0) {
            metrics.increment_counter(
                "lake_mcp_stream_turns_total", {}, static_cast<double>(done.turns));
        }
        if (!done.problems.empty()) {
            metrics.increment_counter(
                "lake_mcp_stream_problems_total", {}, static_cast<double>(done.problems.size()));
        }
        // Agents started by a workspace stream (lake#677), on the same owner-only pass.
        const AgentStreamPass started = agent_streams.pass(context);
        metrics.increment_counter("lake_agent_stream_passes_total");
        if (started.runs > 0) {
            metrics.increment_counter(
                "lake_agent_stream_runs_total", {}, static_cast<double>(started.runs));
        }
        if (!started.problems.empty()) {
            metrics.increment_counter("lake_agent_stream_problems_total",
                                      {},
                                      static_cast<double>(started.problems.size()));
        }
    }

    void log_store_owner(nlohmann::json event) const {
        event["event"] = "store_owner";
        event["node"] = options.node_name;
        std::println(stderr, "{}", event.dump());
    }

    // The owner thread: take the store root, waiting for a predecessor to drain, then open the
    // customer stores. Requests that need the root wait for StoreRootState::open.
    void own_store_root() {
        const auto started = std::chrono::steady_clock::now();
        StoreOwnerWait wait;
        wait.limit = options.store_owner_wait;
        wait.abandoned = [this] { return owner_wait_abandoned.load(); };
        wait.on_held = [this](const std::string& holder) {
            {
                std::lock_guard lock(store_root_mutex);
                store_root_holder = holder;
            }
            store_root_handoff.store(true);
            store_root_state.store(StoreRootState::waiting);
            log_store_owner({{"state", "waiting"},
                             {"holder", holder},
                             {"wait_ms", options.store_owner_wait.count()}});
        };
        auto owner = StoreOwnerLock::acquire(options.store_root, options.node_name, wait);
        if (!owner) {
            if (owner_wait_abandoned.load()) {
                log_store_owner({{"state", "abandoned"}, {"error", owner.error()}});
                return;
            }
            {
                std::lock_guard lock(store_root_mutex);
                store_root_failure = owner.error();
            }
            store_root_state.store(StoreRootState::failed);
            log_store_owner({{"state", "failed"}, {"error", owner.error()}});
            work_cv.notify_all();
            return;
        }
        store_owner = std::move(*owner);
        // A predecessor's drain announcement (lake#536) names a lock that is gone now.
        StoreOwnerLock::clear_draining(options.store_root);
        const auto acquired = std::chrono::steady_clock::now();
        // Open the existing stores and indexes before store-touching requests are admitted.
        const auto warmed = sync_store.warm_metering();
        if (!warmed) {
            std::println(stderr, "metering warmup: {}", warmed.error().message);
        }
        schedule_maintenance("workspace usage priming", [this] { prime_workspace_usage(); });
        schedule_maintenance("space search index purge", [this] { purge_space_search_indexes(); });
        store_root_state.store(StoreRootState::open);
        const auto milliseconds = [](auto duration) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        };
        log_store_owner({{"state", "open"},
                         {"handoff", store_root_handoff.load()},
                         {"waited_ms", milliseconds(acquired - started)},
                         {"opened_ms", milliseconds(std::chrono::steady_clock::now() - acquired)}});
    }

    // Middleware: a request that may touch the store root waits, without blocking the io thread,
    // until this process owns the root and has opened it. It never fails while a hand-off is in
    // progress; it answers 503 only if the wait expired, the open failed, or the process is
    // stopping before it ever owned the root.
    // ---- Cross-site request protection (lake#561, docs/serving/http.md#cross-site-requests) ----

    // The browser session authenticates this request: no bearer, and a lake_session cookie.
    // context_for resolves exactly this case to authentication_method "session".
    static bool session_cookie_request(const boost::http::route_params& route) {
        return bearer_of(route).empty() && !cookie_value(route, "lake_session").empty();
    }

    static bool unsafe_method(const boost::http::route_params& route) {
        const auto method = route.req.method();
        return method == boost::http::method::post || method == boost::http::method::put ||
               method == boost::http::method::patch || method == boost::http::method::delete_;
    }

    // Whether a browser says this request came from lake's own origin. A missing Origin and a
    // missing Sec-Fetch-Site are a non-browser client's, and pass: the cookie is the browser's.
    bool from_own_origin(const boost::http::route_params& route) const {
        const auto lower = [](std::string_view text) {
            std::string out(text);
            std::ranges::transform(out, out.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return out;
        };
        if (route.req.count("sec-fetch-site") != 0 &&
            lower(route.req.at("sec-fetch-site")) != "same-origin") {
            return false;
        }
        if (route.req.count(boost::http::field::origin) == 0) {
            return true;
        }
        std::string origin = lower(route.req.at(boost::http::field::origin));
        while (origin.ends_with('/')) {
            origin.pop_back();
        }
        if (origin == "null" || origin.empty()) {
            return false;
        }
        std::string configured = lower(options.external_origin);
        while (configured.ends_with('/')) {
            configured.pop_back();
        }
        if ((!configured.empty() && origin == configured) ||
            (!options.public_host.empty() && origin == "https://" + lower(options.public_host))) {
            return true;
        }
        // The origin this request was sent to: the Host it carries, under either scheme (a proxy
        // in front of lake terminates TLS).
        const std::size_t scheme = origin.find("://");
        if (scheme == std::string::npos || route.req.count(boost::http::field::host) == 0) {
            return false;
        }
        const std::string scheme_name = origin.substr(0, scheme);
        return (scheme_name == "https" || scheme_name == "http") &&
               origin.substr(scheme + 3) == lower(route.req.at(boost::http::field::host));
    }

    static bool json_content_type(const boost::http::route_params& route) {
        if (route.req.count(boost::http::field::content_type) == 0) {
            return false;
        }
        std::string_view type = route.req.at(boost::http::field::content_type);
        type = type.substr(0, type.find(';'));
        while (!type.empty() && type.back() == ' ') {
            type.remove_suffix(1);
        }
        std::string lowered(type);
        std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return lowered == "application/json";
    }

    // An unsafe request the browser session authenticates must come from lake's own origin, and a
    // /v1 or /mcp POST must be JSON, which no cross-site form or simple request can send.
    // Bearer-authenticated calls are not affected. Empty: the request may proceed.
    std::string cross_site_refusal(const boost::http::route_params& route) const {
        if (!unsafe_method(route) || !session_cookie_request(route)) {
            return {};
        }
        // The SAML Assertion Consumer Service takes the identity provider's cross-site form post
        // (lake#836); it never reads the session the cookie carries, it replaces it.
        if (const std::string path(route.url.path());
            path.starts_with("/v1/saml/") && path.ends_with("/acs")) {
            return {};
        }
        if (!from_own_origin(route)) {
            return "Cross-site requests cannot use the browser session.";
        }
        const std::string path(route.url.path());
        const std::string local =
            workspace_path_id(path).empty() ? path : workspace_prefixed_local_path(path);
        const bool json_api = local == "/mcp" || (local.starts_with("/v1/") &&
                                                  !local.starts_with("/v1/repositories/"));
        if (json_api && route.req.method() == boost::http::method::post &&
            !json_content_type(route)) {
            return "Cross-site requests cannot use the browser session.";
        }
        return {};
    }

    boost::http::route_task refuse_cross_site(boost::http::route_params& route) {
        const std::string refusal = cross_site_refusal(route);
        if (refusal.empty()) {
            co_return boost::http::route_next;
        }
        co_return co_await send_text(route,
                                     boost::http::status::forbidden,
                                     error_body("unauthorized", refusal),
                                     "application/json; charset=utf-8",
                                     "no-store");
    }

    boost::http::route_task await_store_root(boost::http::route_params& route) {
        namespace http = boost::http;
        if (store_root_state.load() == StoreRootState::open) {
            co_return http::route_next;
        }
        const std::string path(route.url.path());
        if (store_root_exempt(path)) {
            co_return http::route_next;
        }
        // The owner thread gives up after store_owner_wait; the margin covers opening the stores.
        const auto deadline =
            std::chrono::steady_clock::now() + options.store_owner_wait + std::chrono::minutes(2);
        boost::corosio::timer timer(io_context);
        auto pause = std::chrono::milliseconds(5);
        std::string reason = "Lake gave up waiting for the store root";
        for (;;) {
            const StoreRootState state = store_root_state.load();
            if (state == StoreRootState::open) {
                co_return http::route_next;
            }
            if (state == StoreRootState::failed) {
                std::lock_guard lock(store_root_mutex);
                reason = store_root_failure;
                break;
            }
            if (draining.load()) {
                reason = "Lake is stopping before it opened the store root";
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            timer.expires_after(pause);
            auto [error] = co_await timer.wait();
            if (error) {
                reason = "Lake is stopping before it opened the store root";
                break;
            }
            pause = std::min(pause * 2, std::chrono::milliseconds(100));
        }
        co_return co_await send_text(route,
                                     http::status::service_unavailable,
                                     error_body("store_unavailable", reason),
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
            JsonBodySink sink;
            auto [read_error, bytes_read] = co_await boost::capy::push_to(route.req_body, sink);
            if (read_error || bytes_read == 0) {
                co_return co_await send_scim(
                    route,
                    refuse_unread_body(
                        route,
                        capability,
                        ApiResult::failure("invalid_input", "SCIM resource body is required"),
                        read_error ? "unreadable" : "empty",
                        bytes_read),
                    success_status);
            }
            nlohmann::json resource = body_json(sink);
            if (resource.is_discarded() || !resource.is_object()) {
                co_return co_await send_scim(
                    route,
                    refuse_unread_body(
                        route,
                        capability,
                        ApiResult::failure("invalid_input", "SCIM resource body is not valid JSON"),
                        resource.is_discarded() ? "not_json" : "not_an_object",
                        bytes_read),
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
            JsonBodySink sink;
            auto [read_error, bytes_read] = co_await boost::capy::push_to(route.req_body, sink);
            if (read_error || bytes_read == 0) {
                co_return co_await send_scim(
                    route,
                    refuse_unread_body(
                        route,
                        capability,
                        ApiResult::failure("invalid_input", "SCIM PATCH body is required"),
                        read_error ? "unreadable" : "empty",
                        bytes_read),
                    success_status);
            }
            nlohmann::json patch = body_json(sink);
            if (patch.is_discarded() || !patch.is_object()) {
                co_return co_await send_scim(
                    route,
                    refuse_unread_body(
                        route,
                        capability,
                        ApiResult::failure("invalid_input", "SCIM PATCH body is not valid JSON"),
                        patch.is_discarded() ? "not_json" : "not_an_object",
                        bytes_read),
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
            const ApiResult result =
                unwrap(co_await registry.call_async(capability_name, input, context));
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
        // push reads branch protection from the repository's workspace first (lake#341).
        const ApiResult result =
            unwrap(co_await registry.call_async(capability_name, input, context));
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
        const ApiResult result = unwrap(co_await registry.call_async("read_chunk", input, context));
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
    // /workspaces/<customer_id>/zetta/v1/<capability>: a workspace's Zetta for agents, in
    // Zetta's own REST shape (reads GET with the query string, actions POST with a JSON body),
    // through workspace_query / workspace_act so the allow-lists, authorization and audit are
    // the ones every other lake caller gets (docs/api/workspaces.md, Agents reach Zetta).
    boost::http::route_task serve_workspace_zetta(boost::http::route_params& route, bool openapi) {
        namespace http = boost::http;
        const std::string customer_id = route_value(route, "customer_id");
        const bool get = route.req.method() == http::method::get;
        ApiContext context = context_for(route);
        nlohmann::json scoped_input{{"customer_id", customer_id}};
        if (!context.path_workspace_id.empty()) {
            scoped_input["workspace_id"] = context.path_workspace_id;
        }
        http::status status = http::status::ok;
        std::string body;
        std::string allow;
        if (openapi) {
            auto completed =
                co_await registry.call_async("workspace_openapi", scoped_input, context);
            const ApiResult result =
                completed ? std::move(*completed)
                          : ApiResult::failure("store_error", completed.error().message);
            status = http::int_to_status(static_cast<unsigned>(rest_status(result)));
            body = result.ok ? result.value.dump() : rest_envelope(result).dump();
        } else {
            const std::string capability = route_value(route, "capability");
            const bool read = workspace_read_allowed(capability);
            const bool action = workspace_action_allowed(capability);
            nlohmann::json input = nlohmann::json::object();
            if (!read && !action) {
                status = http::status::not_found;
                body =
                    error_body("unknown_capability", capability + " is not available through Lake");
            } else if (workspace_uses_get(capability) != get) {
                status = http::status::method_not_allowed;
                allow = workspace_uses_get(capability) ? "GET" : "POST";
                body = error_body("method_not_allowed",
                                  capability + " takes " + allow + " through Lake");
            } else if (get) {
                // Values stay strings: the workspace coerces them by its own input schema, as a
                // direct call to Zetta would.
                for (const auto& parameter : route.url.params()) {
                    input[std::string(parameter.key)] = std::string(parameter.value);
                }
            } else {
                JsonBodySink sink;
                auto [read_error, bytes_read] = co_await boost::capy::push_to(route.req_body, sink);
                std::string reason;
                std::string message;
                if (read_error) {
                    reason = "unreadable";
                    message = "could not read the request body as JSON";
                } else if (bytes_read > 0) {
                    input = body_json(sink);
                    if (input.is_discarded() || !input.is_object()) {
                        reason = input.is_discarded() ? "not_json" : "not_an_object";
                        message = "the request body is not a JSON object";
                    }
                }
                if (!reason.empty()) {
                    // Audited as the workspace action it was meant to be (lake#18): the reason
                    // and the body's size, never its contents.
                    status = http::status::bad_request;
                    const ApiResult refused =
                        registry.refuse(read ? "workspace_query" : "workspace_act",
                                        ApiResult::failure("invalid_input", message),
                                        {{"capability", capability},
                                         {"input", reason},
                                         {"body_bytes", static_cast<std::uint64_t>(bytes_read)}},
                                        context);
                    if (refused.error.code != "invalid_input") {
                        status = http::int_to_status(static_cast<unsigned>(rest_status(refused)));
                    }
                    body = error_body(refused.error.code, refused.error.message);
                }
            }
            if (body.empty()) {
                scoped_input["capability"] = capability;
                scoped_input["input"] = input;
                auto completed = co_await registry.call_async(
                    read ? "workspace_query" : "workspace_act", scoped_input, context);
                const ApiResult result =
                    completed ? std::move(*completed)
                              : ApiResult::failure("store_error", completed.error().message);
                status = http::int_to_status(static_cast<unsigned>(rest_status(result)));
                body = rest_envelope(result).dump();
            }
        }
        route.status(status);
        route.res.set(http::field::content_type, "application/json; charset=utf-8");
        route.res.set(http::field::cache_control, "no-store");
        if (!allow.empty()) {
            route.res.set(http::field::allow, allow);
        }
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
        if (send_error)
            co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // A streamed model answer on its way to the caller (lake#665): the outbound client's thread
    // offers each piece as it arrives, and the route's relay takes and writes them.
    struct ModelStreamRelay {
        // What a caller that reads slower than the provider writes may fall behind by.
        static constexpr std::size_t queue_limit = 8 * 1024 * 1024;
        std::mutex mutex;
        std::string queued;
        std::atomic<bool> gone{false};   // the caller stopped reading: the provider is stopped
        std::atomic<bool> closed{false}; // the call has finished: nothing more is offered

        bool offer(std::string_view piece) {
            if (gone.load()) {
                return false;
            }
            std::lock_guard lock(mutex);
            if (queued.size() + piece.size() > queue_limit) {
                gone.store(true);
                return false;
            }
            queued.append(piece);
            return true;
        }
        std::string take() {
            std::lock_guard lock(mutex);
            return std::exchange(queued, std::string());
        }
    };

    static liquid::runtime::task<ApiResult>
    call_then_close(liquid::runtime::task<ApiResult> call, std::shared_ptr<ModelStreamRelay> relay) {
        // Closed however the call ends, so the relay never waits on a call that threw.
        struct Close {
            ModelStreamRelay* relay;
            ~Close() {
                relay->closed.store(true);
            }
        } close{relay.get()};
        co_return co_await std::move(call);
    }

    // Writes each piece the provider has sent since the last look, starting the chunked response
    // with the first; returns once the call has finished and everything offered is written. A
    // write that fails leaves the relay `gone`, so the provider is stopped, and is answered in
    // `failed` once the call has finished.
    boost::capy::io_task<> relay_model_stream(boost::http::route_params& route,
                                              std::shared_ptr<ModelStreamRelay> relay,
                                              bool& started,
                                              std::error_code& failed) {
        namespace http = boost::http;
        for (;;) {
            // Read `closed` first: every piece is offered before the call finishes, so once it
            // is seen, the take below is the last.
            const bool finished = relay->closed.load();
            const std::string pieces = relay->take();
            if (!pieces.empty() && !relay->gone.load()) {
                if (!started) {
                    started = true;
                    route.status(http::status::ok);
                    route.res.set(http::field::content_type, "text/event-stream; charset=utf-8");
                    route.res.set(http::field::cache_control, "no-store");
                    route.res.set("Server-Timing", server_timing_of(route));
                    route.res.erase(http::field::etag);
                    route.res.erase(http::field::content_length);
                    route.res.set_chunked(true);
                }
                std::size_t offset = 0;
                while (offset < pieces.size()) {
                    auto buffers = std::array<boost::capy::mutable_buffer, 16>{};
                    const auto output = route.res_body.prepare(buffers);
                    const auto copied = boost::capy::buffer_copy(
                        output,
                        boost::capy::const_buffer(pieces.data() + offset, pieces.size() - offset));
                    offset += copied;
                    // A caller that takes no bytes for the idle timeout loses its stream.
                    auto [send_error] = co_await boost::capy::timeout(
                        route.res_body.commit(copied), options.registry_stream_idle_timeout);
                    if (send_error) {
                        failed = send_error;
                        relay->gone.store(true);
                        break;
                    }
                }
                continue;
            }
            if (finished) {
                co_return {};
            }
            [[maybe_unused]] auto [waited] =
                co_await boost::capy::delay(std::chrono::milliseconds(5));
        }
    }

    // POST /openai/v1/<endpoint> — the model gateway (docs/api/model-gateway.md, lake#248): the
    // OpenAI API's own request and answer around model_completion, so an agent's OpenAI SDK points
    // its base URL at `<lake>/openai/v1` with its task, launch or schedule credential as the key.
    // A failure is OpenAI's error shape, `{error: {message, type, code, param}}`, with Lake's code.
    // A streamed answer is relayed event by event as the provider sends it (lake#665).
    boost::http::route_task serve_model_gateway(boost::http::route_params& route,
                                                std::string endpoint) {
        namespace http = boost::http;
        http::status status = http::status::ok;
        std::string body;
        std::string content_type = "application/json; charset=utf-8";
        const auto openai_error = [](const ApiResult& result) {
            return nlohmann::json{
                {"error",
                 {{"message", result.error.message},
                  {"type", result.error.code},
                  {"code", result.error.code},
                  {"param", result.error.path.empty() ? nlohmann::json(nullptr)
                                                      : nlohmann::json(result.error.path)}}}}
                .dump();
        };
        nlohmann::json request;
        JsonBodySink sink;
        auto [read_error, bytes_read] = co_await boost::capy::push_to(route.req_body, sink);
        // A body lake cannot read is audited as a failed model_completion (lake#18).
        const auto refused = [&](ApiResult failure, std::string_view reason) {
            const ApiResult result = refuse_unread_body(
                route, "model_completion", std::move(failure), reason, bytes_read);
            if (result.error.code == "store_error") {
                status = http::status::internal_server_error;
            }
            return openai_error(result);
        };
        if (read_error || bytes_read == 0) {
            status = read_error && body_is_too_large(read_error) ? http::status::payload_too_large
                                                                 : http::status::bad_request;
            body = refused(
                ApiResult::failure(status == http::status::payload_too_large ? "size_invalid"
                                                                             : "invalid_input",
                                   "the request body must be the endpoint's JSON request"),
                !read_error                                 ? "empty"
                : status == http::status::payload_too_large ? "too_large"
                                                            : "unreadable");
        } else {
            request = body_json(sink);
            if (request.is_discarded() || !request.is_object()) {
                status = http::status::bad_request;
                body = refused(
                    ApiResult::failure("invalid_input", "the request body is not a JSON object"),
                    request.is_discarded() ? "not_json" : "not_an_object");
            }
        }
        if (body.empty()) {
            ApiContext ctx = context_for(route);
            const auto stream_field = request.find("stream");
            const bool streamed = stream_field != request.end() && stream_field->is_boolean() &&
                                  stream_field->get<bool>();
            const nlohmann::json input{{"endpoint", endpoint}, {"request", std::move(request)}};
            liquid::result<ApiResult> completed;
            if (streamed) {
                // A stream goes to the caller event by event as the provider sends it (lake#665):
                // the call and the relay run together, and the relay writes the head and each
                // piece as it arrives, in chunked transfer. A call refused before its first event
                // is answered below like any other.
                auto relay = std::make_shared<ModelStreamRelay>();
                ctx.model_stream = [relay](std::string_view piece) { return relay->offer(piece); };
                bool started = false;
                std::error_code relay_error;
                auto [wait_error, call_done, relay_done] = co_await liquid::runtime::when_all(
                    liquid::runtime::as_io(
                        call_then_close(registry.call_async("model_completion", input, ctx), relay)),
                    relay_model_stream(route, relay, started, relay_error));
                completed = wait_error ? std::unexpected(liquid::error{liquid::errc::internal,
                                                                      wait_error.message(),
                                                                      "model_gateway"})
                                       : std::move(call_done);
                if (started) {
                    // The head is sent: a failure now can only end the stream.
                    if (!completed || !completed->ok) {
                        std::cerr << nlohmann::json{
                                         {"event", "model_stream_failed"},
                                         {"request_id", ctx.request_id},
                                         {"error",
                                          completed ? completed->error.code + ": " +
                                                          completed->error.message
                                                    : completed.error().message}}
                                         .dump()
                                  << '\n';
                    }
                    if (relay_error) {
                        co_return http::route_error(relay_error);
                    }
                    auto [end_error] = co_await route.res_body.commit_eof(0);
                    if (end_error)
                        co_return http::route_error(end_error);
                    co_return http::route_done;
                }
            } else {
                completed = co_await registry.call_async("model_completion", input, ctx);
            }
            const ApiResult result =
                completed ? std::move(*completed)
                          : ApiResult::failure("store_error", completed.error().message);
            status = http::int_to_status(static_cast<unsigned>(rest_status(result)));
            if (!result.ok) {
                body = openai_error(result);
            } else if (result.value.value("streamed", false)) {
                content_type = "text/event-stream; charset=utf-8";
                body = result.value.value("events", std::string());
            } else {
                body = result.value.value("response", nlohmann::json::object()).dump();
            }
        }
        route.status(status);
        route.res.set(http::field::content_type, content_type);
        route.res.set(http::field::cache_control, "no-store");
        if (status == http::status::service_unavailable ||
            status == http::status::too_many_requests) {
            route.res.set(http::field::retry_after, "1");
        }
        route.res.set("Server-Timing", server_timing_of(route));
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
        if (send_error)
            co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // A call to `capability` whose body lake could not read (lake#18): audited once, as any failed
    // call is, with the reason and the body's size, never its contents. Answers the refusal, or
    // the audit's own failure when the event could not be recorded.
    ApiResult refuse_unread_body(const boost::http::route_params& route,
                                 const std::string& capability,
                                 ApiResult failure,
                                 std::string_view reason,
                                 std::size_t bytes) const {
        ApiContext ctx = context_for(route);
        return registry.refuse(
            capability,
            std::move(failure),
            {{"input", std::string(reason)}, {"body_bytes", static_cast<std::uint64_t>(bytes)}},
            ctx);
    }

    boost::http::route_task serve_capability(boost::http::route_params& route,
                                             std::shared_ptr<const Capability> capability) {
        namespace http = boost::http;
        nlohmann::json input = nlohmann::json::object();
        std::string body;
        std::string set_cookie;
        std::string www_authenticate;
        http::status status = http::status::ok;

        if (route.req.method() == http::method::get) {
            std::vector<std::pair<std::string, std::string>> query;
            for (const auto& parameter : route.url.params())
                query.emplace_back(parameter.key, parameter.value);
            input = rest_input_from_query(*capability, query);
        } else {
            JsonBodySink sink;
            auto [read_error, bytes_read] = co_await boost::capy::push_to(route.req_body, sink);
            std::optional<ApiResult> unread;
            std::string reason;
            if (read_error) {
                status = body_is_too_large(read_error) ? http::status::payload_too_large
                                                       : http::status::bad_request;
                reason = body_is_too_large(read_error) ? "too_large" : "unreadable";
                unread = body_is_too_large(read_error)
                             ? ApiResult::failure("size_invalid",
                                                  "request body exceeds the 67108864 byte limit")
                             : ApiResult::failure("invalid_input",
                                                  "could not read the request body as JSON: " +
                                                      read_error.message());
            } else if (bytes_read > 0) {
                input = body_json(sink);
                if (input.is_discarded()) {
                    status = http::status::bad_request;
                    reason = "not_json";
                    unread =
                        ApiResult::failure("invalid_input", "the request body is not valid JSON");
                }
            }
            if (unread) {
                const ApiResult refused =
                    refuse_unread_body(route, capability->name, *unread, reason, bytes_read);
                if (refused.error.code != unread->error.code) {
                    status = http::int_to_status(static_cast<unsigned>(rest_status(refused)));
                }
                body = error_body(refused.error.code, refused.error.message);
            }
        }
        if (body.empty()) {
            ApiContext ctx = context_for(route);
            auto completed = co_await registry.call_async(capability->name, input, ctx);
            const ApiResult result =
                completed ? std::move(*completed)
                          : ApiResult::failure("store_error", completed.error().message);
            status = http::int_to_status(static_cast<unsigned>(rest_status(result)));
            body = rest_envelope(result).dump();
            if (!result.ok && result.error.code == "unauthenticated") {
                www_authenticate = bearer_challenge(result);
            }
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
        if (!www_authenticate.empty()) {
            route.res.set(http::field::www_authenticate, www_authenticate);
        }
        if (status == http::status::service_unavailable ||
            status == http::status::too_many_requests) {
            route.res.set(http::field::retry_after, "1");
        }
        // A repository write names its workspace store requests as a page does (lake#527).
        route.res.set("Server-Timing", server_timing_of(route));
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
        if (send_error)
            co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // The tool-list digest of /mcp (Lake's own) or of /w/<workspace>/mcp (lake#684): Lake's own
    // and the digests the workspace's re-exporting connections recorded. nullopt when the store
    // cannot answer, so a session is never told on a guess.
    std::optional<std::string> mcp_endpoint_digest(const std::string& workspace_id) {
        if (workspace_id.empty() || !options.store) {
            return mcp_tool_digest;
        }
        const auto rows = options.store->mcp_exported_dispatch(workspace_id);
        if (!rows) {
            return std::nullopt;
        }
        std::vector<McpExportedDigest> exported;
        for (const control::McpConnectionDispatch& row : *rows) {
            exported.push_back({row.connection_id, row.exported_digest});
        }
        return mcp_workspace_tool_list_digest(mcp_tool_digest, std::move(exported));
    }

    // POST /mcp — JSON-RPC 2.0 over HTTP. Notifications get 202 and no body. initialize answers an
    // Mcp-Session-Id naming this build's tool list; a later request from a session that began on
    // another list (lake was upgraded since) is told notifications/tools/list_changed ahead of its
    // response, as Server-Sent Events, when the client accepts them (docs/api/mcp.md).
    boost::http::route_task serve_mcp(boost::http::route_params& route) {
        namespace http = boost::http;
        std::string body;
        std::string content_type = "application/json; charset=utf-8";
        std::string session_id;
        http::status status = http::status::ok;
        JsonBodySink sink;
        auto [read_error, bytes_read] = co_await boost::capy::push_to(route.req_body, sink);
        nlohmann::json request;
        if (body_is_too_large(read_error)) {
            status = http::status::payload_too_large;
            body = nlohmann::json{{"jsonrpc", "2.0"},
                                  {"id", nullptr},
                                  {"error",
                                   {{"code", -32700},
                                    {"message", "request body exceeds the 67108864 byte limit"}}}}
                       .dump();
        } else if (read_error || bytes_read == 0 || (request = body_json(sink)).is_discarded()) {
            status = http::status::bad_request;
            body = nlohmann::json{{"jsonrpc", "2.0"},
                                  {"id", nullptr},
                                  {"error", {{"code", -32700}, {"message", "parse error"}}}}
                       .dump();
        } else {
            ApiContext ctx = context_for(route);
            auto completed = co_await mcp_dispatch_async(registry, request, ctx, version_string());
            if (!completed) {
                status = http::status::internal_server_error;
                body = nlohmann::json{{"jsonrpc", "2.0"},
                                      {"id", request.value("id", nlohmann::json())},
                                      {"error",
                                       {{"code", -32603}, {"message", completed.error().message}}}}
                           .dump();
            } else if (!*completed) {
                status = http::status::accepted;
            } else {
                body = **completed;
                const std::string method = request.is_object() ? request.value("method", "") : "";
                const std::string presented(route.req.value_or("Mcp-Session-Id", ""));
                const std::string accept(route.req.value_or(http::field::accept, ""));
                // A workspace's endpoint also lists the tools its connections re-export, so its
                // list changes when one is exported or what it exports changes (lake#684).
                std::optional<std::string> current;
                if (method == "initialize" || !presented.empty()) {
                    current = mcp_endpoint_digest(ctx.path_workspace_id);
                }
                if (method == "initialize") {
                    session_id = mcp_session_id(current.value_or(mcp_tool_digest));
                } else if (method == "tools/list") {
                    if (current) {
                        mcp_session_lists.listed(presented, *current);
                    }
                } else if (current && accept.find("text/event-stream") != std::string::npos &&
                           mcp_session_lists.changed(presented, *current)) {
                    // tools/list is itself the fresh list, so it never carries the notice:
                    // a client that refetches on the notice is not told again.
                    content_type = "text/event-stream";
                    body = "event: message\ndata: " + mcp_tool_list_changed_notification().dump() +
                           "\n\nevent: message\ndata: " + body + "\n\n";
                }
            }
        }
        route.status(status);
        route.res.set(http::field::content_type, content_type);
        route.res.set(http::field::cache_control, "no-store");
        if (!session_id.empty()) {
            route.res.set("Mcp-Session-Id", session_id);
        }
        route.res.set("Server-Timing", server_timing_of(route));
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
        if (send_error)
            co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // GET and DELETE /mcp. Streamable HTTP: lake opens no standalone event stream and keeps no
    // session to delete, so both are 405 — a 404 would tell the client its session expired.
    boost::http::route_task serve_mcp_method_not_allowed(boost::http::route_params& route) {
        route.res.set(boost::http::field::allow, "POST");
        return send_text(route,
                         boost::http::status::method_not_allowed,
                         error_body("method_not_allowed", "MCP is POST /mcp"),
                         "application/json; charset=utf-8",
                         "no-store");
    }

    // Anything no route claimed. The router falls through (route_next) past the
    // last handler and nothing answers otherwise — a request must never hang, so
    // this trailing middleware terminates every miss with a 404.
    boost::http::route_task serve_not_found(boost::http::route_params& route) {
        // A tool called with the wrong method is named, with the method to use (lake#256).
        // url.path() is a decoded copy: keep it, never a view of it (a view dangled on Linux).
        const std::string path(route.url.path());
        const std::string workspace_local = workspace_prefixed_local_path(path);
        const std::string capability_path =
            workspace_api_path(workspace_local) ? workspace_local : path;
        if (capability_path.starts_with("/v1/")) {
            const std::string name(capability_path.substr(4));
            if (const auto capability = registry.find(name); capability != nullptr) {
                const bool read = capability->kind == CapabilityKind::Read;
                return send_text(
                    route,
                    boost::http::status::method_not_allowed,
                    error_body("method_not_allowed",
                               name + (read ? " is a read: call it with GET and a query string"
                                            : " changes something: call it with POST and a JSON "
                                              "body")),
                    "application/json; charset=utf-8",
                    "no-store");
            }
        }
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
    const void* context =
        static_cast<const void*>(static_cast<boost::capy::execution_context*>(&impl_->io_context));
    impl_.reset();
    forget_execution_context(context);
}

namespace {
// glibc raises its mmap threshold each time it frees a large mapped block, up to 32 MiB, after
// which the request bodies, windows and buffers a transfer passes through come from the heap and
// stay resident when freed. A fixed threshold keeps them mapped and returned, so lake's resident
// set follows what it holds (lake#534: with per-connection sessions a 304 MiB pull grew the
// resident set by 230 MiB on Linux, and by 15 MiB with this). MALLOC_MMAP_THRESHOLD_ overrides it.
void fix_malloc_mmap_threshold() {
#if defined(__GLIBC__)
    static std::once_flag once;
    std::call_once(once, [] {
        if (std::getenv("MALLOC_MMAP_THRESHOLD_") == nullptr) {
            mallopt(M_MMAP_THRESHOLD, 128 * 1024);
        }
    });
#endif
}
} // namespace

// liquid's local identity (`$LIQUID_HOME`, or `$HOME/.liquid`, `/identity`) signs what lake's
// repositories record. liquid creates it on first use without a lock: two first uses at once each
// write a wrapping key, and the identity left on disk is wrapped with the other one, so every later
// repository open answers `cannot unwrap local identity` for the life of the process. A new pod's
// home is empty, and its first uses run at once (the pre-open's workers, the first requests). Lake
// therefore creates or loads it here, once, before anything opens a repository, under a file lock
// that also orders lake processes sharing one home (lake#534 follow-up).
static std::expected<void, std::string> prepare_local_identity() {
    static std::mutex mutex;
    // The homes whose identity this process has loaded (one, outside tests).
    static std::set<std::filesystem::path> ready;
    std::lock_guard guard(mutex);
    std::filesystem::path home;
    if (const char* configured = std::getenv("LIQUID_HOME"); configured != nullptr && *configured) {
        home = configured;
    } else if (const char* user_home = std::getenv("HOME"); user_home != nullptr && *user_home) {
        home = std::filesystem::path(user_home) / ".liquid";
    } else {
        home = std::filesystem::current_path() / ".liquid";
    }
    if (ready.contains(home)) {
        return {};
    }
    std::error_code created;
    std::filesystem::create_directories(home / "identity", created);
    int lock = -1;
    if (!created) {
        lock = ::open(
            (home / "identity" / ".lake-create.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (lock >= 0) {
            static_cast<void>(::flock(lock, LOCK_EX));
        }
    }
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    if (lock >= 0) {
        static_cast<void>(::flock(lock, LOCK_UN));
        ::close(lock);
    }
    if (!signer) {
        return std::unexpected("liquid's local identity in " + (home / "identity").string() +
                               " is unusable: " + signer.error().message);
    }
    ready.insert(home);
    return {};
}

bool ApiServer::start() {
    fix_malloc_mmap_threshold();
    namespace http = boost::http;
    namespace corosio = boost::corosio;
    namespace beast2 = boost::beast2;
    Impl& impl = *impl_;
    if (impl.is_running.load())
        return true;
    if (!impl.failure_reason.empty()) {
        return false;
    }
    if (const auto identity = prepare_local_identity(); !identity) {
        impl.failure_reason = identity.error();
        return false;
    }
    // An ApiServer is started once: stop() leaves search shut down, the maintenance and search
    // threads stopped and its drain begun, none of which start() undoes. A restart is a new
    // ApiServer on the same options, as main and the tests do (lake#536).
    if (impl.drain_begun.load()) {
        impl.failure_reason = "this server was stopped; start a new ApiServer instead";
        return false;
    }

    impl.options.external_origin = repository_external_origin(impl.options);
    if (!impl.options.external_origin.empty()) {
        const auto origin = boost::urls::parse_uri(impl.options.external_origin);
        if (!origin ||
            (origin->scheme_id() != boost::urls::scheme::http &&
             origin->scheme_id() != boost::urls::scheme::https) ||
            !origin->has_authority() || origin->encoded_host().empty() || origin->has_userinfo() ||
            origin->has_query() || origin->has_fragment() ||
            (!origin->encoded_path().empty() && origin->encoded_path() != "/") ||
            (origin->has_port() && origin->port_number() == 0)) {
            impl.failure_reason =
                "LAKE_EXTERNAL_ORIGIN or LAKE_PUBLIC_HOST must resolve to an HTTP(S) origin "
                "with a host and optional port (1-65535), without credentials, path, query "
                "or fragment";
            return false;
        }
        const std::string scheme =
            origin->scheme_id() == boost::urls::scheme::http ? "http://" : "https://";
        const auto authority = origin->encoded_authority();
        impl.options.external_origin = scheme + std::string(authority.data(), authority.size());
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

    // One process owns the store root (lake#467). The root is taken after the listener is up,
    // by the owner thread started below: a successor answers probes and holds store-touching
    // requests while its predecessor drains, instead of refusing to start (lake#479).
    impl.store_root_state.store(impl.options.store_root.empty() ? StoreRootState::open
                                                                : StoreRootState::opening);
    impl.store_root_handoff.store(false);
    impl.owner_wait_abandoned.store(false);
    impl.draining.store(false);
    impl.drain_begun.store(false);
    impl.sync_store.reset_drain();

    CatalogRouter router(impl.get_routes);
    // Every handler is guarded: an exception it throws is logged with the route and request id
    // and answered 500, never a dropped connection (lake#440). It also counts itself in flight
    // for the drain in stop() (lake#479).
    const auto guarded = [&impl](std::string pattern, auto handler) {
        return guard_route(
            std::move(pattern),
            [&impl, handler = std::move(handler)](http::route_params& route) -> http::route_task {
                return count_in_flight(impl.in_flight, handler(route));
            },
            [&impl](http::route_params& route, const RouteFailure& failure) {
                const auto timing = request_timing(route);
                impl.record_request(route,
                                    failure.route,
                                    timing ? timing->started : std::chrono::steady_clock::now());
            });
    };
    const auto add_route = [&router, &guarded](auto method, auto path, auto handler) {
        const std::string original(path);
        auto guarded_handler = guarded(original, std::move(handler));
        // A workspace's Zetta is also reached at /w/<workspace>/workspaces/<space>/zetta/....
        if (original.starts_with("/workspaces/")) {
            router.add(
                method, "/w/:workspace_id" + original, decltype(guarded_handler)(guarded_handler));
        }
        router.add(method, original, std::move(guarded_handler));
    };
    const auto add_workspace_api_route = [&router, &guarded](auto method, auto path, auto handler) {
        const std::string original(path);
        auto guarded_handler = guarded(original, std::move(handler));
        router.add(
            method, "/w/:workspace_id" + original, decltype(guarded_handler)(guarded_handler));
        router.add(method, original, std::move(guarded_handler));
    };
    router.use([&impl, cors = http::cors()](http::route_params& route) -> http::route_task {
        // Beast2 reuses this response across requests and client connections.
        // Reset before CORS or handlers add fields belonging to this request.
        route.res.clear();
        route.res.set_start_line(http::status::ok, route.req.version());
        // A draining server answers what it has accepted and closes each connection after it.
        route.res.set_keep_alive(route.req.keep_alive() && !impl.draining.load());
        const auto start = std::chrono::steady_clock::now();
        start_request_timing(route, start);
        auto result = co_await cors(route);
        if (result.what() != http::route_what::next) {
            const RequestTimingScope timing_scope(route, start);
            impl.record_request(route, impl.route_pattern_for(route), start);
        }
        co_return result;
    });
    // The apex host's region picker (lake#757, docs/ui/regions.md): before everything else that
    // could answer, so no path on the apex reaches the app or the API.
    router.use(guarded("region_picker", [&impl](http::route_params& route) -> http::route_task {
        if (!impl.region_picker_serves(route)) {
            co_return http::route_next;
        }
        const auto timing = request_timing(route);
        const auto start = timing ? timing->started : std::chrono::steady_clock::now();
        const RequestTimingScope timing_scope(route, start);
        auto result = co_await impl.serve_region_picker(route);
        impl.record_request(route, "region_picker", start);
        co_return result;
    }));
    // A cookie-authenticated unsafe request from another origin is refused before any route.
    router.use(guarded("cross_site", [&impl](http::route_params& route) -> http::route_task {
        auto result = co_await impl.refuse_cross_site(route);
        if (result.what() != http::route_what::next) {
            const auto timing = request_timing(route);
            const auto start = timing ? timing->started : std::chrono::steady_clock::now();
            const RequestTimingScope timing_scope(route, start);
            impl.record_request(route, impl.route_pattern_for(route), start);
        }
        co_return result;
    }));
    // Before any route that may touch the store root: wait for this process to own it.
    router.use(guarded("store_root", [&impl](http::route_params& route) -> http::route_task {
        auto result = co_await impl.await_store_root(route);
        if (result.what() != http::route_what::next) {
            const auto timing = request_timing(route);
            const auto start = timing ? timing->started : std::chrono::steady_clock::now();
            const RequestTimingScope timing_scope(route, start);
            impl.record_request(route, impl.route_pattern_for(route), start);
        }
        co_return result;
    }));
    // The web app (docs/ui/web-app.md): a browser opening any page is answered with the app's
    // index.html, before the workspace check and the npm catch-all, which would otherwise claim
    // single-segment paths. The app routes, guards and renders the page itself.
    router.use(guarded("web_app", [&impl](http::route_params& route) -> http::route_task {
        const std::string path(route.url.path());
        const std::string accept(route.req.value_or(http::field::accept, ""));
        const bool custom_domain =
            !impl.options.public_host.empty() && request_host(route) != impl.options.public_host;
        if (!web_app_serves(route.req.method_text(), path, accept, custom_domain)) {
            co_return http::route_next;
        }
        const auto timing = request_timing(route);
        const auto start = timing ? timing->started : std::chrono::steady_clock::now();
        const RequestTimingScope timing_scope(route, start);
        auto result = co_await impl.serve_web_app_index(route, path);
        impl.record_request(route, "web_app", start);
        co_return result;
    }));
    router.use(guarded("workspace", [&impl](http::route_params& route) -> http::route_task {
        auto result = co_await impl.workspace_route(route);
        if (result.what() != http::route_what::next) {
            const auto timing = request_timing(route);
            const auto start = timing ? timing->started : std::chrono::steady_clock::now();
            const RequestTimingScope timing_scope(route, start);
            impl.record_request(route, impl.route_pattern_for(route), start);
        }
        co_return result;
    }));
    add_route(
        http::method::get, "/ui/:file", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(
                route, "/ui/:file", impl.serve_web_app_file(route, std::string(route.url.path())));
        });
    add_route(
        http::method::get, "/style.css", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/style.css", impl.serve_style(route));
        });
    const std::string stylesheet_path(web_ui_stylesheet_url());
    add_route(http::method::get,
              stylesheet_path,
              [&impl, stylesheet_path](http::route_params& route) -> http::route_task {
                  return impl.observe_request(route, stylesheet_path, impl.serve_style(route));
              });
    for (const std::string_view font : web_ui_font_names()) {
        const std::string route_path = "/fonts/" + std::string(font);
        add_route(http::method::get,
                  route_path,
                  [&impl, font, route_path](http::route_params& route) -> http::route_task {
                      return impl.observe_request(route, route_path, impl.serve_font(route, font));
                  });
        const std::string hashed_route_path(web_ui_font_url(font));
        add_route(http::method::get,
                  hashed_route_path,
                  [&impl, font, hashed_route_path](http::route_params& route) -> http::route_task {
                      return impl.observe_request(
                          route, hashed_route_path, impl.serve_font(route, font));
                  });
    }
    add_route(
        http::method::get, "/healthz", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/healthz", impl.serve_healthz(route));
        });
    add_route(http::method::get, "/readyz", [&impl](http::route_params& route) -> http::route_task {
        return impl.observe_request(route, "/readyz", impl.serve_readyz(route));
    });
    add_route(
        http::method::get, "/metrics", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/metrics", impl.serve_metrics(route));
        });
    add_route(
        http::method::post, "/session", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/session", impl.serve_session(route));
        });
    add_route(
        http::method::post, "/logout", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/logout", impl.serve_logout(route));
        });
    add_route(
        http::method::get, "/sign-up", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(
                route, "/sign-up", impl.serve_custom_domain_page(route, "sign-up"));
        });
    add_route(
        http::method::get,
        "/apps/:id/sign-up",
        [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(
                route, "/apps/:id/sign-up", impl.serve_application_identity_page(route, "sign-up"));
        });
    add_route(
        http::method::get,
        "/apps/:id/sign-in",
        [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(
                route, "/apps/:id/sign-in", impl.serve_application_identity_page(route, "sign-in"));
        });
    add_route(
        http::method::get, "/apps/:id/mfa", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(
                route, "/apps/:id/mfa", impl.serve_application_identity_page(route, "mfa"));
        });
    add_route(http::method::get,
              "/apps/:id/passkey-sign-in",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(
                      route,
                      "/apps/:id/passkey-sign-in",
                      impl.serve_application_identity_page(route, "passkey"));
              });
    add_route(http::method::post,
              "/apps/:id/identity/sign-up",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(
                      route,
                      "/apps/:id/identity/sign-up",
                      impl.serve_application_password(route, "sign_up_password"));
              });
    add_route(http::method::post,
              "/apps/:id/identity/sign-in",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(
                      route,
                      "/apps/:id/identity/sign-in",
                      impl.serve_application_password(route, "sign_in_password"));
              });
    add_route(http::method::post,
              "/apps/:id/identity/mfa/verify",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(route,
                                              "/apps/:id/identity/mfa/verify",
                                              impl.serve_application_password(route, "verify_mfa"));
              });
    add_route(http::method::post,
              "/apps/:id/identity/passkey/begin",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(
                      route,
                      "/apps/:id/identity/passkey/begin",
                      impl.serve_application_passkey(route, "begin_passkey_sign_in"));
              });
    add_route(http::method::post,
              "/apps/:id/identity/passkey/sign-in",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(
                      route,
                      "/apps/:id/identity/passkey/sign-in",
                      impl.serve_application_passkey(route, "sign_in_passkey"));
              });
    add_route(http::method::post,
              "/apps/:id/identity/oauth",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(
                      route, "/apps/:id/identity/oauth", impl.serve_application_oauth(route));
              });
    add_route(http::method::get,
              "/v1/apps/:id/.well-known/jwks.json",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(route,
                                              "/v1/apps/:id/.well-known/jwks.json",
                                              impl.serve_application_jwks(route));
              });
    add_route(
        http::method::get, "/sign-in", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(
                route, "/sign-in", impl.serve_custom_domain_page(route, "sign-in"));
        });
    add_route(http::method::get,
              "/apps/authorize",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(
                      route, "/apps/authorize", impl.serve_app_authorize(route));
              });
    add_workspace_api_route(
        http::method::get, "/v1/apps/auth", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/v1/apps/auth", impl.serve_app_forward_auth(route));
        });
    add_route(http::method::get, "/mfa", [&impl](http::route_params& route) -> http::route_task {
        return impl.observe_request(route, "/mfa", impl.serve_custom_domain_page(route, "mfa"));
    });
    add_route(http::method::get,
              "/passkey-sign-in",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(
                      route, "/passkey-sign-in", impl.serve_custom_domain_page(route, "passkey"));
              });
    add_route(http::method::get,
              "/developer/mcp/skill",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(
                      route, "/developer/mcp/skill", impl.serve_mcp_skill(route));
              });
    add_route(http::method::get,
              "/skills/lake/SKILL.md",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(
                      route, "/skills/lake/SKILL.md", impl.serve_mcp_skill(route));
              });
    add_route(
        http::method::get, "/llms.txt", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/llms.txt", impl.serve_llms(route, false));
        });
    add_route(http::method::get,
              "/llms-full.txt",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(
                      route, "/llms-full.txt", impl.serve_llms(route, true));
              });
    add_route(http::method::post,
              "/billing/stripe/webhook",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(
                      route, "/billing/stripe/webhook", impl.serve_payment_notice(route));
              });
    add_route(http::method::post,
              "/identity/clerk-webhook",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(
                      route, "/identity/clerk-webhook", impl.serve_clerk_webhook(route));
              });
    add_route(http::method::post,
              "/identity/clerk-session",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(
                      route, "/identity/clerk-session", impl.serve_clerk_session(route));
              });
    add_route(http::method::get,
              "/exports/download",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(
                      route, "/exports/download", impl.serve_export_download(route));
              });
    add_route(
        http::method::get, "/file/raw", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(
                route, "/file/raw", impl.serve_repository_file_content(route, false));
        });
    add_route(http::method::get,
              "/file/download",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(
                      route, "/file/download", impl.serve_repository_file_content(route, true));
              });
    add_route(http::method::get,
              "/drive/download",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(
                      route, "/drive/download", impl.serve_drive_download(route));
              });
    add_route(http::method::get,
              "/drive/files/download",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(
                      route, "/drive/files/download", impl.serve_workspace_file_download(route));
              });
    add_route(http::method::get,
              "/tunnel-clients/:platform",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(
                      route, "/tunnel-clients/:platform", impl.serve_tunnel_client(route));
              });
    add_route(http::method::get, "/share/:token/download", [&impl](http::route_params& route) {
        return impl.observe_request(
            route, "/share/:token/download", impl.serve_drive_share_download(route));
    });
    add_route(http::method::post,
               "/api/repos/create",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/api/repos/create", impl.serve_hub_create(route));
               });
    add_route(http::method::post,
               "/api/validate-yaml",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/api/validate-yaml", impl.serve_hub_validate_yaml(route));
               });
    add_route(http::method::get,
               "/models/:id/resolve/:revision/*file",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(route,
                                               "/models/:id/resolve/:revision/*file",
                                               impl.serve_hub_file(route, "model"));
               });
    add_route(http::method::head,
               "/models/:id/resolve/:revision/*file",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(route,
                                               "/models/:id/resolve/:revision/*file",
                                               impl.serve_hub_file(route, "model"));
               });
    const auto add_hub_info = [&add_route, &impl](std::string_view pattern, std::string_view kind) {
        add_route(http::method::get,
                   pattern,
                   [&impl, pattern, kind](http::route_params& route) -> http::route_task {
                       return impl.observe_request(
                           route, std::string(pattern), impl.serve_hub_info(route, kind));
                   });
    };
    const auto add_hub_tree = [&add_route, &impl](std::string_view pattern, std::string_view kind) {
        add_route(http::method::get,
                   pattern,
                   [&impl, pattern, kind](http::route_params& route) -> http::route_task {
                       return impl.observe_request(
                           route, std::string(pattern), impl.serve_hub_tree(route, kind));
                   });
    };
    const auto add_hub_file =
        [&add_route, &impl](http::method method, std::string_view pattern, std::string_view kind) {
            add_route(method,
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
    const auto add_hub_post = [&add_route, &impl](std::string_view pattern, bool commit) {
        add_route(http::method::post,
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
    const auto add_lfs_batch = [&add_route, &impl](std::string_view pattern) {
        add_route(http::method::post,
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
    add_route(http::method::put,
              "/api/lfs/:repository/:oid",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(
                      route, "/api/lfs/:repository/:oid", impl.serve_hub_lfs_upload(route));
              });
    add_route(http::method::get,
              "/v1/saml/:provider/metadata",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(
                      route, "/v1/saml/:provider/metadata", impl.serve_saml_metadata(route));
              });
    add_route(http::method::get,
              "/v1/saml/:provider/start",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(
                      route, "/v1/saml/:provider/start", impl.serve_saml_start(route));
              });
    add_route(http::method::post,
              "/v1/saml/:provider/acs",
              [&impl](http::route_params& route) -> http::route_task {
                  return impl.observe_request(
                      route, "/v1/saml/:provider/acs", impl.serve_saml_acs(route));
              });
    add_route(http::method::get,
               "/v1/oauth/:provider/callback",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/v1/oauth/:provider/callback", impl.serve_oauth_callback(route));
               });
    add_route(http::method::get,
               "/scim/v2/Users",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/scim/v2/Users", impl.serve_scim_collection(route, "Users"));
               });
    add_route(http::method::post,
               "/scim/v2/Users",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/scim/v2/Users", impl.serve_scim_collection(route, "Users"));
               });
    add_route(http::method::get,
               "/scim/v2/Users/:id",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/scim/v2/Users/:id", impl.serve_scim_resource(route, "Users"));
               });
    add_route(http::method::patch,
               "/scim/v2/Users/:id",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/scim/v2/Users/:id", impl.serve_scim_resource(route, "Users"));
               });
    add_route(http::method::delete_,
               "/scim/v2/Users/:id",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/scim/v2/Users/:id", impl.serve_scim_resource(route, "Users"));
               });
    add_route(http::method::get,
               "/scim/v2/Groups",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/scim/v2/Groups", impl.serve_scim_collection(route, "Groups"));
               });
    add_route(http::method::post,
               "/scim/v2/Groups",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/scim/v2/Groups", impl.serve_scim_collection(route, "Groups"));
               });
    add_route(http::method::get,
               "/scim/v2/Groups/:id",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/scim/v2/Groups/:id", impl.serve_scim_resource(route, "Groups"));
               });
    add_route(http::method::patch,
               "/scim/v2/Groups/:id",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/scim/v2/Groups/:id", impl.serve_scim_resource(route, "Groups"));
               });
    add_route(http::method::delete_,
               "/scim/v2/Groups/:id",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/scim/v2/Groups/:id", impl.serve_scim_resource(route, "Groups"));
               });
    add_workspace_api_route(http::method::get,
                            "/v1/openapi.json",
                            [&impl](http::route_params& route) -> http::route_task {
                                return impl.observe_request(
                                    route, "/v1/openapi.json", impl.serve_openapi(route));
                            });
    add_route(http::method::get,
               "/acme/:customer_id/directory",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/acme/:customer_id/directory", impl.serve_acme_directory(route));
               });
    add_route(http::method::head,
               "/acme/:customer_id/new-nonce",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/acme/:customer_id/new-nonce", impl.serve_acme_nonce(route));
               });
    add_route(http::method::get,
               "/.well-known/acme-challenge/:token",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/.well-known/acme-challenge/:token", impl.serve_acme_http01(route));
               });
    const auto add_acme_post = [&add_route, &impl](std::string path, std::string capability_name) {
        add_route(http::method::post,
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
    const auto add_sync_post = [&add_route, &impl](std::string path, std::string capability_name) {
        add_route(http::method::post,
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
    add_route(http::method::get,
               "/v1/repositories/:repository_id/sync/chunks/:digest",
               [&impl](http::route_params& route) -> http::route_task {
                   const std::string path = "/v1/repositories/:repository_id/sync/chunks/:digest";
                   return impl.observe_request(route, path, impl.serve_sync_range(route));
               });
    add_route(http::method::get,
               "/workspaces/:customer_id/zetta/v1/openapi.json",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(route,
                                               "/workspaces/:customer_id/zetta/v1/openapi.json",
                                               impl.serve_workspace_zetta(route, true));
               });
    for (const auto method : {http::method::get, http::method::post}) {
        add_route(method,
                   "/workspaces/:customer_id/zetta/v1/:capability",
                   [&impl](http::route_params& route) -> http::route_task {
                       return impl.observe_request(route,
                                                   "/workspaces/:customer_id/zetta/v1/:capability",
                                                   impl.serve_workspace_zetta(route, false));
                   });
    }
    // The gateway also answers under /w/<workspace>/: an agent launched in a workspace namespace
    // has LAKE_URL = <lake>/w/<workspace>, and its SDK calls <LAKE_URL>/openai/v1
    // (orama-operator#779). model_completion refuses a run whose workspace is not the one the path
    // names.
    for (const std::string_view endpoint : model_gateway_endpoints) {
        const std::string path = "/openai/v1/" + std::string(endpoint);
        add_workspace_api_route(
            http::method::post,
            path,
            [&impl, path, endpoint](http::route_params& route) -> http::route_task {
                return impl.observe_request(
                    route, path, impl.serve_model_gateway(route, std::string(endpoint)));
            });
    }
    for (const auto& capability : impl.registry.list()) {
        if (capability->transport != CapabilityTransport::Json) {
            continue;
        }
        const auto method =
            rest_method(*capability) == "GET" ? http::method::get : http::method::post;
        const std::string path = rest_path(*capability);
        if (capability->kind == CapabilityKind::Read && capability->post_alias) {
            add_workspace_api_route(
                http::method::post,
                path,
                [&impl, capability, path](http::route_params& route) -> http::route_task {
                    return impl.observe_request(
                        route, path, impl.serve_capability(route, capability));
                });
        }
        add_workspace_api_route(
            method,
            path,
            [&impl, capability, path](http::route_params& route) -> http::route_task {
                return impl.observe_request(route, path, impl.serve_capability(route, capability));
            });
    }
    add_workspace_api_route(
        http::method::post,
        "/mcp",
        [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/mcp", impl.serve_mcp(route));
        });
    for (const auto method : {http::method::get, http::method::delete_}) {
        add_workspace_api_route(
            method, "/mcp", [&impl](http::route_params& route) -> http::route_task {
                return impl.observe_request(
                    route, "/mcp", impl.serve_mcp_method_not_allowed(route));
            });
    }
    add_route(http::method::get, "/v2/", [&impl](http::route_params& route) -> http::route_task {
        return impl.observe_request(route, "/v2/", impl.serve_oci_ping(route));
    });
    for (const std::string prefix :
         {"/v2/:repository", "/v2/:space/:repository", "/v2/:space/:category/:repository"}) {
        const std::string start_path = prefix + "/blobs/uploads/";
        add_route(http::method::post,
                   start_path,
                   [&impl, start_path](http::route_params& route) -> http::route_task {
                       return impl.observe_request(
                           route, start_path, impl.serve_oci_upload_start(route));
                   });
        const std::string upload_path = prefix + "/blobs/uploads/:upload";
        add_route(http::method::patch,
                   upload_path,
                   [&impl, upload_path](http::route_params& route) -> http::route_task {
                       return impl.observe_request(
                           route, upload_path, impl.serve_oci_upload(route, false));
                   });
        add_route(http::method::put,
                   upload_path,
                   [&impl, upload_path](http::route_params& route) -> http::route_task {
                       return impl.observe_request(
                           route, upload_path, impl.serve_oci_upload(route, true));
                   });
        const std::string blob_path = prefix + "/blobs/:digest";
        add_route(http::method::get,
                   blob_path,
                   [&impl, blob_path](http::route_params& route) -> http::route_task {
                       return impl.observe_request(
                           route, blob_path, impl.serve_oci_blob(route, false));
                   });
        add_route(http::method::head,
                   blob_path,
                   [&impl, blob_path](http::route_params& route) -> http::route_task {
                       return impl.observe_request(route, blob_path, impl.serve_oci_blob(route, true));
                   });
        const std::string manifest_path = prefix + "/manifests/:reference";
        for (const auto method : {http::method::get, http::method::head, http::method::put}) {
            add_route(
                method,
                manifest_path,
                [&impl, method, manifest_path](http::route_params& route) -> http::route_task {
                    return impl.observe_request(route,
                                                manifest_path,
                                                impl.serve_oci_manifest(
                                                    route, method == http::method::head));
                });
        }
    }
    add_route(
        http::method::post, "/legacy/", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/legacy/", impl.serve_python_publish(route));
        });
    add_route(http::method::get,
               "/simple/:package/",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/simple/:package/", impl.serve_python_simple(route));
               });
    add_route(http::method::get,
               "/packages/:filename",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/packages/:filename", impl.serve_python_package(route, false));
               });
    add_route(http::method::head,
               "/packages/:filename",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/packages/:filename", impl.serve_python_package(route, true));
               });
    add_route(http::method::put,
               "/_lake/debug/:build_id/:kind",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/_lake/debug/:build_id/:kind", impl.serve_debug_publish(route));
               });
    add_route(http::method::get,
               "/buildid/:build_id/executable",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(route,
                                               "/buildid/:build_id/executable",
                                               impl.serve_debug_artifact(route, "executable"));
               });
    add_route(http::method::get,
               "/buildid/:build_id/debuginfo",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(route,
                                               "/buildid/:build_id/debuginfo",
                                               impl.serve_debug_artifact(route, "debuginfo"));
               });
    add_route(http::method::get,
               "/buildid/:build_id/source/*source",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(route,
                                               "/buildid/:build_id/source/*source",
                                               impl.serve_debug_artifact(route, "source"));
               });
    add_route(http::method::get,
               "/:package/-/:filename",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.observe_request(
                       route, "/:package/-/:filename", impl.serve_npm_tarball(route));
               });
    add_route(
        http::method::get, "/:package", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/:package", impl.serve_npm_metadata(route));
        });
    add_route(
        http::method::put, "/:package", [&impl](http::route_params& route) -> http::route_task {
            return impl.observe_request(route, "/:package", impl.serve_npm_metadata(route));
        });
    router.use(guarded("unmatched", [&impl](http::route_params& route) -> http::route_task {
        return impl.observe_request(route, "unmatched", impl.serve_not_found(route));
    }));

    auto parser_options = http::parser_config{true};
    parser_options.body_limit =
        std::max<std::uint64_t>(maximum_sync_body_size, impl.options.max_registry_blob_bytes);
    HttpConnectionLimits limits;
    limits.workers = impl.options.worker_coroutines > 0
                         ? static_cast<std::size_t>(impl.options.worker_coroutines)
                         : 0;
    limits.max_connections = impl.options.max_connections;
    limits.idle_timeout = impl.options.idle_timeout;
    limits.header_timeout = impl.options.header_timeout;
    limits.worker_wait = impl.options.worker_wait;
    limits.listen_backlog = impl.options.listen_backlog;
    // The probes never wait behind busy workers, so a saturated lake is not restarted for it.
    limits.needs_no_worker = [](std::string_view path) {
        return path == "/healthz" || path == "/readyz";
    };
    limits.io_timeout = impl.options.io_timeout;
    limits.min_transfer_rate = impl.options.min_transfer_rate;
    limits.max_long_requests = impl.options.max_long_requests;
    limits.max_long_requests_per_caller = impl.options.max_long_requests_per_caller;
    limits.in_flight = &impl.in_flight;
    // A request that waits for the store root (lake#479) waits before it takes a worker, so a
    // rollout's requests are all answered after the hand-off, however many arrive.
    limits.admitted = [&impl](std::string_view path) {
        const StoreRootState state = impl.store_root_state.load();
        return state == StoreRootState::open || state == StoreRootState::failed ||
               impl.draining.load() || store_root_exempt(path);
    };
    limits.admission_wait = impl.options.store_owner_wait + std::chrono::minutes(2);
    // Answered before any route: logged and counted like a routed request, named by the reason.
    limits.refused = [&impl](std::string_view method, unsigned status, std::string_view reason) {
        const telemetry::Labels labels{{"method", std::string(method)},
                                       {"route", std::string(reason)},
                                       {"status", std::to_string(status)}};
        impl.metrics.increment_counter("lake_http_requests_total", labels);
        RequestLogEntry entry{.ts = log_timestamp(),
                              .method = std::string(method),
                              .route = std::string(reason),
                              .status = static_cast<int>(status),
                              .duration_ms = 0.0,
                              .node = impl.options.node_name};
        std::println("{}", request_log_line(entry));
    };
    impl.server = std::make_unique<HttpConnections>(impl.io_context,
                                                    router.release(),
                                                    http::make_parser_config(parser_options),
                                                    http::make_serializer_config(http::serializer_config()),
                                                    std::move(limits),
                                                    &impl.metrics);

    corosio::ipv4_address address(impl.options.listen_address);
    const std::string bind_error = impl.server->bind(corosio::endpoint(address, impl.options.port));
    if (!bind_error.empty()) {
        impl.failure_reason = bind_error;
        impl.server.reset();
        return false;
    }
    impl.bound_port = impl.server->port();
    if (impl.options.store && impl.options.workspaces && !impl.options.instance_kek.empty()) {
        impl.metering_recorder =
            std::make_unique<MeteringRecorder>(impl.options.store.get(),
                                               impl.options.workspaces.get(),
                                               impl.options.instance_kek,
                                               impl.options.metering_max_pending);
        impl.next_metering_flush =
            std::chrono::steady_clock::now() + impl.options.metering_flush_interval;
        impl.audit_recorder = std::make_unique<AuditRecorder>(impl.options.store.get(),
                                                              impl.options.workspaces.get(),
                                                              impl.options.instance_kek,
                                                              impl.options.identity_seed,
                                                              impl.options.audit_max_pending);
        impl.next_audit_flush =
            std::chrono::steady_clock::now() + impl.options.audit_flush_interval;
    }
    if (impl.options.store && impl.options.workspaces) {
        impl.request_recorder = std::make_unique<RequestRecorder>(
            impl.options.store.get(), impl.options.workspaces.get(),
            impl.options.request_series_max_pending);
        impl.next_request_series_flush =
            std::chrono::steady_clock::now() + impl.options.request_series_flush_interval;
        impl.usage_recorder = std::make_unique<UsageRecorder>(impl.options.store.get(),
                                                              impl.options.workspaces.get(),
                                                              impl.options.usage_max_pending);
        impl.next_usage_flush =
            std::chrono::steady_clock::now() + impl.options.usage_flush_interval;
        impl.next_usage_sample = std::chrono::steady_clock::now();
    }
    impl.server->start();
    impl.io_thread = lake::Thread([&impl] { impl.io_context.run(); });
    impl.maintenance_thread = lake::Thread([&impl] { impl.run_maintenance(); });
    impl.search_jobs_thread = lake::Thread([&impl] { impl.run_search_jobs(); });
    impl.is_running.store(true);
    impl.metrics.set_gauge("lake_up", {}, 1.0);
    if (!impl.options.store_root.empty()) {
        impl.owner_thread = lake::Thread([&impl] { impl.own_store_root(); });
        if (impl.options.preopen_workspace_stores) {
            impl.preopen_thread = lake::Thread([&impl] { impl.preopen_workspace_stores(); });
        }
    }
    return true;
}

OpenFileBudget fit_connections_to_open_files(std::size_t wanted) {
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) {
        return {0, wanted};
    }
    if (limit.rlim_cur < limit.rlim_max) {
        // macOS refuses RLIM_INFINITY and anything above OPEN_MAX for the soft limit.
        for (const rlim_t target : {limit.rlim_max, static_cast<rlim_t>(1U << 20),
                                    static_cast<rlim_t>(65536), static_cast<rlim_t>(10240)}) {
            if (target <= limit.rlim_cur || target > limit.rlim_max) {
                continue;
            }
            rlimit raised = limit;
            raised.rlim_cur = target;
            if (::setrlimit(RLIMIT_NOFILE, &raised) == 0) {
                limit = raised;
                break;
            }
        }
    }
    const auto open_files = static_cast<std::uint64_t>(
        limit.rlim_cur == RLIM_INFINITY ? std::numeric_limits<std::uint32_t>::max() : limit.rlim_cur);
    return {open_files, connections_for_open_files(wanted, open_files)};
}

void ApiServer::begin_drain() {
    if (impl_->is_running.load()) {
        impl_->begin_drain();
    }
}

void ApiServer::stop() {
    Impl& impl = *impl_;
    if (!impl.is_running.exchange(false))
        return;
    impl.metrics.set_gauge("lake_up", {}, 0.0);
    // Stop taking new work, unless begin_drain() already did at SIGTERM: /readyz answers 503,
    // every response closes its connection, a request still waiting for the store root is
    // answered, the owner thread stops waiting, and each workspace store is handed off as soon
    // as its admitted requests are done (lake#536).
    impl.begin_drain();
    // Drain: let the requests already accepted finish. The count must read zero twice, so a
    // request between two of its route steps is not taken for finished.
    const auto drain_started = impl.drain_started;
    const auto drain_deadline = impl.drain_deadline;
    int idle_reads = 0;
    while (idle_reads < 2 && std::chrono::steady_clock::now() < drain_deadline) {
        idle_reads = impl.in_flight.load() == 0 ? idle_reads + 1 : 0;
        if (idle_reads < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    const int abandoned_requests = impl.in_flight.load();
    // Closes every connection and lets the sessions unwind on the running io_context first.
    impl.server->stop(std::chrono::seconds(2));
    impl.io_context.stop();
    if (impl.io_thread.joinable())
        impl.io_thread.join();
    if (impl.owner_thread.joinable())
        impl.owner_thread.join();
    // The pre-open stops at the drain; an open in progress finishes, and close() below closes it.
    if (impl.preopen_thread.joinable())
        impl.preopen_thread.join();
    if (impl.workspace_release_thread.joinable())
        impl.workspace_release_thread.join();
    impl.work_cv.notify_all();
    // In-flight requests have finished: write the repository usage they counted (lake#348) and
    // the workspace audit events they queued (decision 52).
    impl.flush_metering();
    impl.flush_audit();
    impl.flush_request_series();
    impl.flush_usage();
    // In-flight requests have finished: close every store (releasing liquid's per-store owner
    // locks), then the root lock, so the next owner opens a store nobody is writing.
    // A maintenance job in progress finishes (its Zetta calls are bounded); queued ones are dropped.
    impl.stop_search_jobs();
    impl.stop_maintenance();
    const bool owned = impl.store_owner.held();
    if (impl.options.search) {
        impl.options.search->set_repository_access({});
    }
    impl.sync_store.close();
    // Repository-store work still on the blocking pool gets the rest of the drain deadline; then
    // the io_context is closed to it, so no task posts into it once it is destroyed. A task
    // abandoned here loses its response and its audit event (docs/architecture/workspace-store.md).
    const int abandoned_tasks =
        wait_for_blocking_tasks(std::max(drain_deadline, std::chrono::steady_clock::now()));
    close_execution_context(static_cast<const void*>(
        static_cast<boost::capy::execution_context*>(&impl.io_context)));
    if (abandoned_tasks > 0) {
        impl.metrics.increment_counter("lake_abandoned_store_tasks_total", {},
                                       static_cast<double>(abandoned_tasks));
    }
    impl.store_owner.release();
    if (!impl.options.store_root.empty()) {
        impl.log_store_owner(
            {{"state", owned ? "released" : "not_owned"},
             {"drain_ms",
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - drain_started)
                  .count()},
             {"unfinished_requests", abandoned_requests},
             {"abandoned_store_tasks", abandoned_tasks}});
    }
}

std::string ApiServer::fatal_error() const {
    if (impl_->store_root_state.load() != StoreRootState::failed) {
        return {};
    }
    std::lock_guard lock(impl_->store_root_mutex);
    return impl_->store_root_failure;
}

bool ApiServer::store_root_open() const {
    return impl_->store_root_state.load() == StoreRootState::open;
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

std::vector<std::string> ApiServer::get_route_patterns() const {
    std::vector<std::string> routes = impl_->get_routes;
    std::ranges::sort(routes);
    const auto unique = std::ranges::unique(routes);
    routes.erase(unique.begin(), unique.end());
    return routes;
}

void ApiServer::drain() {
    // Timestamp evidence lives under the store root: renew it only as its owner.
    if (impl_->store_root_state.load() == StoreRootState::open) {
        impl_->renew_due_timestamps();
    }
    const auto now = std::chrono::steady_clock::now();
    if (impl_->store_root_state.load() == StoreRootState::open &&
        now >= impl_->next_workspace_usage_refresh) {
        impl_->next_workspace_usage_refresh = now + std::chrono::seconds(10);
        Impl* impl = impl_.get();
        impl_->schedule_maintenance("workspace usage", [impl] { impl->sync_store.refresh_workspace_usage(); });
    }
    // Abandoned OCI uploads under the store root: only its owner removes them (lake#383 phase B).
    if (impl_->store_root_state.load() == StoreRootState::open && impl_->artifacts.available() &&
        now >= impl_->next_upload_sweep) {
        impl_->next_upload_sweep =
            now + std::min<std::chrono::steady_clock::duration>(
                      std::chrono::minutes(10), impl_->options.registry_upload_max_age);
        Impl* impl = impl_.get();
        impl_->schedule_maintenance("registry upload sweep", [impl] {
            static_cast<void>(impl->artifacts.sweep_uploads(impl->options.registry_upload_max_age));
        });
    }
    if (impl_->metering_recorder &&
        (now >= impl_->next_metering_flush ||
         impl_->metering_recorder->pending() * 2 >= impl_->options.metering_max_pending)) {
        impl_->next_metering_flush = now + impl_->options.metering_flush_interval;
        impl_->flush_metering();
    }
    if (impl_->audit_recorder &&
        (now >= impl_->next_audit_flush ||
         impl_->audit_recorder->pending() * 2 >= impl_->options.audit_max_pending)) {
        impl_->next_audit_flush = now + impl_->options.audit_flush_interval;
        impl_->flush_audit();
    }
    if (impl_->request_recorder &&
        (now >= impl_->next_request_series_flush ||
         impl_->request_recorder->pending() * 2 >= impl_->options.request_series_max_pending)) {
        impl_->next_request_series_flush = now + impl_->options.request_series_flush_interval;
        impl_->flush_request_series();
    }
    if (impl_->usage_recorder &&
        (now >= impl_->next_usage_flush ||
         impl_->usage_recorder->pending() * 2 >= impl_->options.usage_max_pending)) {
        impl_->next_usage_flush = now + impl_->options.usage_flush_interval;
        impl_->flush_usage();
    }
    // Stored bytes and service time are sampled by the store root's owner only, so several lakes
    // never count one service twice (lake#564).
    if (impl_->usage_recorder &&
        (impl_->options.store_root.empty() ||
         impl_->store_root_state.load() == StoreRootState::open) &&
        now >= impl_->next_usage_sample) {
        impl_->next_usage_sample = now + impl_->options.usage_sample_interval;
        Impl* impl = impl_.get();
        impl_->schedule_maintenance("usage sample", [impl] { impl->sample_usage(); });
    }
    // A connection's streams hold sessions and start turns: the store root's owner alone runs them,
    // so two lakes never mirror one notification twice (lake#652).
    if (impl_->options.store && impl_->options.workspaces &&
        (impl_->options.store_root.empty() ||
         impl_->store_root_state.load() == StoreRootState::open) &&
        now >= impl_->next_mcp_stream_pass) {
        impl_->next_mcp_stream_pass = now + impl_->options.mcp_stream_interval;
        Impl* impl = impl_.get();
        impl_->schedule_maintenance("mcp connection streams", [impl] { impl->run_mcp_streams(); });
    }
    // The registry and search segments live under the store root: only its owner removes them.
    if (impl_->options.store &&
        (impl_->options.store_root.empty() ||
         impl_->store_root_state.load() == StoreRootState::open) &&
        now >= impl_->next_workspace_deletion_pass) {
        impl_->next_workspace_deletion_pass = now + std::chrono::minutes(1);
        // The deletion pass closes workspace stores, which waits on their lanes.
        Impl* impl = impl_.get();
        impl_->schedule_maintenance("workspace deletions", [impl] { impl->resume_workspace_deletions(); });
    }
    // The orders' email and audit (lake#831): the store root's owner alone, so two lakes never
    // send one email twice; an order sent just now asks for it sooner.
    if (impl_->options.store &&
        (impl_->options.store_root.empty() ||
         impl_->store_root_state.load() == StoreRootState::open) &&
        (now >= impl_->next_order_pass || impl_->order_pass_requested.exchange(false))) {
        impl_->next_order_pass = now + impl_->options.order_pass_interval;
        Impl* impl = impl_.get();
        impl_->schedule_maintenance("cloud orders", [impl] { impl->run_order_pass(); });
    }
    if (impl_->options.store && impl_->options.workspaces && !impl_->options.instance_kek.empty() &&
        now >= impl_->next_secret_lease_sweep) {
        impl_->next_secret_lease_sweep = now + impl_->options.secret_lease_expiry_interval;
        impl_->expire_secret_leases();
    }
    if (impl_->options.store && impl_->options.workspaces && now >= impl_->next_topic_reap) {
        impl_->next_topic_reap = now + std::chrono::minutes(1);
        if (impl_->options.agent_bus) {
            static_cast<void>(liquid::runtime::block_on(impl_->options.agent_bus->reap()));
            const auto count = impl_->options.agent_bus->reaped_topics();
            impl_->metrics.increment_counter("lake_bus_topics_reaped_total", {}, count - impl_->reported_bus_reaps);
            impl_->reported_bus_reaps = count;
        }
        const auto result = liquid::runtime::block_on(control::reap_workspace_topics(
            *impl_->options.store, *impl_->options.workspaces));
        if (result && *result) {
            impl_->metrics.increment_counter("lake_workspace_topics_reaped_total", {}, **result);
        } else {
            impl_->metrics.increment_counter("lake_workspace_topic_reap_errors_total");
        }
    }
    if (impl_->options.platform && impl_->options.platform->sync_due()) {
        // The mirror lives in each repository's workspace (lake#358, lake#359).
        control::WorkspaceService* workspaces = impl_->options.workspaces.get();
        const std::string kek = impl_->options.instance_kek;
        Impl* impl = impl_.get();
        const auto synced = liquid::runtime::block_on(impl_->options.platform->sync(
            [workspaces, kek](const std::string& customer_id, const std::string& workspace_id)
                -> control::ZettaControlResult<control::WorkspaceRecords> {
                if (!workspaces) {
                    return std::unexpected(control::ZettaControlError{
                        "workspace_not_ready", "a workspace Zetta is required"});
                }
                return control::WorkspaceRecords::open(
                    [workspaces, customer_id, workspace_id](std::string capability, bool read,
                                                            nlohmann::json input) {
                        return workspaces->call(customer_id, workspace_id, std::move(capability),
                                                read, std::move(input));
                    },
                    workspace_id, kek);
            },
            // The id liquid gave a repository, which `liquid deploy` sends (lake#805).
            [impl](const control::Repository& repository) -> control::PlatformResult<std::string> {
                auto id = impl->sync_store.repository_liquid_id(repository.customer_id, repository.id);
                if (!id) {
                    return std::unexpected(control::PlatformError{id.error().code, id.error().message});
                }
                return std::move(*id);
            }));
        // A poll that left rows out says so once per change, with the reason (lake#805): a row
        // whose repository lake cannot place is never dropped quietly.
        const std::string outcome =
            !synced ? synced.error().message : (*synced ? std::string() : synced->error().message);
        if (outcome != impl_->last_platform_sync_problem) {
            impl_->last_platform_sync_problem = outcome;
            if (!outcome.empty()) {
                std::cerr << nlohmann::json{{"event", "platform_sync_incomplete"},
                                            {"code", synced && !*synced ? synced->error().code
                                                                        : std::string("store_unavailable")},
                                            {"message", outcome}}
                                 .dump()
                          << '\n';
            }
        }
        impl_->metrics.set_gauge(
            "lake_platform_unresolved_repositories", {},
            static_cast<double>(impl_->options.platform->unresolved_repositories().size()));
    }
}

void ApiServer::wait_for_work(std::chrono::milliseconds max_idle) {
    Impl& impl = *impl_;
    std::unique_lock lock(impl.work_mutex);
    impl.work_cv.wait_for(lock, max_idle, [&impl] { return !impl.is_running.load(); });
}

} // namespace lake::api
