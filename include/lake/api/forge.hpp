#pragma once

#include <expected>
#include <string>
#include <vector>

#include <liquid/runtime.hpp>

#include "lake/api/sync_store.hpp"
#include "lake/control/policy.hpp"

namespace lake::api {

struct BranchProtectionEvaluation {
    std::vector<std::string> matched_rules;
};

struct ApiContext;

// What branch protection reads at a reference update: the repository's rules and, only when a
// rule asks for them, its reviews and releases. They are forge records in the workspace Zetta of
// the repository's workspace (lake#341), read once before a push starts (load_branch_protection),
// because the push itself runs synchronously inside the repository store.
struct BranchProtectionRecords {
    std::vector<control::ForgeRecord> rules, reviews, releases;
};

// Reads the records branch protection needs from the repository's workspace. A repository without
// a workspace, or a workspace that cannot be read, is an error: the push then refuses every
// reference update rather than skip rules it could not read. A lake with no workspace service
// configured holds no forge records at all, so it answers no rules.
liquid::runtime::task<std::expected<BranchProtectionRecords, SyncStoreError>>
load_branch_protection(ApiContext& context, control::Repository repository);

SyncStoreResult<BranchProtectionEvaluation>
evaluate_branch_protection(control::Store& store,
                           const std::string& acting_identity,
                           control::AuthorizationCache& authorization_cache,
                           const BranchProtectionRecords& records,
                           const BranchRefUpdate& update);

} // namespace lake::api
