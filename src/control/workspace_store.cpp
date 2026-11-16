// SPDX-License-Identifier: MIT

#include "lake/control/workspace_store.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <span>
#include <tuple>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "secrets_crypto.hpp"

namespace lake::control {
namespace {

constexpr std::size_t header_size = 48;
constexpr std::array<char, 8> header_magic{'L', 'K', 'S', 'T', 'E', 'N', 'C', '1'};
constexpr std::uint32_t header_format = 1;
constexpr std::string_view tail_marker = ".lake-tail-";
// One Zetta append frame carries at most 4 MiB; a PATCH no larger is applied as one frame, so it
// is absent or whole after a crash and an object never ends inside a block.
constexpr std::size_t zetta_slice_bytes = 4 * 1024 * 1024;
constexpr std::string_view wrap_label = "lake/workspace-store/v1/";
// The largest object whose plaintext, sent whole, a view remembers, to skip rewriting it.
constexpr std::uint64_t sent_plain_bytes = 16 * 1024;
// What a view keeps of such plaintext in all: at most 1 MiB per open store.
constexpr std::uint64_t sent_plain_limit = 1024 * 1024;

liquid::error failure(liquid::errc code, std::string message, std::string context = {}) {
    return liquid::error{code, std::move(message), std::move(context)};
}

template <class T = void> liquid::result<T> refused(liquid::errc code, std::string message,
                                                    std::string context = {}) {
    return std::unexpected(failure(code, std::move(message), std::move(context)));
}

std::string encode_component(std::string_view input, bool keep_slash) {
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::string output;
    output.reserve(input.size());
    for (const unsigned char character : input) {
        if (std::isalnum(character) || character == '-' || character == '_' || character == '.' ||
            character == '~' || (keep_slash && character == '/')) {
            output += static_cast<char>(character);
            continue;
        }
        output += '%';
        output += hex[character >> 4U];
        output += hex[character & 0x0fU];
    }
    return output;
}

std::string hex_of(std::span<const unsigned char> bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string output;
    output.reserve(bytes.size() * 2);
    for (const unsigned char value : bytes) {
        output += digits[value >> 4U];
        output += digits[value & 0x0fU];
    }
    return output;
}

void put_u32(std::string& output, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        output += static_cast<char>((value >> shift) & 0xffU);
    }
}

void put_u64(std::string& output, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        output += static_cast<char>((value >> shift) & 0xffU);
    }
}

std::uint32_t get_u32(std::string_view bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (int index = 3; index >= 0; --index) {
        value = (value << 8U) | static_cast<unsigned char>(bytes[offset + index]);
    }
    return value;
}

std::uint64_t get_u64(std::string_view bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (int index = 7; index >= 0; --index) {
        value = (value << 8U) | static_cast<unsigned char>(bytes[offset + index]);
    }
    return value;
}

// The error envelope Zetta answers with: {"ok": false, "error": {"code", "message", "details"}}.
struct ZettaError {
    std::string code;
    std::string message;
    nlohmann::json details;
};

ZettaError zetta_error(const ObjectResponse& response) {
    const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    ZettaError output;
    if (body.is_object() && body.contains("error") && body.at("error").is_object()) {
        const nlohmann::json& error = body.at("error");
        output.code = error.value("code", std::string());
        output.message = error.value("message", std::string());
        output.details = error.value("details", nlohmann::json());
    }
    if (output.message.empty()) {
        output.message = "HTTP " + std::to_string(response.status);
    }
    return output;
}

liquid::error mapped(const ObjectResponse& response, std::string context) {
    if (!response.error.empty()) {
        return failure(liquid::errc::unavailable,
                       "the workspace Zetta did not answer: " + response.error, std::move(context));
    }
    const ZettaError error = zetta_error(response);
    switch (response.status) {
    case 400:
        return failure(liquid::errc::invalid_argument, error.message, std::move(context));
    case 401:
    case 403:
        return failure(liquid::errc::unauthorized, error.message, std::move(context));
    case 404:
        return failure(liquid::errc::not_found, error.message, std::move(context));
    case 409:
        if (error.code == "fenced") {
            return failure(liquid::errc::locked,
                           "the workspace store's lease has passed to another holder (Zetta "
                           "refused the write as fenced): " + error.message,
                           std::move(context));
        }
        if (error.code == "held") {
            return failure(liquid::errc::locked, error.message, std::move(context));
        }
        return failure(liquid::errc::conflict, error.message, std::move(context));
    case 416:
        return failure(liquid::errc::corrupt, error.message, std::move(context));
    case 503:
        return failure(liquid::errc::unavailable, error.message, std::move(context));
    case 507:
        return failure(liquid::errc::busy, "the workspace store's quota is reached: " + error.message,
                       std::move(context));
    default:
        return failure(response.status >= 500 ? liquid::errc::unavailable : liquid::errc::io,
                       error.message, std::move(context));
    }
}

// The refusals of a workspace Zetta lake will not open a store on start with one of these
// (lake#543): one without the prefix durability lake relies on, and one of several nodes, where the
// incarnation is not yet per owner.
constexpr std::string_view durability_refusal =
    "the workspace Zetta does not report the object log durability Lake needs";
constexpr std::string_view several_nodes_refusal =
    "the workspace Zetta has more than one node, and Lake checks its incarnation on one node only";

// The answer's `value`, or an empty object.
nlohmann::json answered_value(const ObjectResponse& response) {
    const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    if (!body.is_object() || !body.contains("value") || !body.at("value").is_object()) {
        return nlohmann::json::object();
    }
    return body.at("value");
}

// Why lake will not use a workspace Zetta that answered a lease with `value` (lake#543), or empty.
std::string durability_shortfall(const nlohmann::json& value) {
    const nlohmann::json durability = value.value("object_log_prefix_durability", nlohmann::json());
    if (!durability.is_number_unsigned() && !durability.is_number_integer()) {
        return "it reports no object_log_prefix_durability, so it runs a Zetta image from before "
               "oramasearch/zetta#1237";
    }
    if (durability.get<std::int64_t>() < static_cast<std::int64_t>(workspace_store_prefix_durability)) {
        return "it reports object_log_prefix_durability " + durability.dump() + ", and Lake needs " +
               std::to_string(workspace_store_prefix_durability) + " or later";
    }
    const auto incarnation = value.find("incarnation");
    if (incarnation == value.end() || !incarnation->is_string() || incarnation->get<std::string>().empty()) {
        return "it reports no incarnation";
    }
    return {};
}

liquid::error durability_failure(const std::string& reason, std::string context) {
    return failure(liquid::errc::unsupported,
                   std::string(durability_refusal) + ": " + reason +
                       ". Lake makes each store operation durable with one :barrier, which needs "
                       "zetta#1235's prefix durability, and checks it and the incarnation only on a "
                       "Zetta that reports them (oramasearch/zetta#1237, object_log_prefix_durability "
                       "1 or later); it opens no workspace store until its Zetta runs an image built "
                       "with zetta#1237 (lake#543)",
                   std::move(context));
}

std::string shown_incarnation(const std::string& incarnation) {
    return incarnation.empty() ? std::string("none") : incarnation;
}

// The `value` of a successful JSON envelope.
liquid::result<nlohmann::json> envelope_value(const ObjectResponse& response, std::string context) {
    const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    if (!body.is_object() || !body.value("ok", false)) {
        return std::unexpected(failure(liquid::errc::corrupt,
                                       "the workspace Zetta answered without a JSON envelope",
                                       std::move(context)));
    }
    return body.value("value", nlohmann::json::object());
}

} // namespace

namespace {
// The deadline of the settling before a release on this thread (steady clock nanoseconds), or 0.
thread_local std::int64_t settle_deadline = 0;
} // namespace

namespace {
// Why a client refuses every later write and :barrier (lake#543): an unsupported workspace Zetta
// (no durability format, several nodes) keeps answering as such, so lake keeps answering
// store_unavailable naming the reason; a restart inside an operation is `unavailable`.
liquid::error stopped_error(const std::string& reason, const std::string& context) {
    const bool unsupported =
        reason.starts_with(durability_refusal) || reason.starts_with(several_nodes_refusal);
    return failure(unsupported ? liquid::errc::unsupported : liquid::errc::unavailable, reason, context);
}
} // namespace

bool workspace_zetta_unsupported(const liquid::error& error) {
    return error.code == liquid::errc::unsupported &&
           (error.message.starts_with(durability_refusal) || error.message.starts_with(several_nodes_refusal));
}

std::shared_ptr<ObjectTransport> make_zetta_object_transport() {
    return make_pooled_object_transport();
}

std::string_view object_request_kind_name(ObjectRequestKind kind) {
    switch (kind) {
    case ObjectRequestKind::put:
        return "put";
    case ObjectRequestKind::put_if_absent:
        return "put_if_absent";
    case ObjectRequestKind::patch:
        return "patch";
    case ObjectRequestKind::barrier:
        return "barrier";
    case ObjectRequestKind::get:
        return "get";
    case ObjectRequestKind::ranged_get:
        return "ranged_get";
    case ObjectRequestKind::list:
        return "list";
    case ObjectRequestKind::remove:
        return "delete";
    case ObjectRequestKind::lease_acquire:
        return "lease_acquire";
    case ObjectRequestKind::lease_renew:
        return "lease_renew";
    case ObjectRequestKind::lease_release:
        return "lease_release";
    case ObjectRequestKind::usage:
        return "usage";
    case ObjectRequestKind::create_bucket:
        return "create_bucket";
    }
    return "other";
}

void ObjectRequestTally::add_difference(
    const ObjectRequestTally& later,
    const std::array<std::uint64_t, object_request_kinds>& earlier_count,
    const std::array<std::uint64_t, object_request_kinds>& earlier_nanoseconds) {
    for (std::size_t index = 0; index < object_request_kinds; ++index) {
        const auto kind = static_cast<ObjectRequestKind>(index);
        if (kind == ObjectRequestKind::lease_renew || kind == ObjectRequestKind::usage) {
            continue;
        }
        const std::uint64_t requests = later.count[index].load(std::memory_order_relaxed);
        const std::uint64_t spent = later.nanoseconds[index].load(std::memory_order_relaxed);
        if (requests > earlier_count[index]) {
            count[index].fetch_add(requests - earlier_count[index], std::memory_order_relaxed);
            nanoseconds[index].fetch_add(spent - earlier_nanoseconds[index], std::memory_order_relaxed);
        }
    }
}

std::uint64_t ObjectRequestTally::total_requests() const {
    std::uint64_t total = 0;
    for (const auto& value : count) {
        total += value.load(std::memory_order_relaxed);
    }
    return total;
}

std::uint64_t ObjectRequestTally::total_nanoseconds() const {
    std::uint64_t total = 0;
    for (const auto& value : nanoseconds) {
        total += value.load(std::memory_order_relaxed);
    }
    return total;
}

std::string ObjectRequestTally::summary() const {
    std::string output;
    for (std::size_t index = 0; index < object_request_kinds; ++index) {
        const std::uint64_t value = count[index].load(std::memory_order_relaxed);
        if (value == 0) {
            continue;
        }
        if (!output.empty()) {
            output += ' ';
        }
        output += object_request_kind_name(static_cast<ObjectRequestKind>(index));
        output += '=';
        output += std::to_string(value);
    }
    return output;
}

namespace {
thread_local ObjectRequestTally* current_request_tally = nullptr;
} // namespace

ObjectRequestScope::ObjectRequestScope(ObjectRequestTally* tally) : previous_(current_request_tally) {
    current_request_tally = tally;
}

ObjectRequestScope::~ObjectRequestScope() {
    current_request_tally = previous_;
}

ObjectRequestTally* ObjectRequestScope::current() {
    return current_request_tally;
}

// ---- ZettaObjectClient -----------------------------------------------------------------------

ZettaObjectClient::ZettaObjectClient(std::shared_ptr<ObjectTransport> transport,
                                     ObjectEndpoint endpoint,
                                     std::string bucket,
                                     std::shared_ptr<ObjectStoreCounters> counters,
                                     ObjectRequestObserver observer)
    : transport_(std::move(transport)), endpoint_(std::move(endpoint)), bucket_(std::move(bucket)),
      counters_(counters ? std::move(counters) : std::make_shared<ObjectStoreCounters>()),
      observer_(std::move(observer)) {
    while (endpoint_.url.ends_with('/')) {
        endpoint_.url.pop_back();
    }
}

std::string ZettaObjectClient::object_url(std::string_view key) const {
    return endpoint_.url + "/v1/objects/" + bucket_ + "/" + encode_component(key, true);
}

std::string ZettaObjectClient::bucket_url(std::string_view suffix) const {
    return endpoint_.url + "/v1/objects/" + bucket_ + std::string(suffix);
}

liquid::runtime::task<ObjectResponse>
ZettaObjectClient::send(ObjectRequest request, bool idempotent, ObjectRequestKind kind) {
    request.headers.emplace_back("Authorization", "Bearer " + endpoint_.token);
    int attempts = idempotent ? 4 : 1;
    // A request that is not retried goes on a new connection (lake#544): a kept one that Zetta
    // closed as it was sent would fail it with no second attempt.
    request.fresh_connection = request.fresh_connection || !idempotent;
    if (const std::int64_t deadline = settle_deadline; deadline != 0) {
        const auto left = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(deadline)) -
                          std::chrono::steady_clock::now();
        if (left <= std::chrono::steady_clock::duration::zero()) {
            co_return ObjectResponse{0, {}, "the time to settle the workspace store before release ran out"};
        }
        attempts = 1;
        // Whole seconds, rounded up: the transport's timeout granularity.
        const auto seconds = std::chrono::ceil<std::chrono::seconds>(left).count();
        request.timeout_seconds = std::min<int>(request.timeout_seconds, static_cast<int>(seconds));
    }
    ObjectResponse response;
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        const auto started = std::chrono::steady_clock::now();
        auto answered = co_await transport_->exchange(request);
        if (!answered) {
            response = ObjectResponse{0, {}, answered.error().message};
        } else {
            response = std::move(*answered);
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started);
        const auto nanoseconds = static_cast<std::uint64_t>(elapsed.count());
        counters_->requests.fetch_add(1);
        counters_->request_nanoseconds.fetch_add(nanoseconds);
        counters_->by_kind.add(kind, nanoseconds);
        if (observer_) {
            observer_(kind, response.error.empty() ? response.status : 0, elapsed);
        }
        counters_->bytes_sent.fetch_add(request.body.size());
        counters_->bytes_received.fetch_add(response.body.size());
        const bool retry = !response.error.empty() || response.status == 503;
        if (!retry || attempt == attempts) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10 * attempt * attempt));
    }
    co_return response;
}

