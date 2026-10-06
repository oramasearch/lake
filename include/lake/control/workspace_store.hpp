#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <liquid/store.hpp>

// A workspace's liquid store in its workspace Zetta (decision 54 S1 and S3, lake#380,
// docs/architecture/workspace-store.md). Lake opens one liquid store per workspace on
// ZettaObjectBackend, a liquid store_backend over Zetta's object routes (zetta
// docs/data/object-store.md) in bucket `lake_store`, and owns it through ZettaStoreOwner, a liquid
// store_owner over the bucket's lease. Every byte the backend writes is encrypted below liquid
// with the workspace's data key, which is wrapped under a key derived from LAKE_INSTANCE_KEK, so
// the workspace Zetta holds only ciphertext and liquid's object identities are unchanged.
namespace lake::control {

inline constexpr std::string_view workspace_store_bucket = "lake_store";
// The object that holds the workspace's wrapped data key. Liquid writes `keys/` only when a
// store has a keyring, which a workspace store never has; the backend refuses liquid keys there.
inline constexpr std::string_view workspace_store_key_object = "keys/workspace-v1.wrapped";
// The search index's sealed objects in the same bucket (lake#381, search::workspace_index_prefix):
// never liquid's, so the backend leaves them out of its directory and refuses liquid keys there.
inline constexpr std::string_view workspace_store_search_prefix = "search/";

// One raw exchange with a workspace Zetta's object routes. Bodies are bytes in a std::string.
struct ObjectRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int timeout_seconds = 60;
};

// `error` is a transport failure (no answer); otherwise `status` and `body` are Zetta's.
struct ObjectResponse {
    int status = 0;
    std::string body;
    std::string error;
};

// Injected so tests can answer the object routes without a network.
class ObjectTransport {
public:
    virtual ~ObjectTransport() = default;
    virtual liquid::runtime::task<ObjectResponse> exchange(ObjectRequest request) = 0;
};

// Zetta's installed outbound HTTP client (zetta::connect, installed by
// install_zetta_control_transport). The bearer never appears in an error.
std::shared_ptr<ObjectTransport> make_zetta_object_transport();

// Where a workspace Zetta answers and the credential lake presents to it (the operator's mounted
// connection for the workspace's Zetta service).
struct ObjectEndpoint {
    std::string url;
    std::string token;
};

// The kinds of request lake sends to a workspace Zetta's object routes (lake#527). A bounded set:
// it is the `kind` label of lake_workspace_store_requests_total and of the Server-Timing entries.
enum class ObjectRequestKind : std::uint8_t {
    put,           // PUT, replacing
    put_if_absent, // PUT ?if_absent=true
    patch,         // PATCH ?offset= (an append)
    barrier,       // POST :barrier
    get,           // GET of a whole object
    ranged_get,    // GET with Range
    list,          // GET of the bucket listing, one page
    remove,        // DELETE
    lease_acquire, // POST :acquire
    lease_renew,   // POST :renew
    lease_release, // POST :release
    usage,         // GET :usage
    create_bucket, // POST create_bucket
};
inline constexpr std::size_t object_request_kinds = 13;
std::string_view object_request_kind_name(ObjectRequestKind kind);

// Requests by kind: how many, and how long they took (each attempt of a retried request counts).
struct ObjectRequestTally {
    std::array<std::atomic<std::uint64_t>, object_request_kinds> count{};
    std::array<std::atomic<std::uint64_t>, object_request_kinds> nanoseconds{};

