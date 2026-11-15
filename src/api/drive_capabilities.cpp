#include "lake/api/capabilities.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "lake/api/drive.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/drive_files.hpp"
#include "lake/control/drive_share_links.hpp"
#include "lake/control/drives.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/store.hpp"
#include "lake/control/workspace_records.hpp"
#include "lake/search/index.hpp"
#include "lake_attestation.hpp"
#include "repository_people.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

struct DriveLocation {
    control::Repository repository;
    // What authorization checks: the nearest object on the path whose stored parents are the
    // path's own folders at this ref (lake#929).
    control::PolicyObject object;
    std::string node_id;
    std::string path;
    // The parent a direct child's own object must name for that object to count: the nearest
    // object on the path. Empty when that object's own parents are not the path's.
    std::string child_parent_id;
};

std::string token_hash(std::string_view token);

ApiResult store_error(const control::StoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

ApiResult sync_error(const SyncStoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

std::string path_external_id(std::string_view repository_id, std::string_view node_id) {
    return std::string(repository_id) + ":" + std::string(node_id);
}

// The policy object of one node of a repository, found by its derived id. The id is unique across
// every account, so an object holding it that does not sit under the repository's own object was
// made elsewhere to claim the node (lake#913): it is never the node's object. It is reported as
// `conflict`, so a reader falls back to the repository's own object and a caller that would create
// the object refuses instead of adopting it.
control::Result<control::PolicyObject> repository_path_object(control::Store& store,
                                                              const std::string& repository_object,
                                                              std::string_view repository_id,
                                                              std::string_view node_id) {
    auto object = store.find_policy_object("path", path_external_id(repository_id, node_id));
    if (!object) {
        return object;
    }
    const auto within = control::object_within(store, object->id, repository_object);
    if (!within) {
        return std::unexpected(within.error());
    }
    if (!*within) {
        return std::unexpected(control::StoreError{
            "conflict", "the policy object for this folder belongs to another repository"});
    }
    return object;
}

// A node with no object of its own, or only another repository's: either way the folder's
// authority comes from above it.
bool no_own_object(const control::StoreError& error) {
    return error.code == "not_found" || error.code == "conflict";
}

std::string parent_path(std::string path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

const RepositoryTreeEntry* find_entry(const std::vector<RepositoryTreeEntry>& entries,
                                      std::string_view path) {
    const auto found = std::ranges::find(entries, path, &RepositoryTreeEntry::path);
    return found == entries.end() ? nullptr : &*found;
}

// A repository nobody has written to yet has no ref at all, so `main` does not resolve; the first
// change written to `main` creates it (lake#560).
bool main_is_unborn(const json& input, ApiContext& context, const control::Repository& repository) {
    if (input.value("ref", "") != "main") {
        return false;
    }
    const auto refs = context.sync_store->repository_refs(repository.customer_id, repository.id);
    return refs && refs->empty();
}

// `unborn_main_is_empty`: an upload reads a repository whose `main` is not born yet as an empty
// tree, since its change creates `main` (lake#560). Every other caller is answered `not_found`.
std::expected<std::vector<RepositoryTreeEntry>, ApiError>
repository_tree(const json& input,
                ApiContext& context,
                control::Repository& repository,
                bool unborn_main_is_empty = false) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    if (!context.sync_store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no repository store is configured", {}});
    }
    const auto found = context.store->get_repository(input.at("repository_id"));
    if (!found) {
        return std::unexpected(
            ApiError{found.error().code, found.error().message, "/repository_id"});
    }
    if (!found->deleted_at.empty()) {
        return std::unexpected(ApiError{"conflict", "repository is tombstoned", {}});
    }
    repository = *found;
    const auto entries = context.sync_store->repository_tree(
        repository.customer_id, repository.id, input.at("ref").get<std::string>());
    if (!entries) {
        if (unborn_main_is_empty && entries.error().code == "not_found" &&
            main_is_unborn(input, context, repository)) {
            return std::vector<RepositoryTreeEntry>{};
        }
        return std::unexpected(ApiError{entries.error().code, entries.error().message, "/ref"});
    }
    return *entries;
}

// Where a drive path's access is decided. A folder's policy object keeps the parent it was given,
// but folders move (move_drive_path, a pushed change) and a ref can hold another layout, so the
// stored parent is followed only while it is the folder above at this ref (lake#929). Every object
// on the path is read, top down: one that inherits counts only when its parent is the object
// above it (the repository's for the first) and that one counted too; one that does not inherit
// stands alone. The answer is the nearest object that counts, so a parent left over from where a
// folder used to be, or one it never had, loses the grants below it and never adds its own.
std::expected<DriveLocation, ApiError>
drive_location_at(const control::Repository& repository,
                  const std::vector<RepositoryTreeEntry>& entries,
                  std::string path,
                  ApiContext& context,
                  bool require_path = true) {
    if (require_path && !path.empty() && !find_entry(entries, path)) {
        return std::unexpected(ApiError{"not_found", "no drive path " + path, "/path"});
    }
    const auto repository_object = context.store->find_policy_object("repository", repository.id);
    if (!repository_object) {
        return std::unexpected(ApiError{
            repository_object.error().code, repository_object.error().message, "/repository_id"});
    }
    std::vector<std::pair<control::PolicyObject, std::string>> on_path; // nearest first
    for (std::string candidate = find_entry(entries, path) ? path : parent_path(path);
         !candidate.empty();
         candidate = parent_path(candidate)) {
        const RepositoryTreeEntry* entry = find_entry(entries, candidate);
        if (entry && !entry->node_id.empty()) {
            const auto object = repository_path_object(
                *context.store, repository_object->id, repository.id, entry->node_id);
            if (object) {
                on_path.emplace_back(*object, entry->node_id);
            } else if (!no_own_object(object.error())) {
                return std::unexpected(
                    ApiError{object.error().code, object.error().message, "/path"});
            }
        }
    }
    DriveLocation location{repository, *repository_object, "", path, repository_object->id};
    bool holds = true;
    for (const auto& [object, node_id] : std::views::reverse(on_path)) {
        if (!object.inherit || (holds && object.parent_id == location.child_parent_id)) {
            location.object = object;
            location.node_id = node_id;
            holds = true;
        } else {
            holds = false;
        }
        location.child_parent_id = object.id;
    }
    if (!holds) {
        location.child_parent_id.clear();
    }
    return location;
}

std::expected<DriveLocation, ApiError> nearest_drive_location(const json& input,
                                                              ApiContext& context,
                                                              bool require_path = true,
                                                              bool unborn_main_is_empty = false) {
    control::Repository repository;
    const auto entries = repository_tree(input, context, repository, unborn_main_is_empty);
    if (!entries) {
        return std::unexpected(entries.error());
    }
    return drive_location_at(repository, *entries, input.value("path", ""), context, require_path);
}

// Gives every folder on `path` at this ref a policy object under the object of the folder above
// it (the repository's for the first), creating the missing ones and putting back under the folder
// above any whose stored parent is another (lake#929). Answers the last object, or the
// repository's for the top.
std::expected<control::PolicyObject, ApiError>
link_folder_chain(const std::vector<RepositoryTreeEntry>& entries,
                  const control::Repository& repository,
                  std::string_view path,
                  ApiContext& context,
                  std::string field) {
    auto parent = context.store->find_policy_object("repository", repository.id);
    if (!parent) {
        return std::unexpected(
            ApiError{parent.error().code, parent.error().message, "/repository_id"});
    }
    const std::string repository_object = parent->id;
    std::size_t offset = 0;
    while (offset < path.size()) {
        const std::size_t slash = path.find('/', offset);
        const std::string prefix(path.substr(0, slash));
        const RepositoryTreeEntry* entry = find_entry(entries, prefix);
        if (!entry || entry->kind != "tree" || entry->node_id.empty()) {
            return std::unexpected(
                ApiError{"invalid_input", "every shared path component must be a folder", field});
        }
        // Only an object under this repository's own object is the folder's (lake#913); another
        // account's object holding the folder's id is refused, never adopted or moved here.
        auto object =
            repository_path_object(*context.store, repository_object, repository.id, entry->node_id);
        if (!object && object.error().code == "not_found") {
            object = context.store->create_policy_object(
                control::NewPolicyObject{"path",
                                         path_external_id(repository.id, entry->node_id),
                                         parent->id,
                                         context.acting_identity,
                                         context.request_id,
                                         true});
        } else if (object && object->parent_id != parent->id) {
            object = context.store->set_policy_object_parent(object->id, parent->id);
        }
        if (!object && object.error().code == "conflict") {
            // Another account holds this folder's id. Its object can neither be adopted nor
            // replaced (the id is unique), so the folder cannot be shared until the operator
            // removes it.
            return std::unexpected(
                ApiError{"conflict",
                         "the policy object for " + prefix +
                             " belongs to another repository; ask the operator to remove it",
                         field});
        }
        if (!object) {
            return std::unexpected(ApiError{object.error().code, object.error().message, field});
        }
        parent = object;
        if (slash == std::string::npos) {
            break;
        }
        offset = slash + 1;
    }
    return *parent;
}

std::expected<DriveLocation, ApiError> ensure_folder_location(const json& input,
                                                              ApiContext& context) {
    control::Repository repository;
    const auto entries = repository_tree(input, context, repository);
    if (!entries) {
        return std::unexpected(entries.error());
    }
    const std::string path = input.at("path");
    const RepositoryTreeEntry* target = find_entry(*entries, path);
    if (!target || target->kind != "tree" || target->node_id.empty()) {
        return std::unexpected(ApiError{"invalid_input", "path must name a folder", "/path"});
    }
    const auto object = link_folder_chain(*entries, repository, path, context, "/path");
    if (!object) {
        return std::unexpected(object.error());
    }
    return DriveLocation{repository, *object, target->node_id, path, object->id};
}

std::expected<std::string, ApiError> drive_object(const json& input, ApiContext& context) {
    const auto location = nearest_drive_location(input, context);
    if (!location) {
        return std::unexpected(location.error());
    }
    return location->object.id;
}

// An upload's object (lake#560): the target folder when it exists, otherwise its nearest existing
// folder, so an upload into a folder that does not exist yet creates it under the access of the
// folder it is made in. A repository with no `main` yet is checked at the repository.
std::expected<std::string, ApiError> drive_upload_object(const json& input, ApiContext& context) {
    const auto location = nearest_drive_location(input, context, false, true);
    if (!location) {
        return std::unexpected(location.error());
    }
    return location->object.id;
}

// Whether the caller may `action` the node at `path` as it was at `revision` (LSR-014,
// lake#923). A history operation reads revisions other than the `ref` its object was resolved
// from, and the node a path names can differ between them (a folder renamed away and another
// made under its name), so each revision it reads is authorized on its own.
std::optional<ApiResult> authorize_drive_revision(const json& input,
                                                  const std::string& revision,
                                                  std::string_view action,
                                                  ApiContext& context) {
    if (context.operator_credential || context.acting_identity == "admin") {
        return std::nullopt;
    }
    json at = input;
    at["ref"] = revision;
    const auto location = nearest_drive_location(at, context, false);
    if (!location) {
        return ApiResult::failure(location.error().code, location.error().message, location.error().path);
    }
    const auto decision = control::authorize(*context.store,
                                             context.acting_identity,
                                             location->object.id,
                                             std::string(action),
                                             context.authorization_cache);
    if (!decision) {
        return ApiResult::failure(decision.error().code, decision.error().message);
    }
    if (!decision->allowed) {
        return ApiResult::failure(
            "unauthorized", "principal is not allowed to read this path at " + revision);
    }
    return std::nullopt;
}

std::expected<std::string, ApiError> drive_history_object(const json& input, ApiContext& context) {
    const auto location = nearest_drive_location(input, context, false);
    if (!location) {
        return std::unexpected(location.error());
    }
    return location->object.id;
}

// A link's folder, from its synthetic `share-link:<id>` grant: the policy that stays central
// (lake#350). The link's own record lives in its workspace Zetta.
std::expected<std::string, ApiError>
share_link_grant_object(const std::string& link_id, ApiContext& context, std::string field) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const auto grants = context.store->list_grants({}, "share-link:" + link_id, 1);
    if (!grants) {
        return std::unexpected(ApiError{grants.error().code, grants.error().message, {}});
    }
    if (grants->empty()) {
        return std::unexpected(
            ApiError{"not_found", "no drive share link " + link_id, std::move(field)});
    }
    return grants->front().object_id;
}

std::expected<std::string, ApiError> link_object(const json& input, ApiContext& context) {
    return share_link_grant_object(input.at("link_id"), context, "/link_id");
}

// Where a bearer token's link lives. A token made since lake#350 names its workspace and record;
// an earlier token is routed by its digest through the content-free drive_share_link_routes.
std::expected<control::DriveShareToken, ApiError> share_token_route(const std::string& token,
                                                                    ApiContext& context) {
    if (auto routed = control::parse_drive_share_token(token)) {
        return *routed;
    }
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const auto route = context.store->drive_share_link_route(token_hash(token));
    if (!route) {
        return std::unexpected(ApiError{route.error().code, route.error().message, "/token"});
    }
    return control::DriveShareToken{route->workspace_id, route->link_id};
}

std::expected<std::string, ApiError> share_token_object(const json& input, ApiContext& context) {
    const auto route = share_token_route(input.at("token"), context);
    if (!route) {
        return std::unexpected(route.error());
    }
    return share_link_grant_object(route->link_id, context, "/token");
}

ApiError records_error(const control::ZettaControlError& error, std::string path = {}) {
    return ApiError{error.code, error.message, error.path.empty() ? std::move(path) : error.path};
}

// The live workspace that holds links, in `customer_id`'s space when one is given. Never a
// default: the caller names it, or the bearer token does.
std::expected<control::Workspace, ApiError> share_link_workspace(ApiContext& context,
                                                                 const std::string& workspace_id,
                                                                 const std::string& customer_id,
                                                                 std::string field) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const auto workspace = context.store->get_workspace(workspace_id);
    if (!workspace || workspace->desired == "deleted" ||
        (!customer_id.empty() && workspace->customer_id != customer_id)) {
        return std::unexpected(ApiError{
            "invalid_input",
            "workspace_id must name a workspace Zetta in the repository's account",
            std::move(field)});
    }
    return *workspace;
}