void ZettaObjectClient::mark_restarted(const std::string& current) {
    if (!restarted_.empty()) {
        return;
    }
    restarted_ = "the workspace Zetta restarted, or reopened the bucket's log, inside this operation "
                 "(incarnation " +
                 shown_incarnation(incarnation_.value_or(std::string())) + " before, " +
                 shown_incarnation(current) +
                 " now): writes it acknowledged since the last :barrier may be lost, so the "
                 "operation fails and the workspace store is opened again (lake#543)";
}

void ZettaObjectClient::mark_unsupported(std::string message) {
    std::lock_guard lock(incarnation_mutex_);
    if (restarted_.empty()) {
        restarted_ = std::move(message);
    }
}

liquid::result<void> ZettaObjectClient::observe_incarnation(const ObjectResponse& response,
                                                            const std::string& sent,
                                                            bool write,
                                                            std::uint64_t covered,
                                                            const std::string& context) {
    const nlohmann::json value = answered_value(response);
    const auto found = value.find("incarnation");
    const std::string answered =
        found != value.end() && found->is_string() ? found->get<std::string>() : std::string();
    // One node only (lake#543): the incarnation is the deciding node's, so a write placed on
    // several owners, or a barrier several nodes answered, is not covered by it.
    const bool several = value.value("replicas_expected", std::uint64_t{1}) > 1 ||
                         value.value("nodes", std::uint64_t{1}) > 1;
    std::lock_guard lock(incarnation_mutex_);
    if (several && restarted_.empty()) {
        restarted_ = std::string(several_nodes_refusal) +
                     ": an object answer named more than one owner or node; Lake opens no "
                     "workspace store on it until the incarnation is per owner (lake#543)";
        return refused(liquid::errc::unsupported, restarted_, context);
    }
    if (restarted_.empty()) {
        if (!sent.empty()) {
            // Never adopted from a success (lake#543): a success under another incarnation than
            // the one sent, or under none (a rollback to an older Zetta), is a restart.
            if (answered != sent) {
                mark_restarted(answered);
            }
        } else if (!incarnation_) {
            // A client never opened through :acquire takes the first incarnation it sees.
            if (!answered.empty()) {
                incarnation_ = answered;
            }
        } else if (answered != *incarnation_) {
            mark_restarted(answered);
        }
    }
    if (!restarted_.empty()) {
        return std::unexpected(stopped_error(restarted_, context));
    }
    if (write) {
        ++unsynced_writes_;
    } else {
        unsynced_writes_ -= std::min(covered, unsynced_writes_);
    }
    return {};
}

liquid::runtime::task<ObjectResponse> ZettaObjectClient::send_checked(ObjectRequest request,
                                                                      bool idempotent,
                                                                      ObjectRequestKind kind,
                                                                      std::string context) {
    const bool write = kind != ObjectRequestKind::barrier;
    // Bounded: each pass either sends the incarnation another request adopted meanwhile, or adopts
    // one after the lease check.
    for (int attempt = 1; attempt <= 4; ++attempt) {
        ObjectRequest sent_request = request;
        std::string sent;
        std::uint64_t covered = 0;
        {
            std::lock_guard lock(incarnation_mutex_);
            if (!restarted_.empty()) {
                co_return std::unexpected(stopped_error(restarted_, context));
            }
            sent = incarnation_.value_or(std::string());
            if (!sent.empty()) {
                sent_request.headers.emplace_back("Zetta-Incarnation", sent);
            }
            covered = unsynced_writes_;
        }
        auto response = co_await send(std::move(sent_request), idempotent, kind);
        if (!response) {
            co_return std::unexpected(response.error());
        }
        if (response->status == 409 && response->error.empty()) {
            const ZettaError error = zetta_error(*response);
            if (error.code == "incarnation_changed") {
                const std::string current = error.details.is_object()
                                                ? error.details.value("incarnation", std::string())
                                                : std::string();
                std::optional<std::tuple<std::string, std::uint64_t, std::chrono::milliseconds>> check;
                {
                    std::lock_guard lock(incarnation_mutex_);
                    if (restarted_.empty()) {
                        if (incarnation_.value_or(std::string()) != sent) {
                            // Another request adopted a new incarnation since this one was sent:
                            // send it again with that one.
                            continue;
                        }
                        if (current.empty()) {
                            mark_restarted(current);
                        } else if (unsynced_writes_ != 0) {
                            mark_restarted(current);
                        } else if (lease_holder_.empty()) {
                            // Nothing unsynced and no lease to check (a client never opened through
                            // :acquire): nothing was lost.
                            incarnation_ = current;
                            continue;
                        } else {
                            check.emplace(lease_holder_, lease_fence_, lease_ttl_);
                        }
                    }
                }
                if (check) {
                    // Nothing acknowledged was unsynced, but a restart can also lose the last
                    // second of Zetta's catalogue, the lease with it: adopt the new incarnation
                    // only while this holder still holds the lease with its fence (lake#543).
                    const auto& [holder, fence, ttl] = *check;
                    auto renewed = co_await renew(holder, fence, ttl, std::chrono::seconds(5));
                    std::lock_guard lock(incarnation_mutex_);
                    if (restarted_.empty()) {
                        if (!renewed) {
                            restarted_ = "the workspace Zetta restarted and this Lake process no longer "
                                         "holds the workspace store's lease (" +
                                         renewed.error().message +
                                         "): the store is opened again (lake#543)";
                        } else if (incarnation_.value_or(std::string()) == sent) {
                            if (unsynced_writes_ != 0) {
                                mark_restarted(current);
                            } else {
                                incarnation_ = current;
                            }
                        }
                    }
                    if (restarted_.empty()) {
                        continue;
                    }
                }
                std::lock_guard lock(incarnation_mutex_);
                co_return std::unexpected(stopped_error(restarted_, context));
            }
        }
        if (response->status == 200 || response->status == 201) {
            if (auto observed = observe_incarnation(*response, sent, write, covered, context); !observed) {
                co_return std::unexpected(observed.error());
            }
        }
        co_return response;
    }
    std::lock_guard lock(incarnation_mutex_);
    if (restarted_.empty()) {
        restarted_ = "the workspace Zetta's incarnation kept changing (lake#543)";
    }
    co_return std::unexpected(stopped_error(restarted_, context));
}

bool ZettaObjectClient::restarted() const {
    std::lock_guard lock(incarnation_mutex_);
    return !restarted_.empty();
}

std::string ZettaObjectClient::incarnation() const {
    std::lock_guard lock(incarnation_mutex_);
    return incarnation_.value_or(std::string());
}

void ZettaObjectClient::set_before_release(std::function<liquid::runtime::task<void>()> settle,
                                           std::function<void()> abandon) {
    std::lock_guard lock(settle_mutex_);
    settle_ = std::move(settle);
    abandon_ = std::move(abandon);
}

void ZettaObjectClient::set_owned(bool owned) {
    owned_.store(owned);
}

bool ZettaObjectClient::owned() const {
    return owned_.load();
}

void ZettaObjectClient::abandon_before_release() {
    std::function<void()> abandon;
    {
        std::lock_guard lock(settle_mutex_);
        abandon = abandon_;
    }
    if (abandon) {
        abandon();
    }
}

liquid::runtime::task<void> ZettaObjectClient::before_release(std::chrono::milliseconds budget) {
    std::function<liquid::runtime::task<void>()> settle;
    {
        std::lock_guard lock(settle_mutex_);
        settle = settle_;
    }
    if (!settle) {
        co_return liquid::result<void>{};
    }
    // Only the settling requests are cut: they run on this thread (block_on keeps a store call's
    // coroutines on its thread); other users of the client, such as search, keep their timeouts.
    const std::int64_t previous = settle_deadline;
    settle_deadline = (std::chrono::steady_clock::now() + budget).time_since_epoch().count();
    auto settled = co_await settle();
    settle_deadline = previous;
    co_return settled;
}

liquid::runtime::task<void> ZettaObjectClient::create_bucket(std::uint64_t quota_bytes) {
    nlohmann::json input{{"bucket", bucket_}};
    if (quota_bytes != 0) {
        input["quota_bytes"] = quota_bytes;
    }
    ObjectRequest request;
    request.method = "POST";
    request.url = endpoint_.url + "/v1/create_bucket";
    request.headers = {{"Content-Type", "application/json"}, {"Accept", "application/json"}};
    request.body = input.dump();
    auto response = co_await send(std::move(request), true, ObjectRequestKind::create_bucket);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    // A bucket that exists with another definition (another quota) is used as it is.
    if (response->status == 409 && response->error.empty() &&
        zetta_error(*response).code == "conflict") {
        co_return liquid::result<void>{};
    }
    if (response->status != 200 && response->status != 201) {
        co_return std::unexpected(mapped(*response, "zetta bucket " + bucket_));
    }
    co_return liquid::result<void>{};
}

liquid::runtime::task<void> ZettaObjectClient::put(std::string key,
                                                   std::string bytes,
                                                   std::optional<std::uint64_t> fence,
                                                   bool if_absent,
                                                   bool durable) {
    counters_->puts.fetch_add(1);
    ObjectRequest request;
    request.method = "PUT";
    request.url = object_url(key);
    if (if_absent || durable) {
        request.url += if_absent && durable ? "?if_absent=true&durable=true"
                       : if_absent          ? "?if_absent=true"
                                            : "?durable=true";
    }
    request.headers = {{"Content-Type", "application/octet-stream"}};
    if (fence) {
        request.headers.emplace_back("Zetta-Fence", std::to_string(*fence));
    }
    request.body = std::move(bytes);
    auto response = co_await send_checked(std::move(request), !if_absent,
                                          if_absent ? ObjectRequestKind::put_if_absent
                                                    : ObjectRequestKind::put,
                                          bucket_ + "/" + key);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status == 200 || response->status == 201) {
        co_return liquid::result<void>{};
    }
    auto error = mapped(*response, bucket_ + "/" + key);
    if (if_absent && error.code == liquid::errc::conflict) {
        error.code = liquid::errc::already_exists;
    }
    co_return std::unexpected(std::move(error));
}

liquid::runtime::task<void> ZettaObjectClient::append(std::string key,
                                                      std::uint64_t offset,
                                                      std::string bytes,
                                                      std::optional<std::uint64_t> fence,
                                                      bool durable) {
    counters_->appends.fetch_add(1);
    ObjectRequest request;
    request.method = "PATCH";
    request.url = object_url(key) + "?offset=" + std::to_string(offset) + (durable ? "&durable=true" : "");
    request.headers = {{"Content-Type", "application/octet-stream"}};
    if (fence) {
        request.headers.emplace_back("Zetta-Fence", std::to_string(*fence));
    }
    request.body = std::move(bytes);
    // A retried append of the same bytes at the same offset changes nothing.
    auto response = co_await send_checked(std::move(request), true, ObjectRequestKind::patch,
                                          bucket_ + "/" + key);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status == 200 || response->status == 201) {
        co_return liquid::result<void>{};
    }
    co_return std::unexpected(mapped(*response, bucket_ + "/" + key));
}

liquid::runtime::task<std::string> ZettaObjectClient::get(std::string key,
                                                          std::uint64_t offset,
                                                          std::uint64_t size) {
    if (size == 0) {
        co_return std::string();
    }
    counters_->gets.fetch_add(1);
    ObjectRequest request;
    request.method = "GET";
    request.url = object_url(key);
    request.headers = {{"Range", std::format("bytes={}-{}", offset, offset + size - 1)}};
    auto response = co_await send(std::move(request), true, ObjectRequestKind::ranged_get);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status != 200 && response->status != 206) {
        co_return std::unexpected(mapped(*response, bucket_ + "/" + key));
    }
    if (response->body.size() != size) {
        co_return std::unexpected(failure(
            liquid::errc::corrupt,
            std::format("the workspace Zetta answered {} bytes for a range of {}",
                        response->body.size(), size),
            bucket_ + "/" + key));
    }
    co_return std::move(response->body);
}

liquid::runtime::task<std::string> ZettaObjectClient::get_all(std::string key) {
    counters_->gets.fetch_add(1);
    ObjectRequest request;
    request.method = "GET";
    request.url = object_url(key);
    auto response = co_await send(std::move(request), true, ObjectRequestKind::get);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status != 200 && response->status != 206) {
        co_return std::unexpected(mapped(*response, bucket_ + "/" + key));
    }
    co_return std::move(response->body);
}

liquid::runtime::task<void> ZettaObjectClient::remove(std::string key,
                                                      std::optional<std::uint64_t> fence,
                                                      bool durable) {
    counters_->removes.fetch_add(1);
    ObjectRequest request;
    request.method = "DELETE";
    request.url = object_url(key) + (durable ? "?durable=true" : "");
    if (fence) {
        request.headers.emplace_back("Zetta-Fence", std::to_string(*fence));
    }
    auto response =
        co_await send_checked(std::move(request), true, ObjectRequestKind::remove, bucket_ + "/" + key);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status == 200 || response->status == 204) {
        co_return liquid::result<void>{};
    }
    co_return std::unexpected(mapped(*response, bucket_ + "/" + key));
}

liquid::runtime::task<std::vector<ObjectListEntry>> ZettaObjectClient::list(std::string prefix) {
    std::vector<ObjectListEntry> output;
    std::string after;
    for (;;) {
        counters_->lists.fetch_add(1);
        ObjectRequest request;
        request.method = "GET";
        request.url = bucket_url("") + "?prefix=" + encode_component(prefix, false) + "&limit=1000";
        if (!after.empty()) {
            request.url += "&after=" + encode_component(after, false);
        }
        request.headers = {{"Accept", "application/json"}};
        auto response = co_await send(std::move(request), true, ObjectRequestKind::list);
        if (!response) {
            co_return std::unexpected(response.error());
        }
        if (response->status != 200) {
            co_return std::unexpected(mapped(*response, bucket_ + "/" + prefix));
        }
        auto value = envelope_value(*response, bucket_ + "/" + prefix);
        if (!value) {
            co_return std::unexpected(value.error());
        }
        for (const nlohmann::json& object : value->value("objects", nlohmann::json::array())) {
            output.push_back({object.value("key", std::string()), object.value("size", std::uint64_t{0})});
        }
        after = value->value("next_after", std::string());
        if (after.empty()) {
            break;
        }
    }
    co_return output;
}