    void add(ObjectRequestKind kind, std::uint64_t elapsed) {
        count[static_cast<std::size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
        nanoseconds[static_cast<std::size_t>(kind)].fetch_add(elapsed, std::memory_order_relaxed);
    }
    std::uint64_t requests(ObjectRequestKind kind) const {
        return count[static_cast<std::size_t>(kind)].load(std::memory_order_relaxed);
    }
    // Adds `later - earlier` of each kind but the lease renewal and usage, which run on their own
    // threads beside the calls.
    void add_difference(const ObjectRequestTally& later,
                        const std::array<std::uint64_t, object_request_kinds>& earlier_count,
                        const std::array<std::uint64_t, object_request_kinds>& earlier_nanoseconds);
    std::uint64_t total_requests() const;
    std::uint64_t total_nanoseconds() const;
    // "put=3 patch=1 barrier=2" for the kinds with requests, in the enum's order.
    std::string summary() const;
};

// While alive, this thread's capability call collects into `tally` the workspace store requests
// of the store calls it makes. Lake opens one around each capability call (Registry::call, and
// call_async's blocking-pool call). The requests themselves are not attributed by thread (liquid
// runs parts of a store call on its own pool); the repository store adds, when a call leaves a
// workspace store's lane, the requests that store sent while the call held the lane, which only
// that call can have sent (lease renewals and usage reads excepted, and not counted).
class ObjectRequestScope {
public:
    explicit ObjectRequestScope(ObjectRequestTally* tally);
    ~ObjectRequestScope();
    ObjectRequestScope(const ObjectRequestScope&) = delete;
    ObjectRequestScope& operator=(const ObjectRequestScope&) = delete;
    static ObjectRequestTally* current();

private:
    ObjectRequestTally* previous_;
};

// Called once per request attempt with its kind, Zetta's status (0 when no answer came) and its
// duration; lake records it as process metrics with bounded labels.
using ObjectRequestObserver =
    std::function<void(ObjectRequestKind kind, int status, std::chrono::nanoseconds elapsed)>;

// Cumulative counters for one workspace store, read by tests and the latency measurement.
struct ObjectStoreCounters {
    ObjectRequestTally by_kind;
    std::atomic<std::uint64_t> requests{0};
    std::atomic<std::uint64_t> request_nanoseconds{0};
    std::atomic<std::uint64_t> bytes_sent{0};
    std::atomic<std::uint64_t> bytes_received{0};
    std::atomic<std::uint64_t> puts{0};
    std::atomic<std::uint64_t> appends{0};
    std::atomic<std::uint64_t> gets{0};
    std::atomic<std::uint64_t> lists{0};
    std::atomic<std::uint64_t> removes{0};
    std::atomic<std::uint64_t> barriers{0};
    std::atomic<std::uint64_t> window_hits{0};
    std::atomic<std::uint64_t> window_misses{0};
};

struct ObjectListEntry {
    std::string key;
    std::uint64_t size = 0;
};

struct ObjectLease {
    std::string holder;
    std::uint64_t fence = 0;
    std::int64_t expires_ms = 0;
};

struct ObjectUsage {
    std::uint64_t objects = 0;
    std::uint64_t bytes = 0;
    std::uint64_t quota_bytes = 0;
};

// One bucket of one workspace Zetta. Errors are liquid's (the mapping in zetta
// docs/data/object-store.md, "What lake's liquid store backend calls"): 404 not_found, 409
// conflict (already_exists for a conditional create), 409 fenced or held locked, 416 corrupt,
// 401/403 unauthorized, 507 busy (quota), 503 or no answer unavailable. Idempotent requests are
// retried on unavailable a few times.
class ZettaObjectClient {
public:
    ZettaObjectClient(std::shared_ptr<ObjectTransport> transport,
                      ObjectEndpoint endpoint,
                      std::string bucket = std::string(workspace_store_bucket),
                      std::shared_ptr<ObjectStoreCounters> counters = {},
                      ObjectRequestObserver observer = {});

    const std::string& bucket() const {
        return bucket_;
    }
    const std::shared_ptr<ObjectStoreCounters>& counters() const {
        return counters_;
    }

    // create_bucket {bucket, quota_bytes}: idempotent for an identical definition. A zero quota
    // leaves the bucket without one. An existing bucket with another quota is accepted as is.
    liquid::runtime::task<void> create_bucket(std::uint64_t quota_bytes);
    // `durable`: Zetta fsyncs the write before it answers (`?durable=true`), so an acknowledged
    // durable write needs no barrier.
    liquid::runtime::task<void> put(std::string key,
                                    std::string bytes,
                                    std::optional<std::uint64_t> fence,
                                    bool if_absent,
                                    bool durable = false);
    liquid::runtime::task<void> append(std::string key,
                                       std::uint64_t offset,
                                       std::string bytes,
                                       std::optional<std::uint64_t> fence,
                                       bool durable = false);
    // Exactly [offset, offset + size); size 0 answers an empty string without a request.
    liquid::runtime::task<std::string> get(std::string key, std::uint64_t offset, std::uint64_t size);
    liquid::runtime::task<std::string> get_all(std::string key);
    liquid::runtime::task<void>
    remove(std::string key, std::optional<std::uint64_t> fence, bool durable = false);
    // Every key under prefix with its size, by key, following next_after.
    liquid::runtime::task<std::vector<ObjectListEntry>> list(std::string prefix);
    liquid::runtime::task<void> barrier();
    // One attempt within `timeout`: a caller with a last known value uses it on failure.
    liquid::runtime::task<ObjectUsage> usage(std::chrono::milliseconds timeout = std::chrono::seconds(5));
    liquid::runtime::task<ObjectLease>
    acquire(std::string holder, std::chrono::milliseconds ttl, std::chrono::milliseconds wait);
    // One attempt that gives up after `timeout`, so a renewal never outlasts the lease.
    liquid::runtime::task<ObjectLease> renew(std::string holder,
                                             std::uint64_t fence,
                                             std::chrono::milliseconds ttl,
                                             std::chrono::milliseconds timeout);
    liquid::runtime::task<void> release(std::string holder, std::uint64_t fence);

