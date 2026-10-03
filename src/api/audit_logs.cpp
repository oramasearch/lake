#include "audit_logs.hpp"

#include <algorithm>
#include <memory>
#include <set>
#include <tuple>
#include <utility>

#include "lake/api/audit_recorder.hpp"
#include "lake/control/store.hpp"
#include "lake/control/workspace_audit.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

using json = nlohmann::json;
using WorkspaceEvents = std::vector<control::WorkspaceAuditEvent>;

struct WorkspaceRead {
    liquid::result<control::ZettaControlResult<WorkspaceEvents>> value;
};

boost::capy::io_task<WorkspaceRead>
complete_read(liquid::runtime::task<control::ZettaControlResult<WorkspaceEvents>> task) {
    auto result = co_await std::move(task);
    co_return boost::capy::io_result<WorkspaceRead>{{}, {std::move(result)}};
}

liquid::runtime::task<control::ZettaControlResult<WorkspaceEvents>>
read_workspace(std::shared_ptr<control::WorkspaceRecords> records, audit::Filter filter) {
    const std::size_t newest =
        filter.newest_first && filter.limit > 0 ? static_cast<std::size_t>(filter.limit) : 0;
    const std::int64_t since_us = filter.from.empty() ? 0 : control::audit_instant_us(filter.from);
    co_return control::settled(co_await control::list_workspace_audit(
        *records,
        [filter](const audit::Event& event) { return audit_filter_matches(filter, event); },
        newest, since_us));
}

liquid::runtime::task<control::ZettaControlResult<WorkspaceEvents>>
refused(control::ZettaControlError error) {
    co_return control::ZettaControlResult<WorkspaceEvents>(std::unexpected(std::move(error)));
}

ApiError api_error(const control::ZettaControlError& error) {
    return ApiError{error.code, error.message, error.path};
}

} // namespace

bool audit_filter_matches(const audit::Filter& filter, const audit::Event& event) {
    if (!filter.request_id.empty() && event.draft.request_id != filter.request_id) {
        return false;
    }
    if (!filter.principal.empty() && event.draft.principal != filter.principal) {
        return false;
    }
    if (!filter.object.empty() && event.draft.object != filter.object) {
        return false;
    }
    if (!filter.outcome.empty() && event.draft.outcome != filter.outcome) {
        return false;
    }
    if (!filter.from.empty() &&
        control::audit_instant_us(event.occurred_at) < control::audit_instant_us(filter.from)) {
        return false;
    }
    if (!filter.to.empty() &&
        control::audit_instant_us(event.occurred_at) > control::audit_instant_us(filter.to)) {
        return false;
    }
    return !(event.draft.outcome == "success" &&
             std::ranges::find(filter.omit_successful_actions, event.draft.action) !=
                 filter.omit_successful_actions.end());
}

std::vector<std::pair<std::string, std::string>>
live_workspaces_of(const ApiContext& context, const std::vector<std::string>& customer_ids) {
    std::vector<std::pair<std::string, std::string>> workspaces;
    if (context.store == nullptr) {
        return workspaces;
    }
    for (const std::string& customer_id : customer_ids) {
        const auto rows = context.store->list_workspaces(customer_id);
        if (!rows) {
            continue;
        }
        for (const control::Workspace& row : *rows) {
            if (row.desired != "deleted") {
                workspaces.emplace_back(row.customer_id, row.id);
            }
        }
    }
    return workspaces;
}