liquid::runtime::task<void> ZettaObjectClient::barrier() {
    counters_->barriers.fetch_add(1);
    ObjectRequest request;
    request.method = "POST";
    request.url = bucket_url(":barrier");
    request.headers = {{"Content-Type", "application/json"}, {"Accept", "application/json"}};
    request.body = "{}";
    auto response =
        co_await send_checked(std::move(request), true, ObjectRequestKind::barrier, bucket_ + ":barrier");
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status != 200) {
        co_return std::unexpected(mapped(*response, bucket_ + ":barrier"));
    }
    co_return liquid::result<void>{};
}

liquid::runtime::task<ObjectUsage> ZettaObjectClient::usage(std::chrono::milliseconds timeout) {
    ObjectRequest request;
    request.method = "GET";
    request.url = bucket_url(":usage");
    request.headers = {{"Accept", "application/json"}};
    request.timeout_seconds = std::max<int>(1, static_cast<int>(timeout.count() / 1000));
    auto response = co_await send(std::move(request), false, ObjectRequestKind::usage);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status != 200) {
        co_return std::unexpected(mapped(*response, bucket_ + ":usage"));
    }
    auto value = envelope_value(*response, bucket_ + ":usage");
    if (!value) {
        co_return std::unexpected(value.error());
    }
    co_return ObjectUsage{value->value("objects", std::uint64_t{0}),
                          value->value("bytes", std::uint64_t{0}),
                          value->value("quota_bytes", std::uint64_t{0})};
}

namespace {

liquid::result<ObjectLease> lease_of(const ObjectResponse& response, std::string context) {
    auto value = envelope_value(response, context);
    if (!value) {
        return std::unexpected(value.error());
    }
    return ObjectLease{value->value("holder", std::string()), value->value("fence", std::uint64_t{0}),
                       value->value("expires_ms", std::int64_t{0})};
}

} // namespace

liquid::runtime::task<ObjectLease> ZettaObjectClient::acquire(std::string holder,
                                                              std::chrono::milliseconds ttl,
                                                              std::chrono::milliseconds wait) {
    ObjectRequest request;
    request.method = "POST";
    request.url = bucket_url(":acquire");
    request.headers = {{"Content-Type", "application/json"}, {"Accept", "application/json"}};
    request.body =
        nlohmann::json{{"holder", holder}, {"ttl_ms", ttl.count()}, {"wait_ms", wait.count()}}.dump();
    request.timeout_seconds = static_cast<int>(wait.count() / 1000) + 30;
    auto response = co_await send(std::move(request), false, ObjectRequestKind::lease_acquire);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status != 200) {
        auto error = mapped(*response, bucket_ + ":acquire");
        if (error.code == liquid::errc::locked) {
            const ZettaError details = zetta_error(*response);
            std::string current = details.details.is_object()
                                      ? details.details.value("holder", std::string())
                                      : std::string();
            error.message = "the workspace store is owned by " +
                            (current.empty() ? std::string("another holder") : current) +
                            "; one Lake process owns a workspace store at a time";
        }
        co_return std::unexpected(std::move(error));
    }
    auto lease = lease_of(*response, bucket_ + ":acquire");
    if (!lease) {
        co_return std::unexpected(lease.error());
    }
    // lake#543: a Zetta without the durability format lake needs is refused before the store opens,
    // and the lease it granted is given back.
    const nlohmann::json value = answered_value(*response);
    if (const std::string reason = durability_shortfall(value); !reason.empty()) {
        static_cast<void>(co_await release(lease->holder, lease->fence));
        co_return std::unexpected(durability_failure(reason, bucket_ + ":acquire"));
    }
    const std::string current = value.at("incarnation").get<std::string>();
    {
        std::lock_guard lock(incarnation_mutex_);
        lease_holder_ = lease->holder;
        lease_fence_ = lease->fence;
        lease_ttl_ = ttl;
        if (!incarnation_ || (unsynced_writes_ == 0 && restarted_.empty())) {
            incarnation_ = current;
            // What the store serves when it opens may include writes a predecessor never made
            // durable: until its first :barrier answers, a change of incarnation is a restart.
            unsynced_writes_ = 1;
        } else if (*incarnation_ != current) {
            mark_restarted(current);
        }
    }
    co_return lease;
}

liquid::runtime::task<ObjectLease> ZettaObjectClient::renew(std::string holder,
                                                            std::uint64_t fence,
                                                            std::chrono::milliseconds ttl,
                                                            std::chrono::milliseconds timeout) {
    ObjectRequest request;
    request.method = "POST";
    request.url = bucket_url(":renew");
    request.headers = {{"Content-Type", "application/json"}, {"Accept", "application/json"}};
    request.body =
        nlohmann::json{{"holder", holder}, {"fence", fence}, {"ttl_ms", ttl.count()}}.dump();
    // One attempt, answered well inside the lease: a renewal that cannot finish in time is worth
    // nothing, and the owner's next renewal is the retry.
    request.timeout_seconds = std::max<int>(1, static_cast<int>(timeout.count() / 1000));
    auto response = co_await send(std::move(request), false, ObjectRequestKind::lease_renew);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status != 200) {
        co_return std::unexpected(mapped(*response, bucket_ + ":renew"));
    }
    // lake#543: a renewal answered by a Zetta without the durability format (rolled back while
    // the store is open) fails the client at once, so nothing more is sent; one answered under
    // another incarnation while writes are unsynced is a restart.
    const nlohmann::json value = answered_value(*response);
    if (const std::string reason = durability_shortfall(value); !reason.empty()) {
        auto error = durability_failure(reason, bucket_ + ":renew");
        mark_unsupported(error.message);
        co_return std::unexpected(std::move(error));
    }
    {
        std::lock_guard lock(incarnation_mutex_);
        const std::string current = value.at("incarnation").get<std::string>();
        if (incarnation_ && *incarnation_ != current && unsynced_writes_ != 0) {
            mark_restarted(current);
        }
    }
    co_return lease_of(*response, bucket_ + ":renew");
}

liquid::runtime::task<void> ZettaObjectClient::release(std::string holder, std::uint64_t fence) {
    ObjectRequest request;
    request.method = "POST";
    request.url = bucket_url(":release");
    request.headers = {{"Content-Type", "application/json"}, {"Accept", "application/json"}};
    request.body = nlohmann::json{{"holder", holder}, {"fence", fence}}.dump();
    auto response = co_await send(std::move(request), true, ObjectRequestKind::lease_release);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status != 200) {
        co_return std::unexpected(mapped(*response, bucket_ + ":release"));
    }
    co_return liquid::result<void>{};
}

// ---- WorkspaceStoreCipher --------------------------------------------------------------------

namespace {

struct CipherContext {
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    ~CipherContext() {
        EVP_CIPHER_CTX_free(context);
    }
};

liquid::result<std::string> gcm_seal(const WorkspaceStoreCipher::Key& key,
                                     std::string_view associated,
                                     std::span<const std::byte> plain) {
    std::array<unsigned char, WorkspaceStoreCipher::nonce_size> nonce{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        return refused<std::string>(liquid::errc::internal, "could not generate a nonce");
    }
    CipherContext cipher;
    if (cipher.context == nullptr ||
        EVP_EncryptInit_ex(cipher.context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(cipher.context, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()),
                            nullptr) != 1 ||
        EVP_EncryptInit_ex(cipher.context, nullptr, nullptr, key.data(), nonce.data()) != 1) {
        return refused<std::string>(liquid::errc::internal, "could not start AES-256-GCM");
    }
    int length = 0;
    if (!associated.empty() &&
        EVP_EncryptUpdate(cipher.context, nullptr, &length,
                          reinterpret_cast<const unsigned char*>(associated.data()),
                          static_cast<int>(associated.size())) != 1) {
        return refused<std::string>(liquid::errc::internal, "could not authenticate a block");
    }
    std::string output(WorkspaceStoreCipher::nonce_size + plain.size() + WorkspaceStoreCipher::tag_size,
                       '\0');
    std::memcpy(output.data(), nonce.data(), nonce.size());
    auto* out = reinterpret_cast<unsigned char*>(output.data() + nonce.size());
    if (!plain.empty() &&
        EVP_EncryptUpdate(cipher.context, out, &length,
                          reinterpret_cast<const unsigned char*>(plain.data()),
                          static_cast<int>(plain.size())) != 1) {
        return refused<std::string>(liquid::errc::internal, "could not encrypt a block");
    }
    int final_length = 0;
    if (EVP_EncryptFinal_ex(cipher.context, out + plain.size(), &final_length) != 1 ||
        EVP_CIPHER_CTX_ctrl(cipher.context, EVP_CTRL_GCM_GET_TAG,
                            static_cast<int>(WorkspaceStoreCipher::tag_size),
                            out + plain.size()) != 1) {
        return refused<std::string>(liquid::errc::internal, "could not finish a block");
    }
    return output;
}

liquid::result<std::vector<std::byte>> gcm_open(const WorkspaceStoreCipher::Key& key,
                                                std::string_view associated,
                                                std::string_view sealed) {
    if (sealed.size() < WorkspaceStoreCipher::overhead) {
        return refused<std::vector<std::byte>>(liquid::errc::corrupt,
                                               "an encrypted block is truncated");
    }
    const auto* nonce = reinterpret_cast<const unsigned char*>(sealed.data());
    const std::size_t size = sealed.size() - WorkspaceStoreCipher::overhead;
    const auto* body = nonce + WorkspaceStoreCipher::nonce_size;
    const auto* tag = body + size;
    CipherContext cipher;
    if (cipher.context == nullptr ||
        EVP_DecryptInit_ex(cipher.context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(cipher.context, EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(WorkspaceStoreCipher::nonce_size), nullptr) != 1 ||
        EVP_DecryptInit_ex(cipher.context, nullptr, nullptr, key.data(), nonce) != 1) {
        return refused<std::vector<std::byte>>(liquid::errc::internal, "could not start AES-256-GCM");
    }
    int length = 0;
    if (!associated.empty() &&
        EVP_DecryptUpdate(cipher.context, nullptr, &length,
                          reinterpret_cast<const unsigned char*>(associated.data()),
                          static_cast<int>(associated.size())) != 1) {
        return refused<std::vector<std::byte>>(liquid::errc::internal, "could not authenticate a block");
    }
    std::vector<std::byte> output(size);
    if (size != 0 &&
        EVP_DecryptUpdate(cipher.context, reinterpret_cast<unsigned char*>(output.data()), &length,
                          body, static_cast<int>(size)) != 1) {
        return refused<std::vector<std::byte>>(liquid::errc::corrupt, "an encrypted block does not decrypt");
    }
    if (EVP_CIPHER_CTX_ctrl(cipher.context, EVP_CTRL_GCM_SET_TAG,
                            static_cast<int>(WorkspaceStoreCipher::tag_size),
                            const_cast<unsigned char*>(tag)) != 1) {
        return refused<std::vector<std::byte>>(liquid::errc::internal, "could not set a block's tag");
    }
    int final_length = 0;
    if (EVP_DecryptFinal_ex(cipher.context, reinterpret_cast<unsigned char*>(output.data()) + size,
                            &final_length) != 1) {
        return refused<std::vector<std::byte>>(
            liquid::errc::corrupt,
            "an encrypted block failed authentication: it was altered, moved, or written under "
            "another workspace's key");
    }
    return output;
}

std::string key_associated(std::string_view workspace_id, std::uint32_t version) {
    return std::string(wrap_label) + std::string(workspace_id) + "/data-key/" +
           std::to_string(version);
}

} // namespace

WorkspaceStoreCipher::WorkspaceStoreCipher(std::string workspace_id,
                                           std::uint32_t key_version,
                                           Key data_key)
    : workspace_id_(std::move(workspace_id)), key_version_(key_version), key_(data_key) {}

WorkspaceStoreCipher::~WorkspaceStoreCipher() {
    secrets_crypto::cleanse(key_);
}

liquid::result<WorkspaceStoreCipher::Key>
WorkspaceStoreCipher::wrapping_key(std::string_view instance_kek, std::string_view workspace_id) {
    if (workspace_id.empty()) {
        return refused<Key>(liquid::errc::invalid_argument, "a workspace store needs a workspace id");
    }
    auto root = secrets_crypto::instance_key(instance_kek);
    if (!root) {
        return refused<Key>(liquid::errc::not_configured,
                            "a workspace store needs LAKE_INSTANCE_KEK: " + root.error().message);
    }
    auto derived =
        secrets_crypto::derive_hkdf(*root, std::string(wrap_label) + std::string(workspace_id));
    secrets_crypto::cleanse(*root);
    if (!derived) {
        return refused<Key>(liquid::errc::internal, derived.error().message);
    }
    return *derived;
}

liquid::result<std::string> WorkspaceStoreCipher::wrap(const Key& wrapping,
                                                       std::string_view workspace_id,
                                                       std::uint32_t version,
                                                       const Key& key) {
    auto sealed = gcm_seal(wrapping, key_associated(workspace_id, version),
                           std::as_bytes(std::span(key.data(), key.size())));
    if (!sealed) {
        return std::unexpected(sealed.error());
    }
    auto encoded = secrets_crypto::base64url_encode(*sealed);
    if (!encoded) {
        return refused<std::string>(liquid::errc::internal, encoded.error().message);
    }
    return nlohmann::json{{"format", "lake-workspace-store-key/1"},
                          {"workspace_id", std::string(workspace_id)},
                          {"version", version},
                          {"algorithm", "AES-256-GCM"},
                          {"wrapped", *encoded}}
        .dump();
}

liquid::result<std::pair<std::uint32_t, WorkspaceStoreCipher::Key>>
WorkspaceStoreCipher::unwrap(const Key& wrapping,
                             std::string_view workspace_id,
                             std::string_view document) {
    using Output = std::pair<std::uint32_t, Key>;
    const nlohmann::json value = nlohmann::json::parse(document, nullptr, false);
    if (!value.is_object() || value.value("format", std::string()) != "lake-workspace-store-key/1" ||
        !value.contains("version") || !value.at("version").is_number_unsigned() ||
        !value.contains("wrapped") || !value.at("wrapped").is_string()) {
        return refused<Output>(liquid::errc::corrupt, "the workspace store key document is malformed",
                               std::string(workspace_store_key_object));
    }
    if (value.value("workspace_id", std::string()) != workspace_id) {
        return refused<Output>(liquid::errc::unauthorized,
                               "the workspace store key belongs to another workspace",
                               std::string(workspace_store_key_object));
    }
    const auto version = value.at("version").get<std::uint32_t>();
    auto sealed = secrets_crypto::base64url_decode(value.at("wrapped").get<std::string>());
    if (!sealed) {
        return refused<Output>(liquid::errc::corrupt, "the workspace store key is not base64url",
                               std::string(workspace_store_key_object));
    }
    auto opened = gcm_open(wrapping, key_associated(workspace_id, version), *sealed);
    if (!opened || opened->size() != 32) {
        return refused<Output>(liquid::errc::unauthorized,
                               "the workspace store key does not unwrap under this Lake's "
                               "LAKE_INSTANCE_KEK",
                               std::string(workspace_store_key_object));
    }
    Key key{};
    std::memcpy(key.data(), opened->data(), key.size());
    std::fill(opened->begin(), opened->end(), std::byte{0});
    return Output{version, key};
}

liquid::result<std::string> WorkspaceStoreCipher::seal(std::string_view associated,
                                                       std::span<const std::byte> plain) const {
    return gcm_seal(key_, associated, plain);
}

liquid::result<std::vector<std::byte>> WorkspaceStoreCipher::open(std::string_view associated,
                                                                  std::string_view sealed) const {
    return gcm_open(key_, associated, sealed);
}

// ---- ZettaObjectBackend ----------------------------------------------------------------------

struct ZettaObjectBackend::Shared {
    std::shared_ptr<ZettaObjectClient> client;
    ZettaObjectBackendOptions options;