    // Sends what a backend over this client holds for the next barrier (lake#527); the owner
    // calls it before it releases the lease, so a closed store leaves nothing unsent. Set by
    // ZettaObjectBackend; the last one constructed over the client is the open store's.
    void set_before_release(std::function<liquid::runtime::task<void>()> settle);
    // Bounded by `budget`: each request then gets one attempt, cut to the time left, so a drain
    // with the workspace Zetta unreachable does not stall.
    liquid::runtime::task<void> before_release(std::chrono::milliseconds budget = std::chrono::seconds(10));

private:
    liquid::runtime::task<ObjectResponse>
    send(ObjectRequest request, bool idempotent, ObjectRequestKind kind);
    std::string object_url(std::string_view key) const;
    std::string bucket_url(std::string_view suffix) const;

    std::shared_ptr<ObjectTransport> transport_;
    ObjectEndpoint endpoint_;
    std::string bucket_;
    std::shared_ptr<ObjectStoreCounters> counters_;
    ObjectRequestObserver observer_;
    mutable std::mutex settle_mutex_;
    std::function<liquid::runtime::task<void>()> settle_;
};

// The workspace's data key (S3): random, 32 bytes, wrapped with AES-256-GCM under
// HKDF-SHA256(LAKE_INSTANCE_KEK, "lake/workspace-store/v1/<workspace id>") and kept in the
// bucket at workspace_store_key_object.
class WorkspaceStoreCipher {
public:
    using Key = std::array<unsigned char, 32>;
    static constexpr std::size_t nonce_size = 12;
    static constexpr std::size_t tag_size = 16;
    static constexpr std::size_t overhead = nonce_size + tag_size;

    WorkspaceStoreCipher(std::string workspace_id, std::uint32_t key_version, Key data_key);
    ~WorkspaceStoreCipher();
    WorkspaceStoreCipher(const WorkspaceStoreCipher&) = delete;
    WorkspaceStoreCipher& operator=(const WorkspaceStoreCipher&) = delete;

    // The wrapping key for a workspace. Refuses an empty or malformed KEK.
    static liquid::result<Key> wrapping_key(std::string_view instance_kek,
                                            std::string_view workspace_id);
    // The wrapped-key document stored in the bucket, and its inverse.
    static liquid::result<std::string>
    wrap(const Key& wrapping, std::string_view workspace_id, std::uint32_t version, const Key& key);
    static liquid::result<std::pair<std::uint32_t, Key>>
    unwrap(const Key& wrapping, std::string_view workspace_id, std::string_view document);

