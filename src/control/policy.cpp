#include "lake/control/policy.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <utility>

namespace lake::control {
namespace {

constexpr std::array<std::string_view, 6> actions = {
    "read",
    "write",
    "admin",
    "deploy",
    "review",
    "share",
};

Result<void>
load_paths(Store& store, const std::vector<std::string>& object_ids, AuthorizationCache& cache) {
    std::vector<std::string> missing;
    for (const auto& object_id : object_ids) {
        if (!cache.paths.contains(object_id)) {
            missing.push_back(object_id);
        }
    }
    if (missing.empty()) {
        return {};
    }

    auto loaded = store.policy_paths(missing);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    for (const auto& object_id : missing) {
        cache.paths.emplace(object_id, std::vector<ObjectPathEntry>{});
    }
    for (auto& entry : *loaded) {
        cache.paths[entry.requested_object_id].push_back(std::move(entry));
    }
    for (const auto& object_id : missing) {
        if (cache.paths[object_id].empty()) {
            return std::unexpected(StoreError{"not_found", "no policy object " + object_id});
        }
    }
    return {};
}

Result<void> load_grants(Store& store,
                         const std::string& principal_taap_id,
                         const std::vector<std::string>& object_ids,
                         AuthorizationCache& cache) {
    std::vector<std::string> missing;
    auto& loaded = cache.loaded_grant_objects[principal_taap_id];
    for (const auto& object_id : object_ids) {
        if (!loaded.contains(object_id)) {
            missing.push_back(object_id);
        }
    }
    if (missing.empty()) {
        return {};
    }

    auto grants = store.list_grants(missing, principal_taap_id, 10000);
    if (!grants) {
        return std::unexpected(grants.error());
    }
    for (const auto& object_id : missing) {
        loaded.insert(object_id);
        cache.grants[principal_taap_id].try_emplace(object_id);
    }
    for (auto& grant : *grants) {
        cache.grants[principal_taap_id][grant.object_id].push_back(std::move(grant));
    }
    return {};
}

AuthorizationDecision decide(const std::string& object_id,
                             const std::string& action,
                             const std::vector<ObjectPathEntry>& path,
                             const std::map<std::string, std::vector<Grant>>& grants) {
    for (const auto& entry : path) {
        const auto object_grants = grants.find(entry.object.id);
        if (object_grants == grants.end()) {
            continue;
        }
        for (const auto& grant : object_grants->second) {
            if (role_allows(grant.role, action)) {
                return AuthorizationDecision{
                    true,
                    object_id,
                    entry.object.id,
                    grant.role,
                    entry.depth != 0,
                };
            }
        }
    }
    return AuthorizationDecision{false, object_id, {}, {}, false};
}

} // namespace

bool role_allows(std::string_view role, std::string_view action) {
    if (role == "admin") {
        return std::ranges::find(actions, action) != actions.end();
    }
    if (role == "read") {
        return action == "read";
    }
    if (role == "write") {
        return action == "read" || action == "write";
    }
    if (role == "deploy") {
        return action == "read" || action == "deploy";
    }
    if (role == "review") {
        return action == "read" || action == "review";
    }
    if (role == "share") {
        return action == "read" || action == "share";
    }
    return false;
}

bool role_valid_for_object_type(std::string_view object_type, std::string_view role) {
    if (role == "admin" || role == "read") {
        return true;
    }
    if (role == "deploy") {
        return object_type == "environment";
    }
    if (role == "review") {
        return object_type == "repository" || object_type == "path";
    }
    if (role == "share") {
        return object_type != "member";
    }
    if (role == "write") {
        return object_type != "customer" && object_type != "member";
    }
    return false;
}

bool object_parent_allowed(std::string_view object_type, std::string_view parent_type) {
    if (object_type == "application") {
        return parent_type == "customer";
    }
    if (object_type == "organization") {
        return parent_type == "customer";
    }
    if (object_type == "team") {
        return parent_type == "organization";
    }
    if (object_type == "member") {
        return parent_type == "organization" || parent_type == "team";
    }
    if (object_type == "repository") {
        return parent_type == "customer" || parent_type == "organization" || parent_type == "team";
    }
    if (object_type == "path") {
        return parent_type == "repository" || parent_type == "path";
    }
    if (object_type == "environment") {
        return parent_type == "customer" || parent_type == "organization" ||
               parent_type == "team" || parent_type == "repository";
    }
    if (object_type == "secret_mount") {
        return parent_type == "environment";
    }
    if (object_type == "secret") {
        return parent_type == "secret_mount";
    }
    return false;
}

Result<std::map<std::string, AuthorizationDecision>>
authorize_batch(Store& store,
                const std::string& principal_taap_id,
                const std::vector<std::string>& object_ids,
                const std::string& action,
                AuthorizationCache& cache) {
    std::map<std::string, AuthorizationDecision> decisions;
    if (principal_taap_id == "admin") {
        for (const auto& object_id : object_ids) {
            decisions.emplace(object_id,
                              AuthorizationDecision{true, object_id, object_id, "admin", false});
        }
        return decisions;
    }
    if (principal_taap_id.empty()) {
        for (const auto& object_id : object_ids) {
            decisions.emplace(object_id, AuthorizationDecision{false, object_id, {}, {}, false});
        }
        return decisions;
    }

    const std::string self_object = "principal/" + principal_taap_id;
    std::vector<std::string> policy_objects;
    policy_objects.reserve(object_ids.size());
    for (const auto& object_id : object_ids) {
        if (object_id == self_object && role_allows("admin", action)) {
            decisions.emplace(object_id,
                              AuthorizationDecision{true, object_id, object_id, "admin", false});
            continue;
        }
        policy_objects.push_back(object_id);
    }
    if (policy_objects.empty()) {
        return decisions;
    }

    if (auto loaded = load_paths(store, policy_objects, cache); !loaded) {
        return std::unexpected(loaded.error());
    }
    std::set<std::string> path_object_ids;
    for (const auto& object_id : policy_objects) {
        for (const auto& entry : cache.paths[object_id]) {
            path_object_ids.insert(entry.object.id);
        }
    }
    const std::vector<std::string> grant_object_ids(path_object_ids.begin(), path_object_ids.end());
    if (auto loaded = load_grants(store, principal_taap_id, grant_object_ids, cache); !loaded) {
        return std::unexpected(loaded.error());
    }
    const auto& grants = cache.grants[principal_taap_id];
    for (const auto& object_id : policy_objects) {
        decisions.emplace(object_id, decide(object_id, action, cache.paths[object_id], grants));
    }
    return decisions;
}

Result<AuthorizationDecision> authorize(Store& store,
                                        const std::string& principal_taap_id,
                                        const std::string& object_id,
                                        const std::string& action,
                                        AuthorizationCache& cache) {
    if (principal_taap_id == "admin") {
        return AuthorizationDecision{true, object_id, object_id, "admin", false};
    }
    const std::string self_object = "principal/" + principal_taap_id;
    if (!principal_taap_id.empty() && object_id == self_object && role_allows("admin", action)) {
        return AuthorizationDecision{true, object_id, object_id, "admin", false};
    }
    if (object_id.empty()) {
        return AuthorizationDecision{false, {}, {}, {}, false};
    }
    auto decisions = authorize_batch(store, principal_taap_id, {object_id}, action, cache);
    if (!decisions) {
        return std::unexpected(decisions.error());
    }
    return decisions->at(object_id);
}

Result<std::vector<EffectiveAccessEntry>>
effective_access(Store& store, const std::string& object_id, AuthorizationCache& cache) {
    if (auto loaded = load_paths(store, {object_id}, cache); !loaded) {
        return std::unexpected(loaded.error());
    }
    std::vector<std::string> path_object_ids;
    for (const auto& entry : cache.paths[object_id]) {
        path_object_ids.push_back(entry.object.id);
    }
    auto grants = store.list_grants(path_object_ids, std::nullopt, 10000);
    if (!grants) {
        return std::unexpected(grants.error());
    }

    std::map<std::string, int> depth_by_object;
    for (const auto& entry : cache.paths[object_id]) {
        depth_by_object.emplace(entry.object.id, entry.depth);
    }
    std::vector<EffectiveAccessEntry> entries;
    for (const auto& grant : *grants) {
        std::vector<std::string> allowed_actions;
        for (const auto& action : actions) {
            if (role_allows(grant.role, action)) {
                allowed_actions.emplace_back(action);
            }
        }
        entries.push_back(EffectiveAccessEntry{
            grant.principal_taap_id,
            grant.object_id,
            grant.role,
            std::move(allowed_actions),
            depth_by_object.at(grant.object_id) != 0,
        });
    }
    return entries;
}

nlohmann::json to_json(const AuthorizationDecision& decision) {
    return {
        {"allowed", decision.allowed},
        {"object_id", decision.object_id},
        {"source_object_id",
         decision.source_object_id.empty() ? nlohmann::json(nullptr)
                                           : nlohmann::json(decision.source_object_id)},
        {"role", decision.role.empty() ? nlohmann::json(nullptr) : nlohmann::json(decision.role)},
        {"inherited", decision.inherited},
    };
}

nlohmann::json to_json(const EffectiveAccessEntry& entry) {
    return {
        {"principal_taap_id", entry.principal_taap_id},
        {"source_object_id", entry.source_object_id},
        {"role", entry.role},
        {"actions", entry.actions},
        {"inherited", entry.inherited},
    };
}

} // namespace lake::control
