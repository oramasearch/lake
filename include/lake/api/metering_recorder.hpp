#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <liquid/runtime.hpp>

#include "lake/control/store.hpp"

namespace lake::control {
class WorkspaceService;
}

namespace lake::api {

// Transfer counted in this process and not yet written to its workspace.
struct PendingTransfer {
    std::string customer_id;
    std::string workspace_id;
    std::string hour;
    std::uint64_t transfer_in_bytes = 0;
    std::uint64_t transfer_out_bytes = 0;
};

// Repository usage counted on the request path and written to the workspace Zettas off it
// (docs/api/metering.md, lake#348, lake#349). A sync request only adds its bytes to an in-memory
// counter for its repository's workspace and hour; `flush`, run by the server's owner loop every
// few seconds and once more when the server drains, adds each counter to that workspace's hourly
// record with a conditional write. A build result found by a usage read is queued the same way and
// written `if_absent`. Nothing here touches lake's PostgreSQL.
//
// Bounded: at most `max_pending` counters and queued build records are held. An increment for a
// counter already held always merges; a new one beyond the bound is dropped and counted
// (`dropped_bytes`, `dropped_builds`), never blocked on. A failed write is kept for the next flush
// unless its workspace is gone (deleted, unknown, or of another space), in which case it is
// dropped and counted.
//
// Loss on a crash: what was counted since the last successful flush (the flush interval, five
// seconds by default, plus any write that kept failing). A drain flushes before the process
// exits. Build records lose nothing: the next usage read finds the result again and writes it.
class MeteringRecorder {
public:
    struct Totals {
        std::uint64_t flushed_transfers = 0; // hourly records written
        std::uint64_t flushed_builds = 0;    // build records written (new results)
        std::uint64_t flush_errors = 0;      // writes kept for the next flush
        std::uint64_t dropped_bytes = 0; // transfer bytes dropped at the bound or gone workspace
        std::uint64_t dropped_builds = 0;
        std::uint64_t unattributed_bytes = 0; // transfer of repositories without a workspace
    };

    MeteringRecorder(control::Store* store,
                     control::WorkspaceService* workspaces,
                     std::string instance_kek,
                     std::size_t max_pending = 4096);

    // Counts a sync request's bytes for its repository's workspace in `hour` (the current UTC
    // hour when empty). Never does I/O. A repository without a workspace (one migration 0051
    // recorded as needs_selection) is counted as unattributed and not recorded.
    void add_transfer(const control::Repository& repository,
                      std::uint64_t transfer_in_bytes,
                      std::uint64_t transfer_out_bytes,
                      std::string hour = {});

    // Queues a build result for its repository's workspace, once per result until flushed.
    void add_build(const std::string& workspace_id, control::MeteredBuild build);

    // This process's unflushed transfer of one space, so a usage read includes it.
    std::vector<PendingTransfer> pending_transfers(const std::string& customer_id) const;
    std::size_t pending() const;

    // Writes everything held. Safe to call from any thread; a flush already running makes this
    // one return at once.
    liquid::runtime::task<void> flush();

    Totals totals() const;

private:
    using TransferKey = std::tuple<std::string, std::string, std::string>; // space, workspace, hour
    using BuildKey = std::pair<std::string, std::string>;                  // workspace, result
    bool held_locked(std::size_t extra) const;
    bool workspace_gone(const std::string& customer_id, const std::string& workspace_id) const;

    control::Store* store_;
    control::WorkspaceService* workspaces_;
    std::string instance_kek_;
    std::size_t max_pending_;
    mutable std::mutex mutex_;
    std::map<TransferKey, std::pair<std::uint64_t, std::uint64_t>> transfers_;
    std::map<BuildKey, control::MeteredBuild> builds_;
    Totals totals_;
    std::atomic<bool> flushing_{false};
};

} // namespace lake::api
