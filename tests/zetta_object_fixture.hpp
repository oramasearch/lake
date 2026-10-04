#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/control/workspace_store.hpp"

namespace lake::test {

// Zetta's object routes as zetta docs/data/object-store.md specifies them, in memory, for one or
// more workspace Zettas told apart by endpoint (`http://<name>.test`): buckets with quotas,
// objects addressed by (bucket, key), put / put if absent / put range at an exact offset (a range
// inside the object must equal the bytes there, otherwise 409 conflict: the decided contract that
// refuses in-place rewrites), ranged reads (416 outside, 404 absent), paged listing, remove,
// barrier, usage, and the owner lease whose fence every mutation carries once the bucket has had
// a holder (409 fenced, 409 held). Tests reach in to read, alter, expire or count.
class FakeZettaObjects final : public control::ObjectTransport {
public:
    struct Lease {
        std::string holder;
        std::uint64_t fence = 0;
        std::chrono::steady_clock::time_point expires{};
        bool had_holder = false;
    };
    struct Bucket {
        std::uint64_t quota = 0;
        std::map<std::string, std::string> objects;
        Lease lease;
        std::uint64_t barriers = 0;
    };
    struct Node {
        std::string token;
        std::map<std::string, Bucket> buckets;
    };

    // A workspace Zetta at `http://<name>.test` answering bearer `token`.
    void add_node(const std::string& endpoint, std::string token) {
        std::lock_guard lock(mutex_);
        nodes_[endpoint].token = std::move(token);
    }

    std::map<std::string, std::string> objects(const std::string& endpoint,
                                               const std::string& bucket = "lake_store") {
        std::lock_guard lock(mutex_);
        return nodes_[endpoint].buckets[bucket].objects;
    }
    bool has_bucket(const std::string& endpoint, const std::string& bucket = "lake_store") {
        std::lock_guard lock(mutex_);
        return nodes_[endpoint].buckets.contains(bucket);
    }
    void set_object(const std::string& endpoint, const std::string& key, std::string bytes,
                    const std::string& bucket = "lake_store") {
        std::lock_guard lock(mutex_);
        nodes_[endpoint].buckets[bucket].objects[key] = std::move(bytes);
    }
    Lease lease(const std::string& endpoint, const std::string& bucket = "lake_store") {
        std::lock_guard lock(mutex_);
        return nodes_[endpoint].buckets[bucket].lease;
    }
    // The holder's lease runs out now, as if it had stopped renewing.
    void expire(const std::string& endpoint, const std::string& bucket = "lake_store") {
        std::lock_guard lock(mutex_);
        nodes_[endpoint].buckets[bucket].lease.expires = std::chrono::steady_clock::now();
        changed_.notify_all();
    }
    std::uint64_t barriers(const std::string& endpoint, const std::string& bucket = "lake_store") {
        std::lock_guard lock(mutex_);
        return nodes_[endpoint].buckets[bucket].barriers;
    }
    std::uint64_t requests() {
        std::lock_guard lock(mutex_);
        return requests_;
    }
    std::map<std::string, std::uint64_t> requests_by_method() {
        std::lock_guard lock(mutex_);
        return by_method_;
    }
    // Every request body this server was sent, in order.
    std::vector<std::string> bodies() {
        std::lock_guard lock(mutex_);
        return bodies_;
    }
    void keep_bodies(bool keep) {
        std::lock_guard lock(mutex_);
        keep_bodies_ = keep;
    }
    // The next `count` requests get no answer (a transport failure).
    void drop_next(int count) {
        std::lock_guard lock(mutex_);
        drop_ = count;
    }
    // When the `nth` request with `method` arrives from now on, it and the next `count - 1`
    // requests get no answer: an interruption part way through a sequence of writes.
    void drop_on(std::string method, int nth, int count) {
        std::lock_guard lock(mutex_);
        drop_method_ = std::move(method);
        drop_nth_ = nth;
        drop_count_ = count;
    }
    // Another process takes the bucket's lease, as a successor does once the holder's lease ran
    // out: the fence rises and the old holder's writes are refused from now on.
    std::uint64_t take_over(const std::string& endpoint, const std::string& holder,
                            const std::string& bucket = "lake_store") {
        std::lock_guard lock(mutex_);
        Lease& lease = nodes_[endpoint].buckets[bucket].lease;
        lease.holder = holder;
        ++lease.fence;
        lease.had_holder = true;
        lease.expires = std::chrono::steady_clock::now() + std::chrono::hours(1);
        changed_.notify_all();
        return lease.fence;
    }
    void release(const std::string& endpoint, const std::string& bucket = "lake_store") {
        std::lock_guard lock(mutex_);
        nodes_[endpoint].buckets[bucket].lease.holder.clear();
        changed_.notify_all();
    }
    // Every request to `endpoint` whose URL contains `fragment` (empty: every one) waits `delay`
    // first. A delay past the request's timeout ends at the timeout with no answer, as a
    // transport does.
    void hang(const std::string& endpoint, std::chrono::milliseconds delay, std::string fragment = {}) {
        std::lock_guard lock(mutex_);
        hangs_[endpoint] = {delay, std::move(fragment)};
    }
    void clear_bucket(const std::string& endpoint, const std::string& bucket = "lake_store") {
        std::lock_guard lock(mutex_);
        nodes_[endpoint].buckets[bucket].objects.clear();
    }
    void erase_object(const std::string& endpoint, const std::string& key,
                      const std::string& bucket = "lake_store") {
        std::lock_guard lock(mutex_);
        nodes_[endpoint].buckets[bucket].objects.erase(key);
    }
    // Added to every exchange, to stand in for a network.
    void set_latency(std::chrono::microseconds latency) {
        std::lock_guard lock(mutex_);
        latency_ = latency;
    }

