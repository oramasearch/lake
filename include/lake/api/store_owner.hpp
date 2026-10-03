#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

// One lake process owns a store root (LAKE_STORE_ROOT) at a time.
//
// The libliquid customer stores under the root keep their pack append position, object map and
// chunk index in process memory and publish them by overwriting shared files, so two processes
// writing one store lose each other's objects (lake#467, docs/deployment/store-volume.md).
// ApiServer::start takes this exclusive flock on <root>/.lake-owner.lock before it touches the
// root and refuses to start when another process holds it. The kernel releases the lock when the
// owning process exits, however it exits.
namespace lake::api {

class StoreOwnerLock {
public:
    // The lock file under a store root.
    static std::filesystem::path lock_path(const std::filesystem::path& store_root);

    // Takes the lock without waiting. On success the lock file names this process (pid, host,
    // node, start time). When another process holds it, the error names that holder and says
    // why lake refuses to share the root.
    static std::expected<StoreOwnerLock, std::string> acquire(const std::filesystem::path& store_root,
                                                               std::string_view node_name);

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

private:
    explicit StoreOwnerLock(int descriptor) noexcept : descriptor_(descriptor) {}

    int descriptor_ = -1;
};

} // namespace lake::api
