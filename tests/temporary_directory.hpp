// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace lake::test {

namespace detail {

inline auto process_id() noexcept -> std::uint64_t {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

class ProcessFixtureDirectory final {
public:
    ProcessFixtureDirectory() : ProcessFixtureDirectory(temporary_root()) {}

    explicit ProcessFixtureDirectory(const std::filesystem::path& temporary_root) {
        auto error = std::error_code{};
        auto random = std::random_device{};
        for (auto attempt = std::uint32_t{}; attempt < 128; ++attempt) {
            path_ = temporary_root / ("lake-tests-" + std::to_string(process_id()) + "-" +
                                      std::to_string(random()) + "-" + std::to_string(attempt));
            error.clear();
            if (std::filesystem::create_directory(path_, error)) {
                return;
            }
            if (!error || error == std::errc::file_exists) {
                continue;
            }
            throw std::runtime_error("lake tests require a writable temporary root; cannot create "
                                     "a fixture directory under '" +
                                     temporary_root.string() + "': " + error.message());
        }

        throw std::runtime_error("lake tests could not create a unique fixture directory under '" +
                                 temporary_root.string() + "'");
    }

    ~ProcessFixtureDirectory() {
        auto error = std::error_code{};
        std::filesystem::remove_all(path_, error);
    }

    ProcessFixtureDirectory(const ProcessFixtureDirectory&) = delete;
    auto operator=(const ProcessFixtureDirectory&) -> ProcessFixtureDirectory& = delete;

    [[nodiscard]] auto path() const noexcept -> const std::filesystem::path& {
        return path_;
    }

private:
    static auto temporary_root() -> std::filesystem::path {
        auto error = std::error_code{};
        const auto root = std::filesystem::temp_directory_path(error);
        if (error) {
            throw std::runtime_error("lake tests require a writable temporary root: " +
                                     error.message());
        }
        return root;
    }

    std::filesystem::path path_;
};

} // namespace detail

inline auto process_fixture_root() -> const std::filesystem::path& {
    static auto directory = detail::ProcessFixtureDirectory{};
    return directory.path();
}

inline auto fixture_path(const std::filesystem::path& name) -> std::filesystem::path {
    return process_fixture_root() / name;
}

} // namespace lake::test
