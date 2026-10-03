#pragma once

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/control/forge_records.hpp"
#include "lake/control/store.hpp"
#include "lake/control/workspace_records.hpp"

namespace lake::api {

// One repository's forge records (docs/api/forge.md, lake#341), in the workspace Zetta of the
// repository's workspace. The workspace is never chosen by the caller: it is the repository's
// immutable `workspace_id` (lake#315), so every record of one repository is in one place, and a
// caller who names the repository has named the workspace. Every call goes through
// call_workspace, which checks the workspace's space and readiness and refuses rather than falling
// back.
class RepositoryForge {
public:
    template <class T> using Task = liquid::runtime::task<control::ZettaControlResult<T>>;

    // A repository whose workspace is not recorded yet (`needs_selection` in migration 0051) has
    // nowhere to keep forge records: `conflict` at /repository_id until an operator assigns one.
    static std::expected<RepositoryForge, ApiError> open(const ApiContext& context,
                                                         control::Repository repository);

    const control::Repository& repository() const {
        return repository_;
    }

    // A new record of `kind`, `if_absent` under a new id (or the given one: a copy keeps the row's
    // id and creation instant, so a rerun is a conflict and never a duplicate).
    Task<control::ForgeRecord> create(std::string kind, nlohmann::json value,
                                      control::RecordWrite write = {});
    // A copied row as it was, `updated_at` included.
    Task<control::ForgeRecord> copy(control::ForgeRecord record);
    // The newest `limit` records of `kind` (every kind when empty), newest first; `limit` <= 0 is
    // all of them. One `list_records` per kind.
    Task<std::vector<control::ForgeRecord>> list(std::string kind, int limit);
    // One record of this repository by id; nullopt when it does not exist here.
    Task<std::optional<control::ForgeRecord>> get(std::string id);
    // Replaces the value of this repository's record `id` of `kind` with `change(value)`, written
    // only at the revision read and reread on a conflict; nullopt when there is no such record.
    Task<std::optional<control::ForgeRecord>>
    update(std::string id, std::string kind,
           std::function<nlohmann::json(nlohmann::json)> change, int attempts = 5);

private:
    RepositoryForge(control::WorkspaceRecords records, control::Repository repository)
        : records_(std::move(records)), repository_(std::move(repository)) {}

    control::WorkspaceRecords records_;
    control::Repository repository_;
};

// The repository a forge call names (`not_found` at /repository_id when there is none).
std::expected<control::Repository, ApiError> forge_repository(const ApiContext& context,
                                                              const std::string& repository_id);

// A workspace record failure as a lake error. The engine's field names are not the caller's, so
// no path is carried.
ApiResult forge_records_failure(const control::ZettaControlError& error);

} // namespace lake::api