std::vector<RepositoryTreeEntry> folder_children(const std::vector<RepositoryTreeEntry>& entries,
                                                 std::string_view folder) {
    const std::string prefix = folder.empty() ? std::string{} : std::string(folder) + "/";
    std::vector<RepositoryTreeEntry> output;
    for (const RepositoryTreeEntry& entry : entries) {
        if (!entry.path.starts_with(prefix)) {
            continue;
        }
        const std::string_view remaining(entry.path.data() + prefix.size(),
                                         entry.path.size() - prefix.size());
        if (!remaining.empty() && remaining != ".lake-folder" &&
            remaining.find('/') == std::string_view::npos) {
            output.push_back(entry);
        }
    }
    return output;
}

json tree_entry_json(const RepositoryTreeEntry& entry) {
    return {{"path", entry.path},
            {"kind", entry.kind},
            {"blob", entry.blob},
            {"mode", entry.mode},
            {"node_id", entry.node_id},
            {"size", entry.size},
            {"stored_size", entry.stored_size},
            {"modified_at", entry.last_changed_at}};
}

std::string path_name(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

std::string path_join(std::string_view folder, std::string_view name) {
    return folder.empty() ? std::string(name) : std::string(folder) + "/" + std::string(name);
}

bool valid_path_name(std::string_view name) {
    return !name.empty() && name != "." && name != ".." && name != ".lake-folder" &&
           name.find('/') == std::string_view::npos && name.find('\\') == std::string_view::npos;
}

bool valid_drive_path(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.back() == '/' ||
        path.find('\\') != std::string_view::npos) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < path.size()) {
        const std::size_t slash = path.find('/', offset);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        if (!valid_path_name(path.substr(offset, end - offset))) {
            return false;
        }
        offset = end + 1;
    }
    return true;
}

bool path_touches(std::string_view changed, std::string_view entry) {
    return changed == entry || (changed.size() > entry.size() && changed.starts_with(entry) &&
                                changed[entry.size()] == '/');
}

std::expected<std::vector<RepositoryTreeEntry>, ApiError>
folder_entries(const json& input,
               ApiContext& context,
               const control::Repository& repository,
               const std::vector<RepositoryTreeEntry>& tree) {
    std::vector<RepositoryTreeEntry> entries = folder_children(tree, input.value("path", ""));
    const auto changes = context.sync_store->repository_log(repository.customer_id, repository.id);
    if (!changes) {
        return std::unexpected(ApiError{changes.error().code, changes.error().message, {}});
    }
    for (const RepositoryChange& change : *changes) {
        const auto differences =
            context.sync_store->repository_diff(repository.customer_id, repository.id, change.id);
        if (!differences) {
            return std::unexpected(
                ApiError{differences.error().code, differences.error().message, {}});
        }
        for (RepositoryTreeEntry& entry : entries) {
            const bool touched = std::ranges::any_of(*differences, [&](const auto& difference) {
                return path_touches(difference.path, entry.path) ||
                       path_touches(difference.old_path, entry.path);
            });
            if (touched && change.occurred_at > entry.last_changed_at) {
                entry.last_changed_at = change.occurred_at;
            }
        }
    }
    const std::string sort = input.value("sort", "name");
    const bool descending = input.value("direction", "asc") == "desc";
    std::ranges::sort(
        entries, [&](const RepositoryTreeEntry& left, const RepositoryTreeEntry& right) {
            if ((left.kind == "tree") != (right.kind == "tree")) {
                return left.kind == "tree";
            }
            if (left.path == right.path) {
                return false;
            }
            int order = 0;
            if (sort == "size" && left.size != right.size) {
                order = left.size < right.size ? -1 : 1;
            } else if (sort == "modified" && left.last_changed_at != right.last_changed_at) {
                order = left.last_changed_at < right.last_changed_at ? -1 : 1;
            } else {
                order = path_name(left.path) < path_name(right.path) ? -1 : 1;
            }
            return descending ? order > 0 : order < 0;
        });
    return entries;
}

std::expected<std::vector<std::byte>, ApiError> decode_base64(std::string_view input,
                                                              std::string path) {
    if (input.empty()) {
        return std::vector<std::byte>{};
    }
    if (input.size() % 4 != 0) {
        return std::unexpected(ApiError{"invalid_input", "invalid base64 data", std::move(path)});
    }
    std::vector<std::byte> output((input.size() / 4) * 3);
    const int size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(output.data()),
                                     reinterpret_cast<const unsigned char*>(input.data()),
                                     static_cast<int>(input.size()));
    if (size < 0) {
        return std::unexpected(ApiError{"invalid_input", "invalid base64 data", std::move(path)});
    }
    std::size_t padding = 0;
    if (input.back() == '=') {
        ++padding;
    }
    if (input.size() > 1 && input[input.size() - 2] == '=') {
        ++padding;
    }
    output.resize(static_cast<std::size_t>(size) - padding);
    return output;
}

json file_json(const RepositoryFile& file) {
    return {{"path", file.path}, {"blob", file.blob}, {"size", file.size}};
}

json diff_json(const DriveVersionDiff& difference) {
    json lines = json::array();
    for (const DriveDiffLine& line : difference.lines) {
        lines.push_back({{"kind", line.kind}, {"text", line.text}});
    }
    return {{"left", file_json(difference.left)},
            {"right", file_json(difference.right)},
            {"lines", std::move(lines)}};
}

json conflict_json(const RepositoryConflict& conflict) {
    json region{{"path", conflict.region.path}};
    if (conflict.region.bytes) {
        region["bytes"] = {{"offset", conflict.region.bytes->offset},
                           {"length", conflict.region.bytes->length}};
    }
    if (conflict.region.nodes) {
        region["nodes"] = {{"first", conflict.region.nodes->first},
                           {"last", conflict.region.nodes->last}};
    }
    json output{{"id", conflict.id},
                {"base", conflict.base},
                {"left", conflict.left},
                {"right", conflict.right},
                {"region", std::move(region)},
                {"kind", conflict.kind},
                {"explanation", conflict.explanation},
                {"reasons", conflict.reasons}};
    if (conflict.proposal) {
        output["proposal"] = *conflict.proposal;
    }
    if (conflict.node) {
        output["node"] = *conflict.node;
    }
    return output;
}

std::expected<void, ApiError> require_drive_write(const json& input, ApiContext& context) {
    if (context.operator_credential || context.acting_identity == "admin") {
        return {};
    }
    const auto location = nearest_drive_location(input, context);
    if (!location) {
        return std::unexpected(location.error());
    }
    const auto decision = control::authorize(*context.store,
                                             context.acting_identity,
                                             location->object.id,
                                             "write",
                                             context.authorization_cache);
    if (!decision) {
        return std::unexpected(ApiError{decision.error().code, decision.error().message, {}});
    }
    if (!decision->allowed) {
        return std::unexpected(
            ApiError{"unauthorized", "principal is not allowed to write here", {}});
    }
    return {};
}

// Live grants and bearer links delegate only actions the caller holds on this folder.
std::expected<void, ApiError> require_drive_share_role(const DriveLocation& location,
                                                       std::string_view role,
                                                       ApiContext& context) {
    if (context.operator_credential) {
        return {};
    }
    // ensure_folder_location may have materialized policy objects since registry authorization.
    control::AuthorizationCache cache = control::fresh_cache_like(context.authorization_cache);
    for (const std::string action : {"read", "write", "admin", "deploy", "review", "share"}) {
        if (action != "share" && !control::role_allows(role, action)) {
            continue;
        }
        const auto decision = control::authorize(
            *context.store, context.acting_identity, location.object.id, action, cache);
        if (!decision) {
            return std::unexpected(ApiError{decision.error().code, decision.error().message, {}});
        }
        if (!decision->allowed) {
            return std::unexpected(ApiError{
                "unauthorized", "sharing requires every action granted by the role", "/role"});
        }
    }
    return {};
}

// A drive write is a change like a push's, so the search index follows it as it follows a push
// (lake#584): the repository's branch tips are published again. A failure is recorded in the audit
// details and never fails the write; search then names the repository's workspace as stale or
// building until its worker catches up (docs/api/search.md).
void follow_in_search(const control::Repository& repository, ApiContext& context) {
    if (!context.search) {
        return;
    }
    const auto indexed = context.search->reindex_artifacts(repository.customer_id, repository.id);
    if (!indexed) {
        context.audit_details["search_index"] = indexed.error().message;
    }
}

// Check every destination and every node an operation removes or moves. All checks use the
// immutable tree being committed, supplied while the store holds its repository mutation lock.
SyncStoreResult<void>
authorize_drive_operations(const control::Repository& repository,
                           const std::vector<RepositoryTreeEntry>& entries,
                           const std::vector<RepositoryWriteOperation>& operations,
                           ApiContext& context) {
    control::AuthorizationCache cache = control::fresh_cache_like(context.authorization_cache);
    auto require_write = [&](const std::string& path) -> SyncStoreResult<void> {
        const auto location = drive_location_at(repository, entries, path, context, false);
        if (!location) {
            return std::unexpected(SyncStoreError{location.error().code, location.error().message});
        }
        if (context.operator_credential) {
            return {};
        }
        const auto allowed = control::authorize(
            *context.store, context.acting_identity, location->object.id, "write", cache);
        if (!allowed) {
            return std::unexpected(SyncStoreError{allowed.error().code, allowed.error().message});
        }
        if (!allowed->allowed) {
            return std::unexpected(SyncStoreError{
                "unauthorized", "principal is not allowed to write drive path", path});
        }
        return {};
    };
    for (const auto& operation : operations) {
        if (const auto allowed = require_write(operation.path); !allowed) {
            return allowed;
        }
        if (operation.kind == RepositoryWriteKind::Moved) {
            if (!find_entry(entries, operation.source_path)) {
                return std::unexpected(SyncStoreError{"not_found", "drive path does not exist"});
            }
            if (find_entry(entries, operation.path)) {
                return std::unexpected(
                    SyncStoreError{"conflict", "move destination already exists"});
            }
            if (const auto allowed = require_write(operation.source_path); !allowed) {
                return allowed;
            }
        } else if (operation.kind == RepositoryWriteKind::Folder &&
                   find_entry(entries, operation.path)) {
            return std::unexpected(SyncStoreError{"conflict", "drive path already exists"});
        }
        for (const auto& entry : entries) {
            // File uploads replace an existing node, including its whole subtree. Folder
            // deletion and moves likewise require write on every affected descendant.
            if (path_touches(entry.path, operation.path) ||
                (operation.kind == RepositoryWriteKind::Moved &&
                 path_touches(entry.path, operation.source_path))) {
                if (const auto allowed = require_write(entry.path); !allowed) {
                    return allowed;
                }
            }
        }
    }
    return {};
}

