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

// Cumulative counters for one workspace store, read by tests and the latency measurement.
struct ObjectStoreCounters {
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
                      std::shared_ptr<ObjectStoreCounters> counters = {});

    const std::string& bucket() const {
        return bucket_;
    }
    const std::shared_ptr<ObjectStoreCounters>& counters() const {
        return counters_;
    }

    // create_bucket {bucket, quota_bytes}: idempotent for an identical definition. A zero quota
    // leaves the bucket without one. An existing bucket with another quota is accepted as is.
    liquid::runtime::task<void> create_bucket(std::uint64_t quota_bytes);
    liquid::runtime::task<void>
    put(std::string key, std::string bytes, std::optional<std::uint64_t> fence, bool if_absent);
    liquid::runtime::task<void> append(std::string key,
                                       std::uint64_t offset,
                                       std::string bytes,
                                       std::optional<std::uint64_t> fence);
    // Exactly [offset, offset + size); size 0 answers an empty string without a request.
    liquid::runtime::task<std::string> get(std::string key, std::uint64_t offset, std::uint64_t size);
    liquid::runtime::task<std::string> get_all(std::string key);
    liquid::runtime::task<void> remove(std::string key, std::optional<std::uint64_t> fence);
    // Every key under prefix with its size, by key, following next_after.
    liquid::runtime::task<std::vector<ObjectListEntry>> list(std::string prefix);
    liquid::runtime::task<void> barrier();
    liquid::runtime::task<ObjectUsage> usage();
    liquid::runtime::task<ObjectLease>
    acquire(std::string holder, std::chrono::milliseconds ttl, std::chrono::milliseconds wait);
    liquid::runtime::task<ObjectLease>
    renew(std::string holder, std::uint64_t fence, std::chrono::milliseconds ttl);
    liquid::runtime::task<void> release(std::string holder, std::uint64_t fence);

private:
    liquid::runtime::task<ObjectResponse> send(ObjectRequest request, bool idempotent);
    std::string object_url(std::string_view key) const;
    std::string bucket_url(std::string_view suffix) const;

    std::shared_ptr<ObjectTransport> transport_;
    ObjectEndpoint endpoint_;
    std::string bucket_;
    std::shared_ptr<ObjectStoreCounters> counters_;
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
    // Reads fetch aligned windows of ciphertext this large and keep them.
    std::size_t window_bytes = 4 * 1024 * 1024;
    // The window cache's bound, per workspace store.
    std::size_t cache_bytes = 64 * 1024 * 1024;
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
// barrier() answers. A range rewritten with different bytes is refused with errc::conflict: Zetta
// keeps objects append-only, so liquid's index merge, which rewrites a run's header, needs
// liquid#332. Reads go through a bounded cache of aligned ciphertext windows keyed by the object
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
    liquid::runtime::task<void> flush_entry(const std::string& key, bool barrier);
    liquid::runtime::task<std::string>
    read_main(const std::string& key, std::uint64_t offset, std::uint64_t size);
    liquid::runtime::task<std::vector<std::byte>>
    read_plain(const std::string& key, std::uint64_t offset, std::uint64_t size);
    liquid::runtime::task<void> remove_stale_tails();

    std::shared_ptr<Shared> shared_;
    std::optional<std::uint64_t> fence_;
    std::mutex mutex_;
    bool directory_loaded_ = false;
    std::map<std::string, std::unique_ptr<Entry>> directory_;
    std::set<std::string> stale_tails_;
    bool dirty_ = false;
};

// Liquid's store_owner over a bucket's lease: acquire waits (in pieces Zetta accepts) up to the
// open's owner_wait and refuses with errc::locked naming the holder; a renewal thread renews every
// ttl/3, and a renewal refused as fenced, or none accepted before the lease's expiry, revokes the
// grant and calls on_lost. release stops renewing and releases the lease.
class ZettaStoreOwner final : public liquid::store_owner {
public:
    ZettaStoreOwner(std::shared_ptr<ZettaObjectClient> client,
                    std::chrono::milliseconds ttl = std::chrono::seconds(30),
                    std::function<void()> on_lost = {});
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