    std::mutex cipher_mutex;
    std::shared_ptr<const WorkspaceStoreCipher> cipher;
    std::atomic<bool> failed{false};

    // Aligned ciphertext windows: "<key>\n<salt>\n<window index>" -> bytes (the window's bytes as
    // far as the object reached when it was read). A version's bytes never change: an object only
    // gains complete blocks, and a replacement has a new salt.
    // The fenced views liquid writes through (one per open). Kept here so the owner can settle
    // them before it releases the lease, after liquid let go of them; the root backend drops them.
    std::mutex views_mutex;
    std::vector<std::shared_ptr<ZettaObjectBackend>> views;

    std::mutex cache_mutex;
    std::list<std::string> lru;
    std::map<std::string, std::pair<std::string, std::list<std::string>::iterator>> windows;
    std::size_t cached_bytes = 0;

    // [from, to) of a cached window that reaches at least `to`, or nothing.
    std::optional<std::string> slice(const std::string& name, std::size_t from, std::size_t to) {
        std::lock_guard lock(cache_mutex);
        const auto found = windows.find(name);
        if (found == windows.end() || found->second.first.size() < to) {
            return std::nullopt;
        }
        lru.splice(lru.begin(), lru, found->second.second);
        return found->second.first.substr(from, to - from);
    }

    void remember(const std::string& name, std::string bytes) {
        std::lock_guard lock(cache_mutex);
        if (bytes.size() > options.cache_bytes) {
            return;
        }
        if (const auto found = windows.find(name); found != windows.end()) {
            cached_bytes -= found->second.first.size();
            lru.erase(found->second.second);
            windows.erase(found);
        }
        while (!lru.empty() && cached_bytes + bytes.size() > options.cache_bytes) {
            const auto victim = windows.find(lru.back());
            cached_bytes -= victim->second.first.size();
            windows.erase(victim);
            lru.pop_back();
        }
        lru.push_front(name);
        cached_bytes += bytes.size();
        windows.emplace(name, std::make_pair(std::move(bytes), lru.begin()));
    }

    // Bytes this process just appended at `offset` of an object version: they extend its cached
    // windows, so reading back what was just written costs no request.
    void extend(const std::string& key, const std::string& salt, std::uint64_t offset,
                std::string_view bytes) {
        const std::uint64_t window = options.window_bytes;
        while (!bytes.empty()) {
            const std::uint64_t index = offset / window;
            const std::uint64_t start = index * window;
            const std::size_t take =
                static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(), start + window - offset));
            const std::string name = key + "\n" + salt + "\n" + std::to_string(index);
            {
                std::lock_guard lock(cache_mutex);
                const auto found = windows.find(name);
                if (found != windows.end() && found->second.first.size() == offset - start &&
                    cached_bytes + take <= options.cache_bytes) {
                    found->second.first.append(bytes.substr(0, take));
                    cached_bytes += take;
                    lru.splice(lru.begin(), lru, found->second.second);
                    offset += take;
                    bytes.remove_prefix(take);
                    continue;
                }
            }
            if (offset == start) {
                remember(name, std::string(bytes.substr(0, take)));
            }
            offset += take;
            bytes.remove_prefix(take);
        }
    }

    void forget(const std::string& key) {
        std::lock_guard lock(cache_mutex);
        const std::string prefix = key + "\n";
        for (auto it = windows.lower_bound(prefix);
             it != windows.end() && it->first.starts_with(prefix);) {
            cached_bytes -= it->second.first.size();
            lru.erase(it->second.second);
            it = windows.erase(it);
        }
    }
};

struct ZettaObjectBackend::Entry {
    // The object K in Zetta and its ciphertext size there.
    bool remote = false;
    // Not in Zetta as this version, but the bucket still holds an older K (this version replaced
    // or followed a removal since it was last sent): its first write is a replacing PUT.
    bool replaces_remote = false;
    // The plaintext of the version in Zetta, when this view sent it whole and it is small
    // (lake#527): a put of the same bytes again changes nothing and sends nothing. The bucket's
    // log holds that version before anything issued after it (lake#535), so skipping the repeat
    // leaves every prefix of the issue order as it was.
    std::optional<std::vector<std::byte>> sent_plain;
    std::uint64_t ct_size = 0;
    // The header, once known: always for an object this view created or replaced.
    std::optional<std::string> header;
    std::uint64_t base_len = 0;
    std::uint32_t block_size = 0;
    std::string salt_hex;
    // Tail objects in Zetta for K: key -> ciphertext size.
    std::map<std::string, std::uint64_t> tails;
    // The open region: the plaintext after the last complete block in Zetta. `open_remote` of its
    // first bytes are held by the current tail object; the rest are buffered appends.
    bool open_loaded = false;
    std::vector<std::byte> open;
    std::uint64_t open_remote = 0;

    std::uint64_t base_ct() const {
        if (base_len == 0) {
            return 0;
        }
        const std::uint64_t blocks = (base_len + block_size - 1) / block_size;
        return base_len + blocks * WorkspaceStoreCipher::overhead;
    }
    std::uint64_t block_ct() const {
        return block_size + WorkspaceStoreCipher::overhead;
    }
    // Complete appended blocks in Zetta.
    std::uint64_t append_blocks() const {
        if (!remote || !header) {
            return 0;
        }
        return (ct_size - header_size - base_ct()) / block_ct();
    }
    std::string tail_key(const std::string& key, std::uint64_t block) const {
        return key + std::string(tail_marker) + std::to_string(block) + "-" + salt_hex;
    }
    std::optional<std::uint64_t> valid_tail_plain(const std::string& key) const {
        const auto found = tails.find(tail_key(key, append_blocks()));
        if (found == tails.end() || found->second < WorkspaceStoreCipher::overhead) {
            return std::nullopt;
        }
        return found->second - WorkspaceStoreCipher::overhead;
    }
    std::uint64_t open_size(const std::string& key) const {
        if (open_loaded) {
            return open.size();
        }
        return valid_tail_plain(key).value_or(0);
    }
    std::uint64_t size(const std::string& key) const {
        return base_len + append_blocks() * block_size + open_size(key);
    }
};

namespace {

liquid::result<void> check_liquid_key(std::string_view key) {
    if (key.empty() || key.starts_with('/') || key.size() > 1000) {
        return refused(liquid::errc::invalid_argument, "invalid store backend key", std::string(key));
    }
    if (key.starts_with("keys/") || key.starts_with(workspace_store_search_prefix) ||
        key.find(tail_marker) != std::string_view::npos) {
        return refused(liquid::errc::invalid_argument,
                       "the key is reserved by Lake's workspace store encryption", std::string(key));
    }
    return {};
}

std::string block_associated(const std::string& workspace_id,
                             std::string_view key,
                             std::string_view header,
                             char region,
                             std::uint64_t index,
                             std::size_t length) {
    std::string output = "lake-store/v1";
    output += '\0';
    output += workspace_id;
    output += '\0';
    output += key;
    output += '\0';
    output += header;
    output += region;
    put_u64(output, index);
    put_u64(output, length);
    return output;
}

struct ParsedHeader {
    std::uint32_t key_version = 0;
    std::uint32_t block_size = 0;
    std::uint64_t base_len = 0;
    std::string salt_hex;
};

liquid::result<ParsedHeader> parse_header(std::string_view bytes, const std::string& key) {
    if (bytes.size() != header_size ||
        !std::equal(header_magic.begin(), header_magic.end(), bytes.begin())) {
        return refused<ParsedHeader>(liquid::errc::corrupt,
                                     "the object is not a Lake workspace store object", key);
    }
    if (get_u32(bytes, 8) != header_format) {
        return refused<ParsedHeader>(liquid::errc::unsupported,
                                     "unknown workspace store object format", key);
    }
    ParsedHeader output;
    output.key_version = get_u32(bytes, 12);
    output.block_size = get_u32(bytes, 16);
    output.base_len = get_u64(bytes, 24);
    output.salt_hex = hex_of(std::span(reinterpret_cast<const unsigned char*>(bytes.data()) + 32, 8));
    if (output.block_size == 0 || output.block_size > 16U * 1024U * 1024U) {
        return refused<ParsedHeader>(liquid::errc::corrupt, "invalid workspace store block size",
                                     key);
    }
    return output;
}

liquid::result<std::string> make_header(std::uint32_t key_version,
                                        std::uint32_t block_size,
                                        std::uint64_t base_len) {
    std::string output(header_magic.begin(), header_magic.end());
    put_u32(output, header_format);
    put_u32(output, key_version);
    put_u32(output, block_size);
    put_u32(output, 0);
    put_u64(output, base_len);
    std::array<unsigned char, 16> salt{};
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1) {
        return refused<std::string>(liquid::errc::internal, "could not generate an object salt");
    }
    output.append(reinterpret_cast<const char*>(salt.data()), salt.size());
    return output;
}

void apply_header(ZettaObjectBackend::Entry& entry, std::string header, const ParsedHeader& parsed) {
    entry.header = std::move(header);
    entry.base_len = parsed.base_len;
    entry.block_size = parsed.block_size;
    entry.salt_hex = parsed.salt_hex;
}
} // namespace

ZettaObjectBackend::ZettaObjectBackend(std::shared_ptr<ZettaObjectClient> client,
                                       ZettaObjectBackendOptions options)
    : shared_(std::make_shared<Shared>()) {
    shared_->client = std::move(client);
    shared_->options = std::move(options);
    shared_->client->set_before_release(
        [weak = std::weak_ptr<Shared>(shared_)]() -> liquid::runtime::task<void> {
            return settle(weak.lock());
        },
        [weak = std::weak_ptr<Shared>(shared_)] {
            if (auto shared = weak.lock()) {
                std::vector<std::shared_ptr<ZettaObjectBackend>> views;
                std::lock_guard lock(shared->views_mutex);
                views.swap(shared->views);
            }
        });
}

ZettaObjectBackend::ZettaObjectBackend(std::shared_ptr<Shared> shared,
                                       std::optional<std::uint64_t> fence)
    : shared_(std::move(shared)), fence_(fence) {
    // A new view starts unsynced (lake#535): what the bucket holds may include writes a
    // predecessor (or this process before a reopen) sent but never made durable, and they are
    // served from now on. Its first sync therefore sends :barrier, even with nothing new.
    dirty_ = true;
}

ZettaObjectBackend::~ZettaObjectBackend() {
    // The root backend (the one lake and liquid's options hold) ends the views' lives, unless one
    // still holds writes or needs its :barrier (lake#535): liquid's close lets go of the backend
    // before it releases the owner, and the owner's release syncs them, or abandons them when the
    // lease is lost.
    // Kept only while an owner will come to settle or abandon them; otherwise the views and
    // Shared would keep each other alive.
    if (!fence_ && shared_) {
        std::vector<std::shared_ptr<ZettaObjectBackend>> views;
        {
            std::lock_guard lock(shared_->views_mutex);
            if (!shared_->client->owned() ||
                std::ranges::none_of(shared_->views, [](const auto& view) { return view->unsettled(); })) {
                views.swap(shared_->views);
            }
        }
    }
}

bool ZettaObjectBackend::unsettled() const {
    std::lock_guard lock(mutex_);
    return !queue_.empty() || dirty_;
}

liquid::runtime::task<void> ZettaObjectBackend::settle(std::shared_ptr<Shared> shared) {
    if (!shared) {
        co_return liquid::result<void>{};
    }
    std::vector<std::shared_ptr<ZettaObjectBackend>> views;
    {
        std::lock_guard lock(shared->views_mutex);
        views.swap(shared->views);
    }
    for (const auto& view : views) {
        if (shared->failed.load()) {
            break;
        }
        if (auto settled = co_await view->sync(); !settled) {
            co_return std::unexpected(settled.error());
        }
    }
    co_return liquid::result<void>{};
}

const ZettaObjectClient& ZettaObjectBackend::client() const {
    return *shared_->client;
}

liquid::result<std::shared_ptr<liquid::store_backend>>
ZettaObjectBackend::fenced(std::uint64_t fence) const {
    auto view = std::make_shared<ZettaObjectBackend>(shared_, fence);
    {
        std::lock_guard lock(shared_->views_mutex);
        shared_->views.push_back(view);
    }
    return std::shared_ptr<liquid::store_backend>(view);
}

liquid::runtime::task<void> ZettaObjectBackend::ensure_directory() {
    {
        std::lock_guard lock(mutex_);
        if (directory_loaded_) {
            co_return liquid::result<void>{};
        }
    }
    auto listed = co_await shared_->client->list("");
    if (!listed) {
        co_return std::unexpected(listed.error());
    }
    std::lock_guard lock(mutex_);
    if (directory_loaded_) {
        co_return liquid::result<void>{};
    }
    std::map<std::string, std::uint64_t> orphans;
    for (const ObjectListEntry& object : *listed) {
        // The data key and the search index's sealed objects (lake#381, written beside liquid
        // with the store's client) are not liquid's.
        if (object.key == workspace_store_key_object || object.key.starts_with("keys/") ||
            object.key.starts_with(workspace_store_search_prefix)) {
            continue;
        }
        const std::size_t marker = object.key.find(tail_marker);
        if (marker != std::string::npos) {
            orphans.emplace(object.key, object.size);
            continue;
        }
        auto& entry = directory_[object.key];
        if (!entry) {
            entry = std::make_unique<Entry>();
        }
        entry->remote = true;
        entry->ct_size = object.size;
    }
    for (const auto& [tail, size] : orphans) {
        const std::string owner = tail.substr(0, tail.find(tail_marker));
        const auto found = directory_.find(owner);
        if (found == directory_.end()) {
            stale_tails_.insert(tail);
            continue;
        }
        found->second->tails.emplace(tail, size);
    }
    directory_loaded_ = true;
    co_return liquid::result<void>{};
}