    liquid::runtime::task<control::ObjectResponse> exchange(control::ObjectRequest request) override {
        std::chrono::microseconds latency{};
        std::chrono::milliseconds delay{};
        {
            std::lock_guard lock(mutex_);
            latency = latency_;
            for (const auto& [endpoint, hung] : hangs_) {
                if (request.url.starts_with(endpoint) &&
                    (hung.second.empty() || request.url.find(hung.second) != std::string::npos)) {
                    delay = hung.first;
                }
            }
        }
        if (latency.count() > 0) {
            std::this_thread::sleep_for(latency);
        }
        if (delay.count() > 0) {
            const auto timeout = std::chrono::seconds(request.timeout_seconds);
            if (delay >= timeout) {
                std::this_thread::sleep_for(timeout);
                co_return control::ObjectResponse{0, {}, "timed out"};
            }
            std::this_thread::sleep_for(delay);
        }
        co_return answer(std::move(request));
    }

private:
    static control::ObjectResponse ok(nlohmann::json value) {
        return {200, nlohmann::json{{"ok", true}, {"value", std::move(value)}}.dump(), {}};
    }
    static control::ObjectResponse refused(int status, std::string code, std::string message,
                                           nlohmann::json details = nlohmann::json::object()) {
        return {status,
                nlohmann::json{{"ok", false},
                               {"error",
                                {{"code", std::move(code)},
                                 {"message", std::move(message)},
                                 {"details", std::move(details)}}}}
                    .dump(),
                {}};
    }
    static std::string header(const control::ObjectRequest& request, std::string_view name) {
        for (const auto& [key, value] : request.headers) {
            if (key.size() == name.size() &&
                std::equal(key.begin(), key.end(), name.begin(), [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                           std::tolower(static_cast<unsigned char>(b));
                })) {
                return value;
            }
        }
        return {};
    }
    static std::string decode(std::string_view input) {
        std::string output;
        for (std::size_t index = 0; index < input.size(); ++index) {
            if (input[index] == '%' && index + 2 < input.size()) {
                output += static_cast<char>(std::stoi(std::string(input.substr(index + 1, 2)), nullptr, 16));
                index += 2;
            } else {
                output += input[index];
            }
        }
        return output;
    }
    static std::map<std::string, std::string> query(std::string_view text) {
        std::map<std::string, std::string> output;
        while (!text.empty()) {
            const std::size_t amp = text.find('&');
            const std::string_view pair = text.substr(0, amp);
            const std::size_t equals = pair.find('=');
            output[decode(pair.substr(0, equals))] =
                equals == std::string_view::npos ? std::string() : decode(pair.substr(equals + 1));
            if (amp == std::string_view::npos) {
                break;
            }
            text.remove_prefix(amp + 1);
        }
        return output;
    }
    std::uint64_t used(const Bucket& bucket) const {
        std::uint64_t total = 0;
        for (const auto& [key, bytes] : bucket.objects) {
            total += bytes.size();
        }
        return total;
    }
    // Once a bucket has had a holder, a mutation must carry the current fence.
    std::optional<control::ObjectResponse> fenced(const control::ObjectRequest& request,
                                                  const Bucket& bucket) const {
        if (!bucket.lease.had_holder) {
            return std::nullopt;
        }
        const std::string fence = header(request, "Zetta-Fence");
        if (fence.empty() || std::stoull(fence) != bucket.lease.fence) {
            return refused(409, "fenced", "the bucket's lease has a newer fence",
                           {{"fence", bucket.lease.fence}});
        }
        return std::nullopt;
    }