    const std::string& workspace_id() const {
        return workspace_id_;
    }
    std::uint32_t key_version() const {
        return key_version_;
    }
    // nonce || ciphertext || tag, with a random 96-bit nonce.
    liquid::result<std::string> seal(std::string_view associated, std::span<const std::byte> plain) const;
    // Fails corrupt when authentication fails.
    liquid::result<std::vector<std::byte>> open(std::string_view associated,
                                                std::string_view sealed) const;

private:
    std::string workspace_id_;
    std::uint32_t key_version_ = 1;
    Key key_{};
};

struct ZettaObjectBackendOptions {
    std::string workspace_id;
    // LAKE_INSTANCE_KEK, as lake accepts it elsewhere.
    std::string instance_kek;
    // Plaintext bytes per encrypted block. Fixed for a store: it is recorded in every object.
    std::size_t block_size = 64 * 1024;
    // Complete blocks of buffered appends are sent once at least this much is held.
    std::size_t flush_bytes = 1024 * 1024;
    // A put of at most this many bytes is held until the next barrier and sent then, in its place
    // among the held writes, as one PUT (lake#527). A larger put is sent at once.
    std::size_t deferred_put_bytes = 1024 * 1024;
    // Held writes past either bound are sent at once, without durability (the next barrier then
    // sends :barrier), so a long interval holds bounded memory.
    std::size_t held_bytes_limit = 8 * 1024 * 1024;
    std::size_t held_puts_limit = 256;
    // Requests one barrier keeps in flight at once: the ordered writes count as one, the removal
    // of stale tails beside them as one each.
    std::size_t barrier_concurrency = 16;
    // Reads fetch aligned windows of ciphertext this large and keep them.
    std::size_t window_bytes = 4 * 1024 * 1024;
    // The window cache's bound, per workspace store.
    std::size_t cache_bytes = 64 * 1024 * 1024;
    // Whether a missing data key may be created, which the backend still does only when the
    // bucket holds no object. Lake sets it only while it creates a workspace's store, before the
    // workspace is switched to it.
    bool create_key = false;
};

// Liquid's store_backend over one workspace Zetta bucket (docs/architecture/workspace-store.md).
//
// Layout: liquid key K is the object K: a 48-byte header (format, key version, block size, the
// length of the bytes K was created with by `put`, and a random salt), then those bytes as
// encrypted blocks, then each later complete block of appended bytes. An appended partial block
// is kept as the object `K.lake-tail-<block>-<salt>`, replaced at each barrier; a tail whose block
// or salt no longer matches is stale and ignored. Every block is AES-256-GCM with a random nonce,
// its associated data binding the workspace, K, the header, the block's region and index.
//
// Appends are buffered and sent as complete blocks once flush_bytes is held, and always before
// barrier() answers. Small puts are held until barrier() too (or until a later write needs them
// sent), and every write is sent in liquid's issue order, one at a time, durably unless a
// write since the last barrier went without (lake#527). A range rewritten with different bytes is refused with errc::conflict: Zetta
// keeps objects append-only (liquid writes each index run once since liquid#332). Reads go through a bounded cache of aligned ciphertext windows keyed by the object
// version's salt. The backend keeps a directory of the bucket from one listing, maintained by its
// own writes; that is exact because the owner's fence makes it the only writer.
class ZettaObjectBackend final : public liquid::store_backend {
public:
    struct Shared;

    ZettaObjectBackend(std::shared_ptr<ZettaObjectClient> client, ZettaObjectBackendOptions options);
    ZettaObjectBackend(std::shared_ptr<Shared> shared, std::optional<std::uint64_t> fence);
    ~ZettaObjectBackend() override;

    liquid::runtime::task<void> put(std::string key, std::vector<std::byte> bytes) override;
    liquid::runtime::task<void> put_if_absent(std::string key, std::vector<std::byte> bytes) override;
    liquid::runtime::task<void>
    put_range(std::string key, std::uint64_t offset, std::vector<std::byte> bytes) override;
    liquid::runtime::task<std::vector<std::byte>>
    get_range(std::string key, std::uint64_t offset, std::uint64_t size) override;
    liquid::runtime::task<void> remove(std::string key) override;
    liquid::runtime::task<std::vector<liquid::store_backend_entry>> list(std::string prefix) override;
    liquid::runtime::task<void> barrier() override;
    liquid::result<std::shared_ptr<liquid::store_backend>> fenced(std::uint64_t fence) const override;

    std::optional<std::uint64_t> fence() const {
        return fence_;
    }
    const ZettaObjectClient& client() const;
    // A mutation through this backend or any of its views failed: liquid's memory of the store
    // may be ahead of the bucket, so the store must be closed and opened again before more use.
    bool failed() const;
    // What this view holds for its next barrier: bytes and puts.
    std::pair<std::uint64_t, std::size_t> held() const;
    // Barriers every view of this backend: what they hold is sent and durable (lake#527).
    static liquid::runtime::task<void> settle(std::shared_ptr<Shared> shared);

    struct Entry;

private:
    liquid::runtime::task<void> put_impl(std::string key, std::vector<std::byte> bytes);
    liquid::runtime::task<void> put_if_absent_impl(std::string key, std::vector<std::byte> bytes);
    liquid::runtime::task<void>
    put_range_impl(std::string key, std::uint64_t offset, std::vector<std::byte> bytes);
    liquid::runtime::task<void> remove_impl(std::string key);
    liquid::runtime::task<void> barrier_impl();
    liquid::result<void> observe(liquid::result<void> outcome, bool tolerated);
    liquid::runtime::task<void> ensure_directory();
    liquid::runtime::task<void> ensure_cipher();
    liquid::runtime::task<void> ensure_header(const std::string& key);
    liquid::runtime::task<void> ensure_open_region(const std::string& key);
    struct FlushPlan;
    struct SendGuard;
    liquid::runtime::task<void> flush_entry(const std::string& key);
    liquid::runtime::task<void> drain(bool durable);
    liquid::runtime::task<void> keep_order_before_holding(const std::string& key);
    // Under mutex_: `key` has a held write of `bytes` (a put when `put`, which replaces what was
    // held for the key); unhold forgets what is held for `key`.
    void hold(const std::string& key, std::uint64_t bytes, bool put);
    void unhold(const std::string& key);
    // Under mutex_.
    bool durable_now() const;
    void release_plain(Entry& entry);
    // The writes that bring `key` up to date in Zetta, built under mutex_, or nothing.
    liquid::result<std::optional<FlushPlan>> plan_flush(const std::string& key, bool barrier);
    liquid::runtime::task<void> send_plan(FlushPlan plan, bool durable);
    liquid::runtime::task<void> remove_stale_tail(std::string tail);
    liquid::runtime::task<std::string>
    read_main(const std::string& key, std::uint64_t offset, std::uint64_t size);
    liquid::runtime::task<std::vector<std::byte>>
    read_plain(const std::string& key, std::uint64_t offset, std::uint64_t size);

