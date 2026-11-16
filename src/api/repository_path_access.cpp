#include "repository_path_access.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <utility>

#include "lake/control/policy.hpp"

namespace lake::api {
namespace {

// A level of path objects is read a page at a time; a repository with more than this many path
// objects in all is refused rather than read in part (fail closed).
constexpr int page_limit = 1000;
constexpr std::size_t total_limit = 10000;

std::unexpected<ApiError> failure(const control::StoreError& error) {
    return std::unexpected(ApiError{error.code, error.message, {}});
}

std::string parent_path(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? std::string{} : std::string(path.substr(0, slash));
}

} // namespace

bool repository_path_access_exempt(const ApiContext& context,
                                   const control::Repository& repository,
                                   std::string_view ref) {
    if (context.operator_credential || context.acting_identity == control::operator_principal) {
        return true;
    }
    // The build or app it was minted for reads the whole revision, as readable_repository lets it.
    return context.authorized &&
           (context.credential_kind == "app_source" || context.credential_kind == "build_source") &&
           repository.id == context.credential_repository_id &&
           repository.customer_id == context.credential_customer_id && !ref.empty() &&
           ref == context.credential_revision;
}

std::expected<RepositoryPathAccess, ApiError> RepositoryPathAccess::load(
    ApiContext& context, const control::Repository& repository, std::string_view ref) {
    RepositoryPathAccess access;
    if (repository_path_access_exempt(context, repository, ref)) {
        access.exempt_ = true;
        return access;
    }
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const auto object = context.store->find_policy_object("repository", repository.id);
    if (!object) {
        return failure(object.error());
    }
    access.repository_object_ = object->id;
    const std::string prefix = repository.id + ":";
    // Every object below the repository's own, level by level. Only objects there count for its
    // nodes (lake#913); one holding a node's id elsewhere is never the node's.
    std::deque<std::string> parents{object->id};
    std::size_t seen = 0;
    while (!parents.empty()) {
        const std::string parent = std::move(parents.front());
        parents.pop_front();
        const auto children = context.store->list_policy_objects(parent, "path", page_limit + 1);
        if (!children) {
            return failure(children.error());
        }
        seen += children->size();
        if (children->size() > static_cast<std::size_t>(page_limit) || seen > total_limit) {
            return std::unexpected(ApiError{
                "store_error",
                "this repository has more path objects than Lake reads to decide its paths",
                {}});
        }
        for (const control::PolicyObject& child : *children) {
            parents.push_back(child.id);
            if (!child.external_id.starts_with(prefix) ||
                child.external_id.size() == prefix.size()) {
                continue;
            }
            if (!child.inherit) {
                access.cuts_.push_back(child.id);
            }
            access.by_node_.emplace(child.external_id.substr(prefix.size()), child);
        }
    }
    return access;
}

std::expected<std::set<std::string>, ApiError>
RepositoryPathAccess::unreadable(ApiContext& context,
                                 const std::vector<RepositoryTreeEntry>& tree,
                                 const std::vector<std::string>& paths) const {
    std::set<std::string> refused;
    if (unrestricted()) {
        return refused;
    }
    std::unordered_map<std::string_view, const RepositoryTreeEntry*> entries;
    entries.reserve(tree.size());
    for (const RepositoryTreeEntry& entry : tree) {
        entries.emplace(entry.path, &entry);
    }
    // Each path's deciding object, as drive_location_at finds it: the objects on the path, top
    // down; one that inherits counts only when its parent is the object above it that counted
    // (the repository's for the first) and that one held; one that does not inherit stands alone.
    // The path needs its own check only when a cut lies on the chain that decides it.
    std::map<std::string, std::string> checked; // path -> object to authorize
    for (const std::string& path : paths) {
        if (path.empty()) {
            continue; // the repository itself, which the caller reads
        }
        if (!entries.contains(path)) {
            refused.insert(path);
            continue;
        }
        std::vector<const control::PolicyObject*> on_path; // nearest first
        for (std::string candidate = path; !candidate.empty(); candidate = parent_path(candidate)) {
            const auto entry = entries.find(candidate);
            if (entry == entries.end() || entry->second->node_id.empty()) {
                continue;
            }
            if (const auto object = by_node_.find(entry->second->node_id);
                object != by_node_.end()) {
                on_path.push_back(&object->second);
            }
        }
        std::string deciding = repository_object_;
        std::string child_parent = repository_object_;
        bool holds = true;
        bool cut = false;
        for (const control::PolicyObject* object : std::views::reverse(on_path)) {
            if (!object->inherit || (holds && object->parent_id == child_parent)) {
                deciding = object->id;
                cut = cut || !object->inherit;
                holds = true;
            } else {
                holds = false;
            }
            child_parent = object->id;
        }
        if (cut) {
            checked.emplace(path, deciding);
        }
    }
    if (checked.empty()) {
        return refused;
    }
    if (!context.authorized || context.acting_identity.empty()) {
        for (const auto& path : checked | std::views::keys) {
            refused.insert(path);
        }
        return refused;
    }
    std::vector<std::string> objects;
    for (const auto& object : checked | std::views::values) {
        objects.push_back(object);
    }
    std::ranges::sort(objects);
    objects.erase(std::ranges::unique(objects).begin(), objects.end());
    const auto decisions = control::authorize_batch(
        *context.store, context.acting_identity, objects, "read", context.authorization_cache);
    if (!decisions) {
        return failure(decisions.error());
    }
    for (const auto& [path, object] : checked) {
        const auto decision = decisions->find(object);
        if (decision == decisions->end() || !decision->second.allowed) {
            refused.insert(path);
        }
    }
    return refused;
}

std::expected<std::vector<RepositoryTreeEntry>, ApiError>
RepositoryPathAccess::readable_entries(ApiContext& context,
                                       std::vector<RepositoryTreeEntry> tree) const {
    if (unrestricted()) {
        return tree;
    }
    std::vector<std::string> paths;
    paths.reserve(tree.size());
    for (const RepositoryTreeEntry& entry : tree) {
        paths.push_back(entry.path);
    }
    const auto refused = unreadable(context, tree, paths);
    if (!refused) {
        return std::unexpected(refused.error());
    }
    std::erase_if(tree,
                  [&](const RepositoryTreeEntry& entry) { return refused->contains(entry.path); });
    return tree;
}

std::expected<bool, ApiError> RepositoryPathAccess::reads_every_cut(ApiContext& context) const {
    if (unrestricted()) {
        return true;
    }
    if (!context.authorized || context.acting_identity.empty()) {
        return false;
    }
    const auto decisions = control::authorize_batch(
        *context.store, context.acting_identity, cuts_, "read", context.authorization_cache);
    if (!decisions) {
        return failure(decisions.error());
    }
    for (const std::string& cut : cuts_) {
        const auto decision = decisions->find(cut);
        if (decision == decisions->end() || !decision->second.allowed) {
            return false;
        }
    }
    return true;
}

} // namespace lake::api
