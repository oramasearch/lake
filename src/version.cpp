// SPDX-License-Identifier: MIT

#include "lake/version.hpp"

#ifndef LAKE_BUILD_REVISION
#define LAKE_BUILD_REVISION "unknown"
#endif

namespace lake {
std::string_view version_string() noexcept {
    return "0.9.0";
}
std::string_view build_revision_string() noexcept {
    return LAKE_BUILD_REVISION;
}
} // namespace lake
