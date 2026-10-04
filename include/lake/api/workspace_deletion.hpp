#pragma once

#include <cstddef>
#include <expected>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/api/registry.hpp"
#include "lake/control/store.hpp"

// Deleting a workspace cascades (decision 54 S12, lake#514, docs/api/workspaces.md#deletion).
//
// One PostgreSQL transaction (Store::delete_workspace_cascade) tombstones the workspace's
// repositories with their retention ended, deletes its apps, cancels its unfinished builds, sets
// the workspace's desired state to `deleted` (the operator then deletes its Zetta, and with it the
// workspace's record store and audit chain) and records what it destroyed in
// `workspace_deletions`. What lives outside PostgreSQL is removed after the commit by
// finish_workspace_deletion, which is idempotent: a crash part way through leaves the deletion
// without `completed_at`, and the owner loop (resume_workspace_deletions) or a repeated
// delete_workspace finishes it.
namespace lake::api {

// Where a workspace's content lives outside PostgreSQL, so that deleting the workspace can remove
// it. The HTTP server, which owns the instance registry, injects it through
// ApiContext::workspace_content. Every member may be empty when this lake has no such storage, and
// every action is idempotent.
// One tag of a tenant repository that the instance registry still holds, written before lake#383
// moved tenant manifests and tags into workspace records: `repository` is `<space>/<name>`.
struct LegacyImageTag {
    std::string repository, tag, digest, content_type;
};

struct WorkspaceContent {
    // Written before lake#383: the tags one OCI repository still holds in the instance registry.
    std::function<std::vector<std::string>(const std::string& repository)> image_tags;
    // Written before lake#383: removes one OCI repository's tags and manifest pointers from the
    // instance registry. Blobs stay: the registry has no collection yet, and other manifests may
    // share them. Since lake#383 phase A a routed repository's manifests and tags are records in
    // its workspace Zetta and go with it, and its central route is deleted in the cascade
    // transaction; phase B (lake#380) moves its blobs into the workspace store.
    std::function<std::expected<void, std::string>(const std::string& repository)>
        remove_image_repository;
    // The one-shot copy of lake#383 (copy_workspace_records, collection oci_tags): the instance
    // registry's tenant tags ordered by `<repository>:<tag>`, the `limit` after `after`.
    std::function<std::vector<LegacyImageTag>(const std::string& after, int limit)>
        legacy_image_tags;
    // A blob's bytes (a manifest's document) and its size; the error says why it is missing.
    std::function<std::expected<std::string, std::string>(const std::string& digest)> read_blob;
    std::function<std::optional<std::uint64_t>(const std::string& digest)> blob_size;
    // Drops one repository's search segments (lake#381 moves them beside the workspace store).
    // Unset: ApiContext::search is used directly.
    std::function<std::expected<void, std::string>(const std::string& customer_id,
                                                   const std::string& repository_id)>
        drop_repository_index;
    // lake#380: closes the workspace's liquid store and releases its bucket lease before the
    // deletion is reported complete. Unset until repositories move into the workspace Zetta; the
    // operator deleting the workspace's namespace and volume then destroys the bytes.
    std::function<std::expected<void, std::string>(const std::string& workspace_id)>
        close_workspace_store;
};

// The tenant OCI repositories that belong to a workspace: `<space>/<name>` and
// `<space>/cache/<name>` for each of its apps and for each image its builds target. A name that an
// app or build of another live workspace of the same space also uses is left out, since images are
// addressed per space until lake#383 routes them per workspace.
std::vector<std::string> workspace_image_candidates(control::Store& store,
                                                    const control::Workspace& workspace);

// What deleting `workspace` would destroy, with counts: the dry-run answer of delete_workspace and
// the content of the typed-name confirmation on /workspaces.
std::expected<nlohmann::json, ApiError> workspace_deletion_plan(const ApiContext& context,
                                                                const control::Workspace& workspace);

// What a recorded deletion destroyed, with counts and whether lake has completed it.
nlohmann::json workspace_deletion_json(const control::WorkspaceDeletion& deletion);

// The request id the central audit events of one workspace deletion carry, which also makes each
// event's recording idempotent: an event is recorded once per (object, request id).
std::string workspace_deletion_request_id(const std::string& workspace_id);

// Removes, for one recorded deletion, what lives outside PostgreSQL and records one central audit
// event per destroyed object plus one for the completed cascade, then marks it complete. Safe to
// repeat at any point.
std::expected<void, std::string> finish_workspace_deletion(const ApiContext& context,
                                                           const control::WorkspaceDeletion& deletion);

// One owner-loop pass: finishes every deletion lake has not completed. Answers how many it
// completed; the first failure is reported after the others have been tried.
std::expected<std::size_t, std::string> resume_workspace_deletions(const ApiContext& context);

} // namespace lake::api
