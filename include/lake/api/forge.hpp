#pragma once

#include <string>
#include <vector>

#include "lake/api/sync_store.hpp"
#include "lake/control/policy.hpp"

namespace lake::api {

struct BranchProtectionEvaluation {
    std::vector<std::string> matched_rules;
};

SyncStoreResult<BranchProtectionEvaluation>
evaluate_branch_protection(control::Store& store,
                           const std::string& acting_identity,
                           control::AuthorizationCache& authorization_cache,
                           const std::string& repository_id,
                           const BranchRefUpdate& update);

} // namespace lake::api