    control::ObjectResponse answer(control::ObjectRequest request) {
        std::unique_lock lock(mutex_);
        ++requests_;
        ++by_method_[request.method];
        if (keep_bodies_) {
            bodies_.push_back(request.body);
        }
        if (drop_nth_ > 0 && request.method == drop_method_ && --drop_nth_ == 0) {
            drop_ = drop_count_;
        }
        if (drop_ > 0) {
            --drop_;
            return {0, {}, "connection reset by the test"};
        }
        const std::size_t scheme = request.url.find("://");
        const std::size_t path_start = request.url.find('/', scheme + 3);
        const std::string endpoint = request.url.substr(0, path_start);
        std::string path = request.url.substr(path_start);
        std::string query_text;
        if (const std::size_t mark = path.find('?'); mark != std::string::npos) {
            query_text = path.substr(mark + 1);
            path = path.substr(0, mark);
        }
        const auto parameters = query(query_text);
        const auto node = nodes_.find(endpoint);
        if (node == nodes_.end()) {
            return {0, {}, "no such host"};
        }
        if (header(request, "Authorization") != "Bearer " + node->second.token) {
            return refused(403, "unauthorized", "bad bearer");
        }
        auto& buckets = node->second.buckets;
        if (path == "/v1/create_bucket") {
            const auto input = nlohmann::json::parse(request.body);
            const std::string name = input.at("bucket");
            const std::uint64_t quota = input.value("quota_bytes", std::uint64_t{0});
            const auto existing = buckets.find(name);
            if (existing != buckets.end() && existing->second.quota != quota) {
                return refused(409, "conflict", "the bucket exists with another definition");
            }
            buckets[name].quota = quota;
            return ok({{"bucket", name}});
        }
        const std::string prefix = "/v1/objects/";
        if (!path.starts_with(prefix)) {
            return refused(404, "not_found", "no route");
        }
        const std::string rest = path.substr(prefix.size());
        const std::size_t slash = rest.find('/');
        const std::size_t colon = rest.find(':');
        if (slash == std::string::npos) {
            const std::string name = rest.substr(0, colon);
            const auto bucket_found = buckets.find(name);
            if (bucket_found == buckets.end()) {
                return refused(404, "not_found", "no bucket " + name);
            }
            Bucket& bucket = bucket_found->second;
            const std::string operation =
                colon == std::string::npos ? std::string() : rest.substr(colon + 1);
            if (operation.empty() && request.method == "GET") {
                const std::string want = parameters.contains("prefix") ? parameters.at("prefix") : "";
                const std::string after = parameters.contains("after") ? parameters.at("after") : "";
                const std::size_t limit =
                    parameters.contains("limit") ? std::stoul(parameters.at("limit")) : 1000;
                nlohmann::json objects = nlohmann::json::array();
                std::string last;
                bool more = false;
                for (auto it = after.empty() ? bucket.objects.lower_bound(want)
                                             : bucket.objects.upper_bound(after);
                     it != bucket.objects.end() && it->first.starts_with(want); ++it) {
                    if (objects.size() == limit) {
                        more = true;
                        break;
                    }
                    objects.push_back({{"key", it->first}, {"size", it->second.size()}, {"updated_ms", 0}});
                    last = it->first;
                }
                nlohmann::json value{{"objects", objects}};
                if (more) {
                    value["next_after"] = last;
                }
                return ok(value);
            }
            if (operation == "barrier") {
                ++bucket.barriers;
                return ok({{"bucket", name}, {"nodes", 1}});
            }
            if (operation == "usage") {
                nlohmann::json value{{"objects", bucket.objects.size()}, {"bytes", used(bucket)}};
                if (bucket.quota != 0) {
                    value["quota_bytes"] = bucket.quota;
                }
                return ok(value);
            }
            const auto input = nlohmann::json::parse(request.body, nullptr, false);
            Lease& lease = bucket.lease;
            const auto lease_value = [&] {
                return nlohmann::json{{"bucket", name},
                                      {"holder", lease.holder},
                                      {"fence", lease.fence},
                                      {"expires_ms", 0}};
            };
            if (operation == "acquire") {
                const std::string holder = input.at("holder");
                const auto ttl = std::chrono::milliseconds(input.at("ttl_ms").get<std::int64_t>());
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(input.value("wait_ms", std::int64_t{0}));
                for (;;) {
                    const auto now = std::chrono::steady_clock::now();
                    const bool free = lease.holder.empty() || lease.expires <= now;
                    if (free || lease.holder == holder) {
                        // A new holder, or a holder whose lease ran out, raises the fence.
                        if (lease.holder != holder || lease.expires <= now) {
                            ++lease.fence;
                        }
                        lease.holder = holder;
                        lease.expires = now + ttl;
                        lease.had_holder = true;
                        changed_.notify_all();
                        return ok(lease_value());
                    }
                    if (now >= deadline) {
                        return refused(409, "held", "the bucket's lease is held",
                                       {{"holder", lease.holder}});
                    }
                    changed_.wait_until(lock, std::min(deadline, lease.expires));
                }
            }
            if (operation == "renew") {
                if (input.at("fence").get<std::uint64_t>() != lease.fence ||
                    input.at("holder").get<std::string>() != lease.holder) {
                    return refused(409, "fenced", "the lease has passed on", {{"fence", lease.fence}});
                }
                lease.expires = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(input.at("ttl_ms").get<std::int64_t>());
                return ok(lease_value());
            }
            if (operation == "release") {
                if (input.at("fence").get<std::uint64_t>() != lease.fence) {
                    return refused(409, "fenced", "the lease has passed on", {{"fence", lease.fence}});
                }
                lease.holder.clear();
                changed_.notify_all();
                return ok(lease_value());
            }
            return refused(404, "not_found", "no route");
        }
        const std::string name = rest.substr(0, slash);
        const std::string key = decode(rest.substr(slash + 1));
        const auto bucket_found = buckets.find(name);
        if (bucket_found == buckets.end()) {
            return refused(404, "not_found", "no bucket " + name);
        }
        Bucket& bucket = bucket_found->second;
        const nlohmann::json written{{"bucket", name}, {"key", key}};
        if (request.method == "GET") {
            const auto found = bucket.objects.find(key);
            if (found == bucket.objects.end()) {
                return refused(404, "not_found", "no object " + key);
            }
            const std::string range = header(request, "Range");
            if (range.empty()) {
                return {200, found->second, {}};
            }
            const std::size_t dash = range.find('-');
            const std::uint64_t from = std::stoull(range.substr(6, dash - 6));
            const std::uint64_t to = std::stoull(range.substr(dash + 1));
            if (from > to || to >= found->second.size()) {
                return refused(416, "range_not_satisfiable", "outside the object",
                               {{"size", found->second.size()}});
            }
            return {206, found->second.substr(from, to - from + 1), {}};
        }
        if (auto refusal = fenced(request, bucket)) {
            return *refusal;
        }
        if (request.method == "PUT") {
            const auto existing = bucket.objects.find(key);
            if (parameters.contains("if_absent") && existing != bucket.objects.end()) {
                return refused(409, "conflict", "the key exists");
            }
            const std::uint64_t freed = existing == bucket.objects.end() ? 0 : existing->second.size();
            if (bucket.quota != 0 && used(bucket) - freed + request.body.size() > bucket.quota) {
                return refused(507, "quota_exceeded", "past the bucket's quota");
            }
            bucket.objects[key] = request.body;
            return ok(written);
        }
        if (request.method == "PATCH") {
            const std::uint64_t offset = std::stoull(parameters.at("offset"));
            std::string& object = bucket.objects[key];
            if (offset == object.size()) {
                if (bucket.quota != 0 && used(bucket) + request.body.size() > bucket.quota) {
                    return refused(507, "quota_exceeded", "past the bucket's quota");
                }
                object += request.body;
                return ok(written);
            }
            if (offset < object.size() && offset + request.body.size() <= object.size() &&
                object.compare(offset, request.body.size(), request.body) == 0) {
                nlohmann::json unchanged = written;
                unchanged["unchanged"] = true;
                return ok(unchanged);
            }
            if (object.empty()) {
                bucket.objects.erase(key);
            }
            return refused(409, "conflict", "a range must append or repeat the bytes there",
                           {{"size", object.size()}});
        }
        if (request.method == "DELETE") {
            if (bucket.objects.erase(key) == 0) {
                return refused(404, "not_found", "no object " + key);
            }
            return ok(written);
        }
        return refused(404, "not_found", "no route");
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::string, Node> nodes_;
    std::uint64_t requests_ = 0;
    std::map<std::string, std::uint64_t> by_method_;
    std::vector<std::string> bodies_;
    int drop_ = 0;
    std::string drop_method_;
    int drop_nth_ = 0;
    int drop_count_ = 0;
    bool keep_bodies_ = true;
    std::chrono::microseconds latency_{};
    std::map<std::string, std::pair<std::chrono::milliseconds, std::string>> hangs_;
};

} // namespace lake::test
