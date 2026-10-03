#pragma once

#include <string>
#include <vector>

#include "lake/api/sync_store.hpp"
#include "lake/control/policy.hpp"

namespace lake::api {

struct BranchProtectionEvaluation {
    std::vector<std::string> matched_rules;
};

// Takes the repository rather than its id so the evaluation knows the workspace that holds the
// repository's rules, reviews and releases (lake#315; they move there in lake#341).
SyncStoreResult<BranchProtectionEvaluation>
evaluate_branch_protection(control::Store& store,
                           const std::string& acting_identity,
                           control::AuthorizationCache& authorization_cache,
                           const control::Repository& repository,
                           const BranchRefUpdate& update);

} // namespace lake::api
