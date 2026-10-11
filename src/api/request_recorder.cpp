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

namespace {

std::int64_t minute_of(std::chrono::system_clock::time_point at, std::int64_t resolution) {
    const std::int64_t at_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
    return at_ms - at_ms % resolution;
}

// Whether a metric's value counts requests or calls (the dropped totals count those), rather
// than milliseconds.
bool counts_calls(std::string_view metric) {
    return metric != RequestRecorder::model_call_ms_metric;
}

} // namespace

void RequestRecorder::add_locked(Key key, std::uint64_t value) {
    auto found = counts_.find(key);
    if (found == counts_.end()) {
        if (counts_.size() >= max_pending_) {
            if (counts_calls(std::get<1>(key))) {
                totals_.dropped_bound = add_saturated(totals_.dropped_bound, value);
            }
            return;
        }
        found = counts_.emplace(std::move(key), 0).first;
    }
    found->second = add_saturated(found->second, value);
}

void RequestRecorder::add(const std::string& workspace_id,
                          const std::string& route,
                          const std::string& capability,
                          int status,
                          std::chrono::system_clock::time_point at) {
    if (workspace_id.empty()) {
        return;
    }
    nlohmann::json labels{{"route", route}, {"status_class", status_class(status)}};
    if (!capability.empty()) {
        labels["capability"] = capability;
    }
    Key key{workspace_id, std::string(metric), minute_of(at, resolution_ms), labels.dump()};
    std::lock_guard lock(mutex_);
    add_locked(std::move(key), 1);
}

void RequestRecorder::add_model_call(const std::string& workspace_id,
                                     const std::string& model_id,
                                     int status,
                                     double duration_ms,
                                     std::chrono::system_clock::time_point at) {
    if (workspace_id.empty() || model_id.empty()) {
        return;
    }
    const std::string labels =
        nlohmann::json{{"model_id", model_id}, {"status_class", status_class(status)}}.dump();
    const std::int64_t minute = minute_of(at, resolution_ms);
    Key calls{workspace_id, std::string(model_calls_metric), minute, labels};
    Key ms{workspace_id, std::string(model_call_ms_metric), minute, labels};
    const auto millis = static_cast<std::uint64_t>(std::max(0.0, duration_ms) + 0.5);
    std::lock_guard lock(mutex_);
    // Both or neither: a call counted without its duration would lower the mean.
    const std::size_t needed = (counts_.contains(calls) ? 0 : 1) + (counts_.contains(ms) ? 0 : 1);
    if (counts_.size() + needed > max_pending_) {
        totals_.dropped_bound = add_saturated(totals_.dropped_bound, 1);
        return;
    }
    add_locked(std::move(calls), 1);
    add_locked(std::move(ms), millis);
}

void RequestRecorder::requeue_locked(const Key& key, std::uint64_t count) {
    auto found = counts_.find(key);
    if (found != counts_.end()) {
        found->second = add_saturated(found->second, count);
        return;
    }
    if (counts_.size() >= max_pending_) {
        if (counts_calls(std::get<1>(key))) {
            totals_.dropped_bound = add_saturated(totals_.dropped_bound, count);
        }
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
    // One workspace at a time, one metric at a time: its counts, in key order.
    using Counts = std::vector<std::pair<Key, std::uint64_t>>;
    std::map<std::string, std::map<std::string, Counts>> by_workspace;
    for (auto& [key, count] : held) {
        by_workspace[std::get<0>(key)][std::get<1>(key)].emplace_back(key, count);
    }

    Totals delta;
    Counts retry;
    std::set<std::string> now_created, forget_created;
    const auto drop =
        [](std::uint64_t& total, const Counts& counts, std::size_t from, std::size_t to) {
            for (std::size_t index = from; index < to; ++index) {
                if (counts_calls(std::get<1>(counts[index].first))) {
                    total = add_saturated(total, counts[index].second);
                }
            }
        };
    for (auto& [workspace_id, metrics] : by_workspace) {
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
                    for (const auto& [name, counts] : metrics) {
                        retry.insert(retry.end(), counts.begin(), counts.end());
                    }
                    continue;
                }
            } else if (workspace->desired == "deleted") {
                gone = true;
            } else {
                customer_id = workspace->customer_id;
            }
        }
        if (gone) {
            for (const auto& [name, counts] : metrics) {
                drop(delta.dropped_workspace_deleted, counts, 0, counts.size());
            }
            continue;
        }
        ApiContext context;
        context.store = store_;
        context.workspaces = workspaces_;
        context.workspace_id = workspace_id;
        for (const auto& [name, counts] : metrics) {
            const std::string defined_key = workspace_id + "/" + name;
            if (!created.contains(defined_key)) {
                auto defined = control::settled(
                    co_await call_workspace(context,
                                            customer_id,
                                            "create_metric",
                                            false,
                                            nlohmann::json{{"metric", name},
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
                now_created.insert(defined_key);
                created.insert(defined_key);
            }
            for (std::size_t from = 0; from < counts.size(); from += max_points_per_call) {
                const std::size_t to = std::min(counts.size(), from + max_points_per_call);
                nlohmann::json points = nlohmann::json::array();
                std::uint64_t values = 0;
                for (std::size_t index = from; index < to; ++index) {
                    const auto& [key, count] = counts[index];
                    points.push_back({{"labels", nlohmann::json::parse(std::get<3>(key))},
                                      {"value", count},
                                      {"at_ms", std::get<2>(key)}});
                    values = add_saturated(values, count);
                }
                auto recorded = control::settled(co_await call_workspace(
                    context,
                    customer_id,
                    "record_metric",
                    false,
                    nlohmann::json{{"metric", name}, {"points", std::move(points)}}));
                if (recorded) {
                    delta.points += to - from;
                    if (name == metric) {
                        delta.recorded = add_saturated(delta.recorded, values);
                    } else if (name == model_calls_metric) {
                        delta.model_calls = add_saturated(delta.model_calls, values);
                    }
                    continue;
                }
                ++delta.flush_errors;
                if (recorded.error().code == "not_found") {
                    // The metric is gone (a recreated workspace Zetta): define it again next time.
                    forget_created.insert(defined_key);
                    now_created.erase(defined_key);
                }
                if (refused_for_good(recorded.error())) {
                    drop(delta.dropped_refused, counts, from, to);
                } else {
                    retry.insert(retry.end(),
                                 counts.begin() + static_cast<std::ptrdiff_t>(from),
                                 counts.begin() + static_cast<std::ptrdiff_t>(to));
                }
            }
        }
    }
    {
        std::lock_guard lock(mutex_);
        totals_.recorded = add_saturated(totals_.recorded, delta.recorded);
        totals_.model_calls = add_saturated(totals_.model_calls, delta.model_calls);
        totals_.points += delta.points;
        totals_.flush_errors += delta.flush_errors;
        totals_.dropped_workspace_deleted =
            add_saturated(totals_.dropped_workspace_deleted, delta.dropped_workspace_deleted);
        totals_.dropped_refused = add_saturated(totals_.dropped_refused, delta.dropped_refused);
        created_.insert(now_created.begin(), now_created.end());
        for (const auto& defined_key : forget_created) {
            created_.erase(defined_key);
        }
        for (const auto& [key, count] : retry) {
            requeue_locked(key, count);
        }
    }
    flushing_.store(false);
    co_return {};
}

} // namespace lake::api
