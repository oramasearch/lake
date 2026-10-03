#pragma once

#include <memory>
#include <string>
#include <utility>

#include "lake/api/capabilities.hpp"
#include "lake/control/workspace_records.hpp"

namespace lake::api {

// The records of one workspace (docs/architecture/workspace-records.md), reached through
// call_workspace so that each Zetta call is checked against the caller's workspace, counted in
// Server-Timing and refused while the workspace is not ready. The caller has already validated
// that `workspace_id` belongs to `customer_id` (Registry::call for a capability's input, the
// workspace route guard for a page); WorkspaceService::call checks it again on every call.
inline control::ZettaControlResult<control::WorkspaceRecords>
workspace_records(const ApiContext& context, std::string customer_id, std::string workspace_id) {
    if (!context.workspaces) {
        return std::unexpected(
            control::ZettaControlError{"workspace_not_ready", "a workspace Zetta is required"});
    }
    auto bound = std::make_shared<ApiContext>(context);
    bound->workspace_id = workspace_id;
    return control::WorkspaceRecords::open(
        [bound, customer_id = std::move(customer_id)](std::string capability, bool read,
                                                      nlohmann::json input) {
            return call_workspace(*bound, customer_id, std::move(capability), read,
                                  std::move(input));
        },
        std::move(workspace_id), context.instance_kek);
}

} // namespace lake::api
