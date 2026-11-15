// SPDX-License-Identifier: MIT

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

// The account and workspace an agent's task, launch or schedule credential was made for (LSR-118,
// lake#985). While `active`, a decision allows only an object under the customer object whose
// external id is `customer_id`, and, when `workspace_id` is set, every repository on its path must
// belong to that workspace. An empty `customer_id` while active matches no object: a credential
// whose row could not be read is confined to nothing.
struct AuthorizationConfinement {
    bool active = false;
    std::string customer_id;
    std::string workspace_id;
};

struct AuthorizationCache {
    std::map<std::string, std::vector<ObjectPathEntry>> paths;
    std::map<std::string, std::map<std::string, std::vector<Grant>>> grants;
    std::map<std::string, std::set<std::string>> loaded_grant_objects;
    AuthorizationConfinement confinement;
    // Whether each object a confined decision checked lies inside the confinement.
    std::map<std::string, bool> confined_objects;
};

// A new, empty cache that keeps `from`'s confinement. A check that must not reuse loaded paths
// (because policy objects may have been made since) still decides inside the same confinement.
inline AuthorizationCache fresh_cache_like(const AuthorizationCache& from) {
    AuthorizationCache fresh;
    fresh.confinement = from.confinement;
    return fresh;
}

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
// Whether the policy object `object_id` is `ancestor_id` or lies below it: its parent chain
// reaches that object. An object's external id names an entity (a repository's folder, an
// environment), and the object counts for that entity only when it sits under the entity's own
// object, never under another account's (lake#913).
Result<bool> object_within(Store &store, const std::string &object_id,
                           const std::string &ancestor_id);
// The nearest object of `object_type` on `object_id`'s parent chain, the object itself included;
// `not_found` when the chain has none.
Result<PolicyObject> nearest_object_of_type(Store &store, const std::string &object_id,
                                            std::string_view object_type);
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

// Principal ids (lake#870, lake#872). Policy decisions are keyed by a principal string, and
// `admin` is the operator's: it bypasses every check, and only the admin token may act as it.
// Every identity a person, organization or grant carries is a TAAP id, `taap:` followed by
// printable ASCII and at most 512 characters in all, so no stored identity can be `admin` or
// another identity lake sets in code (`agent-service:`, `share-link:`, `sso-provider/`).
inline constexpr std::string_view operator_principal = "admin";
bool valid_principal_id(std::string_view value);
// invalid_input naming `field` unless `value` is empty (no identity) or a valid principal id.
Result<void> check_principal_id(std::string_view value, std::string_view field);
// The principal a credential resolved to, or not_found when it may not be used: a credential
// whose identity is the operator's, or a person's credential whose identity is not a TAAP id.
// Workload credentials (app_, build_, methodology_) name their workload, never a person.
Result<Principal> accepted_principal(Result<Principal> principal);

} // namespace lake::control
