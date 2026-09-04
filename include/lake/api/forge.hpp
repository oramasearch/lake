#pragma once

#include <optional>
#include <string>

#include "lake/api/sync_store.hpp"
#include "lake/control/policy.hpp"

namespace lake::api {

struct BranchRefUpdate {
    std::string name;
    std::optional<std::string> old_head;
    std::string new_head;
    bool fast_forward = false;
};

// Evaluates one proposed reference update. liquid #177 will supply this shape
// to the sync authorizer before it applies a batch.
SyncStoreResult<void> evaluate_branch_protection(control::Store& store,
                                                 const std::string& acting_identity,
                                                 control::AuthorizationCache& authorization_cache,
                                                 const std::string& repository_id,
                                                 const BranchRefUpdate& update);

} // namespace lake::api
