#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <liquid/runtime.hpp>

#include "lake/audit/audit.hpp"
#include "lake/control/store.hpp"
#include "lake/control/workspace_audit.hpp"

namespace lake::control {
class WorkspaceService;
}

namespace lake::api {

// An event queued for a workspace's audit and not yet written there.
struct PendingWorkspaceEvent {
    std::string customer_id;
    std::string workspace_id;
    audit::PendingEvent event;
};

// The workspace audit's writer (lake#317, decision 52, docs/api/audit.md#workspace-audit). A
// capability call about a workspace's data only queues its event here, with the id and instant
// fixed when the call finished; `flush`, run by the server's owner loop every second (and at once
// when the queue is half full) and once more when the server drains, signs each workspace's
// events in order and appends them to that workspace's chain, one conditional write per event.
// Nothing here touches lake's PostgreSQL except to read lake's identity and a workspace's row.
//
// Bounded: at most `max_pending` events are held. An event beyond the bound is dropped and counted
// (`dropped`), never blocked on. A failed write keeps the workspace's events, in order, for the
// next flush; the events of a deleted workspace are dropped and counted (`dropped_gone`).
//
// Loss on a crash: the events queued since the last successful flush (the flush interval, one
// second by default, plus any write that kept failing). A drain flushes before the process exits.
class AuditRecorder {
public:
    struct Totals {
        std::uint64_t written = 0;      // events appended to a workspace chain
        std::uint64_t flush_errors = 0; // workspace flushes kept for the next one
        std::uint64_t conflicts = 0;    // places another replica took first
        std::uint64_t dropped = 0;      // events dropped at the bound
        std::uint64_t dropped_gone = 0; // events of a deleted workspace
    };

    AuditRecorder(control::Store* store,
                  control::WorkspaceService* workspaces,
                  std::string instance_kek,
                  std::string identity_seed,
                  std::size_t max_pending = 65536);

    // Queues one event for `workspace_id`'s audit. Never does I/O. False when dropped at the
    // bound.
    bool add(std::string customer_id, std::string workspace_id, audit::PendingEvent event);

    // This process's unwritten events of these workspaces (all when empty), oldest first, so a
    // read right after a call shows it.
    std::vector<PendingWorkspaceEvent>
    pending_events(const std::vector<std::string>& workspace_ids = {}) const;
    std::size_t pending() const;

    // Writes everything held. Safe to call from any thread; a flush already running makes this
    // one return at once.
    liquid::runtime::task<void> flush();

    Totals totals() const;

private:
    struct Queue {
        std::string customer_id;
        std::vector<audit::PendingEvent> events;
    };
    bool workspace_gone(const std::string& customer_id, const std::string& workspace_id) const;

    control::Store* store_;
    control::WorkspaceService* workspaces_;
    std::string instance_kek_;
    std::string identity_seed_;
    std::size_t max_pending_;
    mutable std::mutex mutex_;
    std::map<std::string, Queue> queues_; // by workspace
    std::size_t held_ = 0;
    // The last place this process knows taken in each workspace's chain. Only a hint: a place
    // another replica took is found by the conflict on writing it.
    std::map<std::string, control::WorkspaceAuditTail> tails_;
    Totals totals_;
    std::atomic<bool> flushing_{false};
};

} // namespace lake::api