    std::shared_ptr<Shared> shared_;
    std::optional<std::uint64_t> fence_;
    mutable std::mutex mutex_;
    bool directory_loaded_ = false;
    std::map<std::string, std::unique_ptr<Entry>> directory_;
    // Tails no reader needs once the bucket holds what the last barrier acknowledged: removed,
    // without durability, beside the next barrier's writes.
    std::set<std::string> stale_tails_;
    // Tails that become stale only once the next barrier completes (their replacement is not
    // durable yet); promoted to stale_tails_ when it does.
    std::set<std::string> retiring_tails_;
    // Keys with held writes (a small put, buffered appends), in liquid's issue order; each key
    // once, as the position of its earliest held write (lake#527). A write about a key held before
    // another key's held write is sent with everything before it first, so the bucket never holds
    // a later write without an earlier one.
    std::vector<std::string> queue_;
    // What is held, bounded by options.held_bytes_limit and held_puts_limit, in all and per key.
    std::uint64_t held_bytes_ = 0;
    std::size_t held_puts_ = 0;
    std::map<std::string, std::pair<std::uint64_t, std::size_t>> held_by_key_;
    // The plaintext kept in Entry::durable_plain, in all; at most 1 MiB.
    std::uint64_t durable_plain_held_ = 0;
    // One sender at a time, in issue order (SendGuard).
    bool sending_ = false;
    std::condition_variable flushed_;
    // A write was acknowledged without durability since the last barrier: the next barrier sends
    // Zetta's :barrier too.
    bool dirty_ = false;
};

// Runs up to `width` of `jobs` at once on the caller's executor (a barrier's ordered writes and
// its stale-tail removals). After the first failure no further job starts; the answer is the first
// failure, `locked` (the lease passed on) before any other.
liquid::runtime::task<void>
run_object_requests(std::vector<std::function<liquid::runtime::task<void>()>> jobs, std::size_t width);

// Liquid's store_owner over a bucket's lease: acquire waits (in pieces Zetta accepts) up to the
// open's owner_wait and refuses with errc::locked naming the holder; a renewal thread renews every
// ttl/3, and a renewal refused as fenced, or none accepted before the lease's expiry, revokes the
// grant and calls on_lost. release stops renewing and releases the lease.
class ZettaStoreOwner final : public liquid::store_owner {
public:
    // `settle_budget` bounds the sending of what the store's backend holds before release.
    ZettaStoreOwner(std::shared_ptr<ZettaObjectClient> client,
                    std::chrono::milliseconds ttl = std::chrono::seconds(30),
                    std::function<void()> on_lost = {},
                    std::chrono::milliseconds settle_budget = std::chrono::seconds(10));
    ~ZettaStoreOwner() override;

    liquid::runtime::task<liquid::owner_grant> acquire(std::string holder,
                                                      std::chrono::milliseconds wait) override;
    liquid::runtime::task<void> release() override;

    bool held() const;
    // The lease was lost: a renewal was refused or came too late, or Zetta refused a write as
    // fenced. The store must be closed; a new open acquires the lease again.
    bool lost() const;
    std::uint64_t fence() const;
    // Stops renewing without releasing, as a process that stopped would; tests use it to let the
    // lease lapse.
    void stop_renewing();

private:
    void renew_loop();
    void stop_thread();

    std::shared_ptr<ZettaObjectClient> client_;
    std::chrono::milliseconds ttl_;
    std::function<void()> on_lost_;
    std::chrono::milliseconds settle_budget_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread renewer_;
    bool stopping_ = false;
    std::string holder_;
    liquid::owner_grant grant_;
    bool held_ = false;
    std::atomic<bool> lost_{false};
};

} // namespace lake::control
