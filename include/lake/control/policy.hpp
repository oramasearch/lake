#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/control/store.hpp"

namespace lake::control {

struct AuthorizationDecision {
    bool allowed = false;
    std::string object_id;
    std::string source_object_id;
    std::string role;
    bool inherited = false;
};

struct AuthorizationCache {
    std::map<std::string, std::vector<ObjectPathEntry>> paths;
    std::map<std::string, std::map<std::string, std::vector<Grant>>> grants;
    std::map<std::string, std::set<std::string>> loaded_grant_objects;
};

struct EffectiveAccessEntry {
    std::string principal_taap_id;
    std::string source_object_id;
    std::string role;
    std::vector<std::string> actions;
    bool inherited = false;
};

bool role_allows(std::string_view role, std::string_view action);
bool role_valid_for_object_type(std::string_view object_type, std::string_view role);
bool object_parent_allowed(std::string_view object_type, std::string_view parent_type);
Result<AuthorizationDecision> authorize(Store &store, const std::string &principal_taap_id,
                                        const std::string &object_id, const std::string &action,
                                        AuthorizationCache &cache);
Result<std::map<std::string, AuthorizationDecision>>
authorize_batch(Store &store, const std::string &principal_taap_id,
                const std::vector<std::string> &object_ids, const std::string &action,
                AuthorizationCache &cache);
Result<std::vector<EffectiveAccessEntry>>
effective_access(Store &store, const std::string &object_id, AuthorizationCache &cache);
nlohmann::json to_json(const AuthorizationDecision &decision);
nlohmann::json to_json(const EffectiveAccessEntry &entry);

} // namespace lake::control