liquid::runtime::task<void> ZettaObjectBackend::ensure_cipher() {
    {
        std::lock_guard lock(shared_->cipher_mutex);
        if (shared_->cipher) {
            co_return liquid::result<void>{};
        }
    }
    // The directory decides whether a missing key may be created: only in an empty bucket.
    if (auto ready = co_await ensure_directory(); !ready) {
        co_return std::unexpected(ready.error());
    }
    const std::string& workspace_id = shared_->options.workspace_id;
    auto wrapping = WorkspaceStoreCipher::wrapping_key(shared_->options.instance_kek, workspace_id);
    if (!wrapping) {
        co_return std::unexpected(wrapping.error());
    }
    // The wrapping key is wiped on every path out of this function.
    struct Wipe {
        WorkspaceStoreCipher::Key& key;
        ~Wipe() {
            secrets_crypto::cleanse(key);
        }
    } wipe{*wrapping};
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto document = co_await shared_->client->get_all(std::string(workspace_store_key_object));
        if (document) {
            auto unwrapped = WorkspaceStoreCipher::unwrap(*wrapping, workspace_id, *document);
            if (!unwrapped) {
                co_return std::unexpected(unwrapped.error());
            }
            std::lock_guard lock(shared_->cipher_mutex);
            if (!shared_->cipher) {
                shared_->cipher = std::make_shared<const WorkspaceStoreCipher>(
                    workspace_id, unwrapped->first, unwrapped->second);
            }
            secrets_crypto::cleanse(unwrapped->second);
            co_return liquid::result<void>{};
        }
        if (document.error().code != liquid::errc::not_found) {
            co_return std::unexpected(document.error());
        }
        // A missing key is created only for a store that is being created: one the caller opened
        // to create (before its workspace is switched) whose bucket holds nothing yet. A switched
        // workspace's store, or a bucket that holds objects, without its key was wiped, recreated
        // or is the wrong bucket; serving it as an empty store would lose its repositories.
        bool empty = false;
        {
            std::lock_guard lock(mutex_);
            empty = directory_.empty() && stale_tails_.empty();
        }
        if (!shared_->options.create_key || !empty) {
            co_return std::unexpected(failure(
                liquid::errc::corrupt,
                !shared_->options.create_key
                    ? "the workspace store has no data key: its workspace was switched to it, so "
                      "the store was wiped, recreated, or this is the wrong workspace Zetta; Lake "
                      "will not serve it as an empty store"
                    : "the workspace store has no data key but holds objects: it was damaged or "
                      "this is the wrong workspace Zetta; Lake will not create a new key over it",
                std::string(workspace_store_key_object)));
        }
        auto key = secrets_crypto::random_key();
        if (!key) {
            co_return std::unexpected(failure(liquid::errc::internal, key.error().message));
        }
        auto wrapped = WorkspaceStoreCipher::wrap(*wrapping, workspace_id, 1, *key);
        secrets_crypto::cleanse(*key);
        if (!wrapped) {
            co_return std::unexpected(wrapped.error());
        }
        // Durable at once: every object after it is encrypted under it. It is the bucket's first
        // object, so no earlier write is left unsynced behind it.
        auto created = co_await shared_->client->put(std::string(workspace_store_key_object),
                                                     std::move(*wrapped), fence_, true, true);
        if (!created && created.error().code != liquid::errc::already_exists) {
            co_return std::unexpected(created.error());
        }
        // Read back what won, ours or a concurrent creator's.
    }
    co_return std::unexpected(failure(liquid::errc::internal,
                                      "the workspace store key could not be created",
                                      std::string(workspace_store_key_object)));
}

liquid::runtime::task<void> ZettaObjectBackend::ensure_header(const std::string& key) {
    std::uint64_t ct_size = 0;
    {
        std::lock_guard lock(mutex_);
        const auto found = directory_.find(key);
        if (found == directory_.end() || found->second->header || !found->second->remote) {
            co_return liquid::result<void>{};
        }
        ct_size = found->second->ct_size;
    }
    if (ct_size < header_size) {
        co_return std::unexpected(
            failure(liquid::errc::corrupt, "the object is shorter than its header", key));
    }
    // The first window holds the header and the first blocks: one read serves both.
    const std::uint64_t window =
        std::min<std::uint64_t>(ct_size, shared_->options.window_bytes);
    auto bytes = co_await shared_->client->get(key, 0, window);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    std::string header = bytes->substr(0, header_size);
    auto parsed = parse_header(header, key);
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }
    Entry probe;
    probe.remote = true;
    probe.ct_size = ct_size;
    apply_header(probe, header, *parsed);
    if (ct_size < header_size + probe.base_ct() ||
        (ct_size - header_size - probe.base_ct()) % probe.block_ct() != 0) {
        co_return std::unexpected(failure(liquid::errc::corrupt,
                                          "the object ends inside an encrypted block", key));
    }
    shared_->remember(key + "\n" + parsed->salt_hex + "\n0", std::move(*bytes));
    std::lock_guard lock(mutex_);
    const auto found = directory_.find(key);
    if (found != directory_.end() && !found->second->header && found->second->ct_size == ct_size) {
        Entry& entry = *found->second;
        apply_header(entry, std::move(header), *parsed);
        // Only the tail of the current block can be live; any other was left by an interrupted
        // flush or an older version of the object and is removed by the next sync.
        const std::string current = entry.tail_key(key, entry.append_blocks());
        for (auto it = entry.tails.begin(); it != entry.tails.end();) {
            if (it->first != current) {
                stale_tails_.insert(it->first);
                it = entry.tails.erase(it);
            } else {
                ++it;
            }
        }
    }
    co_return liquid::result<void>{};
}

liquid::runtime::task<std::string>
ZettaObjectBackend::read_main(const std::string& key, std::uint64_t offset, std::uint64_t size) {
    std::string salt;
    std::uint64_t ct_size = 0;
    {
        std::lock_guard lock(mutex_);
        const auto found = directory_.find(key);
        if (found == directory_.end() || !found->second->remote) {
            co_return std::unexpected(failure(liquid::errc::not_found, "no such object", key));
        }
        salt = found->second->salt_hex;
        ct_size = found->second->ct_size;
    }
    if (offset + size > ct_size) {
        co_return std::unexpected(
            failure(liquid::errc::corrupt, "an encrypted range is outside the object", key));
    }
    const std::uint64_t window = shared_->options.window_bytes;
    std::string output;
    output.reserve(size);
    for (std::uint64_t index = offset / window; index * window < offset + size; ++index) {
        const std::uint64_t start = index * window;
        const std::uint64_t end = std::min(start + window, ct_size);
        const std::uint64_t needed = std::min(offset + size, end);
        const std::uint64_t from = std::max(offset, start) - start;
        const std::string name = key + "\n" + salt + "\n" + std::to_string(index);
        if (auto hit = shared_->slice(name, from, needed - start)) {
            shared_->client->counters()->window_hits.fetch_add(1);
            output += *hit;
            continue;
        }
        shared_->client->counters()->window_misses.fetch_add(1);
        auto bytes = co_await shared_->client->get(key, start, end - start);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        output.append(*bytes, from, needed - start - from);
        shared_->remember(name, std::move(*bytes));
    }
    co_return output;
}

liquid::runtime::task<void> ZettaObjectBackend::ensure_open_region(const std::string& key) {
    std::string tail;
    std::uint64_t tail_size = 0;
    std::uint64_t block = 0;
    std::string header;
    {
        std::lock_guard lock(mutex_);
        const auto found = directory_.find(key);
        if (found == directory_.end() || found->second->open_loaded) {
            co_return liquid::result<void>{};
        }
        Entry& entry = *found->second;
        if (!entry.remote || !entry.header) {
            entry.open_loaded = true;
            co_return liquid::result<void>{};
        }
        block = entry.append_blocks();
        tail = entry.tail_key(key, block);
        const auto present = entry.tails.find(tail);
        if (present == entry.tails.end()) {
            entry.open_loaded = true;
            entry.open.clear();
            entry.open_remote = 0;
            co_return liquid::result<void>{};
        }
        tail_size = present->second;
        header = *entry.header;
    }
    auto sealed = co_await shared_->client->get(tail, 0, tail_size);
    if (!sealed) {
        co_return std::unexpected(sealed.error());
    }
    std::shared_ptr<const WorkspaceStoreCipher> cipher;
    {
        std::lock_guard lock(shared_->cipher_mutex);
        cipher = shared_->cipher;
    }
    auto plain = cipher->open(block_associated(shared_->options.workspace_id, key, header, 't',
                                               block, tail_size - WorkspaceStoreCipher::overhead),
                              *sealed);
    if (!plain) {
        co_return std::unexpected(failure(plain.error().code, plain.error().message, tail));
    }
    std::lock_guard lock(mutex_);
    const auto found = directory_.find(key);
    if (found != directory_.end() && !found->second->open_loaded &&
        found->second->append_blocks() == block) {
        found->second->open = std::move(*plain);
        found->second->open_remote = found->second->open.size();
        found->second->open_loaded = true;
    }
    co_return liquid::result<void>{};
}

liquid::runtime::task<std::vector<std::byte>>
ZettaObjectBackend::read_plain(const std::string& key, std::uint64_t offset, std::uint64_t size) {
    std::shared_ptr<const WorkspaceStoreCipher> cipher;
    {
        std::lock_guard lock(shared_->cipher_mutex);
        cipher = shared_->cipher;
    }
    const std::string& workspace_id = shared_->options.workspace_id;
    // A flush can move bytes from the open region into complete blocks between the snapshot and
    // the read; the read then starts again from a new snapshot.
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::uint64_t base_len = 0;
        std::uint64_t block_size = 0;
        std::uint64_t base_ct = 0;
        std::uint64_t blocks = 0;
        std::string header;
        {
            std::lock_guard lock(mutex_);
            const auto found = directory_.find(key);
            if (found == directory_.end()) {
                co_return std::unexpected(failure(liquid::errc::not_found, "no such object", key));
            }
            const Entry& entry = *found->second;
            base_len = entry.base_len;
            block_size = entry.block_size;
            base_ct = entry.base_ct();
            blocks = entry.append_blocks();
            header = entry.header.value_or(std::string());
        }
        std::vector<std::byte> output(size);
        const std::uint64_t end = offset + size;
        std::uint64_t position = offset;
        const std::uint64_t block_ct = block_size + WorkspaceStoreCipher::overhead;
        // The bytes K was created with.
        if (position < end && position < base_len) {
            const std::uint64_t stop = std::min(end, base_len);
            const std::uint64_t first = position / block_size;
            const std::uint64_t last = (stop - 1) / block_size;
            const std::uint64_t last_len = std::min(block_size, base_len - last * block_size);
            const std::uint64_t from = header_size + first * block_ct;
            const std::uint64_t to = header_size + last * block_ct + last_len + WorkspaceStoreCipher::overhead;
            auto sealed = co_await read_main(key, from, to - from);
            if (!sealed) {
                co_return std::unexpected(sealed.error());
            }
            for (std::uint64_t index = first; index <= last; ++index) {
                const std::uint64_t length = std::min(block_size, base_len - index * block_size);
                auto plain = cipher->open(
                    block_associated(workspace_id, key, header, 'b', index, length),
                    std::string_view(*sealed).substr((index - first) * block_ct,
                                                     length + WorkspaceStoreCipher::overhead));
                if (!plain) {
                    co_return std::unexpected(failure(plain.error().code, plain.error().message, key));
                }
                const std::uint64_t block_start = index * block_size;
                const std::uint64_t copy_from = std::max(position, block_start);
                const std::uint64_t copy_to = std::min(stop, block_start + length);
                std::copy_n(plain->begin() + static_cast<std::ptrdiff_t>(copy_from - block_start),
                            copy_to - copy_from,
                            output.begin() + static_cast<std::ptrdiff_t>(copy_from - offset));
            }
            position = stop;
        }
        // Complete appended blocks.
        const std::uint64_t appended_end = base_len + blocks * block_size;
        if (position < end && position < appended_end) {
            const std::uint64_t stop = std::min(end, appended_end);
            const std::uint64_t first = (position - base_len) / block_size;
            const std::uint64_t last = (stop - 1 - base_len) / block_size;
            const std::uint64_t from = header_size + base_ct + first * block_ct;
            auto sealed = co_await read_main(key, from, (last - first + 1) * block_ct);
            if (!sealed) {
                co_return std::unexpected(sealed.error());
            }
            for (std::uint64_t index = first; index <= last; ++index) {
                auto plain = cipher->open(
                    block_associated(workspace_id, key, header, 'a', index, block_size),
                    std::string_view(*sealed).substr((index - first) * block_ct, block_ct));
                if (!plain) {
                    co_return std::unexpected(failure(plain.error().code, plain.error().message, key));
                }
                const std::uint64_t block_start = base_len + index * block_size;
                const std::uint64_t copy_from = std::max(position, block_start);
                const std::uint64_t copy_to = std::min(stop, block_start + block_size);
                std::copy_n(plain->begin() + static_cast<std::ptrdiff_t>(copy_from - block_start),
                            copy_to - copy_from,
                            output.begin() + static_cast<std::ptrdiff_t>(copy_from - offset));
            }
            position = stop;
        }
        if (position >= end) {
            co_return output;
        }
        // The open region: the current tail and buffered appends.
        auto opened = co_await ensure_open_region(key);
        if (!opened) {
            co_return std::unexpected(opened.error());
        }
        std::lock_guard lock(mutex_);
        const auto found = directory_.find(key);
        if (found == directory_.end()) {
            co_return std::unexpected(failure(liquid::errc::not_found, "no such object", key));
        }
        const Entry& entry = *found->second;
        if (entry.append_blocks() != blocks || entry.base_len != base_len) {
            continue;
        }
        if (end - appended_end > entry.open.size()) {
            co_return std::unexpected(
                failure(liquid::errc::corrupt, "a range is outside the object", key));
        }
        std::copy_n(entry.open.begin() + static_cast<std::ptrdiff_t>(position - appended_end),
                    end - position,
                    output.begin() + static_cast<std::ptrdiff_t>(position - offset));
        co_return output;
    }
    co_return std::unexpected(
        failure(liquid::errc::busy, "the object kept changing while it was read", key));
}

