#include "lake/api/capabilities.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "lake/api/drive.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/store.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

struct DriveLocation {
    control::Repository repository;
    control::PolicyObject object;
    std::string node_id;
    std::string path;
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

std::string parent_path(std::string path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

const RepositoryTreeEntry* find_entry(const std::vector<RepositoryTreeEntry>& entries,
                                      std::string_view path) {
    const auto found = std::ranges::find(entries, path, &RepositoryTreeEntry::path);
    return found == entries.end() ? nullptr : &*found;
}

std::expected<std::vector<RepositoryTreeEntry>, ApiError>
repository_tree(const json& input, ApiContext& context, control::Repository& repository) {
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
        return std::unexpected(ApiError{entries.error().code, entries.error().message, "/ref"});
    }
    return *entries;
}

std::expected<DriveLocation, ApiError>
nearest_drive_location(const json& input, ApiContext& context, bool require_path = true) {
    control::Repository repository;
    const auto entries = repository_tree(input, context, repository);
    if (!entries) {
        return std::unexpected(entries.error());
    }
    std::string path = input.value("path", "");
    if (require_path && !path.empty() && !find_entry(*entries, path)) {
        return std::unexpected(ApiError{"not_found", "no drive path " + path, "/path"});
    }
    std::string candidate = find_entry(*entries, path) ? path : parent_path(path);
    while (!candidate.empty()) {
        const RepositoryTreeEntry* entry = find_entry(*entries, candidate);
        if (entry && !entry->node_id.empty()) {
            const auto object = context.store->find_policy_object(
                "path", path_external_id(repository.id, entry->node_id));
            if (object) {
                return DriveLocation{repository, *object, entry->node_id, path};
            }
            if (object.error().code != "not_found") {
                return std::unexpected(
                    ApiError{object.error().code, object.error().message, "/path"});
            }
        }
        candidate = parent_path(candidate);
    }
    const auto object = context.store->find_policy_object("repository", repository.id);
    if (!object) {
        return std::unexpected(
            ApiError{object.error().code, object.error().message, "/repository_id"});
    }
    return DriveLocation{repository, *object, "", path};
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
    auto parent = context.store->find_policy_object("repository", repository.id);
    if (!parent) {
        return std::unexpected(
            ApiError{parent.error().code, parent.error().message, "/repository_id"});
    }
    std::size_t offset = 0;
    while (offset < path.size()) {
        const std::size_t slash = path.find('/', offset);
        const std::string prefix = path.substr(0, slash);
        const RepositoryTreeEntry* entry = find_entry(*entries, prefix);
        if (!entry || entry->kind != "tree" || entry->node_id.empty()) {
            return std::unexpected(
                ApiError{"invalid_input", "every shared path component must be a folder", "/path"});
        }
        const std::string external_id = path_external_id(repository.id, entry->node_id);
        auto object = context.store->find_policy_object("path", external_id);
        if (!object && object.error().code == "not_found") {
            object = context.store->create_policy_object(
                control::NewPolicyObject{"path",
                                         external_id,
                                         parent->id,
                                         context.acting_identity,
                                         context.request_id,
                                         true});
        }
        if (!object) {
            return std::unexpected(ApiError{object.error().code, object.error().message, "/path"});
        }
        parent = object;
        if (slash == std::string::npos) {
            break;
        }
        offset = slash + 1;
    }
    return DriveLocation{repository, *parent, target->node_id, path};
}

std::expected<std::string, ApiError> drive_object(const json& input, ApiContext& context) {
    const auto location = nearest_drive_location(input, context);
    if (!location) {
        return std::unexpected(location.error());
    }
    return location->object.id;
}

std::expected<std::string, ApiError> drive_history_object(const json& input, ApiContext& context) {
    const auto location = nearest_drive_location(input, context, false);
    if (!location) {
        return std::unexpected(location.error());
    }
    return location->object.id;
}

std::expected<std::string, ApiError> link_object(const json& input, ApiContext& context) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const auto link = context.store->get_drive_share_link(input.at("link_id"));
    if (!link) {
        return std::unexpected(ApiError{link.error().code, link.error().message, "/link_id"});
    }
    return link->object_id;
}