ApiResult committed_drive_change(const control::Repository& repository,
                                 std::string_view audit_kind,
                                 std::string_view audit_path,
                                 SyncStoreResult<std::string> committed,
                                 ApiContext& context) {
    if (!committed) {
        return sync_error(committed.error());
    }
    context.audit_customer_id = repository.customer_id;
    context.audit_kind = std::string(audit_kind);
    context.audit_object = "repository/" + repository.id + "/path/" + std::string(audit_path);
    context.audit_details = {{"path", audit_path}, {"change", *committed}};
    follow_in_search(repository, context);
    return ApiResult::success({{"change", *committed}, {"path", audit_path}});
}

json change_json(const RepositoryChange& change) {
    return {{"id", change.id},
            {"parents", change.parents},
            {"author", change.author},
            {"message", change.message},
            {"occurred_at", change.occurred_at}};
}

// What one change did to `path` (lake#123). A change can carry several structural entries for one
// path: an overwrite through `commit_repository_files` removes the old node and adds a new one at the
// same path, and a file can be moved away while another takes its place. The entry that leaves a
// file at `path` is the path's state after the change; the version it replaced comes from the entry
// that held `path` before, so a replacement reads as a modification of the path and never as a
// deletion. A change whose only entries remove or move the path away keeps its first entry.
std::optional<json> drive_path_change(const RepositoryChange& change,
                                      const std::vector<RepositoryDiffEntry>& differences,
                                      const std::string& path) {
    std::vector<const RepositoryDiffEntry*> matches;
    for (const RepositoryDiffEntry& difference : differences) {
        if (difference.path == path || difference.old_path == path) {
            matches.push_back(&difference);
        }
    }
    if (matches.empty()) {
        return std::nullopt;
    }
    const auto present = std::ranges::find_if(matches, [&](const RepositoryDiffEntry* entry) {
        return entry->path == path && !entry->new_blob.empty() &&
               std::ranges::find(entry->changes, "removed") == entry->changes.end();
    });
    const RepositoryDiffEntry& selected = present == matches.end() ? *matches.front() : **present;
    json value = change_json(change);
    value["path"] = selected.path;
    value["old_path"] = selected.old_path;
    value["changes"] = selected.changes;
    value["old_blob"] = selected.old_blob;
    value["new_blob"] = selected.new_blob;
    value["old_conflicts"] = selected.old_conflicts;
    value["new_conflicts"] = selected.new_conflicts;
    if (present == matches.end() || !selected.old_blob.empty()) {
        return value;
    }
    const auto replaced = std::ranges::find_if(matches, [&](const RepositoryDiffEntry* entry) {
        return entry != &selected && !entry->old_blob.empty() &&
               (entry->old_path == path || (entry->path == path && entry->old_path.empty()));
    });
    if (replaced == matches.end()) {
        return value;
    }
    std::vector<std::string> kinds;
    for (const std::string& kind : selected.changes) {
        const std::string named = kind == "added" ? "modified" : kind;
        if (std::ranges::find(kinds, named) == kinds.end()) {
            kinds.push_back(named);
        }
    }
    value["changes"] = std::move(kinds);
    value["old_blob"] = (*replaced)->old_blob;
    value["old_conflicts"] = (*replaced)->old_conflicts;
    return value;
}

std::string token_hash(std::string_view token) {
    static constexpr std::string_view key = "lake-drive-share-link/1";
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    HMAC(EVP_sha256(),
         key.data(),
         static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(token.data()),
         token.size(),
         digest,
         &size);
    std::string output;
    output.reserve(size * 2);
    for (unsigned index = 0; index < size; ++index) {
        output += std::format("{:02x}", digest[index]);
    }
    return output;
}

// 244 random bits, as before lake#350; the token wraps it with the workspace and link ids.
std::string new_link_secret() {
    std::string secret = control::new_uuid_v4() + control::new_uuid_v4();
    std::erase(secret, '-');
    return secret;
}

bool link_is_active(const control::DriveShareLink& link) {
    if (!link.revoked_at.empty()) {
        return false;
    }
    // Postgres emits UTC with microseconds; in-memory links may omit the fraction.
    std::string expiry = link.expires_at;
    if (expiry.size() == 20 && expiry.back() == 'Z') {
        expiry.insert(19, ".000000");
    } else if (expiry.size() > 21 && expiry[19] == '.' && expiry.back() == 'Z') {
        std::string fraction = expiry.substr(20, expiry.size() - 21);
        fraction.resize(6, '0');
        expiry = expiry.substr(0, 20) + fraction + "Z";
    }
    const auto now =
        std::chrono::floor<std::chrono::microseconds>(std::chrono::system_clock::now());
    return expiry > std::format("{:%FT%T}Z", now);
}

ApiResult browse_result(const DriveLocation& location,
                        const std::vector<RepositoryTreeEntry>& entries,
                        ApiContext& context,
                        std::string_view principal) {
    json items = json::array();
    std::optional<std::string> repository_object;
    for (const RepositoryTreeEntry& entry : folder_children(entries, location.path)) {
        bool allowed = true;
        if (context.acting_identity != "admin" && !entry.node_id.empty()) {
            if (!repository_object) {
                const auto found =
                    context.store->find_policy_object("repository", location.repository.id);
                if (!found) {
                    return store_error(found.error());
                }
                repository_object = found->id;
            }
            const auto object = repository_path_object(
                *context.store, *repository_object, location.repository.id, entry.node_id);
            if (object) {
                // A child's own object decides only when its parent is this folder's (lake#929);
                // one left under another folder is decided as this folder is.
                const bool holds =
                    !object->inherit || object->parent_id == location.child_parent_id;
                const auto decision = control::authorize(*context.store,
                                                         std::string(principal),
                                                         holds ? object->id : location.object.id,
                                                         "read",
                                                         context.authorization_cache);
                if (!decision) {
                    return store_error(decision.error());
                }
                allowed = decision->allowed;
            } else if (!no_own_object(object.error())) {
                return store_error(object.error());
            }
        }
        if (allowed) {
            items.push_back(tree_entry_json(entry));
        }
    }
    context.audit_customer_id = location.repository.customer_id;
    context.audit_kind = "drive.folder.read";
    context.audit_object = "repository/" + location.repository.id + "/path/" + location.path;
    context.audit_details = {{"node_id", location.node_id}, {"path", location.path}};
    return ApiResult::success({{"repository", control::to_json(location.repository)},
                               {"object", control::to_json(location.object)},
                               {"path", location.path},
                               {"items", std::move(items)}});
}

struct SharedDriveTarget {
    DriveLocation location;
    std::string root_path;
    RepositoryTreeEntry target;
    std::vector<RepositoryTreeEntry> entries;
};

