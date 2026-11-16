#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/api/registry.hpp"
#include "lake/audit/audit.hpp"

// Reading lake's audit across its two logs (docs/api/audit.md#which-log): the
// central chain in PostgreSQL and each workspace's chain in its own Zetta. The audit capabilities
// use these; nothing here writes.
namespace lake::api {

// Which logs one read covers: the central log, and these workspaces (customer, workspace).
struct AuditScope {
    bool central = true;
    std::vector<std::pair<std::string, std::string>> workspaces;
};

// One event as read, and where it was found.
struct LoggedEvent {
    audit::Event event;
    std::string log = "central"; // or "workspace"
    std::string workspace_id;
    std::int64_t sequence = 0; // its place in the workspace's chain; 0 central or copied
    bool pending = false;      // queued in this process, not yet signed or written
};

struct AuditRead {
    std::vector<LoggedEvent> events;
    // The workspaces whose audit could not be read, with why: [{workspace_id, code, message}].
    nlohmann::json unread = nlohmann::json::array();
};

// A co_await'ed read's own failure (the coroutine machinery's) folded into its result.
template <class T>
std::expected<T, ApiError>
settled_read(std::expected<std::expected<T, ApiError>, liquid::error> completed) {
    if (!completed) {
        return std::unexpected(ApiError{"store_unavailable", completed.error().message, {}});
    }
    return std::move(*completed);
}

// Whether an event passes `filter`'s fields (all but limit, order and after_id).
bool audit_filter_matches(const audit::Filter& filter, const audit::Event& event);

// The live workspaces of these spaces, as a scope's workspaces.
std::vector<std::pair<std::string, std::string>>
live_workspaces_of(const ApiContext& context, const std::vector<std::string>& customer_ids);

// The events matching `filter` in `scope`: the central log's, every workspace's (read
// concurrently, one list_records walk each, in one round), and this process's queued events of
// those workspaces; merged by instant (newest first unless `filter.newest_first` is false),
// without duplicates (an event the copy put in a workspace before the central row is dropped
// counts once), at most `filter.limit`. `after_id` pages the central log only.
liquid::runtime::task<std::expected<AuditRead, ApiError>>
read_audit_logs(ApiContext& context, const audit::Filter& filter, const AuditScope& scope);

// One workspace's audit for export: every event, the copied ones first by instant, then the chain
// by place.
liquid::runtime::task<std::expected<std::vector<LoggedEvent>, ApiError>>
export_workspace_audit(ApiContext& context, const std::string& customer_id,
                       const std::string& workspace_id);

// Verifies one workspace's audit (control::verify_workspace_audit), as the verify_audit answer.
liquid::runtime::task<ApiResult> verify_workspace_audit_log(ApiContext& context,
                                                            const std::string& customer_id,
                                                            const std::string& workspace_id);

} // namespace lake::api
