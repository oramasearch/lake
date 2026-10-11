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

#include <liquid/runtime.hpp>

#include "lake/control/store.hpp"

namespace lake::control {
class WorkspaceService;
}

namespace lake::api {

// The per-workspace request series (decision 54 S9, lake#384, docs/serving/telemetry.md#the-
// workspace-request-series). Lake's process telemetry stays with the instance; each workspace
// Zetta also gets a counter `lake_requests` in its labelled time series (zetta docs/data/
// time-series.md), labelled `route` (the route pattern), `capability` (when the request is a
// capability call) and `status_class` (`2xx`, `4xx`, `5xx`), one bucket a minute, kept 30 days.
//
// The per-model prediction series (lake#599, docs/serving/telemetry.md#the-per-model-prediction-
// series) rides on the same recorder: each `predict` call adds one to the counter
// `lake_model_calls` and its duration in milliseconds to the counter `lake_model_call_ms`, both
// labelled `model_id` and `status_class`, so Ship → Models reads a model's calls, errors and mean
// latency from its own workspace.
//
// A request only adds to an in-memory count for (workspace, metric, minute, labels); nothing here
// does I/O on the request path. `flush`, run by the server's owner loop every ten seconds and once
// more when the server drains, creates each metric once per workspace (`create_metric`,
// idempotent) and writes the counts with `record_metric`, at most 1000 points per call, through
// the workspace's own connection (call_workspace), the same path its records take.
//
// Only requests that lake routed to a workspace are counted (the HTTP server decides; see
// RequestAttribution). Never a default workspace, never the lake Zetta, never PostgreSQL.
//
// Bounded: at most `max_pending` counts are held, across every metric. An increment of a count
// already held always merges; a new one beyond the bound is dropped and counted (`dropped_bound`),
// never blocked on. A failed write is kept for the next flush unless its workspace is gone
// (deleted or unknown), in which case it is dropped and counted (`dropped_workspace_deleted`), or
// the workspace refused it for good (`dropped_refused`: a conflicting definition or an invalid
// point). The dropped totals count requests and calls; a dropped duration is not counted again.
// The series is approximate by design: a crash loses the last interval, and a write whose answer
// was lost is retried and may count twice. Exact usage is metering's, in records
// (docs/api/metering.md).
class RequestRecorder {
public:
    static constexpr std::string_view metric = "lake_requests";
    static constexpr std::string_view model_calls_metric = "lake_model_calls";
    static constexpr std::string_view model_call_ms_metric = "lake_model_call_ms";
    static constexpr std::int64_t resolution_ms = 60'000;
    static constexpr std::int64_t retention_ms = 30LL * 24 * 60 * 60 * 1000;
    static constexpr std::size_t max_points_per_call = 1000;

    struct Totals {
        std::uint64_t recorded = 0;     // requests written to a workspace series
        std::uint64_t model_calls = 0;  // predict calls written to a per-model series
        std::uint64_t points = 0;       // points written, every metric
        std::uint64_t flush_errors = 0; // failed calls (their counts kept or dropped)
        std::uint64_t dropped_bound = 0;             // requests and calls dropped at the bound
        std::uint64_t dropped_workspace_deleted = 0; // of a deleted or unknown workspace
        std::uint64_t dropped_refused = 0;           // the workspace refused for good (a different
                                                     // definition, bad input)
    };

    RequestRecorder(control::Store* store,
                    control::WorkspaceService* workspaces,
                    std::size_t max_pending = 16384);

    // Counts one request routed to `workspace_id`. `capability` may be empty. Never does I/O.
    void add(const std::string& workspace_id,
             const std::string& route,
             const std::string& capability,
             int status,
             std::chrono::system_clock::time_point at = std::chrono::system_clock::now());

    // Counts one `predict` call of `model_id` in `workspace_id` that answered `status` after
    // `duration_ms` (lake#599). Never does I/O.
    void
    add_model_call(const std::string& workspace_id,
                   const std::string& model_id,
                   int status,
                   double duration_ms,
                   std::chrono::system_clock::time_point at = std::chrono::system_clock::now());

    // The label for an HTTP status: `1xx` … `5xx`, or `other`.
    static std::string status_class(int status);

    std::size_t pending() const;

    // Writes everything held. Safe to call from any thread; a flush already running makes this
    // one return at once.
    liquid::runtime::task<void> flush();

    Totals totals() const;

private:
    // workspace, metric, minute start (ms), labels (a JSON object's text: its keys sorted)
    using Key = std::tuple<std::string, std::string, std::int64_t, std::string>;
    void add_locked(Key key, std::uint64_t value);
    void requeue_locked(const Key& key, std::uint64_t count);

    control::Store* store_;
    control::WorkspaceService* workspaces_;
    std::size_t max_pending_;
    mutable std::mutex mutex_;
    std::map<Key, std::uint64_t> counts_;
    std::set<std::string> created_; // "<workspace>/<metric>" this process created
    Totals totals_;
    std::atomic<bool> flushing_{false};
};

} // namespace lake::api
