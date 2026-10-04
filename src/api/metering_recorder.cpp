#include "lake/api/metering_recorder.hpp"

#include <chrono>
#include <format>
#include <limits>

#include "lake/api/capabilities.hpp"
#include "lake/control/metering.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

std::string current_hour() {
    return std::format("{:%FT%H}:00:00Z",
                       std::chrono::floor<std::chrono::hours>(std::chrono::system_clock::now()));
}

std::uint64_t add_saturated(std::uint64_t left, std::uint64_t right) {
    return right > std::numeric_limits<std::uint64_t>::max() - left
               ? std::numeric_limits<std::uint64_t>::max()
               : left + right;
}

} // namespace

MeteringRecorder::MeteringRecorder(control::Store* store,
                                   control::WorkspaceService* workspaces,
                                   std::string instance_kek,
                                   std::size_t max_pending)
    : store_(store), workspaces_(workspaces), instance_kek_(std::move(instance_kek)),
      max_pending_(std::max<std::size_t>(max_pending, 1)) {}

bool MeteringRecorder::held_locked(std::size_t extra) const {
    return transfers_.size() + builds_.size() + extra <= max_pending_;
}

void MeteringRecorder::add_transfer(const control::Repository& repository,
                                    std::uint64_t transfer_in_bytes,
                                    std::uint64_t transfer_out_bytes,
                                    std::string hour) {
    if (transfer_in_bytes == 0 && transfer_out_bytes == 0) {
        return;
    }
    const std::uint64_t bytes = add_saturated(transfer_in_bytes, transfer_out_bytes);
    std::lock_guard lock(mutex_);
    if (repository.workspace_id.empty()) {
        totals_.unattributed_bytes = add_saturated(totals_.unattributed_bytes, bytes);
        return;
    }
    TransferKey key{repository.customer_id,
                    repository.workspace_id,
                    hour.empty() ? current_hour() : std::move(hour)};
    auto found = transfers_.find(key);
    if (found == transfers_.end()) {
        if (!held_locked(1)) {
            totals_.dropped_bytes = add_saturated(totals_.dropped_bytes, bytes);
            return;
        }
        found = transfers_.emplace(std::move(key), std::pair<std::uint64_t, std::uint64_t>{}).first;
    }
    found->second.first = add_saturated(found->second.first, transfer_in_bytes);
    found->second.second = add_saturated(found->second.second, transfer_out_bytes);
}

void MeteringRecorder::add_build(const std::string& workspace_id, control::MeteredBuild build) {
    if (workspace_id.empty() || build.result_id.empty()) {
        return;
    }
    std::lock_guard lock(mutex_);
    BuildKey key{workspace_id, build.result_id};
    if (builds_.contains(key)) {
        return;
    }
    if (!held_locked(1)) {
        ++totals_.dropped_builds; // found again by the next usage read
        return;
    }
    builds_.emplace(std::move(key), std::move(build));
}

std::vector<PendingTransfer>
MeteringRecorder::pending_transfers(const std::string& customer_id) const {
    std::lock_guard lock(mutex_);
    std::vector<PendingTransfer> pending;
    for (const auto& [key, counts] : transfers_) {
        if (std::get<0>(key) == customer_id) {
            pending.push_back({std::get<0>(key),
                               std::get<1>(key),
                               std::get<2>(key),
                               counts.first,
                               counts.second});
        }
    }
    return pending;
}

std::size_t MeteringRecorder::pending() const {
    std::lock_guard lock(mutex_);
    return transfers_.size() + builds_.size();
}

MeteringRecorder::Totals MeteringRecorder::totals() const {
    std::lock_guard lock(mutex_);
    return totals_;
}

// A workspace that was deleted, is unknown, or never belonged to this space, will never accept
// the write. Both stores answer an unknown workspace id with invalid_input, not not_found.
bool MeteringRecorder::workspace_gone(const std::string& customer_id,
                                      const std::string& workspace_id) const {
    if (!store_) {
        return false;
    }
    const auto workspace = store_->get_workspace(workspace_id);
    if (!workspace) {
        return workspace.error().code == "not_found" || workspace.error().code == "invalid_input";
    }
    return workspace->desired == "deleted" || workspace->customer_id != customer_id;
}

liquid::runtime::task<void> MeteringRecorder::flush() {
    if (flushing_.exchange(true)) {
        co_return {};
    }
    std::map<TransferKey, std::pair<std::uint64_t, std::uint64_t>> transfers;
    std::map<BuildKey, control::MeteredBuild> builds;
    {
        std::lock_guard lock(mutex_);
        transfers.swap(transfers_);
        builds.swap(builds_);
    }
    ApiContext context;
    context.store = store_;
    context.workspaces = workspaces_;
    context.instance_kek = instance_kek_;

    // Each failed write goes back into the buffer (merging with what arrived meanwhile), unless
    // its workspace is gone or the bound is reached.
    std::uint64_t written_transfers = 0, written_builds = 0, errors = 0;
    std::map<TransferKey, std::pair<std::uint64_t, std::uint64_t>> retry_transfers;
    std::map<BuildKey, control::MeteredBuild> retry_builds;
    std::uint64_t gone_bytes = 0, gone_builds = 0;
    for (const auto& [key, counts] : transfers) {
        const auto& [customer_id, workspace_id, hour] = key;
        auto records = workspace_records(context, customer_id, workspace_id);
        auto added = records ? control::settled(co_await control::add_metered_transfer(
                                   *records, customer_id, hour, counts.first, counts.second))
                             : control::ZettaControlResult<void>(std::unexpected(records.error()));
        if (added) {
            ++written_transfers;
            continue;
        }
        ++errors;
        if (workspace_gone(customer_id, workspace_id)) {
            gone_bytes = add_saturated(gone_bytes, add_saturated(counts.first, counts.second));
            continue;
        }
        retry_transfers.emplace(key, counts);
    }
    for (auto& [key, build] : builds) {
        auto records = workspace_records(context, build.customer_id, key.first);
        auto recorded =
            records ? control::settled(co_await control::record_metered_build(*records, build))
                    : control::ZettaControlResult<bool>(std::unexpected(records.error()));
        if (recorded) {
            written_builds += *recorded ? 1 : 0;
            continue;
        }
        ++errors;
        if (workspace_gone(build.customer_id, key.first)) {
            ++gone_builds;
            continue;
        }
        retry_builds.emplace(key, std::move(build));
    }
    {
        std::lock_guard lock(mutex_);
        totals_.flushed_transfers += written_transfers;
        totals_.flushed_builds += written_builds;
        totals_.flush_errors += errors;
        totals_.dropped_bytes = add_saturated(totals_.dropped_bytes, gone_bytes);
        totals_.dropped_builds += gone_builds;
        for (auto& [key, counts] : retry_transfers) {
            auto found = transfers_.find(key);
            if (found == transfers_.end()) {
                if (!held_locked(1)) {
                    totals_.dropped_bytes = add_saturated(
                        totals_.dropped_bytes, add_saturated(counts.first, counts.second));
                    continue;
                }
                transfers_.emplace(key, counts);
                continue;
            }
            found->second.first = add_saturated(found->second.first, counts.first);
            found->second.second = add_saturated(found->second.second, counts.second);
        }
        for (auto& [key, build] : retry_builds) {
            if (builds_.contains(key)) {
                continue;
            }
            if (!held_locked(1)) {
                ++totals_.dropped_builds;
                continue;
            }
            builds_.emplace(key, std::move(build));
        }
    }
    flushing_.store(false);
    co_return {};
}

} // namespace lake::api
