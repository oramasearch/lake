#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <liquid/runtime.hpp>

#include "lake/control/store.hpp"

namespace lake::control {
class WorkspaceService;
}

namespace lake::api {

// Usage over time, per workspace, in the workspace Zetta (lake#564, docs/api/metering.md#usage-
// over-time). Two series in each workspace Zetta's labelled time series (zetta docs/data/
// time-series.md), one bucket an hour, kept 400 days:
//
// - `lake_usage`, a counter labelled `dimension` and, by dimension, `kind`, `agent`, `launch_id`,
//   `model`, `object_id` and `class`: API and MCP calls, each model's answered predictions and
//   each index's answered searches, speech-to-text seconds, model tokens, the instance and volume
//   seconds of managed services, stores and apps, the CPU, RAM, GPU and VRAM seconds every
//   workload is scheduled with (lake#643), and transfer outside repository sync (lake#658);
// - `lake_storage`, a gauge labelled `kind` (repositories, images, workspace_data, transcripts,
//   drive_files, and the engine's models, indexes, runs, streams, files and buckets): the
//   workspace's stored bytes, sampled by the owner loop.
//
// A request only adds to an in-memory value for (workspace, metric, hour, labels); nothing here
// does I/O on the request path. `flush`, run by the server's owner loop and once more when the
// server drains, creates each metric once per workspace and process (`create_metric`, idempotent)
// and writes the values with `record_metric`, at most 1000 points per call, through the workspace's
// own connection (call_workspace). Never a default workspace, never the lake Zetta, never
// PostgreSQL.
//
// Bounded: at most `max_pending` values are held, across both metrics. An increment of a value
// already held always merges, and a reading replaces the older reading of its hour; a new value
// beyond the bound is dropped and counted (`dropped_bound`), never blocked on. Values added
// together (a model call's input and output tokens) are kept or dropped together. A failed write
// is kept for the next flush unless its workspace is gone (deleted or unknown,
// `dropped_workspace_deleted`) or refused the metric for good (`dropped_refused`: a conflicting
// definition or an invalid point). The dropped totals count additions and readings, not their
// values. Like the request series, the counter is approximate: a crash loses the last interval,
// and a write whose answer was lost may count twice. Exact repository transfer and build compute
// stay metering records (MeteringRecorder).
class UsageRecorder {
public:
    static constexpr std::string_view usage_metric = "lake_usage";
    static constexpr std::string_view storage_metric = "lake_storage";
    static constexpr std::int64_t resolution_ms = 3'600'000;
    static constexpr std::int64_t retention_ms = 400LL * 24 * 60 * 60 * 1000;
    static constexpr std::size_t max_points_per_call = 1000;

    // One labelled value of the counter: `dimension` and its other labels (each at most 256
    // bytes, bounded by what the workspace holds; never a row's content).
    struct Increment {
        std::string dimension;
        std::uint64_t value = 0;
        std::map<std::string, std::string> labels;
    };

    struct Totals {
        std::uint64_t increments = 0; // additions written to `lake_usage`
        std::uint64_t readings = 0;   // storage readings written to `lake_storage`
        std::uint64_t points = 0;     // points written, both metrics
        std::uint64_t flush_errors = 0;
        std::uint64_t dropped_bound = 0;
        std::uint64_t dropped_workspace_deleted = 0;
        std::uint64_t dropped_refused = 0;
    };

    UsageRecorder(control::Store* store,
                  control::WorkspaceService* workspaces,
                  std::size_t max_pending = 8192);

    // Adds `value` of `dimension` to the workspace's hour. A zero value or an empty workspace is
    // ignored. Never does I/O.
    void add(const std::string& workspace_id,
             std::string_view dimension,
             std::uint64_t value,
             std::map<std::string, std::string> labels = {},
             std::chrono::system_clock::time_point at = std::chrono::system_clock::now());