// The writes that bring one object up to date (lake#527). When sent whole, an object this view has
// not sent yet (a put held until now, or one created by appends) is one PUT of its header and base
// blocks, so it never needs a tail; an object in Zetta gets its complete blocks as PATCHes at its
// exact size and its partial block as a tail.
struct ZettaObjectBackend::FlushPlan {
    std::string key;
    std::string salt;
    // The header the entry had when the plan was made; the plan applies only to that version.
    std::string original_header;
    // One PUT of the whole object: `pieces[0]` is the header and base blocks.
    bool whole = false;
    std::string whole_header;
    std::uint64_t whole_plain = 0;
    // The first piece is a PUT (the object is not in Zetta yet), the rest PATCHes.
    bool create = false;
    std::vector<std::string> pieces;
    std::uint64_t first_offset = 0;
    std::uint64_t full = 0;
    std::uint64_t remainder = 0;
    std::optional<std::pair<std::string, std::string>> tail;
};

liquid::result<std::optional<ZettaObjectBackend::FlushPlan>>
ZettaObjectBackend::plan_flush(const std::string& key, bool barrier) {
    using Output = std::optional<FlushPlan>;
    std::shared_ptr<const WorkspaceStoreCipher> cipher;
    {
        std::lock_guard lock(shared_->cipher_mutex);
        cipher = shared_->cipher;
    }
    std::lock_guard lock(mutex_);
    const auto found = directory_.find(key);
    if (found == directory_.end()) {
        return Output{};
    }
    Entry& entry = *found->second;
    if (!entry.open_loaded || !entry.header) {
        return Output{};
    }
    const std::uint64_t block_size = entry.block_size;
    FlushPlan plan;
    plan.key = key;
    plan.original_header = *entry.header;
    plan.full = entry.open.size() / block_size;
    plan.remainder = entry.open.size() - plan.full * block_size;
    plan.create = !entry.remote;
    if (!barrier && plan.full * block_size < shared_->options.flush_bytes) {
        return Output{};
    }
    if (barrier && plan.create) {
        auto header = make_header(cipher->key_version(), static_cast<std::uint32_t>(block_size),
                                  entry.open.size());
        if (!header) {
            return std::unexpected(header.error());
        }
        auto parsed = parse_header(*header, key);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        std::string object = *header;
        for (std::uint64_t index = 0; index * block_size < entry.open.size(); ++index) {
            const std::uint64_t length =
                std::min<std::uint64_t>(block_size, entry.open.size() - index * block_size);
            auto sealed = cipher->seal(
                block_associated(shared_->options.workspace_id, key, *header, 'b', index, length),
                std::span<const std::byte>(entry.open.data() + index * block_size, length));
            if (!sealed) {
                return std::unexpected(sealed.error());
            }
            object += *sealed;
        }
        plan.whole = true;
        plan.whole_header = std::move(*header);
        plan.whole_plain = entry.open.size();
        plan.salt = parsed->salt_hex;
        plan.pieces.push_back(std::move(object));
        return Output{std::move(plan)};
    }
    if (!plan.create && plan.full == 0 && (!barrier || plan.remainder == entry.open_remote)) {
        return Output{};
    }
    const std::uint64_t blocks = entry.append_blocks();
    std::string payload = plan.create ? *entry.header : std::string();
    plan.first_offset = plan.create ? 0 : entry.ct_size;
    for (std::uint64_t index = 0; index < plan.full; ++index) {
        auto sealed = cipher->seal(
            block_associated(shared_->options.workspace_id, key, *entry.header, 'a', blocks + index,
                             block_size),
            std::span<const std::byte>(entry.open.data() + index * block_size, block_size));
        if (!sealed) {
            return std::unexpected(sealed.error());
        }
        if (!payload.empty() && payload.size() + sealed->size() > zetta_slice_bytes) {
            plan.pieces.push_back(std::move(payload));
            payload.clear();
        }
        payload += *sealed;
    }
    if (!payload.empty()) {
        plan.pieces.push_back(std::move(payload));
    }
    if (barrier && plan.remainder > 0 && (plan.full > 0 || plan.remainder != entry.open_remote)) {
        auto sealed = cipher->seal(
            block_associated(shared_->options.workspace_id, key, *entry.header, 't',
                             blocks + plan.full, plan.remainder),
            std::span<const std::byte>(entry.open.data() + plan.full * block_size, plan.remainder));
        if (!sealed) {
            return std::unexpected(sealed.error());
        }
        plan.tail.emplace(entry.tail_key(key, blocks + plan.full), std::move(*sealed));
    }
    plan.salt = entry.salt_hex;
    return Output{std::move(plan)};
}

liquid::runtime::task<void> ZettaObjectBackend::send_plan(FlushPlan plan) {
    const std::string& key = plan.key;
    if (plan.whole) {
        const std::uint64_t size = plan.pieces.front().size();
        // Liquid reads back what it just wrote (an index run is loaded right after it is
        // written): keep a copy for the window cache when it fits comfortably.
        std::string kept;
        if (size <= shared_->options.cache_bytes / 4) {
            kept = plan.pieces.front();
        }
        auto written =
            co_await shared_->client->put(key, std::move(plan.pieces.front()), fence_, false);
        if (!written) {
            co_return std::unexpected(written.error());
        }
        shared_->forget(key);
        if (!kept.empty()) {
            shared_->extend(key, plan.salt, 0, kept);
        }
        auto parsed = parse_header(plan.whole_header, key);
        std::lock_guard lock(mutex_);
        dirty_ = true;
        const auto found = directory_.find(key);
        if (found == directory_.end() || found->second->remote ||
            found->second->header != plan.original_header ||
            found->second->open.size() < plan.whole_plain) {
            // Liquid serialises its mutations; a version that changed while it was sent means this
            // view's directory can no longer be trusted.
            shared_->failed.store(true);
            co_return std::unexpected(
                failure(liquid::errc::conflict, "the object changed while it was written", key));
        }
        Entry& entry = *found->second;
        entry.remote = true;
        entry.replaces_remote = false;
        entry.ct_size = size;
        release_plain(entry);
        if (plan.whole_plain <= sent_plain_bytes &&
            sent_plain_held_ + plan.whole_plain <= sent_plain_limit) {
            entry.sent_plain.emplace(entry.open.begin(),
                                        entry.open.begin() + static_cast<std::ptrdiff_t>(plan.whole_plain));
            sent_plain_held_ += plan.whole_plain;
        }
        apply_header(entry, std::move(plan.whole_header), *parsed);
        entry.open.erase(entry.open.begin(),
                         entry.open.begin() + static_cast<std::ptrdiff_t>(plan.whole_plain));
        entry.open_remote = 0;
        co_return liquid::result<void>{};
    }
    std::uint64_t offset = plan.first_offset;
    bool first = true;
    for (std::string& piece : plan.pieces) {
        const std::uint64_t size = piece.size();
        // The first piece of an object not in Zetta replaces whatever the bucket holds under its
        // key (an older version liquid removed or replaced since it was last sent).
        auto written = plan.create && first
                           ? co_await shared_->client->put(key, piece, fence_, false)
                           : co_await shared_->client->append(key, offset, piece, fence_);
        if (!written) {
            co_return std::unexpected(written.error());
        }
        if (plan.create && first) {
            shared_->forget(key);
        }
        first = false;
        shared_->extend(key, plan.salt, offset, piece);
        offset += size;
        std::lock_guard lock(mutex_);
        dirty_ = true;
        // Record each piece as it lands, so a failure part way leaves the directory exact.
        const auto found = directory_.find(key);
        if (found != directory_.end()) {
            Entry& entry = *found->second;
            const std::uint64_t had = entry.remote ? entry.append_blocks() : 0;
            entry.remote = true;
            entry.replaces_remote = false;
            entry.ct_size = offset;
            const std::uint64_t gained = entry.append_blocks() - had;
            entry.open.erase(entry.open.begin(),
                             entry.open.begin() + static_cast<std::ptrdiff_t>(gained * entry.block_size));
            if (gained > 0) {
                entry.open_remote = 0;
            }
        }
    }
    if (plan.tail) {
        const std::uint64_t size = plan.tail->second.size();
        auto written =
            co_await shared_->client->put(plan.tail->first, std::move(plan.tail->second), fence_, false);
        if (!written) {
            co_return std::unexpected(written.error());
        }
        std::lock_guard lock(mutex_);
        dirty_ = true;
        const auto found = directory_.find(key);
        if (found != directory_.end()) {
            found->second->tails[plan.tail->first] = size;
            found->second->open_remote = plan.remainder;
        }
    }
    // Every tail but the current one is stale once what replaced it is durable.
    std::lock_guard lock(mutex_);
    const auto found = directory_.find(key);
    if (found != directory_.end()) {
        Entry& entry = *found->second;
        const std::string current = entry.tail_key(key, entry.append_blocks());
        for (auto it = entry.tails.begin(); it != entry.tails.end();) {
            if (it->first != current || entry.open_remote == 0) {
                retiring_tails_.insert(it->first);
                it = entry.tails.erase(it);
            } else {
                ++it;
            }
        }
    }
    co_return liquid::result<void>{};
}

// One sender at a time (lake#527): every mutation this view sends goes out in the order liquid
// issued it, each answered before the next is sent, so the bucket's log holds a prefix of liquid's
// issue order and a crash keeps such a prefix (Zetta truncates its log at the first torn frame).
struct ZettaObjectBackend::SendGuard {
    ZettaObjectBackend& self;
    explicit SendGuard(ZettaObjectBackend& backend) : self(backend) {
        std::unique_lock lock(self.mutex_);
        self.flushed_.wait(lock, [&] { return !self.sending_; });
        self.sending_ = true;
    }
    ~SendGuard() {
        {
            std::lock_guard lock(self.mutex_);
            self.sending_ = false;
        }
        self.flushed_.notify_all();
    }
    SendGuard(const SendGuard&) = delete;
    SendGuard& operator=(const SendGuard&) = delete;
};

void ZettaObjectBackend::release_plain(Entry& entry) {
    if (entry.sent_plain) {
        sent_plain_held_ -= entry.sent_plain->size();
        entry.sent_plain.reset();
    }
}

// Sends every held write in issue order, without durability (the caller holds the SendGuard);
// the next sync's :barrier makes them durable (lake#535).
liquid::runtime::task<void> ZettaObjectBackend::drain() {
    std::vector<std::string> queued;
    {
        std::lock_guard lock(mutex_);
        queued = queue_;
    }
    for (const std::string& key : queued) {
        auto plan = plan_flush(key, true);
        if (!plan) {
            co_return std::unexpected(plan.error());
        }
        if (*plan) {
            if (auto sent = co_await send_plan(std::move(**plan)); !sent) {
                co_return std::unexpected(sent.error());
            }
        }
        std::lock_guard lock(mutex_);
        unhold(key);
    }
    co_return liquid::result<void>{};
}

// Before a write about `key` is held: when other keys were held after `key`'s last held write,
// sending this one with them would put it before them in the bucket, so they are sent first.
// Also sends everything when the held writes passed their bounds. The caller holds the guard.
liquid::runtime::task<void> ZettaObjectBackend::keep_order_before_holding(const std::string& key) {
    bool reorder = false;
    bool over = false;
    {
        std::lock_guard lock(mutex_);
        const auto found = std::find(queue_.begin(), queue_.end(), key);
        reorder = found != queue_.end() && std::next(found) != queue_.end();
        over = held_bytes_ > shared_->options.held_bytes_limit ||
               held_puts_ >= shared_->options.held_puts_limit;
    }
    if (over || reorder) {
        // Bounded memory, or the order kept: what is held is sent now.
        co_return co_await drain();
    }
    co_return liquid::result<void>{};
}

void ZettaObjectBackend::hold(const std::string& key, std::uint64_t bytes, bool put) {
    if (queue_.empty() || queue_.back() != key) {
        queue_.push_back(key);
    }
    auto& [key_bytes, key_puts] = held_by_key_[key];
    if (put) {
        // A put replaces whatever was held for the key: its bytes and its put.
        held_bytes_ -= key_bytes;
        held_puts_ -= key_puts;
        key_bytes = 0;
        key_puts = 1;
        ++held_puts_;
    }
    key_bytes += bytes;
    held_bytes_ += bytes;
}

void ZettaObjectBackend::unhold(const std::string& key) {
    std::erase(queue_, key);
    if (const auto found = held_by_key_.find(key); found != held_by_key_.end()) {
        held_bytes_ -= found->second.first;
        held_puts_ -= found->second.second;
        held_by_key_.erase(found);
    }
}

std::pair<std::uint64_t, std::size_t> ZettaObjectBackend::held() const {
    std::lock_guard lock(mutex_);
    return {held_bytes_, held_puts_};
}

// A threshold flush of buffered appends: the key is the last held key (keep_order_before_holding),
// so the keys held before it are sent first.
liquid::runtime::task<void> ZettaObjectBackend::flush_entry(const std::string& key) {
    SendGuard guard(*this);
    std::vector<std::string> before;
    {
        std::lock_guard lock(mutex_);
        for (const std::string& queued : queue_) {
            if (queued == key) {
                break;
            }
            before.push_back(queued);
        }
    }
    for (const std::string& queued : before) {
        auto plan = plan_flush(queued, true);
        if (!plan) {
            co_return std::unexpected(plan.error());
        }
        if (*plan) {
            if (auto sent = co_await send_plan(std::move(**plan)); !sent) {
                co_return std::unexpected(sent.error());
            }
        }
        std::lock_guard lock(mutex_);
        unhold(queued);
    }
    // An object not in Zetta yet (a held put, appended to) goes whole, as one PUT, so a put is
    // never seen in part; one in Zetta gets its complete blocks.
    bool whole = false;
    {
        std::lock_guard lock(mutex_);
        const auto found = directory_.find(key);
        whole = found != directory_.end() && !found->second->remote;
    }
    auto plan = plan_flush(key, whole);
    if (!plan) {
        co_return std::unexpected(plan.error());
    }
    if (*plan) {
        if (auto sent = co_await send_plan(std::move(**plan)); !sent) {
            co_return std::unexpected(sent.error());
        }
    }
    std::lock_guard lock(mutex_);
    if (whole) {
        unhold(key);
    } else if (const auto found = directory_.find(key); found != directory_.end()) {
        // What stays held of the key: the bytes after its last complete block.
        const std::uint64_t still = found->second->open.size() - found->second->open_remote;
        auto& [key_bytes, key_puts] = held_by_key_[key];
        held_bytes_ = held_bytes_ - key_bytes + still;
        key_bytes = still;
    }
    co_return liquid::result<void>{};
}