std::expected<std::string, ApiError> share_token_object(const json& input, ApiContext& context) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const auto link = context.store->get_drive_share_link_by_token_hash(
        token_hash(input.at("token").get<std::string>()));
    if (!link) {
        return std::unexpected(ApiError{link.error().code, link.error().message, "/token"});
    }
    return link->object_id;
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
    return ApiResult::success({{"change", *committed}, {"path", audit_path}});
}

json change_json(const RepositoryChange& change) {
    return {{"id", change.id},
            {"parents", change.parents},
            {"author", change.author},
            {"message", change.message},
            {"occurred_at", change.occurred_at}};
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

std::string new_link_token() {
    return control::new_uuid_v4() + control::new_uuid_v4();
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
    for (const RepositoryTreeEntry& entry : folder_children(entries, location.path)) {
        bool allowed = true;
        if (context.acting_identity != "admin" && !entry.node_id.empty()) {
            const auto object = context.store->find_policy_object(
                "path", path_external_id(location.repository.id, entry.node_id));
            if (object) {
                const auto decision = control::authorize(*context.store,
                                                         std::string(principal),
                                                         object->id,
                                                         "read",
                                                         context.authorization_cache);
                if (!decision) {
                    return store_error(decision.error());
                }
                allowed = decision->allowed;
            } else if (object.error().code != "not_found") {
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

std::expected<control::DriveShareLink, ApiError> active_share_link(const json& input,
                                                                   ApiContext& context) {
    context.sync_response.clear();
    if (!context.store || !context.sync_store) {
        return std::unexpected(
            ApiError{"store_unavailable", "drive stores are not configured", {}});
    }
    const auto link = context.store->get_drive_share_link_by_token_hash(
        token_hash(input.at("token").get<std::string>()));
    if (!link) {
        return std::unexpected(ApiError{link.error().code, link.error().message, "/token"});
    }
    context.acting_identity = "share-link:" + link->id;
    context.audit_details = {{"link_id", link->id}};
    if (!link_is_active(*link)) {
        return std::unexpected(ApiError{"unauthorized", "share link is expired or revoked", {}});
    }
    if (link->role != "read" && link->role != "write") {
        return std::unexpected(ApiError{"unauthorized", "share link does not allow reads", {}});
    }
    // A reused caller context must not retain policy from a previous link operation.
    context.authorization_cache = {};
    const auto decision = control::authorize(*context.store,
                                             context.acting_identity,
                                             link->object_id,
                                             "read",
                                             context.authorization_cache);
    if (!decision) {
        return std::unexpected(ApiError{decision.error().code, decision.error().message, {}});
    }
    if (!decision->allowed) {
        return std::unexpected(ApiError{"unauthorized", "share link does not allow reads", {}});
    }
    return *link;
}

std::expected<control::PolicyObject, ApiError>
shared_path_object(const control::DriveShareLink& link,
                   const std::vector<RepositoryTreeEntry>& entries,
                   std::string path,
                   std::string_view root_path,
                   ApiContext& context) {
    while (path_touches(path, root_path)) {
        const auto* entry = find_entry(entries, path);
        if (entry && !entry->node_id.empty()) {
            const auto object = context.store->find_policy_object(
                "path", path_external_id(link.repository_id, entry->node_id));
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
            if (object.error().code != "not_found") {
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

std::expected<SharedDriveTarget, ApiError> shared_drive_target(const json& input,
                                                               ApiContext& context) {
    const auto link = active_share_link(input, context);
    if (!link) {
        return std::unexpected(link.error());
    }
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
    return SharedDriveTarget{
        {repository, *object, entry->node_id, path}, root_path, *entry, std::move(*entries)};
}

ApiResult shared_file_result(const json& input, ApiContext& context, bool download) {
    const auto shared = shared_drive_target(input, context);
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
        .description = "Read one drive file as bytes with its digest and media type.",
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
            context.sync_response = file->raw_contents;
            if (context.sync_response.empty() && !file->contents.empty()) {
                context.sync_response.reserve(file->contents.size());
                std::ranges::transform(file->contents,
                                       std::back_inserter(context.sync_response),
                                       [](char value) { return static_cast<std::byte>(value); });
            }
            context.audit_customer_id = repository->customer_id;
            context.audit_kind = "drive.file.downloaded";
            context.audit_object = "repository/" + repository->id + "/path/" + file->path;
            context.audit_details = {{"blob", file->blob}, {"path", file->path}};
            return ApiResult::success({{"path", file->path},
                                       {"blob", file->blob},
                                       {"size", file->size},
                                       {"media_type", "application/octet-stream"}});
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
            return committed_drive_change(
                repository,
                "drive.folder.created",
                target,
                context.sync_store->commit_repository_files(repository.customer_id,
                                                            repository.id,
                                                            input.at("ref"),
                                                            "Create folder " + target,
                                                            std::move(operations)),
                context);
        }});

    registry.add(Capability{
        .name = "upload_drive_files",
        .title = "Upload drive files",
        .description = "Upload files into a drive folder as one liquid change.",
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
                    {{"name",
                      {{"type", "string"},
                       {"description", "A single file name within the target folder."}}},
                     {"content",
                      {{"type", "string"},
                       {"contentEncoding", "base64"},
                       {"description",
                        "File bytes encoded as padded base64; an empty string creates an empty "
                        "file."}}}}},
                   {"required", json::array({"name", "content"})},
                   {"additionalProperties", false}}}}}}},
             {"required", json::array({"repository_id", "ref", "path", "files"})},
             {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "drive folder",
        .action = "write",
        .resolve_object = drive_object,
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
                const std::string name = value.value("name", "");
                if (!valid_path_name(name)) {
                    return ApiResult::failure("invalid_input",
                                              "uploaded file name is invalid",
                                              "/files/" + std::to_string(index) + "/name");
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
            return committed_drive_change(
                *repository,
                "drive.files.uploaded",
                audit_path,
                context.sync_store->commit_repository_files(repository->customer_id,
                                                            repository->id,
                                                            input.at("ref"),
                                                            "Upload drive files",
                                                            std::move(operations)),
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
            std::vector<RepositoryWriteOperation> operations{
                {.kind = RepositoryWriteKind::Moved, .path = destination, .source_path = source}};
            return committed_drive_change(
                repository,
                "drive.path.moved",
                destination,
                context.sync_store->commit_repository_files(repository.customer_id,
                                                            repository.id,
                                                            input.at("ref"),
                                                            "Move " + source + " to " + destination,
                                                            std::move(operations)),
                context);
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
            return committed_drive_change(
                repository,
                "drive.path.deleted",
                path,
                context.sync_store->commit_repository_files(repository.customer_id,
                                                            repository.id,
                                                            input.at("ref"),
                                                            "Delete " + path,
                                                            std::move(operations)),
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
                for (const RepositoryDiffEntry& difference : *differences) {
                    if (difference.path != path && difference.old_path != path) {
                        continue;
                    }
                    json value = change_json(change);
                    value["path"] = difference.path;
                    value["old_path"] = difference.old_path;
                    value["changes"] = difference.changes;
                    value["old_blob"] = difference.old_blob;
                    value["new_blob"] = difference.new_blob;
                    value["old_conflicts"] = difference.old_conflicts;
                    value["new_conflicts"] = difference.new_conflicts;
                    items.push_back(std::move(value));
                    break;
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
            const auto resolved = context.sync_store->resolve_repository_conflict(
                repository->customer_id, repository->id, conflict_id, resolution, std::move(blob));
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
            const auto restored = context.sync_store->restore_repository_file(
                repository->customer_id, repository->id, input.at("change_id"), input.at("path"));
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
            return ApiResult::success(change_json(*restored));
        }});

    registry.add(Capability{
        .name = "share_drive_folder",
        .title = "Share drive folder",
        .description = "Grant a named recipient a live folder with an explicit role.",
        .input_schema =
            {{"type", "object"},
             {"properties",
              {{"repository_id", {{"type", "string"}}},
               {"ref", {{"type", "string"}}},
               {"path", {{"type", "string"}}},
               {"principal_taap_id", {{"type", "string"}}},
               {"role", {{"type", "string"}, {"enum", json::array({"read", "write", "share"})}}},
               {"attestation_digest", {{"type", "string"}}}}},
             {"required",
              json::array({"repository_id",
                           "ref",
                           "path",
                           "principal_taap_id",
                           "role",
                           "attestation_digest"})},
             {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "drive folder",
        .action = "share",
        .resolve_object = drive_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto location = ensure_folder_location(input, context);
            if (!location) {
                return ApiResult::failure(
                    location.error().code, location.error().message, location.error().path);
            }
            const auto grant =
                context.store->create_grant(control::NewGrant{input.at("principal_taap_id"),
                                                              location->object.id,
                                                              input.at("role"),
                                                              input.at("attestation_digest"),
                                                              context.acting_identity,
                                                              context.request_id});
            if (!grant) {
                return store_error(grant.error());
            }
            context.audit_customer_id = location->repository.customer_id;
            context.audit_kind = "drive.folder.shared";
            context.audit_object =
                "repository/" + location->repository.id + "/path/" + location->path;
            context.audit_details = {{"delivery", "live"},
                                     {"node_id", location->node_id},
                                     {"path", location->path},
                                     {"recipient", input.at("principal_taap_id")},
                                     {"role", input.at("role")}};
            return ApiResult::success({{"delivery", "live"},
                                       {"object", control::to_json(location->object)},
                                       {"grant", control::to_json(*grant)}});
        }});

    registry.add(Capability{
        .name = "drive_effective_access",
        .title = "Drive effective access",
        .description = "Show every direct and inherited role plus active and expired share links.",
        .input_schema = drive_input_schema(true),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "drive folder",
        .action = "share",
        .resolve_object = drive_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto location = nearest_drive_location(input, context);
            if (!location) {
                return ApiResult::failure(
                    location.error().code, location.error().message, location.error().path);
            }
            const auto access = control::effective_access(
                *context.store, location->object.id, context.authorization_cache);
            if (!access) {
                return store_error(access.error());
            }
            const auto links = context.store->list_drive_share_links(
                location->repository.id, location->object.id, 1000);
            if (!links) {
                return store_error(links.error());
            }
            json access_items = json::array();
            for (const control::EffectiveAccessEntry& entry : *access) {
                access_items.push_back(control::to_json(entry));
            }
            json link_items = json::array();
            for (const control::DriveShareLink& link : *links) {
                json value = control::to_json(link);
                value["active"] = link_is_active(link);
                value["delivery"] = "link";
                link_items.push_back(std::move(value));
            }
            return ApiResult::success(
                {{"repository", control::to_json(location->repository)},
                 {"object", control::to_json(location->object)},
                 {"path", location->path},
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
        .description = "Create a folder-scoped link with an explicit role and expiry.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"repository_id", {{"type", "string"}}},
                           {"ref", {{"type", "string"}}},
                           {"path", {{"type", "string"}}},
                           {"role", {{"type", "string"}, {"enum", json::array({"read", "write"})}}},
                           {"expires_at", {{"type", "string"}}}}},
                         {"required",
                          json::array({"repository_id", "ref", "path", "role", "expires_at"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "drive folder",
        .action = "share",
        .resolve_object = drive_object,
        .handler = [](const json& input, ApiContext& context) {
            const std::string expires_at = input.at("expires_at");
            if (expires_at <= control::now_iso8601()) {
                return ApiResult::failure(
                    "invalid_input", "expires_at must be in the future", "/expires_at");
            }
            const auto location = ensure_folder_location(input, context);
            if (!location) {
                return ApiResult::failure(
                    location.error().code, location.error().message, location.error().path);
            }
            const std::string token = new_link_token();
            const auto link = context.store->create_drive_share_link(
                control::NewDriveShareLink{location->repository.id,
                                           location->object.id,
                                           location->node_id,
                                           location->path,
                                           input.at("role"),
                                           token_hash(token),
                                           expires_at,
                                           context.acting_identity,
                                           context.request_id});
            if (!link) {
                return store_error(link.error());
            }
            context.audit_customer_id = location->repository.customer_id;
            context.audit_kind = "drive.share_link.created";
            context.audit_object =
                "repository/" + location->repository.id + "/path/" + location->path;
            context.audit_details = {{"link_id", link->id},
                                     {"node_id", link->node_id},
                                     {"path", link->path},
                                     {"role", link->role},
                                     {"expires_at", link->expires_at}};
            json output = control::to_json(*link);
            output["delivery"] = "link";
            output["url"] = "/share/" + token;
            output["token"] = token;
            return ApiResult::success(std::move(output));
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
        .handler = [](const json& input, ApiContext& context) {
            const auto shared = shared_drive_target(input, context);
            if (!shared) {
                return ApiResult::failure(
                    shared.error().code, shared.error().message, shared.error().path);
            }
            if (shared->target.kind != "tree") {
                return ApiResult::failure("invalid_input", "path must name a folder", "/path");
            }
            auto result =
                browse_result(shared->location, shared->entries, context, context.acting_identity);
            if (result.ok) {
                result.value["root_path"] = shared->root_path;
            }
            return result;
        }});

    registry.add(Capability{.name = "preview_drive_share_link",
                            .title = "Preview shared drive file",
                            .description =
                                "Preview a file using only an active link's scoped read authority.",
                            .input_schema = share_read_input_schema(true),
                            .output_schema = shared_file_output_schema(false),
                            .kind = CapabilityKind::Read,
                            .requires_auth = false,
                            .object = "drive share link",
                            .action = "read",
                            .resolve_object = share_token_object,
                            .handler = [](const json& input, ApiContext& context) {
                                return shared_file_result(input, context, false);
                            }});

    registry.add(Capability{
        .name = "download_drive_share_link",
        .title = "Download shared drive file",
        .description =
            "Read exact file bytes as base64 using only an active link's scoped authority.",
        .input_schema = share_read_input_schema(true),
        .output_schema = shared_file_output_schema(true),
        .kind = CapabilityKind::Read,
        .requires_auth = false,
        .object = "drive share link",
        .action = "read",
        .resolve_object = share_token_object,
        .handler = [](const json& input, ApiContext& context) {
            return shared_file_result(input, context, true);
        }});

    registry.add(Capability{
        .name = "revoke_drive_share_link",
        .title = "Revoke drive share link",
        .description = "Revoke a continuing share capability immediately.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"link_id", {{"type", "string"}}}}},
                         {"required", json::array({"link_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "drive share link",
        .action = "share",
        .resolve_object = link_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto link = context.store->revoke_drive_share_link(input.at("link_id"));
            if (!link) {
                return store_error(link.error());
            }
            context.audit_kind = "drive.share_link.revoked";
            context.audit_object = "repository/" + link->repository_id + "/path/" + link->path;
            context.audit_details = {{"link_id", link->id}, {"path", link->path}};
            return ApiResult::success(control::to_json(*link));
        }});
}

} // namespace lake::api
