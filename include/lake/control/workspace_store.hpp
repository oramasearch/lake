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

// What a pooled transport has done since it was made (lake#544).
struct ObjectConnectionStats {
    std::uint64_t resolutions = 0; // host name resolutions
    std::uint64_t connections = 0; // TCP connections opened
    std::uint64_t reused = 0;      // requests sent on a kept connection
    std::uint64_t closed = 0;      // kept connections found closed, or idle too long, and dropped
    std::uint64_t overflow = 0;    // connections closed after their answer because the pool was full
};

struct PooledObjectTransportOptions {
    // Idle connections kept per origin (host and port); more are closed after their answer.
    std::size_t idle_per_origin = 32;
    // A connection idle this long is closed rather than reused.
    std::chrono::milliseconds idle_limit{30'000};
    // A resolved address is used for new connections this long; one that refuses is resolved again.
    std::chrono::milliseconds resolve_ttl{30'000};
    // A dial that gets no answer this soon fails, and the address is resolved again next time: a
    // silent dead peer costs this, not the request's whole timeout.
    std::chrono::milliseconds connect_timeout{2'000};
    // How often kept connections past idle_limit, and resolutions past resolve_ttl, are dropped for
    // every origin, including one no longer used.
    std::chrono::milliseconds sweep_interval{10'000};
};

// Lake's client for a workspace Zetta's object routes (lake#544, src/control/object_http.cpp):
// HTTP/1.1 keep-alive connections kept per origin, TCP_NODELAY, and one name resolution per
// origin while fresh. A kept connection is reused only when it is still open and silent; one that
// failed, timed out, or that Zetta answered `Connection: close` on is closed. The transport never
// sends a request twice: retrying an idempotent request stays ZettaObjectClient's. An https URL
// goes through Zetta's installed client unpooled. The bearer never appears in an error.
class PooledObjectTransport : public ObjectTransport {
public:
    virtual ObjectConnectionStats stats() const = 0;
    // Stops the client: every request it holds is answered as a transport failure, and later ones
    // fail at once. The destructor does it.
    virtual void shutdown() = 0;
};
std::shared_ptr<PooledObjectTransport> make_pooled_object_transport(PooledObjectTransportOptions options = {});

// The transport lake gives every workspace store: make_pooled_object_transport().
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

    // Sends what a backend over this client holds and makes it durable (lake#527, lake#535); the
    // owner calls it before it releases the lease, so a closed store leaves nothing unsent or
    // undurable. Set by ZettaObjectBackend; the last one constructed over the client is the open
    // store's.
    // `abandon` lets go of what the backend holds without sending it: the owner calls it instead
    // when the lease was lost or never held.
    void set_before_release(std::function<liquid::runtime::task<void>()> settle,
                            std::function<void()> abandon = {});
    void abandon_before_release();
    // A ZettaStoreOwner over this client will settle or abandon what its backend keeps; without
    // one, a backend lets go of its views when it is destroyed.
    void set_owned(bool owned);
    bool owned() const;
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
    std::function<void()> abandon_;
    std::atomic<bool> owned_{false};
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
    // A put of at most this many bytes is held until the next sync and sent then, in its place
    // among the held writes, as one PUT (lake#527). A larger put is sent at once.
    std::size_t deferred_put_bytes = 1024 * 1024;
    // Held writes past either bound are sent at once (the next sync's :barrier makes them
    // durable), so a long operation holds bounded memory.
    std::size_t held_bytes_limit = 8 * 1024 * 1024;
    std::size_t held_puts_limit = 256;
    // Requests one sync keeps in flight at once after its ordered writes: the :barrier and the
    // removals of stale tails beside it.
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
// is kept as the object `K.lake-tail-<block>-<salt>`, replaced as it grows; a tail whose block
// or salt no longer matches is stale and ignored. Every block is AES-256-GCM with a random nonce,
// its associated data binding the workspace, K, the header, the block's region and index.
//
// Appends are buffered and sent as complete blocks once flush_bytes is held. Small puts are held
// too, across liquid's barriers, until a later write needs them sent or sync() (lake#527,
// lake#535). Every write is sent in liquid's issue order, one at a time, without durability, so
// a crash keeps a prefix of that order; sync() sends what is held and then one Zetta :barrier,
// and lake calls it (make_durable) at the end of each store operation, before it answers. A range
// rewritten with different bytes is refused with errc::conflict: Zetta
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
    // Liquid's barrier: a point in its issue order, which sends nothing (lake#535).
    liquid::runtime::task<void> barrier() override;
    liquid::result<std::shared_ptr<liquid::store_backend>> fenced(std::uint64_t fence) const override;
    // Sends what this view holds, in issue order, then one :barrier when anything was sent since
    // the last one: everything issued before it is durable when it answers (lake#535).
    liquid::runtime::task<void> sync();
    // sync() on this backend and every view of it. Lake calls it at the end of each operation on
    // a workspace store, before it answers, so an acknowledged operation is durable.
    liquid::runtime::task<void> make_durable();

    std::optional<std::uint64_t> fence() const {
        return fence_;
    }
    const ZettaObjectClient& client() const;
    // A mutation through this backend or any of its views failed: liquid's memory of the store
    // may be ahead of the bucket, so the store must be closed and opened again before more use.
    bool failed() const;
    // What this view holds for its next sync: bytes and puts.
    std::pair<std::uint64_t, std::size_t> held() const;
    // Syncs every view of this backend and lets go of them: what they hold is sent and durable
    // (lake#527, lake#535). The owner's release runs it.
    static liquid::runtime::task<void> settle(std::shared_ptr<Shared> shared);

    struct Entry;

private:
    liquid::runtime::task<void> put_impl(std::string key, std::vector<std::byte> bytes);
    liquid::runtime::task<void> put_if_absent_impl(std::string key, std::vector<std::byte> bytes);
    liquid::runtime::task<void>
    put_range_impl(std::string key, std::uint64_t offset, std::vector<std::byte> bytes);
    liquid::runtime::task<void> remove_impl(std::string key);
    liquid::runtime::task<void> barrier_impl();
    liquid::runtime::task<void> sync_impl();
    liquid::result<void> observe(liquid::result<void> outcome, bool tolerated);
    liquid::runtime::task<void> ensure_directory();
    liquid::runtime::task<void> ensure_cipher();
    liquid::runtime::task<void> ensure_header(const std::string& key);
    liquid::runtime::task<void> ensure_open_region(const std::string& key);
    struct FlushPlan;
    struct SendGuard;
    liquid::runtime::task<void> flush_entry(const std::string& key);
    liquid::runtime::task<void> drain();
    liquid::runtime::task<void> keep_order_before_holding(const std::string& key);
    // Under mutex_: `key` has a held write of `bytes` (a put when `put`, which replaces what was
    // held for the key); unhold forgets what is held for `key`.
    void hold(const std::string& key, std::uint64_t bytes, bool put);
    void unhold(const std::string& key);
    // Under mutex_: a removal of `key` may drop its held write and send nothing.
    bool removable_unsent(const std::string& key) const;
    void release_plain(Entry& entry);
    // Something is held or was sent since the last :barrier: the view needs a sync.
    bool unsettled() const;
    // The writes that bring `key` up to date in Zetta, built under mutex_, or nothing.
    liquid::result<std::optional<FlushPlan>> plan_flush(const std::string& key, bool barrier);
    liquid::runtime::task<void> send_plan(FlushPlan plan);
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
    // Tails no reader needs once the bucket holds what the last sync made durable: removed,
    // without durability, beside the next sync's :barrier.
    std::set<std::string> stale_tails_;
    // Tails that become stale only once the next sync completes (their replacement is not
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
    // The plaintext kept in Entry::sent_plain, in all; at most 1 MiB.
    std::uint64_t sent_plain_held_ = 0;
    // One sender at a time, in issue order (SendGuard).
    bool sending_ = false;
    std::condition_variable flushed_;
    // A write was sent since the last :barrier: the next sync sends one.
    bool dirty_ = false;
};

// Runs up to `width` of `jobs` at once on the caller's executor (a sync's :barrier and
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