namespace {

// Up to `width` jobs in flight at once on the caller's executor. After the first failure no job
// starts; the answer is the first failure, `locked` (the lease passed on) before any other.
struct JobRun {
    std::vector<std::function<liquid::runtime::task<void>()>> jobs;
    std::atomic<std::size_t> next{0};
    std::atomic<bool> stopped{false};
    std::mutex mutex;
    std::optional<liquid::error> error;
};

liquid::runtime::task<void> run_jobs_worker(std::shared_ptr<JobRun> run) {
    for (;;) {
        if (run->stopped.load()) {
            co_return liquid::result<void>{};
        }
        const std::size_t index = run->next.fetch_add(1);
        if (index >= run->jobs.size()) {
            co_return liquid::result<void>{};
        }
        auto done = co_await run->jobs[index]();
        if (!done) {
            run->stopped.store(true);
            std::lock_guard lock(run->mutex);
            if (!run->error || (done.error().code == liquid::errc::locked &&
                                run->error->code != liquid::errc::locked)) {
                run->error = done.error();
            }
        }
    }
}

} // namespace

liquid::runtime::task<void>
run_object_requests(std::vector<std::function<liquid::runtime::task<void>()>> jobs, std::size_t width) {
    if (jobs.empty()) {
        co_return liquid::result<void>{};
    }
    if (jobs.size() == 1) {
        co_return co_await jobs.front()();
    }
    auto run = std::make_shared<JobRun>();
    run->jobs = std::move(jobs);
    std::vector<boost::capy::io_task<liquid::result<void>>> workers;
    const std::size_t count = std::min(std::max<std::size_t>(width, 1), run->jobs.size());
    for (std::size_t index = 0; index < count; ++index) {
        workers.push_back(liquid::runtime::as_io(run_jobs_worker(run)));
    }
    auto [wait_error, results] = co_await liquid::runtime::when_all(std::move(workers));
    if (wait_error) {
        co_return std::unexpected(
            failure(liquid::errc::internal, wait_error.message(), "workspace store barrier"));
    }
    if (run->error) {
        co_return std::unexpected(*run->error);
    }
    for (auto& result : results) {
        if (!result) {
            co_return std::unexpected(result.error());
        }
    }
    co_return liquid::result<void>{};
}

liquid::runtime::task<void> ZettaObjectBackend::remove_stale_tail(std::string tail) {
    auto removed = co_await shared_->client->remove(tail, fence_, false);
    if (!removed && removed.error().code != liquid::errc::not_found) {
        if (removed.error().code == liquid::errc::locked) {
            co_return std::unexpected(removed.error());
        }
        // Ignored by every reader; the next sync tries again.
        std::lock_guard lock(mutex_);
        stale_tails_.insert(tail);
    }
    co_return liquid::result<void>{};
}

liquid::runtime::task<void> ZettaObjectBackend::put_impl(std::string key, std::vector<std::byte> bytes) {
    if (auto checked = check_liquid_key(key); !checked) {
        co_return std::unexpected(checked.error());
    }
    if (auto ready = co_await ensure_directory(); !ready) {
        co_return std::unexpected(ready.error());
    }
    if (auto ready = co_await ensure_cipher(); !ready) {
        co_return std::unexpected(ready.error());
    }
    std::shared_ptr<const WorkspaceStoreCipher> cipher;
    {
        std::lock_guard lock(shared_->cipher_mutex);
        cipher = shared_->cipher;
    }
    const auto block_size = static_cast<std::uint32_t>(shared_->options.block_size);
    SendGuard guard(*this);
    if (bytes.size() <= shared_->options.deferred_put_bytes) {
        {
            std::lock_guard lock(mutex_);
            const auto found = directory_.find(key);
            // The bucket already holds exactly these bytes (liquid publishes the index manifest
            // twice in a row, each between barriers): nothing to write.
            if (found != directory_.end() && found->second->remote && found->second->sent_plain &&
                found->second->open.empty() && found->second->open_remote == 0 &&
                *found->second->sent_plain == bytes) {
                co_return liquid::result<void>{};
            }
        }
        if (auto ordered = co_await keep_order_before_holding(key); !ordered) {
            co_return std::unexpected(ordered.error());
        }
        // Held until it must be sent (a later write about another key held before it, a write sent
        // at once, a bound, or the sync), as one PUT in its place among the held writes (lake#527). Until then reads are answered from memory; the bucket keeps the
        // previous version, which the PUT replaces atomically.
        auto header = make_header(cipher->key_version(), block_size, 0);
        if (!header) {
            co_return std::unexpected(header.error());
        }
        auto parsed = parse_header(*header, key);
        shared_->forget(key);
        std::lock_guard lock(mutex_);
        auto& entry = directory_[key];
        bool replaces = false;
        if (entry) {
            replaces = entry->remote || entry->replaces_remote;
            release_plain(*entry);
            for (const auto& [tail, ignored] : entry->tails) {
                retiring_tails_.insert(tail);
            }
        }
        const std::uint64_t size = bytes.size();
        entry = std::make_unique<Entry>();
        apply_header(*entry, std::move(*header), *parsed);
        entry->replaces_remote = replaces;
        entry->open = std::move(bytes);
        entry->open_loaded = true;
        hold(key, size, true);
        co_return liquid::result<void>{};
    }
    // A large put is sent at once, after the writes held before it.
    if (auto drained = co_await drain(); !drained) {
        co_return std::unexpected(drained.error());
    }
    auto header = make_header(cipher->key_version(), block_size, bytes.size());
    if (!header) {
        co_return std::unexpected(header.error());
    }
    std::string object = *header;
    for (std::uint64_t index = 0; index * block_size < bytes.size(); ++index) {
        const std::uint64_t length = std::min<std::uint64_t>(block_size, bytes.size() - index * block_size);
        auto sealed = cipher->seal(
            block_associated(shared_->options.workspace_id, key, *header, 'b', index, length),
            std::span<const std::byte>(bytes.data() + index * block_size, length));
        if (!sealed) {
            co_return std::unexpected(sealed.error());
        }
        object += *sealed;
    }
    const std::uint64_t size = object.size();
    // Liquid reads back what it just put (an index run is loaded right after it is written):
    // keep a copy for the window cache when it fits comfortably.
    std::string kept;
    if (size <= shared_->options.cache_bytes / 4) {
        kept = object;
    }
    auto written = co_await shared_->client->put(key, std::move(object), fence_, false);
    if (!written) {
        co_return std::unexpected(written.error());
    }
    auto parsed = parse_header(*header, key);
    shared_->forget(key);
    if (!kept.empty()) {
        shared_->extend(key, parsed->salt_hex, 0, kept);
    }
    std::lock_guard lock(mutex_);
    auto& entry = directory_[key];
    if (entry) {
        release_plain(*entry);
        // The replaced version's tails go once the PUT is durable (after the next sync).
        for (const auto& [tail, ignored] : entry->tails) {
            retiring_tails_.insert(tail);
        }
    }
    entry = std::make_unique<Entry>();
    entry->remote = true;
    entry->ct_size = size;
    apply_header(*entry, std::move(*header), *parsed);
    entry->open_loaded = true;
    dirty_ = true;
    co_return liquid::result<void>{};
}

liquid::runtime::task<void> ZettaObjectBackend::put_if_absent_impl(std::string key,
                                                                   std::vector<std::byte> bytes) {
    if (auto checked = check_liquid_key(key); !checked) {
        co_return std::unexpected(checked.error());
    }
    if (auto ready = co_await ensure_directory(); !ready) {
        co_return std::unexpected(ready.error());
    }
    if (auto ready = co_await ensure_cipher(); !ready) {
        co_return std::unexpected(ready.error());
    }
    {
        std::lock_guard lock(mutex_);
        if (directory_.contains(key)) {
            co_return std::unexpected(
                failure(liquid::errc::already_exists, "the object already exists", key));
        }
    }
    // Sent at once, after the writes held before it (lake#527): its answer decides the create.
    SendGuard guard(*this);
    if (auto drained = co_await drain(); !drained) {
        co_return std::unexpected(drained.error());
    }
    std::shared_ptr<const WorkspaceStoreCipher> cipher;
    {
        std::lock_guard lock(shared_->cipher_mutex);
        cipher = shared_->cipher;
    }
    const auto block_size = static_cast<std::uint32_t>(shared_->options.block_size);
    auto header = make_header(cipher->key_version(), block_size, bytes.size());
    if (!header) {
        co_return std::unexpected(header.error());
    }
    std::string object = *header;
    for (std::uint64_t index = 0; index * block_size < bytes.size(); ++index) {
        const std::uint64_t length = std::min<std::uint64_t>(block_size, bytes.size() - index * block_size);
        auto sealed = cipher->seal(
            block_associated(shared_->options.workspace_id, key, *header, 'b', index, length),
            std::span<const std::byte>(bytes.data() + index * block_size, length));
        if (!sealed) {
            co_return std::unexpected(sealed.error());
        }
        object += *sealed;
    }
    const std::uint64_t size = object.size();
    auto written = co_await shared_->client->put(key, std::move(object), fence_, true);
    if (!written) {
        if (written.error().code == liquid::errc::already_exists) {
            // The directory said the key was absent: another writer exists, which the fence should
            // have excluded, so nothing this backend knows can be trusted. The directory and any
            // buffered appends are kept as they are; the store must be closed and reopened.
            shared_->failed.store(true);
        }
        co_return std::unexpected(written.error());
    }
    auto parsed = parse_header(*header, key);
    std::lock_guard lock(mutex_);
    auto& entry = directory_[key];
    entry = std::make_unique<Entry>();
    entry->remote = true;
    entry->ct_size = size;
    apply_header(*entry, std::move(*header), *parsed);
    entry->open_loaded = true;
    dirty_ = true;
    co_return liquid::result<void>{};
}

liquid::runtime::task<void>
ZettaObjectBackend::put_range_impl(std::string key, std::uint64_t offset, std::vector<std::byte> bytes) {
    if (auto checked = check_liquid_key(key); !checked) {
        co_return std::unexpected(checked.error());
    }
    if (auto ready = co_await ensure_directory(); !ready) {
        co_return std::unexpected(ready.error());
    }
    if (auto ready = co_await ensure_cipher(); !ready) {
        co_return std::unexpected(ready.error());
    }
    {
        std::lock_guard lock(mutex_);
        if (!directory_.contains(key)) {
            if (offset != 0) {
                co_return std::unexpected(failure(
                    liquid::errc::conflict, "an append to a missing object must start at 0", key));
            }
            std::shared_ptr<const WorkspaceStoreCipher> cipher;
            {
                std::lock_guard cipher_lock(shared_->cipher_mutex);
                cipher = shared_->cipher;
            }
            auto header = make_header(cipher->key_version(),
                                      static_cast<std::uint32_t>(shared_->options.block_size), 0);
            if (!header) {
                co_return std::unexpected(header.error());
            }
            auto parsed = parse_header(*header, key);
            auto entry = std::make_unique<Entry>();
            apply_header(*entry, std::move(*header), *parsed);
            entry->open_loaded = true;
            directory_.emplace(key, std::move(entry));
        }
    }
    if (auto ready = co_await ensure_header(key); !ready) {
        co_return std::unexpected(ready.error());
    }
    if (auto ready = co_await ensure_open_region(key); !ready) {
        co_return std::unexpected(ready.error());
    }
    std::uint64_t size = 0;
    {
        std::lock_guard lock(mutex_);
        size = directory_.at(key)->size(key);
    }
    if (offset > size) {
        co_return std::unexpected(failure(
            liquid::errc::conflict,
            std::format("an append at {} is past the object's end at {}", offset, size), key));
    }
    const std::uint64_t overlap = std::min<std::uint64_t>(size - offset, bytes.size());
    if (overlap > 0) {
        auto existing = co_await read_plain(key, offset, overlap);
        if (!existing) {
            co_return std::unexpected(existing.error());
        }
        if (!std::equal(existing->begin(), existing->end(), bytes.begin())) {
            co_return std::unexpected(failure(
                liquid::errc::conflict,
                std::format("the workspace store on Zetta refuses to rewrite {} bytes of {} at "
                            "offset {} with other bytes: Zetta objects only grow (liquid writes "
                            "each index run once since liquid#332)",
                            overlap, key, offset),
                key));
        }
    }
    {
        // A retry of bytes already there changes nothing and holds nothing (an empty append that
        // creates the object is held, as its creation).
        std::lock_guard lock(mutex_);
        const auto found = directory_.find(key);
        if (overlap == bytes.size() && found != directory_.end() &&
            (found->second->remote || std::ranges::find(queue_, key) != queue_.end())) {
            co_return liquid::result<void>{};
        }
    }
    bool flush = false;
    {
        SendGuard guard(*this);
        if (auto ordered = co_await keep_order_before_holding(key); !ordered) {
            co_return std::unexpected(ordered.error());
        }
        std::lock_guard lock(mutex_);
        const auto found = directory_.find(key);
        if (found == directory_.end()) {
            co_return std::unexpected(failure(liquid::errc::not_found, "no such object", key));
        }
        Entry& entry = *found->second;
        release_plain(entry);
        entry.open.insert(entry.open.end(), bytes.begin() + static_cast<std::ptrdiff_t>(overlap),
                          bytes.end());
        hold(key, bytes.size() - overlap, false);
        flush = (entry.open.size() / entry.block_size) * entry.block_size >= shared_->options.flush_bytes;
    }
    if (flush) {
        co_return co_await flush_entry(key);
    }
    co_return liquid::result<void>{};
}

liquid::runtime::task<std::vector<std::byte>>
ZettaObjectBackend::get_range(std::string key, std::uint64_t offset, std::uint64_t size) {
    if (auto checked = check_liquid_key(key); !checked) {
        co_return std::unexpected(checked.error());
    }
    if (auto ready = co_await ensure_directory(); !ready) {
        co_return std::unexpected(ready.error());
    }
    {
        std::lock_guard lock(mutex_);
        if (!directory_.contains(key)) {
            co_return std::unexpected(failure(liquid::errc::not_found, "no such object", key));
        }
    }
    if (auto ready = co_await ensure_cipher(); !ready) {
        co_return std::unexpected(ready.error());
    }
    if (auto ready = co_await ensure_header(key); !ready) {
        co_return std::unexpected(ready.error());
    }
    std::uint64_t object_size = 0;
    {
        std::lock_guard lock(mutex_);
        const auto found = directory_.find(key);
        if (found == directory_.end()) {
            co_return std::unexpected(failure(liquid::errc::not_found, "no such object", key));
        }
        object_size = found->second->size(key);
    }
    if (offset > object_size || size > object_size - offset) {
        co_return std::unexpected(failure(
            liquid::errc::corrupt,
            std::format("range {}+{} is outside the object of {} bytes", offset, size, object_size),
            key));
    }
    if (size == 0) {
        co_return std::vector<std::byte>{};
    }
    co_return co_await read_plain(key, offset, size);
}

