#include "lake/api/usage_recorder.hpp"

#include <algorithm>
#include <limits>
#include <utility>

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

// A refusal no retry can change: the workspace defines the metric differently, or a point is
// invalid. Anything else (not ready, unavailable, busy) is retried.
bool refused_for_good(const control::ZettaControlError& error) {
    return error.code == "conflict" || error.code == "invalid_input";
}

std::int64_t epoch_ms(std::chrono::system_clock::time_point at) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
}

std::int64_t hour_of(std::int64_t at_ms) {
    const std::int64_t offset = at_ms % UsageRecorder::resolution_ms;
    return at_ms - (offset < 0 ? offset + UsageRecorder::resolution_ms : offset);
}

} // namespace

const std::vector<UsageDimension>& usage_dimensions() {
    static const std::vector<UsageDimension> dimensions{
        {"storage", "bytes", "last", "Storage"},
        {"transfer", "bytes", "sum", "Transfer"},
        {"build_compute", "seconds", "sum", "Build Compute"},
        {"run_compute", "seconds", "sum", "Workspace Run Compute"},
        {"speech", "seconds", "sum", "Speech to Text"},
        {"calls", "calls", "sum", "API and MCP Calls"},
        {"predictions", "calls", "sum", "Model Predictions"},
        {"model_tokens", "tokens", "sum", "Model Tokens"},
        {"instances", "instance_seconds", "sum", "Service and App Instances"},
        {"volumes", "gib_seconds", "sum", "Service and Store Volumes"},
    };
    return dimensions;
}

const UsageDimension* find_usage_dimension(std::string_view name) {
    for (const UsageDimension& dimension : usage_dimensions()) {
        if (dimension.name == name) {
            return &dimension;
        }
    }
    return nullptr;
}

UsageRecorder::UsageRecorder(control::Store* store,
                             control::WorkspaceService* workspaces,
                             std::size_t max_pending)
    : store_(store), workspaces_(workspaces), max_pending_(std::max<std::size_t>(max_pending, 1)) {}

bool UsageRecorder::gauge(const Key& key) {
    return std::get<1>(key) == storage_metric;
}

void UsageRecorder::merge_locked(const Key& key, const Held& held) {
    auto found = held_.find(key);
    if (found == held_.end()) {
        held_.emplace(key, held);
        return;
    }
    if (gauge(key)) {
        if (held.at_ms >= found->second.at_ms) {
            found->second = held;
        }
        return;
    }
    found->second.value = add_saturated(found->second.value, held.value);
}

void UsageRecorder::add(const std::string& workspace_id,
                        std::string_view dimension,
                        std::uint64_t value,
                        std::map<std::string, std::string> labels,
                        std::chrono::system_clock::time_point at) {
    std::vector<Increment> one;
    one.push_back({std::string(dimension), value, std::move(labels)});
    add_together(workspace_id, std::move(one), at);
}

void UsageRecorder::add_together(const std::string& workspace_id,
                                 std::vector<Increment> increments,
                                 std::chrono::system_clock::time_point at) {
    if (workspace_id.empty()) {
        return;
    }
    std::erase_if(increments, [](const Increment& increment) {
        return increment.value == 0 || increment.dimension.empty();
    });
    if (increments.empty()) {
        return;
    }
    const std::int64_t hour = hour_of(epoch_ms(at));
    std::vector<std::pair<Key, std::uint64_t>> keys;
    keys.reserve(increments.size());
    for (Increment& increment : increments) {
        nlohmann::json labels = nlohmann::json::object();
        for (auto& [name, value] : increment.labels) {
            if (!value.empty()) {
                labels[name] = std::move(value);
            }
        }
        labels["dimension"] = std::move(increment.dimension);
        keys.emplace_back(Key{workspace_id, std::string(usage_metric), hour, labels.dump()},
                          increment.value);
    }
    std::lock_guard lock(mutex_);
    std::size_t needed = 0;
    std::set<Key> fresh;
    for (const auto& [key, value] : keys) {
        if (!held_.contains(key) && fresh.insert(key).second) {
            ++needed;
        }
    }
    if (held_.size() + needed > max_pending_) {
        ++totals_.dropped_bound;
        return;
    }
    for (const auto& [key, value] : keys) {
        merge_locked(key, Held{value, hour});
    }
}

void UsageRecorder::sample(const std::string& workspace_id,
                           std::string_view kind,
                           std::uint64_t bytes,
                           std::chrono::system_clock::time_point at) {
    if (workspace_id.empty() || kind.empty()) {
        return;
    }
    const std::int64_t at_ms = epoch_ms(at);
    Key key{workspace_id,
            std::string(storage_metric),
            hour_of(at_ms),
            nlohmann::json{{"kind", std::string(kind)}}.dump()};
    std::lock_guard lock(mutex_);
    if (!held_.contains(key) && held_.size() >= max_pending_) {
        ++totals_.dropped_bound;
        return;
    }
    merge_locked(key, Held{bytes, at_ms});
}

std::size_t UsageRecorder::pending() const {
    std::lock_guard lock(mutex_);
    return held_.size();
}

