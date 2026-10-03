#include "lake/api/store_owner.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace lake::api {
namespace {

std::string holder_description(int descriptor) {
    std::array<char, 1024> buffer{};
    const ssize_t count = ::pread(descriptor, buffer.data(), buffer.size() - 1, 0);
    if (count <= 0) {
        return "an unidentified process";
    }
    const nlohmann::json holder =
        nlohmann::json::parse(std::string(buffer.data(), static_cast<std::size_t>(count)),
                              nullptr,
                              false);
    if (!holder.is_object()) {
        return "an unidentified process";
    }
    return std::format("pid {} on host {} (node {}, since {})",
                       holder.value("pid", 0),
                       holder.value("host", std::string("unknown")),
                       holder.value("node", std::string("unknown")),
                       holder.value("since", std::string("unknown")));
}

std::string host_name() {
    std::array<char, 256> buffer{};
    if (::gethostname(buffer.data(), buffer.size() - 1) != 0) {
        return "unknown";
    }
    return buffer.data();
}

} // namespace

std::filesystem::path StoreOwnerLock::lock_path(const std::filesystem::path& store_root) {
    return store_root / ".lake-owner.lock";
}

std::expected<StoreOwnerLock, std::string>
StoreOwnerLock::acquire(const std::filesystem::path& store_root, std::string_view node_name) {
    std::error_code error;
    std::filesystem::create_directories(store_root, error);
    if (error) {
        return std::unexpected(
            std::format("could not create store root {}: {}", store_root.string(), error.message()));
    }
    const std::filesystem::path path = lock_path(store_root);
    // O_CLOEXEC: a child process must never inherit, and so outlive us holding, the lock.
    const int descriptor = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return std::unexpected(std::format(
            "could not open the store owner lock {}: {}", path.string(), std::strerror(errno)));
    }
    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        const int lock_error = errno;
        if (lock_error != EWOULDBLOCK) {
            ::close(descriptor);
            return std::unexpected(std::format("could not lock the store owner lock {}: {}",
                                               path.string(),
                                               std::strerror(lock_error)));
        }
        const std::string holder = holder_description(descriptor);
        ::close(descriptor);
        return std::unexpected(std::format(
            "the store root {} is in use by another lake process: {}. One lake process owns a "
            "store root at a time, because the liquid customer stores keep their pack position, "
            "object map and index in process memory and a second writer would lose objects. "
            "Stop the other process first; a Lake whose store is one ReadWriteOnce volume rolls "
            "with Recreate (see docs/deployment/store-volume.md, lake#467)",
            store_root.string(),
            holder));
    }
    StoreOwnerLock lock(descriptor);
    const std::string since = std::format(
        "{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    const std::string record = nlohmann::json{{"pid", static_cast<long long>(::getpid())},
                                              {"host", host_name()},
                                              {"node", std::string(node_name)},
                                              {"since", since}}
                                   .dump() +
                               "\n";
    // The record only names the holder in a refusal; failing to write it does not weaken the lock.
    if (::ftruncate(descriptor, 0) == 0) {
        static_cast<void>(::pwrite(descriptor, record.data(), record.size(), 0));
    }
    return lock;
}

StoreOwnerLock::StoreOwnerLock(StoreOwnerLock&& other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)) {}

StoreOwnerLock& StoreOwnerLock::operator=(StoreOwnerLock&& other) noexcept {
    if (this != &other) {
        release();
        descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
}

StoreOwnerLock::~StoreOwnerLock() {
    release();
}

void StoreOwnerLock::release() noexcept {
    if (descriptor_ >= 0) {
        static_cast<void>(::flock(descriptor_, LOCK_UN));
        static_cast<void>(::close(descriptor_));
        descriptor_ = -1;
    }
}

} // namespace lake::api
