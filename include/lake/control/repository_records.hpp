#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/store.hpp"
#include "lake/control/workspace_records.hpp"

namespace lake::control {

// A repository's settings (lake#315 part 2): its description, and later settings
// (default branch, topics) under the same schema version rules, kept in the workspace Zetta of the
// repository's workspace (docs/architecture/workspace-records.md). Lake's PostgreSQL keeps only the
// routing row (`repositories`: id, space, workspace, name, visibility, tombstone), which finds the
// workspace and authorizes before any workspace is read; the retired `description` column is read
// only by the operator's one-shot copy.
//
// One record per repository, keyed by the repository id, found by its space (`customer:<id>`): a
// workspace belongs to one space, so one list_records reads the settings of every repository the
// workspace holds, and a listing page pays one call per workspace rather than one per row. The
// body names the repository and its space, and a record under another key or of another space is
// not this repository's. A change is a write conditional on the revision read, reread on a
// conflict. RepositorySettings is in store.hpp, beside Repository.
struct RepositorySettingsCodec {
    using Value = RepositorySettings;
    static constexpr RecordCollection collection{"repositories", 1};

    static std::string lookup(const RepositorySettings& settings);
    static nlohmann::json encode(const RepositorySettings& settings);
    static std::optional<RepositorySettings> decode(const StoredRecord& record);
};

// The lookup of a space's repository settings.
std::string repository_settings_lookup(const std::string& customer_id);

// A repository's settings; nullopt when there are none here (or they belong to another repository
// or space, or do not open in this workspace). An empty `customer_id` accepts any space: only the
// one-shot copy reads that way, and its verify compares the space.
liquid::runtime::task<ZettaControlResult<std::optional<RepositorySettings>>>
get_repository_settings(WorkspaceRecords& records, std::string repository_id,
                        std::string customer_id);

// Writes a new repository's settings (or copied ones) only where none exist: `conflict`
// otherwise. `settings.at_us` 0 is now.
liquid::runtime::task<ZettaControlResult<RepositorySettings>>
create_repository_settings(WorkspaceRecords& records, RepositorySettings settings);

// Changes a repository's settings: reads them, applies `change`, and writes them conditionally on
// the revision read, rereading and applying it again on a conflict, up to `attempts` times. A
// repository with no record yet gets one (`if_absent`, so a concurrent first write is reread too).
using RepositorySettingsChange = std::function<void(RepositorySettings&)>;
liquid::runtime::task<ZettaControlResult<RepositorySettings>>
update_repository_settings(WorkspaceRecords& records, std::string repository_id,
                           std::string customer_id, RepositorySettingsChange change,
                           int attempts = 5);

// Every repository settings record of a space in this workspace.
liquid::runtime::task<ZettaControlResult<std::vector<RepositorySettings>>>
list_repository_settings(WorkspaceRecords& records, std::string customer_id);

} // namespace lake::control