UsageRecorder::Totals UsageRecorder::totals() const {
    std::lock_guard lock(mutex_);
    return totals_;
}

liquid::runtime::task<void> UsageRecorder::flush() {
    if (flushing_.exchange(true)) {
        co_return {};
    }
    std::map<Key, Held> held;
    std::set<std::string> created;
    {
        std::lock_guard lock(mutex_);
        held.swap(held_);
        created = created_;
    }
    using Values = std::vector<std::pair<Key, Held>>;
    std::map<std::string, std::map<std::string, Values>> by_workspace;
    for (auto& [key, value] : held) {
        by_workspace[std::get<0>(key)][std::get<1>(key)].emplace_back(key, value);
    }

    Totals delta;
    Values retry;
    std::set<std::string> now_created, forget_created;
    const auto count = [](std::uint64_t& total, std::size_t values) {
        total = add_saturated(total, values);
    };
    for (auto& [workspace_id, metrics] : by_workspace) {
        // The workspace's space, and whether it still exists: one routing-row read per workspace
        // and flush, off the request path.
        std::string customer_id;
        bool gone = store_ == nullptr || workspaces_ == nullptr;
        if (!gone) {
            const auto workspace = store_->get_workspace(workspace_id);
            if (!workspace) {
                if (workspace.error().code == "not_found" ||
                    workspace.error().code == "invalid_input") {
                    gone = true;
                } else {
                    ++delta.flush_errors;
                    for (const auto& [name, values] : metrics) {
                        retry.insert(retry.end(), values.begin(), values.end());
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
            for (const auto& [name, values] : metrics) {
                count(delta.dropped_workspace_deleted, values.size());
            }
            continue;
        }
        ApiContext context;
        context.store = store_;
        context.workspaces = workspaces_;
        context.workspace_id = workspace_id;
        for (const auto& [name, values] : metrics) {
            const bool is_gauge = name == storage_metric;
            const std::string defined_key = workspace_id + "/" + name;
            if (!created.contains(defined_key)) {
                auto defined = control::settled(
                    co_await call_workspace(context,
                                            customer_id,
                                            "create_metric",
                                            false,
                                            nlohmann::json{{"metric", name},
                                                           {"kind", is_gauge ? "gauge" : "counter"},
                                                           {"resolution_ms", resolution_ms},
                                                           {"retention_ms", retention_ms}}));
                if (!defined) {
                    ++delta.flush_errors;
                    if (refused_for_good(defined.error())) {
                        count(delta.dropped_refused, values.size());
                    } else {
                        retry.insert(retry.end(), values.begin(), values.end());
                    }
                    continue;
                }
                now_created.insert(defined_key);
                created.insert(defined_key);
            }
            for (std::size_t from = 0; from < values.size(); from += max_points_per_call) {
                const std::size_t to = std::min(values.size(), from + max_points_per_call);
                nlohmann::json points = nlohmann::json::array();
                for (std::size_t index = from; index < to; ++index) {
                    const auto& [key, value] = values[index];
                    points.push_back({{"labels", nlohmann::json::parse(std::get<3>(key))},
                                      {"value", value.value},
                                      {"at_ms", value.at_ms}});
                }
                auto recorded = control::settled(co_await call_workspace(
                    context,
                    customer_id,
                    "record_metric",
                    false,
                    nlohmann::json{{"metric", name}, {"points", std::move(points)}}));
                if (recorded) {
                    delta.points += to - from;
                    count(is_gauge ? delta.readings : delta.increments, to - from);
                    continue;
                }
                ++delta.flush_errors;
                if (recorded.error().code == "not_found") {
                    // The metric is gone (a recreated workspace Zetta): define it again next time.
                    forget_created.insert(defined_key);
                    now_created.erase(defined_key);
                }
                if (refused_for_good(recorded.error())) {
                    count(delta.dropped_refused, to - from);
                } else {
                    retry.insert(retry.end(),
                                 values.begin() + static_cast<std::ptrdiff_t>(from),
                                 values.begin() + static_cast<std::ptrdiff_t>(to));
                }
            }
        }
    }
    {
        std::lock_guard lock(mutex_);
        totals_.increments = add_saturated(totals_.increments, delta.increments);
        totals_.readings = add_saturated(totals_.readings, delta.readings);
        totals_.points += delta.points;
        totals_.flush_errors += delta.flush_errors;
        totals_.dropped_workspace_deleted =
            add_saturated(totals_.dropped_workspace_deleted, delta.dropped_workspace_deleted);
        totals_.dropped_refused = add_saturated(totals_.dropped_refused, delta.dropped_refused);
        created_.insert(now_created.begin(), now_created.end());
        for (const auto& defined_key : forget_created) {
            created_.erase(defined_key);
        }
        for (const auto& [key, value] : retry) {
            if (held_.contains(key) || held_.size() < max_pending_) {
                merge_locked(key, value);
            } else {
                ++totals_.dropped_bound;
            }
        }
    }
    flushing_.store(false);
    co_return {};
}

} // namespace lake::api
