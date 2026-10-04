#include "lake/api/request_recorder.hpp"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/control/agent_bus.hpp"
#include "lake/control/zetta_workspace.hpp"

namespace lake::api {
namespace {

std::uint64_t add_saturated(std::uint64_t left, std::uint64_t right) {
    return right > std::numeric_limits<std::uint64_t>::max() - left
               ? std::numeric_limits<std::uint64_t>::max()
               : left + right;
}

// A refusal that no retry can change: the workspace already defines `lake_requests` differently,
// or a point is invalid. Anything else (not ready, unavailable, busy) is retried.
bool refused_for_good(const control::ZettaControlError& error) {
    return error.code == "conflict" || error.code == "invalid_input";
}

} // namespace

RequestRecorder::RequestRecorder(control::Store* store,
                                 control::WorkspaceService* workspaces,
                                 std::size_t max_pending)
    : store_(store), workspaces_(workspaces), max_pending_(std::max<std::size_t>(max_pending, 1)) {
}

std::string RequestRecorder::status_class(int status) {
    if (status < 100 || status > 599) {
        return "other";
    }
    return std::to_string(status / 100) + "xx";
}

void RequestRecorder::add(const std::string& workspace_id,
                          const std::string& route,
                          const std::string& capability,
                          int status,
                          std::chrono::system_clock::time_point at) {
    if (workspace_id.empty()) {
        return;
    }
    const std::int64_t at_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
    Key key{workspace_id, at_ms - at_ms % resolution_ms, route, capability, status_class(status)};
    std::lock_guard lock(mutex_);
    auto found = counts_.find(key);
    if (found == counts_.end()) {
        if (counts_.size() >= max_pending_) {
            ++totals_.dropped_bound;
            return;
        }
        found = counts_.emplace(std::move(key), 0).first;
    }
    found->second = add_saturated(found->second, 1);
}

void RequestRecorder::requeue_locked(const Key& key, std::uint64_t count) {
    auto found = counts_.find(key);
    if (found != counts_.end()) {
        found->second = add_saturated(found->second, count);
        return;
    }
    if (counts_.size() >= max_pending_) {
        totals_.dropped_bound = add_saturated(totals_.dropped_bound, count);
        return;
    }
    counts_.emplace(key, count);
}

std::size_t RequestRecorder::pending() const {
    std::lock_guard lock(mutex_);
    return counts_.size();
}

RequestRecorder::Totals RequestRecorder::totals() const {
    std::lock_guard lock(mutex_);
    return totals_;
}

liquid::runtime::task<void> RequestRecorder::flush() {
    if (flushing_.exchange(true)) {
        co_return {};
    }
    std::map<Key, std::uint64_t> held;
    std::set<std::string> created;
    {
        std::lock_guard lock(mutex_);
        held.swap(counts_);
        created = created_;
    }
    // One workspace at a time: its counts, in key order.
    std::map<std::string, std::vector<std::pair<Key, std::uint64_t>>> by_workspace;
    for (auto& [key, count] : held) {
        by_workspace[std::get<0>(key)].emplace_back(key, count);
    }

    Totals delta;
    std::vector<std::pair<Key, std::uint64_t>> retry;
    std::set<std::string> now_created, forget_created;
    const auto drop = [](std::uint64_t& total, const auto& counts, std::size_t from, std::size_t to) {
        for (std::size_t index = from; index < to; ++index) {
            total = add_saturated(total, counts[index].second);
        }
    };
    for (auto& [workspace_id, counts] : by_workspace) {
        // The workspace's space, and whether it still exists: one routing-row read per workspace
        // and flush, off the request path.
        std::string customer_id;
        bool gone = store_ == nullptr || workspaces_ == nullptr;
        if (!gone) {
            const auto workspace = store_->get_workspace(workspace_id);
            if (!workspace) {
                // Both stores answer an unknown workspace id with invalid_input.
                if (workspace.error().code == "not_found" ||
                    workspace.error().code == "invalid_input") {
                    gone = true;
                } else {
                    ++delta.flush_errors; // the control plane did not answer: try again
                    retry.insert(retry.end(), counts.begin(), counts.end());
                    continue;
                }
            } else if (workspace->desired == "deleted") {
                gone = true;
            } else {
                customer_id = workspace->customer_id;
            }
        }
        if (gone) {
            drop(delta.dropped_workspace_deleted, counts, 0, counts.size());
            continue;
        }
        ApiContext context;
        context.store = store_;
        context.workspaces = workspaces_;
        context.workspace_id = workspace_id;
        if (!created.contains(workspace_id)) {
            auto defined = control::settled(co_await call_workspace(
                context, customer_id, "create_metric", false,
                nlohmann::json{{"metric", std::string(metric)},
                               {"kind", "counter"},
                               {"resolution_ms", resolution_ms},
                               {"retention_ms", retention_ms}}));
            if (!defined) {
                ++delta.flush_errors;
                if (refused_for_good(defined.error())) {
                    drop(delta.dropped_refused, counts, 0, counts.size());
                } else {
                    retry.insert(retry.end(), counts.begin(), counts.end());
                }
                continue;
            }
            now_created.insert(workspace_id);
        }
        for (std::size_t from = 0; from < counts.size(); from += max_points_per_call) {
            const std::size_t to = std::min(counts.size(), from + max_points_per_call);
            nlohmann::json points = nlohmann::json::array();
            std::uint64_t requests = 0;
            for (std::size_t index = from; index < to; ++index) {
                const auto& [key, count] = counts[index];
                nlohmann::json labels{{"route", std::get<2>(key)},
                                      {"status_class", std::get<4>(key)}};
                if (!std::get<3>(key).empty()) {
                    labels["capability"] = std::get<3>(key);
                }
                points.push_back({{"labels", std::move(labels)},
                                  {"value", count},
                                  {"at_ms", std::get<1>(key)}});
                requests = add_saturated(requests, count);
            }
            auto recorded = control::settled(co_await call_workspace(
                context, customer_id, "record_metric", false,
                nlohmann::json{{"metric", std::string(metric)}, {"points", std::move(points)}}));
            if (recorded) {
                delta.points += to - from;
                delta.recorded = add_saturated(delta.recorded, requests);
                continue;
            }
            ++delta.flush_errors;
            if (recorded.error().code == "not_found") {
                // The metric is gone (a recreated workspace Zetta): define it again next time.
                forget_created.insert(workspace_id);
                now_created.erase(workspace_id);
            }
            if (refused_for_good(recorded.error())) {
                drop(delta.dropped_refused, counts, from, to);
            } else {
                retry.insert(retry.end(), counts.begin() + static_cast<std::ptrdiff_t>(from),
                             counts.begin() + static_cast<std::ptrdiff_t>(to));
            }
        }
    }
    {
        std::lock_guard lock(mutex_);
        totals_.recorded = add_saturated(totals_.recorded, delta.recorded);
        totals_.points += delta.points;
        totals_.flush_errors += delta.flush_errors;
        totals_.dropped_workspace_deleted =
            add_saturated(totals_.dropped_workspace_deleted, delta.dropped_workspace_deleted);
        totals_.dropped_refused = add_saturated(totals_.dropped_refused, delta.dropped_refused);
        created_.insert(now_created.begin(), now_created.end());
        for (const auto& workspace_id : forget_created) {
            created_.erase(workspace_id);
        }
        for (const auto& [key, count] : retry) {
            requeue_locked(key, count);
        }
    }
    flushing_.store(false);
    co_return {};
}

} // namespace lake::api
