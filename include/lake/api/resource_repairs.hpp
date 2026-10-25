#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/api/registry.hpp"

namespace lake::api {

// When a resource fails (lake#1207, docs/api/notifications.md): notifications, the agent's repair
// and change turns, and opt-in auto-fix, general over the resource kind. Apps are the first kind;
// custom agents (lake#1209) plug in a detector, a repairing agent and its turn kinds.

// What one pass did.
struct ResourceRepairPass {
    std::size_t workspaces = 0;
    std::size_t notified = 0;
    std::size_t resolved = 0;
    std::size_t auto_fix_started = 0;
    std::size_t auto_fix_stopped = 0;
    std::vector<std::string> problems;
};

// One pass over every workspace's resources (the store root's owner runs it with the MCP stream
// pass). For apps: a rollout that ended Degraded, failed or crash-looping, and an app's newest image
// build that failed, each record one notification for the person who deployed it, and an app Ready
// again resolves them. A resource with auto-fix on gets one repair turn per failed generation, as
// the person who turned auto-fix on, until its attempts are spent; then auto-fix stops and says so
// in a notification.
ResourceRepairPass run_resource_repair_pass(ApiContext& context);

// Records who created or deployed a resource and the source it was deployed from, in its repair
// record. Best effort: a workspace that cannot answer leaves the deployment as it is.
liquid::runtime::task<bool> note_deployment(ApiContext& context,
                                            std::string kind,
                                            std::string resource_id,
                                            std::string customer_id,
                                            std::string workspace_id,
                                            nlohmann::json source);

} // namespace lake::api
