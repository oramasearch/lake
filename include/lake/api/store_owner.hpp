#pragma once

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

// One lake process owns a store root (LAKE_STORE_ROOT) at a time.
//
// The libliquid customer stores keep their pack append position, object map and chunk index in
// process memory (liquid#321 now locks each one), and lake keeps its own single-process state
// under the root: search segments, the OCI/npm/Python registry indexes, TUF metadata and
// timestamp evidence (lake#467, docs/deployment/store-volume.md). The server takes this exclusive
// flock on <root>/.lake-owner.lock before it touches the root. A successor waits, bounded, for
// the current owner to drain and release it (lake#479). The kernel releases the lock when the
// owning process exits, however it exits.
namespace lake::api {

// How a successor waits for the current owner.
struct StoreOwnerWait {
    // Zero makes one attempt.
    std::chrono::milliseconds limit{0};
    // Polled between attempts; true abandons the wait (the process is stopping).
    std::function<bool()> abandoned;
    // Called once, with the holder's description, when the first attempt finds the root held.
    std::function<void(const std::string& holder)> on_held;
};

class StoreOwnerLock {
public:
    // The lock file under a store root.
    static std::filesystem::path lock_path(const std::filesystem::path& store_root);

    // Takes the lock, retrying (5 ms, doubling to 100 ms) until wait.limit has elapsed. On
    // success the lock file names this process (pid, host, node, start time). When another
    // process still holds it at the limit, the error names that holder and says why lake does not
    // share the root.
    static std::expected<StoreOwnerLock, std::string> acquire(const std::filesystem::path& store_root,
                                                               std::string_view node_name,
                                                               const StoreOwnerWait& wait = {});

    StoreOwnerLock() = default;
    StoreOwnerLock(StoreOwnerLock&& other) noexcept;
    StoreOwnerLock& operator=(StoreOwnerLock&& other) noexcept;
    StoreOwnerLock(const StoreOwnerLock&) = delete;
    StoreOwnerLock& operator=(const StoreOwnerLock&) = delete;
    ~StoreOwnerLock();

    [[nodiscard]] bool held() const noexcept {
        return descriptor_ >= 0;
    }
    void release() noexcept;

    // lake#536: the owner announces that it is draining. It writes its own lock record to
    // <root>/.lake-owner.draining, after it stopped opening workspace stores, so a successor may
    // take a free workspace store lease before the root is handed over. The record carries a
    // random token, so the marker names this lock and no later or earlier one.
    bool announce_draining(const std::filesystem::path& store_root) const noexcept;
    // True when the root's current owner announced that it is draining: the marker holds the
    // same record as the lock file. A marker an earlier owner left never matches.
    static bool owner_is_draining(const std::filesystem::path& store_root) noexcept;
    // Removed by the next owner once it holds the lock.
    static void clear_draining(const std::filesystem::path& store_root) noexcept;

private:
    explicit StoreOwnerLock(int descriptor) noexcept : descriptor_(descriptor) {}

    int descriptor_ = -1;
};

} // namespace lake::api