bool same_digest(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

template <class T> using DriveTask = liquid::runtime::task<std::expected<T, ApiError>>;

// Flattens a finished drive task: a runtime failure is a store error.
template <class T>
std::expected<T, ApiError> drive_settled(std::expected<std::expected<T, ApiError>, liquid::error> completed) {
    if (!completed) {
        return std::unexpected(ApiError{"store_error", completed.error().message, {}});
    }
    return std::move(*completed);
}

// The link a bearer token opens, read from the workspace Zetta that holds it (lake#350). The
// token routes to the workspace and record; its digest must match the one sealed in the record.
// Any token that does not open a record is `not_found` without saying why.
DriveTask<control::DriveShareLink> active_share_link(const json& input, ApiContext& context) {
    using Result = std::expected<control::DriveShareLink, ApiError>;
    context.sync_response.clear();
    if (!context.store || !context.sync_store) {
        co_return Result(std::unexpected(
            ApiError{"store_unavailable", "drive stores are not configured", {}}));
    }
    const std::string token = input.at("token");
    const ApiError unknown{"not_found", "share link is not recognized", "/token"};
    const auto route = share_token_route(token, context);
    if (!route) {
        co_return Result(std::unexpected(route.error().code == "not_found" ? unknown
                                                                            : route.error()));
    }
    const auto workspace = context.store->get_workspace(route->workspace_id);
    if (!workspace || workspace->desired == "deleted") {
        co_return Result(std::unexpected(unknown));
    }
    auto records = workspace_records(context, workspace->customer_id, workspace->id);
    if (!records) {
        co_return Result(std::unexpected(records_error(records.error())));
    }
    auto stored = control::settled(
        co_await records->get(control::DriveShareLinkCodec::collection, route->link_id));
    if (!stored) {
        co_return Result(std::unexpected(records_error(stored.error())));
    }
    const auto link = *stored ? control::DriveShareLinkCodec::decode(**stored) : std::nullopt;
    if (!link || !same_digest(link->token_hash, token_hash(token))) {
        co_return Result(std::unexpected(unknown));
    }
    context.acting_identity = "share-link:" + link->id;
    context.audit_details = {{"link_id", link->id}, {"workspace_id", workspace->id}};
    if (!link_is_active(*link)) {
        co_return Result(std::unexpected(
            ApiError{"unauthorized", "share link is expired or revoked", {}}));
    }
    if (link->role != "read" && link->role != "write") {
        co_return Result(std::unexpected(
            ApiError{"unauthorized", "share link does not allow reads", {}}));
    }
    // A reused caller context must not retain policy from a previous link operation.
    context.authorization_cache = {};
    const auto decision = control::authorize(*context.store,
                                             context.acting_identity,
                                             link->object_id,
                                             "read",
                                             context.authorization_cache);
    if (!decision) {
        co_return Result(std::unexpected(
            ApiError{decision.error().code, decision.error().message, {}}));
    }
    if (!decision->allowed) {
        co_return Result(std::unexpected(
            ApiError{"unauthorized", "share link does not allow reads", {}}));
    }
    co_return Result(*link);
}

std::expected<control::PolicyObject, ApiError>
shared_path_object(const control::DriveShareLink& link,
                   const std::vector<RepositoryTreeEntry>& entries,
                   std::string path,
                   std::string_view root_path,
                   ApiContext& context) {
    const auto repository_object =
        context.store->find_policy_object("repository", link.repository_id);
    if (!repository_object) {
        return std::unexpected(
            ApiError{repository_object.error().code, repository_object.error().message, {}});
    }
    while (path_touches(path, root_path)) {
        const auto* entry = find_entry(entries, path);
        if (entry && !entry->node_id.empty()) {
            const auto object = repository_path_object(
                *context.store, repository_object->id, link.repository_id, entry->node_id);
            if (object) {
                const auto decision = control::authorize(*context.store,
                                                         context.acting_identity,
                                                         object->id,
                                                         "read",
                                                         context.authorization_cache);
                if (!decision) {
                    return std::unexpected(
                        ApiError{decision.error().code, decision.error().message, {}});
                }
                if (!decision->allowed) {
                    return std::unexpected(
                        ApiError{"unauthorized", "share link does not allow this path", "/path"});
                }
                return *object;
            }
            if (!no_own_object(object.error())) {
                return std::unexpected(ApiError{object.error().code, object.error().message, {}});
            }
        }
        if (path == root_path) {
            break;
        }
        path = parent_path(path);
    }
    return std::unexpected(ApiError{"unauthorized", "share link scope is unavailable", "/path"});
}

std::expected<SharedDriveTarget, ApiError>
shared_drive_target(const json& input, ApiContext& context, const control::DriveShareLink* link) {
    control::Repository repository;
    auto entries = repository_tree(
        {{"repository_id", link->repository_id}, {"ref", input.at("ref")}}, context, repository);
    if (!entries) {
        return std::unexpected(entries.error());
    }
    const auto root = std::ranges::find(*entries, link->node_id, &RepositoryTreeEntry::node_id);
    if (root == entries->end() || root->kind != "tree") {
        return std::unexpected(
            ApiError{"not_found", "shared folder is not present at this ref", {}});
    }
    const std::string root_path = root->path;
    const std::string path = input.value("path", root_path);
    if (!valid_drive_path(path) || path.find('\0') != std::string::npos ||
        !path_touches(path, root_path)) {
        return std::unexpected(
            ApiError{"unauthorized", "path is outside the shared folder", "/path"});
    }
    const auto* entry = find_entry(*entries, path);
    if (!entry) {
        return std::unexpected(ApiError{"not_found", "no shared drive path " + path, "/path"});
    }
    const auto object = shared_path_object(*link, *entries, path, root_path, context);
    if (!object) {
        return std::unexpected(object.error());
    }
    return SharedDriveTarget{{repository, *object, entry->node_id, path, object->id},
                             root_path,
                             *entry,
                             std::move(*entries)};
}

// The folder a bearer token opens at the requested ref and path, after the link's checks.
liquid::runtime::task<ApiResult> shared_target(const json& input, ApiContext& context,
                                               std::expected<SharedDriveTarget, ApiError>& out) {
    const auto link = drive_settled(co_await active_share_link(input, context));
    if (!link) {
        out = std::unexpected(link.error());
        co_return ApiResult::failure(link.error().code, link.error().message, link.error().path);
    }
    out = shared_drive_target(input, context, &*link);
    if (!out) {
        co_return ApiResult::failure(out.error().code, out.error().message, out.error().path);
    }
    co_return ApiResult::success(nullptr);
}

ApiResult shared_file_result(const json& input, ApiContext& context, bool download,
                             const std::expected<SharedDriveTarget, ApiError>& shared) {
    if (!shared) {
        return ApiResult::failure(shared.error().code, shared.error().message, shared.error().path);
    }
    if (shared->target.kind != "blob") {
        return ApiResult::failure("invalid_input", "path must name a file", "/path");
    }
    const auto& repository = shared->location.repository;
    // Read the blob selected by the authorized tree, even if the live ref moves meanwhile.
    const auto file = context.sync_store->repository_blob(
        repository.customer_id, repository.id, shared->target.path, shared->target.blob);
    if (!file) {
        return sync_error(file.error());
    }
    context.audit_customer_id = repository.customer_id;
    context.audit_object = "repository/" + repository.id + "/path/" + file->path;
    context.audit_details["path"] = file->path;
    context.audit_details["blob"] = file->blob;
    json value{{"path", file->path},
               {"root_path", shared->root_path},
               {"blob", file->blob},
               {"size", file->size}};
    if (!download) {
        const auto preview = DriveFormatRegistry::defaults().preview(*file);
        if (!preview) {
            return ApiResult::failure("invalid_input", preview.error(), "/path");
        }
        context.audit_kind = "drive.preview.read";
        value["format"] = preview->format;
        value["media_type"] = preview->media_type;
        value["html"] = preview->html;
    } else {
        context.audit_kind = "drive.file.downloaded";
        context.sync_response = file->raw_contents;
        if (context.sync_response.empty() && !file->contents.empty()) {
            std::ranges::transform(file->contents,
                                   std::back_inserter(context.sync_response),
                                   [](char byte) { return static_cast<std::byte>(byte); });
        }
        // A shared file's bytes leave its repository's workspace (lake#658).
        record_workspace_transfer(context, repository.workspace_id, 0, file->size);
        std::string encoded((context.sync_response.size() + 2) / 3 * 4, '\0');
        // Keep each OpenSSL length within int, with padding only in the final block.
        constexpr std::size_t block_size = 3 * 1024 * 1024;
        for (std::size_t offset = 0; offset < context.sync_response.size(); offset += block_size) {
            const std::size_t length = std::min(block_size, context.sync_response.size() - offset);
            EVP_EncodeBlock(
                reinterpret_cast<unsigned char*>(encoded.data() + offset / 3 * 4),
                reinterpret_cast<const unsigned char*>(context.sync_response.data() + offset),
                static_cast<int>(length));
        }
        value["content_base64"] = std::move(encoded);
        value["media_type"] = "application/octet-stream";
    }
    return ApiResult::success(std::move(value));
}

json share_read_input_schema(bool path_required) {
    json required = json::array({"token", "ref"});
    if (path_required) {
        required.push_back("path");
    }
    return {{"type", "object"},
            {"properties",
             {{"token", {{"type", "string"}}},
              {"ref", {{"type", "string"}}},
              {"path", {{"type", "string"}}}}},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

json shared_file_output_schema(bool download) {
    json properties{{"path", {{"type", "string"}}},
                    {"root_path", {{"type", "string"}}},
                    {"blob", {{"type", "string"}}},
                    {"size", {{"type", "integer"}}},
                    {"media_type", {{"type", "string"}}}};
    if (download) {
        properties["content_base64"] = {{"type", "string"}, {"contentEncoding", "base64"}};
    } else {
        properties["format"] = {{"type", "string"}};
        properties["html"] = {{"type", "string"}};
    }
    json required = json::array();
    for (const auto& [name, property] : properties.items()) {
        required.push_back(name);
    }
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

// A folder and the workspace that holds its links.
json share_links_input_schema() {
    return {{"type", "object"},
            {"properties",
             {{"repository_id", {{"type", "string"}}},
              {"workspace_id", {{"type", "string"}}},
              {"ref", {{"type", "string"}}},
              {"path", {{"type", "string"}}}}},
            {"required", json::array({"repository_id", "workspace_id", "ref", "path"})},
            {"additionalProperties", false}};
}

// The largest file download_drive_file answers inline, as read_drive_file does (16 MiB).
constexpr std::size_t max_answer_bytes = 16 * 1024 * 1024;

std::string encode_base64(std::span<const std::byte> bytes) {
    std::string encoded((bytes.size() + 2) / 3 * 4, '\0');
    // Keep each OpenSSL length within int, with padding only in the final block.
    constexpr std::size_t block_size = 3 * 1024 * 1024;
    for (std::size_t offset = 0; offset < bytes.size(); offset += block_size) {
        const std::size_t length = std::min(block_size, bytes.size() - offset);
        EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data() + offset / 3 * 4),
                        reinterpret_cast<const unsigned char*>(bytes.data() + offset),
                        static_cast<int>(length));
    }
    return encoded;
}

// A query-string value, percent-encoded except for unreserved characters and "/".
std::string url_component(std::string_view value) {
    std::string out;
    for (const char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out.push_back(raw);
        } else {
            out += std::format("%{:02X}", c);
        }
    }
    return out;
}

// One repository path: a file read or written, where browse's ordering means nothing.
json drive_path_schema() {
    return {{"type", "object"},
            {"properties",
             {{"repository_id", {{"type", "string"}}},
              {"ref", {{"type", "string"}}},
              {"path", {{"type", "string"}}}}},
            {"required", json::array({"repository_id", "ref", "path"})},
            {"additionalProperties", false}};
}

// ---- Sharing with a person (lake#585) ----

// Someone a folder can be shared with: one of the repository's people (repository_people.hpp).
using Recipient = RepositoryPerson;

json recipient_json(const Recipient& recipient) {
    return repository_person_json(recipient);
}

std::string lowercase(std::string value) {
    std::ranges::transform(
        value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// The person `recipient` names: an email, a handle (with or without "@") or a TAAP id, among the
// space's people.
std::expected<Recipient, ApiError> resolve_recipient(ApiContext& context,
                                                     const control::Repository& repository,
                                                     std::string recipient) {
    while (!recipient.empty() && std::isspace(static_cast<unsigned char>(recipient.front()))) {
        recipient.erase(recipient.begin());
    }
    while (!recipient.empty() && std::isspace(static_cast<unsigned char>(recipient.back()))) {
        recipient.pop_back();
    }
    if (recipient.empty()) {
        return std::unexpected(
            ApiError{"invalid_input", "name the person by their email or handle", "/recipient"});
    }
    const auto people = repository_people(context, repository);
    if (!people) {
        return std::unexpected(people.error());
    }
    const bool by_email = recipient.find('@', 1) != std::string::npos;
    const std::string wanted =
        lowercase(recipient.starts_with('@') ? recipient.substr(1) : recipient);
    for (const Recipient& person : *people) {
        if (person.taap_id == recipient ||
            (by_email && !person.email.empty() && lowercase(person.email) == wanted) ||
            (!by_email && !person.handle.empty() && lowercase(person.handle) == wanted)) {
            return person;
        }
    }
    return std::unexpected(ApiError{
        "invalid_input",
        "no one in this account has that " + std::string(by_email ? "email" : "handle") +
            ": share with a member of the account (drive_share_people lists them), or invite the "
            "person to the account first",
        "/recipient"});
}

// The statement lake signs for a live folder share, and the digest the grant keeps as its
// evidence (lake#585). The statement names what the grant is (folder, stable node, person, role,
// who shared it) and nothing about when, so lake's Ed25519 signature, which is deterministic, and
// the digest over statement and signature are the same for the same share under the same lake
// identity: a caller can send back the digest lake answered, never one of its own.
// The share's statement, signed by lake (lake#585, `lake_attestation.hpp`).
std::expected<LakeAttestation, ApiError> share_attestation(ApiContext& context,
                                                           const DriveLocation& location,
                                                           const std::string& principal_taap_id,
                                                           const std::string& role) {
    return sign_lake_attestation(context,
                                 {{"type", "lake.drive.share/1"},
                                  {"repository_id", location.repository.id},
                                  {"object_id", location.object.id},
                                  {"node_id", location.node_id},
                                  {"path", location.path},
                                  {"principal_taap_id", principal_taap_id},
                                  {"role", role},
                                  {"granted_by", context.acting_identity}},
                                 "drive-share",
                                 "the share");
}

json drive_input_schema(bool path_required) {
    json required = json::array({"repository_id", "ref"});
    if (path_required) {
        required.push_back("path");
    }
    return {{"type", "object"},
            {"properties",
             {{"repository_id", {{"type", "string"}}},
              {"ref", {{"type", "string"}}},
              {"path", {{"type", "string"}}},
              {"sort", {{"type", "string"}, {"enum", json::array({"name", "size", "modified"})}}},
              {"direction", {{"type", "string"}, {"enum", json::array({"asc", "desc"})}}}}},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

} // namespace

RepositoryWriteAuthorizer
repository_write_authorizer(const control::Repository& repository,
                            const std::vector<RepositoryWriteOperation>& operations,
                            ApiContext& context,
                            std::string ref,
                            BranchProtectionLoad protection) {
    std::vector<RepositoryWriteOperation> paths;
    for (const auto& operation : operations) {
        paths.push_back(
            {.kind = operation.kind, .path = operation.path, .source_path = operation.source_path});
    }
    return [repository, paths = std::move(paths), &context, ref = std::move(ref),
            protection = std::move(protection)](const auto& entries) -> SyncStoreResult<void> {
        // The branch's rules first, as a push's reference update (lake#924): a direct write adds
        // one change on top of the branch, so it is a fast-forward by the acting person.
        if (!protection) {
            return std::unexpected(SyncStoreError{
                "unavailable", "branch protection could not be read: " + protection.error().message});
        }
        const BranchRefUpdate update{.name = ref, .old_head = std::nullopt, .new_head = {},
                                     .fast_forward = true};
        if (auto evaluated = evaluate_branch_protection(*context.store,
                                                        context.acting_identity,
                                                        context.authorization_cache,
                                                        *protection,
                                                        update);
            !evaluated) {
            return std::unexpected(evaluated.error());
        }
        return authorize_drive_operations(repository, entries, paths, context);
    };
}

BranchProtectionLoad branch_protection_blocking(ApiContext& context,
                                                const control::Repository& repository) {
    auto loaded = liquid::runtime::block_on(load_branch_protection(context, repository));
    if (!loaded) {
        return std::unexpected(SyncStoreError{"unavailable", loaded.error().message});
    }
    return std::move(*loaded);
}

void register_drive_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "drive_browse",
        .title = "Browse drive folder",
        .description = "List the immediate children of a repository folder after path policy.",
        .input_schema = drive_input_schema(false),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "drive folder",
        .action = "read",
        .resolve_object = drive_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto location = nearest_drive_location(input, context);
            if (!location) {
                return ApiResult::failure(
                    location.error().code, location.error().message, location.error().path);
            }
            control::Repository repository;
            const auto entries = repository_tree(input, context, repository);
            if (!entries) {
                return ApiResult::failure(
                    entries.error().code, entries.error().message, entries.error().path);
            }
            const auto children = folder_entries(input, context, repository, *entries);
            if (!children) {
                return ApiResult::failure(
                    children.error().code, children.error().message, children.error().path);
            }
            return browse_result(*location, *children, context, context.acting_identity);
        }});

    registry.add(Capability{
        .name = "drive_preview",
        .title = "Preview drive file",
        .description = "Render one file through the server-side format registry.",
        .input_schema = drive_input_schema(true),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "drive file",
        .action = "read",
        .resolve_object = drive_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto repository = context.store->get_repository(input.at("repository_id"));
            if (!repository) {
                return store_error(repository.error());
            }
            const auto file = context.sync_store->repository_file(
                repository->customer_id, repository->id, input.at("ref"), input.at("path"));
            if (!file) {
                return sync_error(file.error());
            }
            const auto preview = DriveFormatRegistry::defaults().preview(*file);
            if (!preview) {
                return ApiResult::failure("invalid_input", preview.error(), "/path");
            }
            context.audit_customer_id = repository->customer_id;
            context.audit_kind = "drive.preview.read";
            context.audit_object = "repository/" + repository->id + "/path/" + file->path;
            context.audit_details = {{"format", preview->format}, {"path", file->path}};
            return ApiResult::success({{"repository", control::to_json(*repository)},
                                       {"path", file->path},
                                       {"blob", file->blob},
                                       {"size", file->size},
                                       {"format", preview->format},
                                       {"media_type", preview->media_type},
                                       {"html", preview->html}});
        }});

    registry.add(Capability{
        .name = "download_drive_file",
        .title = "Download drive file",
        .description =
            "Read one repository-drive file: its bytes as base64 in `content_base64` for a file "
            "of at most 16 MiB (null above that), with its digest, size and media type, and "
            "`download_url`, the GET /drive/download address that streams any size to a "
            "browser or a client holding the same credential. A workspace-files drive's file is "
            "read with read_drive_file.",
        .input_schema = drive_path_schema(),
        .output_schema =
            {{"type", "object"},
             {"properties",
              {{"path", {{"type", "string"}}},
               {"blob", {{"type", "string"}}},
               {"size", {{"type", "integer"}, {"minimum", 0}}},
               {"media_type", {{"type", "string"}}},
               {"content_base64",
                {{"type", json::array({"string", "null"})}, {"contentEncoding", "base64"}}},
               {"download_url", {{"type", "string"}}}}},
             {"required",
              json::array(
                  {"path", "blob", "size", "media_type", "content_base64", "download_url"})},
             {"additionalProperties", false}},
        .kind = CapabilityKind::Read,
        .object = "drive file",
        .action = "read",
        .resolve_object = drive_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto repository = context.store->get_repository(input.at("repository_id"));
            if (!repository) {
                return store_error(repository.error());
            }
            const auto file = context.sync_store->repository_file(
                repository->customer_id, repository->id, input.at("ref"), input.at("path"));
            if (!file) {
                return sync_error(file.error());
            }
            context.sync_response = file->raw_contents;
            if (context.sync_response.empty() && !file->contents.empty()) {
                context.sync_response.reserve(file->contents.size());
                std::ranges::transform(file->contents,
                                       std::back_inserter(context.sync_response),
                                       [](char value) { return static_cast<std::byte>(value); });
            }
            record_workspace_transfer(context, repository->workspace_id, 0, file->size);
            context.audit_customer_id = repository->customer_id;
            context.audit_kind = "drive.file.downloaded";
            context.audit_object = "repository/" + repository->id + "/path/" + file->path;
            context.audit_details = {{"blob", file->blob}, {"path", file->path}};
            // The bytes travel in the answer up to the size read_drive_file answers (lake#587);
            // a larger file is fetched from download_url, which streams it.
            json content = nullptr;
            if (context.sync_response.size() <= max_answer_bytes) {
                content = encode_base64(context.sync_response);
            }
            return ApiResult::success(
                {{"path", file->path},
                 {"blob", file->blob},
                 {"size", file->size},
                 {"media_type", control::drive_media_type(file->path)},
                 {"content_base64", std::move(content)},
                 {"download_url",
                  "/drive/download?repository_id=" + url_component(repository->id) +
                      "&ref=" + url_component(input.at("ref").get<std::string>()) +
                      "&path=" + url_component(file->path)}});
        }});

    registry.add(Capability{
        .name = "create_drive_folder",
        .title = "Create drive folder",
        .description = "Create an empty drive folder as a new liquid change.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"repository_id", {{"type", "string"}}},
                           {"ref", {{"type", "string"}}},
                           {"path", {{"type", "string"}}},
                           {"name", {{"type", "string"}}}}},
                         {"required", json::array({"repository_id", "ref", "path", "name"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "drive folder",
        .action = "write",
        .resolve_object = drive_object,
        .handler = [](const json& input, ApiContext& context) {
            const std::string name = input.at("name");
            if (!valid_path_name(name)) {
                return ApiResult::failure("invalid_input", "folder name is invalid", "/name");
            }
            control::Repository repository;
            const auto entries = repository_tree(input, context, repository);
            if (!entries) {
                return ApiResult::failure(
                    entries.error().code, entries.error().message, entries.error().path);
            }
            const std::string target = path_join(input.at("path").get<std::string>(), name);
            if (find_entry(*entries, target)) {
                return ApiResult::failure(
                    "conflict", "a drive path already has that name", "/name");
            }
            std::vector<RepositoryWriteOperation> operations{
                {.kind = RepositoryWriteKind::Folder, .path = target}};
            auto authorize = repository_write_authorizer(repository, operations, context, input.at("ref"), branch_protection_blocking(context, repository));
            return committed_drive_change(
                repository,
                "drive.folder.created",
                target,
                context.sync_store->commit_repository_files(repository.customer_id,
                                                            repository.id,
                                                            input.at("ref"),
                                                            "Create folder " + target,
                                                            std::move(operations),
                                                            std::move(authorize)),
                context);
        }});

    registry.add(Capability{
        .name = "upload_drive_files",
        .title = "Upload drive files",
        .description = "Upload files into a drive folder as one liquid change. A folder that does "
                       "not exist yet is created, and so is `main` in a repository with none.",
        .input_schema =
            {{"type", "object"},
             {"properties",
              {{"repository_id", {{"type", "string"}}},
               {"ref", {{"type", "string"}}},
               {"path", {{"type", "string"}}},
               {"files",
                {{"type", "array"},
                 {"items",
                  {{"type", "object"},
                   {"properties",
                    {{"path",
                      {{"type", "string"},
                       {"description",
                        "Where the file goes, relative to the target folder; it may name folders "
                        "(\"Q3/report.csv\"), as upload_drive_entries' `path` does. Send `path` "
                        "or `name`."}}},
                     {"name",
                      {{"type", "string"},
                       {"description", "A single file name within the target folder."}}},
                     {"content",
                      {{"type", "string"},
                       {"contentEncoding", "base64"},
                       {"description",
                        "File bytes encoded as padded base64; an empty string creates an empty "
                        "file."}}},
                     {"content_type",
                      {{"type", "string"},
                       {"description",
                        "Accepted so one item shape fits both upload calls; a repository keeps no "
                        "media type, which reads take from the file's name."}}}}},
                   {"required", json::array({"content"})},
                   {"additionalProperties", false}}}}}}},
             {"required", json::array({"repository_id", "ref", "path", "files"})},
             {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .max_body_bytes = maximum_capability_body_bytes,
        .object = "drive folder",
        .action = "write",
        .resolve_object = drive_upload_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!input.at("files").is_array() || input.at("files").empty()) {
                return ApiResult::failure(
                    "invalid_input", "at least one file is required", "/files");
            }
            const auto repository = context.store->get_repository(input.at("repository_id"));
            if (!repository) {
                return store_error(repository.error());
            }
            std::vector<RepositoryWriteOperation> operations;
            for (std::size_t index = 0; index < input.at("files").size(); ++index) {
                const json& value = input.at("files").at(index);
                // One item shape for both upload calls (lake#587): `path`, which may name
                // folders, or `name`, one file name.
                const std::string field = "/files/" + std::to_string(index);
                if (value.contains("path") == value.contains("name")) {
                    return ApiResult::failure("invalid_input",
                                              "send the file's `path` (or its `name`), not both",
                                              field + "/path");
                }
                const bool by_path = value.contains("path");
                const std::string name =
                    by_path ? value.value("path", "") : value.value("name", "");
                if (by_path ? !valid_drive_path(name) : !valid_path_name(name)) {
                    return ApiResult::failure("invalid_input",
                                              "uploaded file name is invalid",
                                              field + (by_path ? "/path" : "/name"));
                }
                const auto contents = decode_base64(value.value("content", ""),
                                                    "/files/" + std::to_string(index) + "/content");
                if (!contents) {
                    return ApiResult::failure(
                        contents.error().code, contents.error().message, contents.error().path);
                }
                operations.push_back({.kind = RepositoryWriteKind::File,
                                      .path = path_join(input.at("path").get<std::string>(), name),
                                      .contents = std::move(*contents)});
            }
            const std::string audit_path = input.at("path");
            std::uint64_t uploaded = 0;
            for (const auto& operation : operations) {
                uploaded += operation.contents.size();
            }
            record_workspace_transfer(context, repository->workspace_id, uploaded, 0);
            auto authorize = repository_write_authorizer(*repository, operations, context, input.at("ref"), branch_protection_blocking(context, *repository));
            return committed_drive_change(
                *repository,
                "drive.files.uploaded",
                audit_path,
                context.sync_store->commit_repository_files(repository->customer_id,
                                                            repository->id,
                                                            input.at("ref"),
                                                            "Upload drive files",
                                                            std::move(operations),
                                                            std::move(authorize)),
                context);
        }});

    registry.add(Capability{
        .name = "move_drive_path",
        .title = "Move drive path",
        .description = "Rename or move a drive path while retaining its stable node and history.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"repository_id", {{"type", "string"}}},
                           {"ref", {{"type", "string"}}},
                           {"path", {{"type", "string"}}},
                           {"destination", {{"type", "string"}}}}},
                         {"required", json::array({"repository_id", "ref", "path", "destination"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "drive path",
        .action = "write",
        .resolve_object = drive_object,
        .handler = [](const json& input, ApiContext& context) {
            control::Repository repository;
            const auto entries = repository_tree(input, context, repository);
            if (!entries) {
                return ApiResult::failure(
                    entries.error().code, entries.error().message, entries.error().path);
            }
            const std::string source = input.at("path");
            const std::string destination = input.at("destination");
            const RepositoryTreeEntry* source_entry = find_entry(*entries, source);
            if (!source_entry) {
                return ApiResult::failure("not_found", "drive path does not exist", "/path");
            }
            if (!valid_drive_path(destination) || find_entry(*entries, destination)) {
                return ApiResult::failure(
                    "conflict", "move destination is invalid or already exists", "/destination");
            }
            if (source_entry->kind == "tree" && destination.starts_with(source + "/")) {
                return ApiResult::failure(
                    "invalid_input", "a folder cannot be moved inside itself", "/destination");
            }
            json target_input = input;
            target_input["path"] = parent_path(destination);
            target_input.erase("destination");
            const auto target_access = require_drive_write(target_input, context);
            if (!target_access) {
                return ApiResult::failure(target_access.error().code,
                                          target_access.error().message,
                                          target_access.error().path);
            }
            // Reparent only after every source descendant and the destination are authorized.
            // Read the carried node from the locked tree, since main may have moved since
            // resolution.
            std::optional<control::PolicyObject> carried;
            std::vector<RepositoryWriteOperation> operations{
                {.kind = RepositoryWriteKind::Moved, .path = destination, .source_path = source}};
            auto authorize =
                [&, check = repository_write_authorizer(repository, operations, context, input.at("ref"), branch_protection_blocking(context, repository))](
                    const std::vector<RepositoryTreeEntry>& tree) -> SyncStoreResult<void> {
                if (const auto allowed = check(tree); !allowed) {
                    return allowed;
                }
                const auto* current = find_entry(tree, source);
                const auto repository_object =
                    context.store->find_policy_object("repository", repository.id);
                if (!repository_object) {
                    return std::unexpected(SyncStoreError{repository_object.error().code,
                                                          repository_object.error().message});
                }
                if (!current->node_id.empty()) {
                    const auto object = repository_path_object(
                        *context.store, repository_object->id, repository.id, current->node_id);
                    if (object) {
                        carried = *object;
                    } else if (!no_own_object(object.error())) {
                        return std::unexpected(
                            SyncStoreError{object.error().code, object.error().message});
                    }
                }
                if (carried) {
                    const auto parent = link_folder_chain(
                        tree, repository, parent_path(destination), context, "/destination");
                    if (!parent) {
                        return std::unexpected(
                            SyncStoreError{parent.error().code, parent.error().message});
                    }
                    if (carried->parent_id != parent->id) {
                        const auto moved =
                            context.store->set_policy_object_parent(carried->id, parent->id);
                        if (!moved) {
                            return std::unexpected(
                                SyncStoreError{moved.error().code, moved.error().message});
                        }
                    }
                }
                return {};
            };
            auto committed =
                context.sync_store->commit_repository_files(repository.customer_id,
                                                            repository.id,
                                                            input.at("ref"),
                                                            "Move " + source + " to " + destination,
                                                            std::move(operations),
                                                            std::move(authorize));
            if (!committed && carried && !carried->parent_id.empty()) {
                static_cast<void>(
                    context.store->set_policy_object_parent(carried->id, carried->parent_id));
            }
            return committed_drive_change(
                repository, "drive.path.moved", destination, std::move(committed), context);
        }});

    registry.add(Capability{
        .name = "delete_drive_path",
        .title = "Delete drive path",
        .description = "Remove a drive path in a new liquid change without erasing its history.",
        .input_schema = drive_input_schema(true),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "drive path",
        .action = "write",
        .resolve_object = drive_object,
        .handler = [](const json& input, ApiContext& context) {
            control::Repository repository;
            const auto entries = repository_tree(input, context, repository);
            if (!entries) {
                return ApiResult::failure(
                    entries.error().code, entries.error().message, entries.error().path);
            }
            const std::string path = input.at("path");
            const RepositoryTreeEntry* entry = find_entry(*entries, path);
            if (!entry) {
                return ApiResult::failure("not_found", "drive path does not exist", "/path");
            }
            const RepositoryWriteKind kind = entry->kind == "tree"
                                                 ? RepositoryWriteKind::DeletedFolder
                                                 : RepositoryWriteKind::DeletedFile;
            std::vector<RepositoryWriteOperation> operations{{.kind = kind, .path = path}};
            auto authorize = repository_write_authorizer(repository, operations, context, input.at("ref"), branch_protection_blocking(context, repository));
            return committed_drive_change(
                repository,
                "drive.path.deleted",
                path,
                context.sync_store->commit_repository_files(repository.customer_id,
                                                            repository.id,
                                                            input.at("ref"),
                                                            "Delete " + path,
                                                            std::move(operations),
                                                            std::move(authorize)),
                context);
        }});

    registry.add(Capability{
        .name = "drive_history",
        .title = "Drive path history",
        .description = "List only changes whose structural diff touches one path.",
        .input_schema = drive_input_schema(true),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "drive path",
        .action = "read",
        .resolve_object = drive_history_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto repository = context.store->get_repository(input.at("repository_id"));
            if (!repository) {
                return store_error(repository.error());
            }
            const auto changes =
                context.sync_store->repository_log(repository->customer_id, repository->id);
            if (!changes) {
                return sync_error(changes.error());
            }
            json items = json::array();
            const std::string path = input.at("path");
            for (const RepositoryChange& change : *changes) {
                const auto differences = context.sync_store->repository_diff(
                    repository->customer_id, repository->id, change.id);
                if (!differences) {
                    return sync_error(differences.error());
                }
                if (auto value = drive_path_change(change, *differences, path)) {
                    items.push_back(std::move(*value));
                }
            }
            context.audit_customer_id = repository->customer_id;
            context.audit_kind = "drive.history.read";
            context.audit_object = "repository/" + repository->id + "/path/" + path;
            context.audit_details = {{"path", path}, {"changes", items.size()}};
            return ApiResult::success({{"repository", control::to_json(*repository)},
                                       {"path", path},
                                       {"items", std::move(items)}});
        }});

    registry.add(Capability{
        .name = "diff_drive_versions",
        .title = "Diff drive versions",
        .description = "Show what changed in a drive file between two versions, line by line.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"repository_id", {{"type", "string"}}},
                           {"ref", {{"type", "string"}}},
                           {"path", {{"type", "string"}}},
                           {"left", {{"type", "string"}}},
                           {"right", {{"type", "string"}}}}},
                         {"required",
                          json::array({"repository_id", "ref", "path", "left", "right"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "drive path versions",
        .action = "read",
        .resolve_object = drive_history_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto repository = context.store->get_repository(input.at("repository_id"));
            if (!repository) {
                return store_error(repository.error());
            }
            for (const char* side : {"left", "right"}) {
                if (auto refused =
                        authorize_drive_revision(input, input.at(side).get<std::string>(), "read", context)) {
                    return *refused;
                }
            }
            auto left = context.sync_store->repository_file(
                repository->customer_id, repository->id, input.at("left"), input.at("path"));
            if (!left) {
                return sync_error(left.error());
            }
            auto right = context.sync_store->repository_file(
                repository->customer_id, repository->id, input.at("right"), input.at("path"));
            if (!right) {
                return sync_error(right.error());
            }
            const auto left_preview = DriveFormatRegistry::defaults().preview(*left);
            const auto right_preview = DriveFormatRegistry::defaults().preview(*right);
            if (!left_preview || !right_preview || !drive_preview_is_text(*left_preview) ||
                !drive_preview_is_text(*right_preview)) {
                return ApiResult::failure(
                    "invalid_input", "only text versions can be compared", "/path");
            }
            const DriveVersionDiff difference =
                drive_text_diff(std::move(*left), std::move(*right));
            context.audit_customer_id = repository->customer_id;
            context.audit_kind = "drive.versions.diffed";
            context.audit_object =
                "repository/" + repository->id + "/path/" + input.at("path").get<std::string>();
            context.audit_details = {{"path", input.at("path")},
                                     {"left", input.at("left")},
                                     {"right", input.at("right")}};
            json result = diff_json(difference);
            result["repository"] = control::to_json(*repository);
            return ApiResult::success(std::move(result));
        }});

    registry.add(Capability{
        .name = "drive_conflict",
        .title = "Present drive conflict",
        .description = "Explain a conflict and show its preserved values and text difference.",
        .input_schema = drive_input_schema(true),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "drive conflict",
        .action = "read",
        .resolve_object = drive_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto repository = context.store->get_repository(input.at("repository_id"));
            if (!repository) {
                return store_error(repository.error());
            }
            const auto conflicts =
                context.sync_store->repository_conflicts(repository->customer_id, repository->id);
            if (!conflicts) {
                return sync_error(conflicts.error());
            }
            const std::string path = input.at("path");
            const auto found = std::ranges::find_if(
                *conflicts, [&](const auto& conflict) { return conflict.region.path == path; });
            if (found == conflicts->end() || found->version_blobs.size() < 2) {
                return ApiResult::failure(
                    "not_found", "no unresolved conflict exists for this path", "/path");
            }
            std::vector<RepositoryFile> versions;
            for (const std::string& blob : found->version_blobs) {
                auto version = context.sync_store->repository_blob(
                    repository->customer_id, repository->id, path, blob);
                if (!version) {
                    return sync_error(version.error());
                }
                versions.push_back(std::move(*version));
            }
            json values = json::array();
            for (const RepositoryFile& version : versions) {
                values.push_back(file_json(version));
            }
            json result{{"repository", control::to_json(*repository)},
                        {"path", path},
                        {"conflict", conflict_json(*found)},
                        {"versions", std::move(values)}};
            const auto left_preview = DriveFormatRegistry::defaults().preview(versions[0]);
            const auto right_preview = DriveFormatRegistry::defaults().preview(versions[1]);
            if (left_preview && right_preview && drive_preview_is_text(*left_preview) &&
                drive_preview_is_text(*right_preview)) {
                result["difference"] =
                    diff_json(drive_text_diff(std::move(versions[0]), std::move(versions[1])));
            }
            context.audit_customer_id = repository->customer_id;
            context.audit_kind = "drive.conflict.read";
            context.audit_object = "repository/" + repository->id + "/path/" + path;
            context.audit_details = {
                {"conflict", found->id}, {"kind", found->kind}, {"path", path}};
            return ApiResult::success(std::move(result));
        }});

    registry.add(Capability{
        .name = "resolve_conflict",
        .title = "Resolve repository conflict",
        .description =
            "Resolve a conflict with its left, right, reviewed proposal, or supplied blob value.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"repository_id", {{"type", "string"}}},
                           {"ref", {{"type", "string"}}},
                           {"path", {{"type", "string"}}},
                           {"conflict_id", {{"type", "string"}}},
                           {"resolution",
                            {{"type", "string"},
                             {"enum", json::array({"accept_proposal", "left", "right", "blob"})}}},
                           {"blob", {{"type", "string"}, {"contentEncoding", "base64"}}}}},
                         {"required",
                          json::array(
                              {"repository_id", "ref", "path", "conflict_id", "resolution"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"conflict", {{"type", "string"}}},
                            {"value", {{"type", "string"}}},
                            {"change", {{"type", "string"}}}}},
                          {"required", json::array({"conflict", "value", "change"})},
                          {"additionalProperties", false}},
        .kind = CapabilityKind::Action,
        .object = "repository conflict",
        .action = "write",
        .resolve_object = drive_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto repository = context.store->get_repository(input.at("repository_id"));
            if (!repository) {
                return store_error(repository.error());
            }
            const auto conflicts =
                context.sync_store->repository_conflicts(repository->customer_id, repository->id);
            if (!conflicts) {
                return sync_error(conflicts.error());
            }
            const std::string conflict_id = input.at("conflict_id");
            const auto conflict =
                std::ranges::find(*conflicts, conflict_id, &RepositoryConflict::id);
            if (conflict == conflicts->end()) {
                return ApiResult::failure(
                    "not_found", "no unresolved conflict has that identifier", "/conflict_id");
            }
            const std::string path = input.at("path");
            if (conflict->region.path != path) {
                return ApiResult::failure(
                    "invalid_input", "path does not match the conflict region", "/path");
            }
            std::vector<std::byte> blob;
            const std::string resolution = input.at("resolution");
            if (resolution == "blob") {
                if (!input.contains("blob")) {
                    return ApiResult::failure(
                        "invalid_input", "blob resolution requires base64 bytes", "/blob");
                }
                const auto decoded = decode_base64(input.at("blob").get<std::string>(), "/blob");
                if (!decoded) {
                    return ApiResult::failure(
                        decoded.error().code, decoded.error().message, decoded.error().path);
                }
                blob = std::move(*decoded);
            }
            std::vector<RepositoryWriteOperation> operations{
                {.kind = RepositoryWriteKind::File, .path = path}};
            const auto resolved = context.sync_store->resolve_repository_conflict(
                repository->customer_id,
                repository->id,
                conflict_id,
                resolution,
                std::move(blob),
                repository_write_authorizer(*repository, operations, context, "main", branch_protection_blocking(context, *repository)));
            if (!resolved) {
                return sync_error(resolved.error());
            }
            context.audit_customer_id = repository->customer_id;
            context.audit_kind = "repository.conflict.resolved";
            context.audit_object = "repository/" + repository->id + "/path/" + path;
            context.audit_details = {{"change", resolved->change},
                                     {"conflict", resolved->conflict},
                                     {"path", path},
                                     {"resolution", resolution},
                                     {"value", resolved->value}};
            follow_in_search(*repository, context);
            return ApiResult::success({{"conflict", resolved->conflict},
                                       {"value", resolved->value},
                                       {"change", resolved->change}});
        }});

    registry.add(Capability{
        .name = "restore_drive_version",
        .title = "Restore drive file version",
        .description = "Write a historical file version and record a new liquid change.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"repository_id", {{"type", "string"}}},
                           {"ref", {{"type", "string"}}},
                           {"path", {{"type", "string"}}},
                           {"change_id", {{"type", "string"}}}}},
                         {"required", json::array({"repository_id", "ref", "path", "change_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "drive file",
        .action = "write",
        .resolve_object = drive_history_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto repository = context.store->get_repository(input.at("repository_id"));
            if (!repository) {
                return store_error(repository.error());
            }
            // The version restored is read as it was (lake#923): its node at change_id must be
            // readable, not only the node the path names now. The write to the current tree is
            // checked by repository_write_authorizer.
            if (auto refused = authorize_drive_revision(
                    input, input.at("change_id").get<std::string>(), "read", context)) {
                return *refused;
            }
            std::vector<RepositoryWriteOperation> operations{
                {.kind = RepositoryWriteKind::File, .path = input.at("path")}};
            const auto restored = context.sync_store->restore_repository_file(
                repository->customer_id,
                repository->id,
                input.at("change_id"),
                input.at("path"),
                repository_write_authorizer(*repository, operations, context, "main", branch_protection_blocking(context, *repository)));
            if (!restored) {
                return sync_error(restored.error());
            }
            context.audit_customer_id = repository->customer_id;
            context.audit_kind = "drive.version.restored";
            context.audit_object =
                "repository/" + repository->id + "/path/" + input.at("path").get<std::string>();
            context.audit_details = {{"path", input.at("path")},
                                     {"source_change", input.at("change_id")},
                                     {"restored_change", restored->id}};
            follow_in_search(*repository, context);
            return ApiResult::success(change_json(*restored));
        }});

    registry.add(Capability{
        .name = "share_drive_folder",
        .title = "Share drive folder",
        .description =
            "Give a person or organization a live grant on a folder. Name one of the account's "
            "people by `recipient` (email or handle; `drive_share_people` lists them), or a known "
            "TAAP identity by `principal_taap_id`. Sharing outside the account requires folder "
            "admin. The caller must hold every action the role grants. Lake signs the grant's "
            "attestation and answers its digest, kept on the grant as evidence. "
            "`attestation_digest` is optional; only the digest Lake answered for this same share "
            "is accepted.",
        .input_schema =
            {{"type", "object"},
             {"properties",
              {{"repository_id", {{"type", "string"}}},
               {"ref", {{"type", "string"}}},
               {"path", {{"type", "string"}}},
               {"recipient",
                {{"type", "string"},
                 {"description",
                  "The person's email or handle (\"@ana\" or \"ana\"), among the account's "
                  "people."}}},
               {"principal_taap_id",
                {{"type", "string"},
                 {"description",
                  "For programs: a person's or organization's TAAP identity that Lake knows. Send "
                  "this or `recipient`; sharing outside the account requires folder admin."}}},
               {"role", {{"type", "string"}, {"enum", json::array({"read", "write", "share"})}}},
               {"attestation_digest",
                {{"type", "string"},
                 {"description",
                  "Optional. Lake makes the attestation; a digest sent must be the one Lake "
                  "answered for this same folder, person and role."}}}}},
             {"required", json::array({"repository_id", "ref", "path", "role"})},
             {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "drive folder",
        .action = "share",
        .resolve_object = drive_object,
        .handler = [](const json& input, ApiContext& context) {
            if (input.contains("recipient") == input.contains("principal_taap_id")) {
                return ApiResult::failure(
                    "invalid_input",
                    "name the person to share with: `recipient` (an email or handle), or, for a "
                    "program, `principal_taap_id`",
                    "/recipient");
            }
            const auto location = ensure_folder_location(input, context);
            if (!location) {
                return ApiResult::failure(
                    location.error().code, location.error().message, location.error().path);
            }
            const std::string role = input.at("role");
            if (auto allowed = require_drive_share_role(*location, role, context); !allowed) {
                return ApiResult::failure(
                    allowed.error().code, allowed.error().message, allowed.error().path);
            }
            Recipient recipient;
            if (input.contains("recipient")) {
                auto resolved = resolve_recipient(
                    context, location->repository, input.at("recipient").get<std::string>());
                if (!resolved) {
                    return ApiResult::failure(
                        resolved.error().code, resolved.error().message, resolved.error().path);
                }
                recipient = std::move(*resolved);
            } else {
                // The identity-level input is validated (lake#585): lake must know the identity.
                const std::string taap_id = input.at("principal_taap_id");
                const auto names = context.store->principal_names({taap_id});
                if (!names) {
                    return store_error(names.error());
                }
                if (!names->contains(taap_id)) {
                    return ApiResult::failure("invalid_input",
                                              "Lake knows no person or organization with that "
                                              "TAAP identity; name the person by `recipient` "
                                              "(their email or handle) instead",
                                              "/principal_taap_id");
                }
                if (!context.operator_credential) {
                    control::AuthorizationCache cache =
                        control::fresh_cache_like(context.authorization_cache);
                    const auto administers = control::authorize(*context.store,
                                                                context.acting_identity,
                                                                location->object.id,
                                                                "admin",
                                                                cache);
                    if (!administers) {
                        return store_error(administers.error());
                    }
                    if (!administers->allowed) {
                        const auto people = repository_people(context, location->repository);
                        if (!people) {
                            return ApiResult::failure(people.error().code, people.error().message);
                        }
                        if (std::ranges::none_of(*people, [&](const auto& person) {
                                return person.taap_id == taap_id;
                            })) {
                            return ApiResult::failure(
                                "unauthorized",
                                "only a folder admin may share outside the account",
                                "/principal_taap_id");
                        }
                    }
                }
                recipient.taap_id = taap_id;
                recipient.display_name = names->at(taap_id);
            }
            const auto attestation = share_attestation(context, *location, recipient.taap_id, role);
            if (!attestation) {
                return ApiResult::failure(attestation.error().code, attestation.error().message);
            }
            if (auto supplied = check_supplied_digest(input, *attestation, "a share"); !supplied) {
                return ApiResult::failure(
                    supplied.error().code, supplied.error().message, supplied.error().path);
            }
            auto grant = context.store->create_grant(control::NewGrant{recipient.taap_id,
                                                                       location->object.id,
                                                                       role,
                                                                       attestation->digest,
                                                                       context.acting_identity,
                                                                       context.request_id});
            bool existing = false;
            if (!grant && grant.error().code == "conflict") {
                // Sharing again what is already shared answers the grant that is there.
                const auto held =
                    context.store->list_grants({location->object.id}, recipient.taap_id, 100);
                if (held) {
                    for (const control::Grant& item : *held) {
                        if (item.role == role) {
                            grant = item;
                            existing = true;
                            break;
                        }
                    }
                }
            }
            if (!grant) {
                return store_error(grant.error());
            }
            const json signed_statement = attestation->to_json();
            context.audit_customer_id = location->repository.customer_id;
            context.audit_kind = "drive.folder.shared";
            context.audit_object =
                "repository/" + location->repository.id + "/path/" + location->path;
            context.audit_details = {{"delivery", "live"},
                                     {"node_id", location->node_id},
                                     {"path", location->path},
                                     {"recipient", recipient.taap_id},
                                     {"role", role},
                                     {"attestation", signed_statement}};
            json person = recipient_json(recipient);
            context.audit_details["existing"] = existing;
            return ApiResult::success({{"delivery", "live"},
                                       {"object", control::to_json(location->object)},
                                       {"grant", control::to_json(*grant)},
                                       {"existing", existing},
                                       {"recipient", std::move(person)},
                                       {"attestation", signed_statement}});
        }});

    registry.add(Capability{
        .name = "drive_share_people",
        .title = "People to Share With",
        .description =
            "The people a folder can be shared with by name: the owner of the repository's account, and "
            "for an organization the organization and its members, each with their TAAP identity, "
            "display name, email and handle. share_drive_folder's `recipient` takes the email or "
            "handle.",
        .input_schema = drive_input_schema(true),
        .output_schema =
            {{"type", "object"},
             {"properties",
              {{"people",
                {{"type", "array"},
                 {"items",
                  {{"type", "object"},
                   {"properties",
                    {{"taap_id", {{"type", "string"}}},
                     {"kind",
                      {{"type", "string"}, {"enum", json::array({"user", "organization"})}}},
                     {"display_name", {{"type", "string"}}},
                     {"email", {{"type", "string"}}},
                     {"handle", {{"type", "string"}}}}},
                   {"required",
                    json::array({"taap_id", "kind", "display_name", "email", "handle"})},
                   {"additionalProperties", false}}}}}}},
             {"required", json::array({"people"})},
             {"additionalProperties", false}},
        .kind = CapabilityKind::Read,
        .object = "drive folder",
        .action = "share",
        .resolve_object = drive_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto repository = context.store->get_repository(input.at("repository_id"));
            if (!repository) {
                return store_error(repository.error());
            }
            const auto people = repository_people(context, *repository);
            if (!people) {
                return ApiResult::failure(people.error().code, people.error().message);
            }
            json out = json::array();
            for (const Recipient& person : *people) {
                out.push_back(recipient_json(person));
            }
            context.audit_customer_id = repository->customer_id;
            context.audit_kind = "drive.share_people.read";
            context.audit_object = "repository/" + repository->id;
            context.audit_details = {{"people", out.size()}};
            return ApiResult::success({{"people", std::move(out)}});
        }});

    registry.add(Capability{
        .name = "drive_effective_access",
        .title = "Drive effective access",
        .description = "Show every direct and inherited role plus the folder's active and expired "
                       "share links held in one workspace (`workspace_id`, a workspace of the "
                       "repository's account).",
        .input_schema = share_links_input_schema(),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "drive folder",
        .action = "share",
        .resolve_object = drive_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            const auto location = nearest_drive_location(input, context);
            if (!location) {
                co_return ApiResult::failure(
                    location.error().code, location.error().message, location.error().path);
            }
            const auto workspace = share_link_workspace(
                context, input.at("workspace_id"), location->repository.customer_id,
                "/workspace_id");
            if (!workspace) {
                co_return ApiResult::failure(
                    workspace.error().code, workspace.error().message, workspace.error().path);
            }
            const auto access = control::effective_access(
                *context.store, location->object.id, context.authorization_cache);
            if (!access) {
                co_return store_error(access.error());
            }
            auto records = workspace_records(context, workspace->customer_id, workspace->id);
            if (!records) {
                co_return ApiResult::failure(records.error().code, records.error().message);
            }
            control::DriveShareLinks held(*records);
            const auto links = control::settled(co_await held.list(
                control::drive_share_link_lookup(location->repository.id, location->object.id),
                1000));
            if (!links) {
                co_return ApiResult::failure(links.error().code, links.error().message);
            }
            json access_items = json::array();
            for (const control::EffectiveAccessEntry& entry : *access) {
                access_items.push_back(control::to_json(entry));
            }
            json link_items = json::array();
            for (auto link = links->rbegin(); link != links->rend(); ++link) {
                json value = control::to_json(*link);
                value["workspace_id"] = workspace->id;
                value["active"] = link_is_active(*link);
                value["delivery"] = "link";
                link_items.push_back(std::move(value));
            }
            co_return ApiResult::success(
                {{"repository", control::to_json(location->repository)},
                 {"object", control::to_json(location->object)},
                 {"path", location->path},
                 {"workspace_id", workspace->id},
                 {"items", std::move(access_items)},
                 {"links", std::move(link_items)},
                 {"delivery_choices",
                  {{{"kind", "live"}, {"meaning", "updates and revocation continue to apply"}},
                   {{"kind", "link"}, {"meaning", "scope, role, expiry, and revocation apply"}},
                   {{"kind", "copy"}, {"meaning", "independent bytes cannot be recalled"}}}}});
        }});

    registry.add(Capability{
        .name = "drive_shared_with_me",
        .title = "Drive folders shared with me",
        .description = "List live folders granted directly to the signed-in identity.",
        .input_schema = {{"type", "object"},
                         {"properties", json::object()},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "shared drive folders",
        .action = "read",
        .resolve_object = [](const json&, ApiContext&) -> std::expected<std::string, ApiError> {
            return std::string{};
        },
        .authorizes_candidates = true,
        .handler =
            [](const json&, ApiContext& context) {
                if (!context.store || !context.sync_store) {
                    return ApiResult::failure("store_unavailable",
                                              "drive stores are not configured");
                }
                if (context.acting_identity.empty() || context.acting_identity == "admin") {
                    return ApiResult::success({{"items", json::array()}});
                }
                const auto grants = context.store->list_grants({}, context.acting_identity, 1000);
                if (!grants) {
                    return store_error(grants.error());
                }
                json items = json::array();
                for (const control::Grant& grant : *grants) {
                    const auto object = context.store->get_policy_object(grant.object_id);
                    if (!object) {
                        return store_error(object.error());
                    }
                    if (object->object_type != "path") {
                        continue;
                    }
                    const std::size_t separator = object->external_id.find(':');
                    if (separator == std::string::npos || separator == 0 ||
                        separator + 1 == object->external_id.size()) {
                        continue;
                    }
                    const std::string repository_id = object->external_id.substr(0, separator);
                    const std::string node_id = object->external_id.substr(separator + 1);
                    const auto repository = context.store->get_repository(repository_id);
                    if (!repository) {
                        return store_error(repository.error());
                    }
                    if (!repository->deleted_at.empty()) {
                        continue;
                    }
                    const auto tree = context.sync_store->repository_tree(
                        repository->customer_id, repository->id, "main");
                    if (!tree) {
                        return sync_error(tree.error());
                    }
                    const auto entry =
                        std::ranges::find(*tree, node_id, &RepositoryTreeEntry::node_id);
                    json item{{"repository", control::to_json(*repository)},
                              {"ref", "main"},
                              {"node_id", node_id},
                              {"role", grant.role},
                              {"shared_by", grant.granted_by_taap_id},
                              {"delivery", "live"},
                              {"available", entry != tree->end()},
                              {"actions", json::array()}};
                    if (entry != tree->end() && entry->kind == "tree") {
                        item["path"] = entry->path;
                    } else {
                        item["path"] = "";
                        item["available"] = false;
                    }
                    for (const std::string_view action : {"read", "write", "share"}) {
                        if (control::role_allows(grant.role, action)) {
                            item["actions"].push_back(action);
                        }
                    }
                    items.push_back(std::move(item));
                }
                context.audit_kind = "drive.shared_with_me.read";
                context.audit_details = {{"items", items.size()}};
                return ApiResult::success({{"items", std::move(items)}});
            }});

    registry.add(Capability{
        .name = "create_drive_share_link",
        .title = "Create drive share link",
        .description = "Create a folder-scoped link with an explicit role and expiry. The link is "
                       "held in the workspace Zetta of `workspace_id`, a workspace of the "
                       "repository's account; its bearer token names that workspace.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"repository_id", {{"type", "string"}}},
                           {"workspace_id", {{"type", "string"}}},
                           {"ref", {{"type", "string"}}},
                           {"path", {{"type", "string"}}},
                           {"role", {{"type", "string"}, {"enum", json::array({"read", "write"})}}},
                           {"expires_at", {{"type", "string"}}}}},
                         {"required",
                          json::array({"repository_id", "workspace_id", "ref", "path", "role",
                                       "expires_at"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "drive folder",
        .action = "share",
        .resolve_object = drive_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            const std::string expires_at = input.at("expires_at");
            if (expires_at <= control::now_iso8601()) {
                co_return ApiResult::failure(
                    "invalid_input", "expires_at must be in the future", "/expires_at");
            }
            const auto location = ensure_folder_location(input, context);
            if (!location) {
                co_return ApiResult::failure(
                    location.error().code, location.error().message, location.error().path);
            }
            if (auto allowed = require_drive_share_role(
                    *location, input.at("role").get<std::string>(), context);
                !allowed) {
                co_return ApiResult::failure(
                    allowed.error().code, allowed.error().message, allowed.error().path);
            }
            const auto workspace = share_link_workspace(
                context, input.at("workspace_id"), location->repository.customer_id,
                "/workspace_id");
            if (!workspace) {
                co_return ApiResult::failure(
                    workspace.error().code, workspace.error().message, workspace.error().path);
            }
            auto records = workspace_records(context, workspace->customer_id, workspace->id);
            if (!records) {
                co_return ApiResult::failure(records.error().code, records.error().message);
            }
            const std::string id = control::new_uuid_v4();
            const std::string token = control::drive_share_token(workspace->id, id, new_link_secret());
            control::DriveShareLink link;
            link.id = id;
            link.repository_id = location->repository.id;
            link.object_id = location->object.id;
            link.node_id = location->node_id;
            link.path = location->path;
            link.role = input.at("role");
            link.token_hash = token_hash(token);
            link.expires_at = expires_at;
            link.created_by_taap_id = context.acting_identity;
            control::DriveShareLinks held(*records);
            const auto stored =
                control::settled(co_await held.append(link, control::RecordWrite{id, 0}));
            if (!stored) {
                co_return ApiResult::failure(stored.error().code, stored.error().message);
            }
            // The policy engine stays the authority for the link's scope and role.
            const auto grant = context.store->create_grant({"share-link:" + id,
                                                            link.object_id,
                                                            link.role,
                                                            "drive_share_link/" + id,
                                                            context.acting_identity,
                                                            context.request_id});
            if (!grant) {
                co_await records->remove(control::DriveShareLinkCodec::collection, id);
                co_return store_error(grant.error());
            }
            context.audit_customer_id = location->repository.customer_id;
            context.audit_kind = "drive.share_link.created";
            context.audit_object =
                "repository/" + location->repository.id + "/path/" + location->path;
            context.audit_details = {{"link_id", stored->id},
                                     {"workspace_id", workspace->id},
                                     {"node_id", stored->node_id},
                                     {"path", stored->path},
                                     {"role", stored->role},
                                     {"expires_at", stored->expires_at}};
            json output = control::to_json(*stored);
            output["workspace_id"] = workspace->id;
            output["delivery"] = "link";
            output["url"] = "/share/" + token;
            output["token"] = token;
            co_return ApiResult::success(std::move(output));
        }});

    registry.add(Capability{
        .name = "resolve_drive_share_link",
        .title = "Open drive share link",
        .description = "Resolve an active link and browse its scoped live folder.",
        .input_schema = share_read_input_schema(false),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .requires_auth = false,
        .object = "drive share link",
        .action = "read",
        .resolve_object = share_token_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            std::expected<SharedDriveTarget, ApiError> shared;
            if (auto opened = co_await shared_target(input, context, shared); !opened) {
                co_return ApiResult::failure("store_error", opened.error().message);
            } else if (!opened->ok) {
                co_return std::move(*opened);
            }
            if (shared->target.kind != "tree") {
                co_return ApiResult::failure("invalid_input", "path must name a folder", "/path");
            }
            auto result =
                browse_result(shared->location, shared->entries, context, context.acting_identity);
            if (result.ok) {
                result.value["root_path"] = shared->root_path;
            }
            co_return result;
        }});

    for (const bool download : {false, true}) {
        registry.add(Capability{
            .name = download ? "download_drive_share_link" : "preview_drive_share_link",
            .title = download ? "Download shared drive file" : "Preview shared drive file",
            .description =
                download
                    ? "Read exact file bytes as base64 using only an active link's scoped authority."
                    : "Preview a file using only an active link's scoped read authority.",
            .input_schema = share_read_input_schema(true),
            .output_schema = shared_file_output_schema(download),
            .kind = CapabilityKind::Read,
            .requires_auth = false,
            .object = "drive share link",
            .action = "read",
            .resolve_object = share_token_object,
            .async_handler = [download](const json& input,
                                        ApiContext& context) -> liquid::runtime::task<ApiResult> {
                std::expected<SharedDriveTarget, ApiError> shared;
                if (auto opened = co_await shared_target(input, context, shared); !opened) {
                    co_return ApiResult::failure("store_error", opened.error().message);
                } else if (!opened->ok) {
                    co_return std::move(*opened);
                }
                co_return shared_file_result(input, context, download, shared);
            }});
    }

    registry.add(Capability{
        .name = "revoke_drive_share_link",
        .title = "Revoke drive share link",
        .description = "Revoke a continuing share capability immediately. The link is revoked in "
                       "the workspace Zetta of `workspace_id` that holds it.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"workspace_id", {{"type", "string"}}},
                           {"link_id", {{"type", "string"}}}}},
                         {"required", json::array({"workspace_id", "link_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "drive share link",
        .action = "share",
        .resolve_object = link_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            const std::string link_id = input.at("link_id");
            const auto folder = share_link_grant_object(link_id, context, "/link_id");
            if (!folder) {
                co_return ApiResult::failure(
                    folder.error().code, folder.error().message, folder.error().path);
            }
            // The folder's path object is `<repository>:<node>`; its repository's space is the
            // only one whose workspaces can hold the link.
            const auto folder_object = context.store->get_policy_object(*folder);
            if (!folder_object) {
                co_return store_error(folder_object.error());
            }
            const auto repository = context.store->get_repository(
                folder_object->external_id.substr(0, folder_object->external_id.find(':')));
            if (!repository) {
                co_return store_error(repository.error());
            }
            const auto workspace = share_link_workspace(
                context, input.at("workspace_id"), repository->customer_id, "/workspace_id");
            if (!workspace) {
                co_return ApiResult::failure(
                    workspace.error().code, workspace.error().message, workspace.error().path);
            }
            auto records = workspace_records(context, workspace->customer_id, workspace->id);
            if (!records) {
                co_return ApiResult::failure(records.error().code, records.error().message);
            }
            const auto collection = control::DriveShareLinkCodec::collection;
            // Read, then write on the revision read; a concurrent write rereads (one revocation
            // wins, a second is `conflict`).
            for (int attempt = 0; attempt < 5; ++attempt) {
                const auto stored = control::settled(co_await records->get(collection, link_id));
                if (!stored) {
                    co_return ApiResult::failure(stored.error().code, stored.error().message);
                }
                const auto link =
                    *stored ? control::DriveShareLinkCodec::decode(**stored) : std::nullopt;
                if (!link || link->object_id != *folder) {
                    co_return ApiResult::failure(
                        "not_found", "no drive share link " + link_id + " in this workspace",
                        "/link_id");
                }
                if (!link->revoked_at.empty()) {
                    co_return ApiResult::failure("conflict", "drive share link is already revoked");
                }
                control::DriveShareLink revoked = *link;
                revoked.revoked_at = control::now_iso8601();
                const auto written = control::settled(co_await records->put(
                    collection, link_id, control::DriveShareLinkCodec::lookup(revoked),
                    control::DriveShareLinkCodec::encode(revoked),
                    control::RecordCondition{.if_revision = (*stored)->revision},
                    (*stored)->lake_revision + 1, (*stored)->at_us));
                if (!written && written.error().code == "conflict") {
                    continue;
                }
                if (!written) {
                    co_return ApiResult::failure(written.error().code, written.error().message);
                }
                const auto saved = control::DriveShareLinkCodec::decode(*written);
                if (!saved) {
                    co_return ApiResult::failure("store_error", "the stored link did not decode");
                }
                context.audit_customer_id = repository->customer_id;
                context.audit_kind = "drive.share_link.revoked";
                context.audit_object =
                    "repository/" + saved->repository_id + "/path/" + saved->path;
                context.audit_details = {
                    {"link_id", saved->id}, {"workspace_id", workspace->id}, {"path", saved->path}};
                json output = control::to_json(*saved);
                output["workspace_id"] = workspace->id;
                co_return ApiResult::success(std::move(output));
            }
            co_return ApiResult::failure("conflict", "the link changed while it was being revoked");
        }});
}

} // namespace lake::api