    // Transfer outside repository sync (lake#658): bytes lake accepted for the workspace (`in`:
    // a drive upload, an image push) or sent from it (`out`: a download, an image pull, a file
    // read), as `transfer` of that kind. Repository sync keeps its exact hourly records instead.
    // Never does I/O.
    void add_transfer(const std::string& workspace_id,
                      std::uint64_t in,
                      std::uint64_t out,
                      std::chrono::system_clock::time_point at = std::chrono::system_clock::now());

    // Adds every increment, or none of them when the bound leaves no room for all. Never does I/O.
    void add_together(const std::string& workspace_id,
                      std::vector<Increment> increments,
                      std::chrono::system_clock::time_point at = std::chrono::system_clock::now());

    // A reading of the workspace's stored bytes of `kind`. It replaces an older reading of the
    // same hour, so the hour keeps its latest. Never does I/O.
    void sample(const std::string& workspace_id,
                std::string_view kind,
                std::uint64_t bytes,
                std::chrono::system_clock::time_point at = std::chrono::system_clock::now());

    std::size_t pending() const;

    // Writes everything held. Safe to call from any thread; a flush already running makes this
    // one return at once.
    liquid::runtime::task<void> flush();

    Totals totals() const;

private:
    // workspace, metric, hour start (ms), labels (a JSON object's text: its keys sorted)
    using Key = std::tuple<std::string, std::string, std::int64_t, std::string>;
    struct Held {
        std::uint64_t value = 0;
        std::int64_t at_ms = 0; // a reading's own time; an increment's hour
    };
    static bool gauge(const Key& key);
    void merge_locked(const Key& key, const Held& held);

    control::Store* store_;
    control::WorkspaceService* workspaces_;
    std::size_t max_pending_;
    mutable std::mutex mutex_;
    std::map<Key, Held> held_;
    std::set<std::string> created_; // "<workspace>/<metric>" this process created
    Totals totals_;
    std::atomic<bool> flushing_{false};
};

// The dimensions `lake_usage` and `lake_storage` carry, and every other dimension usage_over_time
// answers (docs/api/metering.md#dimensions). `unit` is what a value counts. Billing's price list
// (docs/api/billing.md) keys a rate by `name` and multiplies `unit`; nothing here prices.
struct UsageDimension {
    std::string_view name;
    // bytes, seconds, calls, tokens, instance_seconds, gib_seconds, mcpu_seconds (thousandths of a
    // vCPU for a second), mib_seconds (a MiB for a second) or gpu_seconds
    std::string_view unit;
    std::string_view aggregation; // sum (a counter over the period) or last (a reading)
    std::string_view title;
    // Whether the price list may rate it. Build, run and instance time are measured but no longer
    // priced: compute is priced as CPU, RAM, GPU and VRAM (lake#643). A list published before
    // keeps its rates for them.
    bool priced = true;
    // Whether its usage carries a `class` label (a GPU class) and the price list may rate a class.
    bool classes = false;
};
const std::vector<UsageDimension>& usage_dimensions();
const UsageDimension* find_usage_dimension(std::string_view name);

// What is metered where (lake#658, docs/api/metering.md#what-is-metered-where). Every capability
// call routed to a workspace counts in `calls`; a capability that also consumes a resource for a
// workspace names the dimensions its use adds to here. `UsageContract.*` fails when a registered
// capability that looks like it consumes a resource is in neither this table nor
// `calls_only_capabilities()`, or when a dimension here is unknown or unpriced.
struct CapabilityMeter {
    std::string_view capability;
    std::vector<std::string_view> dimensions;
    std::string_view how;
};
const std::vector<CapabilityMeter>& capability_meters();
// Capabilities whose names look like they consume a resource but whose use is only a call (they
// change records, not resources), each with why.
const std::vector<std::pair<std::string_view, std::string_view>>& calls_only_capabilities();
// The workload_resources kinds the owner loop's sample charges: every kind the schema allows.
const std::vector<std::string_view>& metered_workload_kinds();
// The store types whose stores are metered (as `volumes`): a provider may be offered only for
// these.
const std::vector<std::string_view>& metered_store_types();

} // namespace lake::api
