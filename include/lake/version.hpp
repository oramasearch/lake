#pragma once
#include <string_view>

namespace lake {

struct version {
    unsigned major;
    unsigned minor;
    unsigned patch;
};

[[nodiscard]] constexpr version current_version() noexcept {
    return {0, 7, 1};
}
[[nodiscard]] std::string_view version_string() noexcept;
[[nodiscard]] std::string_view build_revision_string() noexcept;

} // namespace lake
