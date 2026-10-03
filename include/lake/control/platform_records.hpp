#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/store.hpp"
#include "lake/control/workspace_records.hpp"

namespace lake::control {

// Repository environments and the platform mirror (docs/api/platform.md, lake#358, lake#359), in
// the workspace Zetta of each repository's workspace (`repositories.workspace_id`, lake#315). Two
// sealed collections, both found by the repository (`repository:<id>`), so a repository's
// environments page reads each once:
//
// - `lake_platform_environments`: one record per environment, keyed by its id,
//   platform_environment_id(repository, name), and written `if_absent`, so two replicas that see
//   an environment first at the same time create it once. The environment's `environment` policy
//   object stays in the control store (its external id is that same id), so authorization never
//   reads the workspace.
// - `lake_platform_mirror`: the last operator-control row lake observed for each deployment,
//   environment value and domain, keyed by platform_mirror_id(kind, key). A row is replaced only
//   by a newer one (by `updated_at`, `ordering_token`, then the canonical row), at the engine
//   revision read, and reread on a conflict.
//
// Lake's PostgreSQL keeps neither; the retired tables are read only by the operator's one-shot
// copy (copy_workspace_records).
struct PlatformEnvironmentCodec {
    static constexpr RecordCollection collection{"platform_environments", 1};
};
struct PlatformMirrorCodec {
    static constexpr RecordCollection collection{"platform_mirror", 1};
};

// md5(repository id, newline, name) as a UUID: an environment's id, its record key and its policy
// object's external id. Migration 0060 computes the same for environments made before the move.
std::string platform_environment_id(const std::string& repository_id, const std::string& name);
// md5(kind, newline, key) as a UUID: a mirror row's record key and workspace_assignments id.
std::string platform_mirror_id(const std::string& kind, const std::string& key);
// The lookup of one repository's records in both collections.
std::string platform_record_lookup(const std::string& repository_id);

// The kinds of mirror row: the retired table's platform_mirror_kind_check.
bool is_platform_mirror_kind(std::string_view kind);

// The total order of operator-control rows (docs/api/platform.md): `updated_at` (seconds-only
// timestamps expanded to six fractional digits), then `ordering_token`, then the canonical row.
std::tuple<std::string, std::string, std::string> platform_row_order(const nlohmann::json& row);
bool platform_row_is_newer(const nlohmann::json& candidate, const nlohmann::json& current);

// One repository's environments, by name. Records that do not open here, or name another
// repository, are skipped.
liquid::runtime::task<ZettaControlResult<std::vector<PlatformEnvironment>>>
list_platform_environment_records(WorkspaceRecords& records, std::string repository_id);

// One environment of the repository; nullopt when this workspace has none by that name.
liquid::runtime::task<ZettaControlResult<std::optional<PlatformEnvironment>>>
get_platform_environment_record(WorkspaceRecords& records, std::string repository_id,
                                std::string name);

// Writes an environment under its derived id, `if_absent` (`at_us` 0: now). Answers the
// environment the workspace holds: this one, or the one another writer created first.
liquid::runtime::task<ZettaControlResult<PlatformEnvironment>>
put_platform_environment_record(WorkspaceRecords& records, PlatformEnvironment environment);

// One repository's mirror rows, every kind. Rows that do not open here, name another repository
// or do not sit under their own key are skipped.
liquid::runtime::task<ZettaControlResult<std::vector<PlatformMirrorRow>>>
list_platform_mirror_records(WorkspaceRecords& records, std::string repository_id);

// Writes a mirror row of `repository_id` unless the workspace already holds it or a newer one:
// `if_absent`, or at the revision read when the stored row is older, rereading on a conflict up to
// `attempts` times. `at_us` is the instant it was seen (0: now). True when written.
liquid::runtime::task<ZettaControlResult<bool>>
put_platform_mirror_record(WorkspaceRecords& records, std::string repository_id,
                           PlatformMirrorRow row, std::int64_t at_us = 0, int attempts = 5);

} // namespace lake::control