liquid::runtime::task<std::expected<AuditRead, ApiError>>
read_audit_logs(ApiContext& context, const audit::Filter& filter, const AuditScope& scope) {
    using Read = std::expected<AuditRead, ApiError>;
    AuditRead read;
    std::set<std::string> seen;
    const auto add = [&read, &seen](LoggedEvent event) {
        if (seen.insert(event.event.id).second) {
            read.events.push_back(std::move(event));
        }
    };
    if (scope.central) {
        auto central = context.store->list_audit_events(filter);
        if (!central) {
            co_return Read(std::unexpect, ApiError{central.error().code, central.error().message});
        }
        for (audit::Event& event : *central) {
            add(LoggedEvent{.event = std::move(event)});
        }
    }
    // after_id continues the central chain in append order; workspaces have their own chains.
    const bool workspaces = filter.after_id.empty() && !scope.workspaces.empty() &&
                            context.workspaces != nullptr;
    if (workspaces) {
        std::vector<std::shared_ptr<control::WorkspaceRecords>> opened;
        std::vector<boost::capy::io_task<WorkspaceRead>> pending;
        for (const auto& [customer_id, workspace_id] : scope.workspaces) {
            auto records = workspace_records(context, customer_id, workspace_id);
            if (!records) {
                opened.push_back(nullptr);
                pending.push_back(complete_read(refused(records.error())));
                continue;
            }
            opened.push_back(std::make_shared<control::WorkspaceRecords>(std::move(*records)));
            pending.push_back(complete_read(read_workspace(opened.back(), filter)));
        }
        auto [wait_error, reads] = co_await liquid::runtime::when_all(std::move(pending));
        for (std::size_t index = 0; index < scope.workspaces.size(); ++index) {
            const std::string& workspace_id = scope.workspaces[index].second;
            const auto unread = [&](std::string code, std::string message) {
                read.unread.push_back(
                    {{"workspace_id", workspace_id}, {"code", code}, {"message", message}});
            };
            if (wait_error) {
                unread("workspace_not_ready", wait_error.message());
                continue;
            }
            auto& value = reads[index].value;
            if (!value) {
                unread("workspace_not_ready", value.error().message);
                continue;
            }
            if (!*value) {
                unread(value->error().code, value->error().message);
                continue;
            }
            for (control::WorkspaceAuditEvent& found : **value) {
                add(LoggedEvent{.event = std::move(found.event),
                                .log = "workspace",
                                .workspace_id = workspace_id,
                                .sequence = found.sequence});
            }
        }
    }
    // This process's events still on their way to their workspace, so a read right after a call
    // shows it (another replica's appear once it has flushed them, within a second).
    if (filter.after_id.empty() && context.audit_recorder != nullptr && !scope.workspaces.empty()) {
        std::vector<std::string> ids;
        for (const auto& [customer_id, workspace_id] : scope.workspaces) {
            ids.push_back(workspace_id);
        }
        for (const PendingWorkspaceEvent& queued : context.audit_recorder->pending_events(ids)) {
            audit::Event event{.id = queued.event.id,
                               .occurred_at = queued.event.occurred_at,
                               .draft = queued.event.draft};
            if (audit_filter_matches(filter, event)) {
                add(LoggedEvent{.event = std::move(event),
                                .log = "workspace",
                                .workspace_id = queued.workspace_id,
                                .pending = true});
            }
        }
    }
    // One log alone keeps its own order (the central log's is append order); several are merged
    // by instant.
    const bool merged = std::ranges::any_of(
        read.events, [](const LoggedEvent& event) { return event.log == "workspace"; });
    if (merged) {
        std::ranges::stable_sort(read.events, [&filter](const LoggedEvent& a,
                                                        const LoggedEvent& b) {
            const auto key_a = std::tie(a.event.occurred_at, a.event.id);
            const auto key_b = std::tie(b.event.occurred_at, b.event.id);
            return filter.newest_first ? key_b < key_a : key_a < key_b;
        });
    }
    if (filter.limit > 0 && read.events.size() > static_cast<std::size_t>(filter.limit)) {
        read.events.resize(static_cast<std::size_t>(filter.limit));
    }
    co_return Read(std::move(read));
}

liquid::runtime::task<std::expected<std::vector<LoggedEvent>, ApiError>>
export_workspace_audit(ApiContext& context, const std::string& customer_id,
                       const std::string& workspace_id) {
    using Events = std::expected<std::vector<LoggedEvent>, ApiError>;
    auto records = workspace_records(context, customer_id, workspace_id);
    if (!records) {
        co_return Events(std::unexpect, api_error(records.error()));
    }
    auto all = control::settled(co_await control::list_workspace_audit(*records, {}, 0));
    if (!all) {
        co_return Events(std::unexpect, api_error(all.error()));
    }
    std::ranges::stable_sort(*all, [](const auto& a, const auto& b) {
        // Copied central events (place 0) first by instant, then the chain by place.
        return std::tie(a.sequence, a.event.occurred_at, a.event.id) <
               std::tie(b.sequence, b.event.occurred_at, b.event.id);
    });
    std::vector<LoggedEvent> events;
    for (control::WorkspaceAuditEvent& found : *all) {
        events.push_back(LoggedEvent{.event = std::move(found.event),
                                     .log = "workspace",
                                     .workspace_id = workspace_id,
                                     .sequence = found.sequence});
    }
    co_return Events(std::move(events));
}

liquid::runtime::task<ApiResult> verify_workspace_audit_log(ApiContext& context,
                                                            const std::string& customer_id,
                                                            const std::string& workspace_id) {
    auto records = workspace_records(context, customer_id, workspace_id);
    if (!records) {
        co_return ApiResult::failure(records.error().code, records.error().message);
    }
    auto checked =
        control::settled(co_await control::verify_workspace_audit(*records, *context.store));
    if (!checked) {
        co_return ApiResult::failure(checked.error().code, checked.error().message);
    }
    if (!checked->verified) {
        std::string where = " (workspace " + workspace_id;
        if (checked->failed_sequence > 0) {
            where += ", place " + std::to_string(checked->failed_sequence);
        }
        if (!checked->failed_event_id.empty()) {
            where += ", event " + checked->failed_event_id;
        }
        co_return ApiResult::failure(checked->error_code == "missing_event" ? "invalid_chain"
                                                                            : checked->error_code,
                                     checked->error_message + where + ")");
    }
    co_return ApiResult::success({{"verified", true},
                                  {"log", "workspace"},
                                  {"workspace_id", workspace_id},
                                  {"chained", checked->chained},
                                  {"copied", checked->copied},
                                  {"last_sequence", checked->last_sequence}});
}

} // namespace lake::api
