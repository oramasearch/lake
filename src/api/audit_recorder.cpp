// SPDX-License-Identifier: MIT

#include "lake/api/audit_recorder.hpp"

#include <algorithm>
#include <utility>

#include "lake/control/zetta_workspace.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {

AuditRecorder::AuditRecorder(control::Store* store,
                             control::WorkspaceService* workspaces,
                             std::string instance_kek,
                             std::string identity_seed,
                             std::size_t max_pending)
    : store_(store), workspaces_(workspaces), instance_kek_(std::move(instance_kek)),
      identity_seed_(std::move(identity_seed)),
      max_pending_(std::max<std::size_t>(max_pending, 2)) {}

bool AuditRecorder::add(std::string customer_id,
                        std::string workspace_id,
                        audit::PendingEvent event) {
    std::lock_guard lock(mutex_);
    const auto count = [](const auto& counts, const std::string& id) {
        const auto found = counts.find(id);
        return found == counts.end() ? 0U : found->second;
    };
    const auto allocation = workspace_held_.find(workspace_id);
    if (held_ >= max_pending_ || count(customer_held_, customer_id) >= max_pending_ / 2 ||
        (allocation != workspace_held_.end() &&
         (allocation->second.customer_id != customer_id ||
          allocation->second.held >= std::max<std::size_t>(max_pending_ / 4, 1)))) {
        ++totals_.dropped;
        return false;
    }
    Queue& queue = queues_[workspace_id];
    if (queue.customer_id.empty()) {
        queue.customer_id = std::move(customer_id);
    }
    queue.events.push_back(std::move(event));
    ++held_;
    ++customer_held_[queue.customer_id];
    auto& allocated = workspace_held_[workspace_id];
    allocated.customer_id = queue.customer_id;
    ++allocated.held;
    return true;
}

std::vector<PendingWorkspaceEvent>
AuditRecorder::pending_events(const std::vector<std::string>& workspace_ids) const {
    std::lock_guard lock(mutex_);
    std::vector<PendingWorkspaceEvent> pending;
    for (const auto& [workspace_id, queue] : queues_) {
        if (!workspace_ids.empty() &&
            std::ranges::find(workspace_ids, workspace_id) == workspace_ids.end()) {
            continue;
        }
        for (const audit::PendingEvent& event : queue.events) {
            pending.push_back({queue.customer_id, workspace_id, event});
        }
    }
    return pending;
}

std::size_t AuditRecorder::pending() const {
    std::lock_guard lock(mutex_);
    return held_;
}

AuditRecorder::Totals AuditRecorder::totals() const {
    std::lock_guard lock(mutex_);
    return totals_;
}

bool AuditRecorder::workspace_gone(const std::string& customer_id,
                                   const std::string& workspace_id) const {
    if (!store_) {
        return false;
    }
    const auto workspace = store_->get_workspace(workspace_id);
    if (!workspace) {
        return workspace.error().code == "not_found";
    }
    return workspace->desired == "deleted" || workspace->customer_id != customer_id;
}

liquid::runtime::task<void> AuditRecorder::flush() {
    if (flushing_.exchange(true)) {
        co_return {};
    }
    std::map<std::string, Queue> queues;
    std::map<std::string, control::WorkspaceAuditTail> tails;
    {
        std::lock_guard lock(mutex_);
        queues.swap(queues_);
        tails = tails_;
    }
    ApiContext context;
    context.store = store_;
    context.workspaces = workspaces_;
    context.instance_kek = instance_kek_;
    const audit::IdentityOptions options{.configured_seed = identity_seed_,
                                         .instance_kek = instance_kek_};
    const auto signer = store_ ? audit::identity(*store_, options)
                               : audit::Result<audit::Identity>(std::unexpected(
                                     audit::Error{"store_unavailable", "no control store"}));

    Totals done;
    std::map<std::string, Queue> retry;
    for (auto& [workspace_id, queue] : queues) {
        // What is not written stays, in order, for the next flush.
        std::size_t written = 0;
        bool failed = !signer.has_value();
        auto records = failed ? control::ZettaControlResult<control::WorkspaceRecords>(
                                    std::unexpected(control::ZettaControlError{
                                        signer.error().code, signer.error().message}))
                              : workspace_records(context, queue.customer_id, workspace_id);
        failed = failed || !records;
        std::optional<control::WorkspaceAuditTail> tail;
        if (!failed) {
            if (const auto known = tails.find(workspace_id); known != tails.end()) {
                tail = known->second;
            } else {
                auto found =
                    control::settled(co_await control::find_workspace_audit_tail(*records));
                if (found) {
                    tail = *found;
                } else {
                    failed = true;
                }
            }
        }
        for (; !failed && written < queue.events.size(); ++written) {
            auto appended = control::settled(co_await control::append_workspace_audit_event(
                *records, *store_, *signer, queue.events[written], options, tail,
                &done.conflicts));
            if (!appended) {
                failed = true;
                break;
            }
            tail = *appended;
            ++done.written;
        }
        if (tail) {
            tails[workspace_id] = *tail;
        }
        if (written == queue.events.size()) {
            continue;
        }
        ++done.flush_errors;
        // The tail this process knew may be wrong now; search again next time.
        tails.erase(workspace_id);
        if (workspace_gone(queue.customer_id, workspace_id)) {
            done.dropped_gone += queue.events.size() - written;
            continue;
        }
        Queue& kept = retry[workspace_id];
        kept.customer_id = queue.customer_id;
        kept.events.assign(queue.events.begin() + static_cast<std::ptrdiff_t>(written),
                           queue.events.end());
    }
    {
        std::lock_guard lock(mutex_);
        totals_.written += done.written;
        totals_.flush_errors += done.flush_errors;
        totals_.conflicts += done.conflicts;
        totals_.dropped_gone += done.dropped_gone;
        tails_ = std::move(tails);
        // In-flight events still own their capacity. Release only written/deleted events;
        // accepted retries cannot be displaced by arrivals during the flush.
        for (const auto& [workspace_id, queue] : queues) {
            const auto kept = retry.find(workspace_id);
            const std::size_t released =
                queue.events.size() - (kept == retry.end() ? 0 : kept->second.events.size());
            held_ -= released;
            auto customer = customer_held_.find(queue.customer_id);
            if ((customer->second -= released) == 0) {
                customer_held_.erase(customer);
            }
            auto workspace = workspace_held_.find(workspace_id);
            if ((workspace->second.held -= released) == 0) {
                workspace_held_.erase(workspace);
            }
        }
        // Retry the older events first within each workspace.
        for (auto& [workspace_id, kept] : retry) {
            Queue& queue = queues_[workspace_id];
            if (queue.customer_id.empty()) {
                queue.customer_id = kept.customer_id;
            }
            queue.events.insert(queue.events.begin(), std::make_move_iterator(kept.events.begin()),
                                std::make_move_iterator(kept.events.end()));
        }
    }
    flushing_.store(false);
    co_return {};
}

} // namespace lake::api
