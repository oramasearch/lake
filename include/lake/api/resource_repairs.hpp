#pragma once

#include <cstddef>
#include <optional>
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
    // App runtime logs read for repeated script errors (lake#1228): only healthy running apps with
    // auto-fix on are read.
    std::size_t log_reads = 0;
    std::vector<std::string> problems;
};

// Repeated script errors in an app's runtime log (lake#1228): an app whose rollout is Ready but
// whose code raises on every page. A failure is Streamlit's `Uncaught app execution` or Uvicorn's
// `Exception in ASGI application`, each followed by a Python traceback.
struct AppErrors {
    std::size_t count = 0; // failure markers in the lines read
    std::string last;      // the newest traceback's exception line, `AttributeError: ...`
    std::string traceback; // the newest traceback, at most app_errors_detail_bytes
};
// The markers that must repeat before a healthy app counts as failing: one bad request is not.
inline constexpr std::size_t app_errors_threshold = 2;
// How far back and how many lines a pass reads from an app's log.
inline constexpr int app_errors_window_seconds = 300;
inline constexpr int app_errors_line_limit = 500;
inline constexpr std::size_t app_errors_detail_bytes = 4000;
// The failures in `items` (app_logs' `items`, each `{ts, line}`, in any order). Lines are read in
// time order; a multi-line entry counts as its lines.
AppErrors app_errors_in_logs(const nlohmann::json& items);

// One pass over every workspace's resources (the store root's owner runs it with the MCP stream
// pass). For apps: a rollout that ended Degraded, failed or crash-looping, and an app's newest
// image build that failed, each record one notification for the person who deployed it, and an app
// Ready again resolves them. An app that is Ready and has auto-fix on is also read for repeated
// script errors (app_errors_in_logs): at least app_errors_threshold in the last
// app_errors_window_seconds record an `app_errors` notification, resolved once they stop. A
// resource with auto-fix on gets one repair turn per failed generation, as the person who turned
// auto-fix on, until its attempts are spent; then auto-fix stops and says so in a notification.
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
