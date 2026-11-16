// SPDX-License-Identifier: MIT

#pragma once

#include <expected>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "lake/api/capabilities.hpp"
#include "lake/api/sync_store.hpp"
#include "lake/control/store.hpp"

namespace lake::api {

// The access a repository's own path objects give its paths (lake#975). A path object
// that does not inherit cuts its folder or file off from the repository's grants, and Drive has
// always decided a path by the nearest object that counts (drive_capabilities.cpp,
// `drive_location_at`). Every other read of repository content (repository_file,
// repository_tree, change_diff, list_conflicts, search, Drive search, and the bulk reads pull,
// read_chunk and export_repository) authorized only the repository, so a repository reader saw
// what such a cut protects. This class decides each path the way Drive does, from one read of the
// repository's path objects and one batch authorization, so every surface answers the same.
class RepositoryPathAccess {
public:
    // Reads every path object under the repository's own object. Refuses (`store_error`) a
    // repository whose path objects cannot all be read, so a cut is never missed. `ref` is the
    // revision read, for the source credentials' exemption.
    static std::expected<RepositoryPathAccess, ApiError>
    load(ApiContext& context, const control::Repository& repository, std::string_view ref = {});

    // True when the caller needs no per-path check: no path object of the repository cuts
    // inheritance (so a repository reader reads every path), or the caller is the operator, or a
    // build or app source credential reading the revision it was minted for.
    bool unrestricted() const {
        return exempt_ || cuts_.empty();
    }

    // The paths of `paths` the caller may not read, decided from `tree`, the repository's tree
    // at the ref those paths come from. A path that is not in `tree` is unreadable: its node, and
    // so whether a cut covers it, is unknown (fail closed). The caller must already hold `read`
    // on the repository.
    std::expected<std::set<std::string>, ApiError>
    unreadable(ApiContext& context,
               const std::vector<RepositoryTreeEntry>& tree,
               const std::vector<std::string>& paths) const;

    // The entries of `tree` the caller may read.
    std::expected<std::vector<RepositoryTreeEntry>, ApiError>
    readable_entries(ApiContext& context, std::vector<RepositoryTreeEntry> tree) const;

    // Whether the caller may read every cut of the repository, which a read of the whole
    // repository (a pull, a chunk, an export) needs: liquid cannot leave a folder out of them.
    std::expected<bool, ApiError> reads_every_cut(ApiContext& context) const;

private:
    bool exempt_ = false;
    std::string repository_object_;
    // This repository's path objects by the node id their external id names.
    std::map<std::string, control::PolicyObject> by_node_;
    // The ids of its path objects that do not inherit.
    std::vector<std::string> cuts_;
};

// Whether a caller may read a repository's content without per-path checks: the operator, or a
// build or app source credential for that repository and revision.
bool repository_path_access_exempt(const ApiContext& context,
                                   const control::Repository& repository,
                                   std::string_view ref);

} // namespace lake::api
