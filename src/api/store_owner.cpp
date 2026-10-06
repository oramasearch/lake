#include "lake/api/store_owner.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <string>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <openssl/rand.h>

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

std::expected<StoreOwnerLock, std::string> StoreOwnerLock::acquire(
    const std::filesystem::path& store_root, std::string_view node_name, const StoreOwnerWait& wait) {
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
    const auto deadline = std::chrono::steady_clock::now() + wait.limit;
    auto pause = std::chrono::milliseconds(5);
    bool reported = false;
    for (;;) {
        if (::flock(descriptor, LOCK_EX | LOCK_NB) == 0) {
            break;
        }
        const int lock_error = errno;
        if (lock_error == EINTR) {
            continue;
        }
        if (lock_error != EWOULDBLOCK) {
            ::close(descriptor);
            return std::unexpected(std::format("could not lock the store owner lock {}: {}",
                                               path.string(),
                                               std::strerror(lock_error)));
        }
        if (!reported) {
            reported = true;
            if (wait.on_held) {
                wait.on_held(holder_description(descriptor));
            }
        }
        const auto now = std::chrono::steady_clock::now();
        const bool abandoned = wait.abandoned && wait.abandoned();
        if (abandoned || now >= deadline) {
            const std::string holder = holder_description(descriptor);
            ::close(descriptor);
            if (abandoned) {
                return std::unexpected(std::format(
                    "stopped while waiting for the store root {} held by {}", store_root.string(), holder));
            }
            return std::unexpected(std::format(
                "the store root {} is still in use by another lake process after waiting {} ms: "
                "{}. One lake process owns a store root at a time, because lake's search index, "
                "registry indexes, TUF metadata and timestamps under it are single-process state; "
                "a successor waits for the owner to drain and exit. Raise "
                "LAKE_STORE_OWNER_WAIT_SECONDS above the owner's drain, or stop the other process "
                "(see docs/deployment/store-volume.md, lake#479)",
                store_root.string(),
                wait.limit.count(),
                holder));
        }
        std::this_thread::sleep_for(
            std::min(pause, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
        pause = std::min(pause * 2, std::chrono::milliseconds(100));
    }
    StoreOwnerLock lock(descriptor);
    std::array<unsigned char, 8> random{};
    std::string token;
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) == 1) {
        for (const unsigned char value : random) {
            token += std::format("{:02x}", value);
        }
    }
    const std::string since = std::format(
        "{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    const std::string record = nlohmann::json{{"pid", static_cast<long long>(::getpid())},
                                              {"host", host_name()},
                                              {"node", std::string(node_name)},
                                              {"since", since},
                                              {"token", token}}
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

namespace {

std::string read_small_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::string text(4096, '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(input.gcount()));
    return text;
}

std::filesystem::path draining_path(const std::filesystem::path& store_root) {
    return store_root / ".lake-owner.draining";
}

} // namespace

bool StoreOwnerLock::announce_draining(const std::filesystem::path& store_root) const noexcept try {
    if (descriptor_ < 0) {
        return false;
    }
    std::array<char, 1024> buffer{};
    const ssize_t count = ::pread(descriptor_, buffer.data(), buffer.size(), 0);
    if (count <= 0) {
        return false;
    }
    const std::filesystem::path target = draining_path(store_root);
    const std::filesystem::path staged = target.string() + ".tmp";
    {
        std::ofstream output(staged, std::ios::binary | std::ios::trunc);
        output.write(buffer.data(), count);
        if (!output) {
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(staged, target, error);
    return !error;
} catch (...) {
    return false;
}

bool StoreOwnerLock::owner_is_draining(const std::filesystem::path& store_root) noexcept try {
    const std::string marker = read_small_file(draining_path(store_root));
    if (marker.empty() || marker.find("\"token\":\"\"") != std::string::npos) {
        return false;
    }
    return marker == read_small_file(lock_path(store_root));
} catch (...) {
    return false;
}

void StoreOwnerLock::clear_draining(const std::filesystem::path& store_root) noexcept {
    std::error_code error;
    std::filesystem::remove(draining_path(store_root), error);
}

void StoreOwnerLock::release() noexcept {
    if (descriptor_ >= 0) {
        static_cast<void>(::flock(descriptor_, LOCK_UN));
        static_cast<void>(::close(descriptor_));
        descriptor_ = -1;
    }
}

} // namespace lake::api