// Under mutex_: `key` is held and at most one held key follows it, one this view sends whole as
// a single PUT (not in Zetta yet), so the key's held write and its removal can both be dropped.
bool ZettaObjectBackend::removable_unsent(const std::string& key) const {
    const auto position = std::ranges::find(queue_, key);
    if (position == queue_.end()) {
        return false;
    }
    const auto after = std::distance(position, queue_.end()) - 1;
    if (after == 0) {
        return true;
    }
    if (after > 1) {
        return false;
    }
    const auto next = directory_.find(queue_.back());
    return next != directory_.end() && !next->second->remote;
}

liquid::runtime::task<void> ZettaObjectBackend::remove_impl(std::string key) {
    if (auto checked = check_liquid_key(key); !checked) {
        co_return std::unexpected(checked.error());
    }
    if (auto ready = co_await ensure_directory(); !ready) {
        co_return std::unexpected(ready.error());
    }
    // Sent at once, after the writes held before it (lake#527). Liquid ends a checkpoint with the
    // removal of LIQUID_JOURNAL and no barrier after it (liquid#343); lake's sync at the end of the
    // operation makes the removal durable before lake answers (lake#535).
    SendGuard guard(*this);
    bool remote = false;
    {
        std::lock_guard lock(mutex_);
        const auto found = directory_.find(key);
        if (found == directory_.end()) {
            co_return std::unexpected(failure(liquid::errc::not_found, "no such object", key));
        }
        // Only held, never sent, and followed by at most one held key that goes as one PUT (a
        // staged object published under its name, then the staging removed): dropping both the
        // held put and its removal leaves every prefix of what is sent equal to a prefix of the
        // issue order (the one PUT after it lands where the removal did), and nothing is sent.
        if (!found->second->remote && !found->second->replaces_remote &&
            removable_unsent(key)) {
            unhold(key);
            release_plain(*found->second);
            directory_.erase(found);
            shared_->forget(key);
            co_return liquid::result<void>{};
        }
    }
    if (auto drained = co_await drain(); !drained) {
        co_return std::unexpected(drained.error());
    }
    {
        std::lock_guard lock(mutex_);
        const auto found = directory_.find(key);
        remote = found != directory_.end() && (found->second->remote || found->second->replaces_remote);
    }
    if (remote) {
        auto removed = co_await shared_->client->remove(key, fence_);
        if (!removed && removed.error().code != liquid::errc::not_found) {
            co_return std::unexpected(removed.error());
        }
        std::lock_guard lock(mutex_);
        dirty_ = true;
    }
    {
        std::lock_guard lock(mutex_);
        const auto found = directory_.find(key);
        if (found != directory_.end()) {
            release_plain(*found->second);
            // The key's tails go after the next sync, once its removal is durable.
            for (const auto& [tail, ignored] : found->second->tails) {
                retiring_tails_.insert(tail);
            }
            directory_.erase(found);
        }
        unhold(key);
    }
    shared_->forget(key);
    co_return liquid::result<void>{};
}

liquid::runtime::task<std::vector<liquid::store_backend_entry>>
ZettaObjectBackend::list(std::string prefix) {
    if (auto ready = co_await ensure_directory(); !ready) {
        co_return std::unexpected(ready.error());
    }
    std::vector<std::string> keys;
    {
        std::lock_guard lock(mutex_);
        for (auto it = directory_.lower_bound(prefix);
             it != directory_.end() && it->first.starts_with(prefix); ++it) {
            keys.push_back(it->first);
        }
    }
    for (const std::string& key : keys) {
        if (auto ready = co_await ensure_header(key); !ready) {
            co_return std::unexpected(ready.error());
        }
    }
    std::vector<liquid::store_backend_entry> output;
    std::lock_guard lock(mutex_);
    for (const std::string& key : keys) {
        const auto found = directory_.find(key);
        if (found != directory_.end()) {
            output.push_back({key, found->second->size(key)});
        }
    }
    co_return output;
}

// Liquid's barrier is a point in its issue order and sends nothing (lake#535). What liquid issued
// reaches the bucket in that order: held writes are sent before any later write that needs
// sending, so a crash keeps a prefix of the issue order, which is also what a crash just after
// some earlier point of it would have left. Held writes keep coalescing across liquid's barriers
// (a put of a held key replaces it in place, a removal of the last held key cancels it). Making
// them durable is the sync's job, once per store operation, before lake answers: an operation's
// own barriers acknowledge nothing outside the process.
liquid::runtime::task<void> ZettaObjectBackend::barrier_impl() {
    co_return liquid::result<void>{};
}

// Everything issued before the sync is durable when it answers (lake#535): the held writes are
// sent in issue order, one at a time, without durability, then one Zetta :barrier fsyncs the
// bucket. Tails made stale by an earlier sync are removed beside the :barrier: no reader needs
// them, and a removal brought back by a crash leaves a stale tail, which readers ignore. A sync
// with nothing sent since the last :barrier and no stale tail costs no request.
liquid::runtime::task<void> ZettaObjectBackend::sync_impl() {
    {
        std::lock_guard lock(mutex_);
        if (!directory_loaded_) {
            co_return liquid::result<void>{};
        }
    }
    SendGuard guard(*this);
    if (auto drained = co_await drain(); !drained) {
        co_return std::unexpected(drained.error());
    }
    // The stale tails go after the drain, so none is a tail the drain just wrote: every tail an
    // object holds now is left alone.
    bool dirty = false;
    std::set<std::string> stale;
    {
        std::lock_guard lock(mutex_);
        dirty = dirty_;
        stale.swap(stale_tails_);
        for (const auto& [key, entry] : directory_) {
            for (const auto& [tail, ignored] : entry->tails) {
                stale.erase(tail);
            }
        }
    }
    std::vector<std::function<liquid::runtime::task<void>()>> jobs;
    if (dirty) {
        jobs.push_back([this]() -> liquid::runtime::task<void> { return shared_->client->barrier(); });
    }
    for (const std::string& tail : stale) {
        jobs.push_back([this, tail]() -> liquid::runtime::task<void> { return remove_stale_tail(tail); });
    }
    auto sent = co_await run_object_requests(std::move(jobs), shared_->options.barrier_concurrency);
    if (!sent) {
        std::lock_guard lock(mutex_);
        stale_tails_.insert(stale.begin(), stale.end());
        co_return std::unexpected(sent.error());
    }
    // What replaced each retiring tail is durable now.
    std::lock_guard lock(mutex_);
    dirty_ = false;
    stale_tails_.insert(retiring_tails_.begin(), retiring_tails_.end());
    retiring_tails_.clear();
    co_return liquid::result<void>{};
}

// A failed mutation can leave liquid's memory of the store ahead of the bucket (an index flush
// that cleared its memtable before the barrier failed, a pack append liquid will retry at the
// same offset). Liquid recovers such a store on its next open, so the backend says it failed and
// lake closes and reopens the store before its next use.
liquid::result<void> ZettaObjectBackend::observe(liquid::result<void> outcome, bool tolerated) {
    if (!outcome && !tolerated) {
        shared_->failed.store(true);
    }
    return outcome;
}

bool ZettaObjectBackend::failed() const {
    return shared_->failed.load();
}

liquid::runtime::task<void> ZettaObjectBackend::put(std::string key, std::vector<std::byte> bytes) {
    co_return observe(co_await put_impl(std::move(key), std::move(bytes)), false);
}

liquid::runtime::task<void> ZettaObjectBackend::put_if_absent(std::string key,
                                                              std::vector<std::byte> bytes) {
    auto outcome = co_await put_if_absent_impl(std::move(key), std::move(bytes));
    const bool tolerated = !outcome && outcome.error().code == liquid::errc::already_exists;
    co_return observe(std::move(outcome), tolerated);
}

liquid::runtime::task<void>
ZettaObjectBackend::put_range(std::string key, std::uint64_t offset, std::vector<std::byte> bytes) {
    co_return observe(co_await put_range_impl(std::move(key), offset, std::move(bytes)), false);
}

liquid::runtime::task<void> ZettaObjectBackend::remove(std::string key) {
    auto outcome = co_await remove_impl(std::move(key));
    const bool tolerated = !outcome && outcome.error().code == liquid::errc::not_found;
    co_return observe(std::move(outcome), tolerated);
}

liquid::runtime::task<void> ZettaObjectBackend::barrier() {
    co_return observe(co_await barrier_impl(), false);
}

liquid::runtime::task<void> ZettaObjectBackend::sync() {
    co_return observe(co_await sync_impl(), false);
}

liquid::runtime::task<void> ZettaObjectBackend::make_durable() {
    if (auto synced = co_await sync(); !synced) {
        co_return synced;
    }
    std::vector<std::shared_ptr<ZettaObjectBackend>> views;
    {
        std::lock_guard lock(shared_->views_mutex);
        views = shared_->views;
    }
    for (const auto& view : views) {
        if (view.get() == this) {
            continue;
        }
        if (auto synced = co_await view->sync(); !synced) {
            co_return synced;
        }
    }
    co_return liquid::result<void>{};
}

// ---- ZettaStoreOwner -------------------------------------------------------------------------

ZettaStoreOwner::ZettaStoreOwner(std::shared_ptr<ZettaObjectClient> client,
                                 std::chrono::milliseconds ttl,
                                 std::function<void()> on_lost,
                                 std::chrono::milliseconds settle_budget)
    : client_(std::move(client)), ttl_(ttl), on_lost_(std::move(on_lost)),
      settle_budget_(settle_budget) {
    client_->set_owned(true);
}

ZettaStoreOwner::~ZettaStoreOwner() {
    stop_thread();
    // Views the backend kept for a release that never came (a store dropped without close) are
    // let go of, so they do not keep their backend's state alive (lake#535).
    client_->abandon_before_release();
    client_->set_owned(false);
}

bool ZettaStoreOwner::held() const {
    std::lock_guard lock(mutex_);
    return held_;
}

bool ZettaStoreOwner::lost() const {
    if (lost_.load()) {
        return true;
    }
    // The store's copy of the grant shares its state: liquid revokes it when Zetta refuses a write
    // as fenced, before this owner's next renewal finds out.
    std::lock_guard lock(mutex_);
    return held_ && grant_.valid() && grant_.lost();
}

std::uint64_t ZettaStoreOwner::fence() const {
    std::lock_guard lock(mutex_);
    return grant_.valid() ? grant_.fence() : 0;
}

void ZettaStoreOwner::stop_thread() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (renewer_.joinable() && renewer_.get_id() != std::this_thread::get_id()) {
        renewer_.join();
    }
}

void ZettaStoreOwner::stop_renewing() {
    stop_thread();
}

liquid::runtime::task<liquid::owner_grant>
ZettaStoreOwner::acquire(std::string holder, std::chrono::milliseconds wait) {
    stop_thread();
    // Zetta lets a holder re-acquire a lease it holds. Two opens in one process describe
    // themselves alike, so each owner adds its own token: a second open waits like any other.
    {
        std::array<unsigned char, 8> token{};
        if (RAND_bytes(token.data(), static_cast<int>(token.size())) != 1) {
            co_return std::unexpected(failure(liquid::errc::internal, "could not name the lease holder"));
        }
        holder += " (lease " + hex_of(token) + ")";
    }
    const auto deadline = std::chrono::steady_clock::now() + wait;
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const auto piece = std::clamp(remaining, std::chrono::milliseconds(0),
                                      std::chrono::milliseconds(25'000));
        auto lease = co_await client_->acquire(holder, ttl_, piece);
        if (lease) {
            liquid::owner_grant grant(lease->fence);
            {
                std::lock_guard lock(mutex_);
                holder_ = holder;
                grant_ = grant;
                held_ = true;
                stopping_ = false;
            }
            lost_.store(false);
            renewer_ = std::thread([this] { renew_loop(); });
            co_return grant;
        }
        if (lease.error().code != liquid::errc::locked ||
            std::chrono::steady_clock::now() >= deadline) {
            co_return std::unexpected(lease.error());
        }
    }
}

void ZettaStoreOwner::renew_loop() {
    auto expires = std::chrono::steady_clock::now() + ttl_;
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        wake_.wait_for(lock, ttl_ / 3, [this] { return stopping_; });
        if (stopping_) {
            return;
        }
        const std::string holder = holder_;
        const std::uint64_t fence = grant_.fence();
        lock.unlock();
        // The lease runs from when Zetta could have granted it at the earliest: the send.
        const auto sent = std::chrono::steady_clock::now();
        auto renewed = liquid::runtime::block_on(client_->renew(holder, fence, ttl_, ttl_ / 3));
        lock.lock();
        if (stopping_) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (renewed) {
            expires = sent + ttl_;
            continue;
        }
        // Refused as fenced, or no renewal accepted before the lease ran out: another holder may
        // write now, so this one must not.
        if (renewed.error().code == liquid::errc::locked || now + ttl_ / 6 >= expires) {
            grant_.revoke();
            lost_.store(true);
            std::function<void()> notify = on_lost_;
            lock.unlock();
            if (notify) {
                notify();
            }
            return;
        }
    }
}

liquid::runtime::task<void> ZettaStoreOwner::release() {
    // What the store's backend holds is sent and made durable while the lease is still held
    // (lake#527, lake#535): a store closed without a last sync leaves nothing unsent.
    // Not lost(): liquid's close revokes the store's copy of the grant before it calls release, so
    // the grant's state cannot tell a close from a fenced refusal. A renewal that failed sets
    // lost_; after a takeover the settling's writes are refused as fenced and land nothing.
    liquid::result<void> settled;
    if (held() && !lost_.load()) {
        settled = co_await client_->before_release(settle_budget_);
    } else {
        client_->abandon_before_release();
    }
    stop_thread();
    std::string holder;
    std::uint64_t fence = 0;
    bool release = false;
    {
        std::lock_guard lock(mutex_);
        release = held_ && !lost_.load();
        holder = holder_;
        fence = grant_.valid() ? grant_.fence() : 0;
        held_ = false;
        if (grant_.valid()) {
            grant_.revoke();
        }
    }
    if (release) {
        auto released = co_await client_->release(holder, fence);
        if (!released && released.error().code != liquid::errc::locked) {
            co_return std::unexpected(released.error());
        }
    }
    co_return settled;
}

} // namespace lake::control
