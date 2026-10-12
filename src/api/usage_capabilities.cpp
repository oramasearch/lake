// Usage over time (lake#564, docs/api/metering.md#usage-over-time): usage_over_time, usage_limits,
// set_usage_limit, record_model_usage, model_completion (the model gateway, lake#248), and the
// owner loop's usage sample. Every workspace's usage is read from its own workspace Zetta; lake's
// control store holds only the limits.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/images.hpp"
#include "lake/api/model_gateway.hpp"
#include "lake/api/sync_store.hpp"
#include "lake/api/usage_recorder.hpp"
#include "lake/control/agent_bus.hpp"
#include "lake/control/builds.hpp"
#include "lake/control/drive_files.hpp"
#include "lake/control/drives.hpp"
#include "lake/control/metering.hpp"
#include "lake/control/store.hpp"
#include "lake/control/transcripts.hpp"
#include "lake/control/web_apps.hpp"
#include "lake/control/workspace_store.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

using nlohmann::json;
using Labels = std::map<std::string, std::string>;

constexpr std::int64_t hour_ms = 3'600'000;
constexpr std::int64_t day_ms = 24 * hour_ms;

// ------------------------------------------------------------------------------------------------
// Time: UTC instants as epoch milliseconds, and the periods a reading is cut into.

// Milliseconds since the Unix epoch for "YYYY-MM-DDTHH:MM:SS[.fff][Z]" (a space may replace the
// T), or a bare "YYYY-MM-DD" (its midnight). Everything lake and Zetta write is UTC.
std::optional<std::int64_t> utc_ms(std::string_view text) {
    if (text.size() == 10) {
        return utc_ms(std::string(text) + "T00:00:00Z");
    }
    if (text.size() < 19 || text[4] != '-' || text[7] != '-' ||
        (text[10] != 'T' && text[10] != ' ') || text[13] != ':' || text[16] != ':') {
        return std::nullopt;
    }
    const auto number = [&](std::size_t offset, std::size_t digits) -> std::optional<int> {
        int value = 0;
        for (std::size_t index = offset; index < offset + digits; ++index) {
            if (text[index] < '0' || text[index] > '9') {
                return std::nullopt;
            }
            value = value * 10 + (text[index] - '0');
        }
        return value;
    };
    const auto year = number(0, 4);
    const auto month = number(5, 2);
    const auto day = number(8, 2);
    const auto hour = number(11, 2);
    const auto minute = number(14, 2);
    const auto second = number(17, 2);
    if (!year || !month || !day || !hour || !minute || !second || *hour > 23 || *minute > 59 ||
        *second > 60) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{*year},
                                           std::chrono::month{static_cast<unsigned>(*month)},
                                           std::chrono::day{static_cast<unsigned>(*day)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    const auto days = std::chrono::sys_days{date}.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(days).count() +
           ((*hour * 60 + *minute) * 60 + *second) * std::int64_t{1000};
}

std::string utc_text(std::int64_t ms) {
    const std::chrono::sys_time<std::chrono::milliseconds> at{std::chrono::milliseconds(ms)};
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", std::chrono::floor<std::chrono::seconds>(at));
}

enum class Granularity { hour, day, month };

std::optional<Granularity> granularity_of(std::string_view name) {
    if (name == "hour") {
        return Granularity::hour;
    }
    if (name == "day") {
        return Granularity::day;
    }
    if (name == "month") {
        return Granularity::month;
    }
    return std::nullopt;
}

std::int64_t floor_to(std::int64_t ms, std::int64_t unit) {
    const std::int64_t offset = ms % unit;
    return ms - (offset < 0 ? offset + unit : offset);
}

std::chrono::year_month_day date_of(std::int64_t ms) {
    return std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(
        std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds(ms)})};
}

std::int64_t ms_of(std::chrono::year_month_day date) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::sys_days{date}.time_since_epoch())
        .count();
}

// The start of the period holding `ms`.
std::int64_t period_floor(std::int64_t ms, Granularity granularity) {
    switch (granularity) {
    case Granularity::hour:
        return floor_to(ms, hour_ms);
    case Granularity::day:
        return floor_to(ms, day_ms);
    case Granularity::month: {
        const auto date = date_of(ms);
        return ms_of(date.year() / date.month() / std::chrono::day{1});
    }
    }
    return ms;
}

std::int64_t period_after(std::int64_t start, Granularity granularity) {
    switch (granularity) {
    case Granularity::hour:
        return start + hour_ms;
    case Granularity::day:
        return start + day_ms;
    case Granularity::month: {
        const auto date = date_of(start);
        const auto next = date.year() / date.month() / std::chrono::day{1} + std::chrono::months{1};
        return ms_of(std::chrono::year_month_day{next});
    }
    }
    return start;
}

// At most this many periods in one reading: 100 days of hours, three years of days or months.
std::size_t max_periods(Granularity granularity) {
    return granularity == Granularity::hour ? 2400 : granularity == Granularity::day ? 1100 : 36;
}

// The periods of a reading: [starts[0], end), each from its start to the next one's.
struct Periods {
    Granularity granularity = Granularity::day;
    std::vector<std::int64_t> starts;
    std::int64_t end = 0;

    // The index of the period holding `ms`, or -1 outside them.
    int index(std::int64_t ms) const {
        if (starts.empty() || ms < starts.front() || ms >= end) {
            return -1;
        }
        const auto after = std::ranges::upper_bound(starts, ms);
        return static_cast<int>(after - starts.begin()) - 1;
    }
    std::size_t size() const {
        return starts.size();
    }
    // The step a workspace is asked for: an hour for hours, a day for days and months (months
    // are added up from days, since a month has no fixed length).
    std::int64_t step_ms() const {
        return granularity == Granularity::hour ? hour_ms : day_ms;
    }
};

std::expected<Periods, ApiResult>
periods_of(std::int64_t from, std::int64_t to, Granularity granularity) {
    Periods periods;
    periods.granularity = granularity;
    std::int64_t start = period_floor(from, granularity);
    while (start < to) {
        if (periods.starts.size() >= max_periods(granularity)) {
            return std::unexpected(ApiResult::failure(
                "invalid_input",
                std::format("at most {} periods of this granularity in one reading",
                            max_periods(granularity)),
                "/to"));
        }
        periods.starts.push_back(start);
        start = period_after(start, granularity);
    }
    periods.end = start;
    return periods;
}

// ------------------------------------------------------------------------------------------------
// Series: one line of values per period, summed (a counter) or the latest reading (a gauge).

struct Line {
    std::vector<std::optional<double>> values;
    std::vector<std::int64_t> at; // a gauge's reading time per period
};

struct RawSeries {
    std::string dimension;
    std::string workspace_id;
    Labels labels;
    bool gauge = false;
    Line line;
};

class Accumulator {
public:
    explicit Accumulator(const Periods& periods) : periods_(periods) {}

    void add(const std::string& dimension,
             const std::string& workspace_id,
             Labels labels,
             std::int64_t at_ms,
             double value) {
        const int index = periods_.index(at_ms);
        if (index < 0) {
            return;
        }
        const UsageDimension* known = find_usage_dimension(dimension);
        if (!known) {
            return;
        }
        labels.erase("dimension");
        const std::string key = dimension + "\n" + workspace_id + "\n" + json(labels).dump();
        auto found = series_.find(key);
        if (found == series_.end()) {
            RawSeries series{
                dimension, workspace_id, std::move(labels), known->aggregation == "last", {}};
            series.line.values.assign(periods_.size(), std::nullopt);
            series.line.at.assign(periods_.size(), std::numeric_limits<std::int64_t>::min());
            found = series_.emplace(key, std::move(series)).first;
        }
        RawSeries& series = found->second;
        auto& slot = series.line.values[static_cast<std::size_t>(index)];
        if (series.gauge) {
            auto& at = series.line.at[static_cast<std::size_t>(index)];
            if (!slot || at_ms >= at) {
                slot = value;
                at = at_ms;
            }
        } else {
            slot = slot.value_or(0) + value;
        }
    }

    const std::map<std::string, RawSeries>& series() const {
        return series_;
    }

private:
    const Periods& periods_;
    std::map<std::string, RawSeries> series_;
};

// Adds `line` into `into`, period by period: a period either has is the sum of what both have.
void merge_into(std::vector<std::optional<double>>& into, const Line& line) {
    if (into.size() < line.values.size()) {
        into.resize(line.values.size());
    }
    for (std::size_t index = 0; index < line.values.size(); ++index) {
        if (line.values[index]) {
            into[index] = into[index].value_or(0) + *line.values[index];
        }
    }
}

json points_json(const std::vector<std::optional<double>>& values, bool gauge) {
    json points = json::array();
    for (const auto& value : values) {
        points.push_back(value ? json(*value) : gauge ? json(nullptr) : json(0));
    }
    return points;
}

// A counter's total over every period; a reading's latest value.
double total_of(const std::vector<std::optional<double>>& values, bool gauge) {
    if (gauge) {
        for (auto it = values.rbegin(); it != values.rend(); ++it) {
            if (*it) {
                return **it;
            }
        }
        return 0;
    }
    double total = 0;
    for (const auto& value : values) {
        total += value.value_or(0);
    }
    return total;
}

// One breakdown of a dimension, e.g. by workspace: {key, total, points}, largest first.
json breakdown(const std::vector<const RawSeries*>& series,
               bool gauge,
               std::size_t width,
               const std::function<std::string(const RawSeries&)>& key_of,
               const std::function<json(const RawSeries&)>& describe = {}) {
    std::map<std::string, std::vector<std::optional<double>>> lines;
    std::map<std::string, json> described;
    for (const RawSeries* one : series) {
        const std::string key = key_of(*one);
        if (key.empty()) {
            continue;
        }
        auto& line = lines[key];
        line.resize(width);
        merge_into(line, one->line);
        if (describe && !described.contains(key)) {
            described[key] = describe(*one);
        }
    }
    std::vector<json> rows;
    for (const auto& [key, line] : lines) {
        json row = described.contains(key) ? described[key] : json::object();
        row["key"] = key;
        row["total"] = total_of(line, gauge);
        row["points"] = points_json(line, gauge);
        rows.push_back(std::move(row));
    }
    std::ranges::stable_sort(rows, [](const json& left, const json& right) {
        return left.at("total").get<double>() > right.at("total").get<double>();
    });
    return json(std::move(rows));
}

std::string label(const RawSeries& series, const std::string& name) {
    const auto found = series.labels.find(name);
    return found == series.labels.end() ? std::string() : found->second;
}

// ------------------------------------------------------------------------------------------------
// Reading each workspace, concurrently.

struct WorkspaceRead {
    liquid::result<control::ZettaControlResult<json>> value;
};

boost::capy::io_task<WorkspaceRead>
complete_read(liquid::runtime::task<control::ZettaControlResult<json>> task) {
    auto result = co_await std::move(task);
    co_return boost::capy::io_result<WorkspaceRead>{{}, {std::move(result)}};
}

liquid::runtime::task<control::ZettaControlResult<json>> refused(control::ZettaControlError error) {
    co_return control::ZettaControlResult<json>(std::unexpected(std::move(error)));
}

liquid::runtime::task<control::ZettaControlResult<json>> nothing() {
    co_return control::ZettaControlResult<json>(json(nullptr));
}

// A workspace that never recorded a metric has none: an empty answer, not a failure.
liquid::runtime::task<control::ZettaControlResult<json>>
query_series(ApiContext& context, std::string customer_id, json input) {
    auto answered = control::settled(
        co_await call_workspace(context, std::move(customer_id), "query_metric", true, input));
    if (!answered && answered.error().code == "not_found") {
        co_return control::ZettaControlResult<json>(json{{"series", json::array()}});
    }
    if (!answered) {
        co_return control::ZettaControlResult<json>(std::unexpected(answered.error()));
    }
    co_return control::ZettaControlResult<json>(std::move(*answered));
}

liquid::runtime::task<control::ZettaControlResult<json>>
transfer_rows(std::shared_ptr<control::WorkspaceRecords> records,
              std::string customer_id,
              std::int64_t since_us) {
    auto listed = control::settled(
        co_await control::list_metered_transfer(*records, std::move(customer_id), since_us));
    if (!listed) {
        co_return control::ZettaControlResult<json>(std::unexpected(listed.error()));
    }
    json rows = json::array();
    for (const control::MeteredTransfer& transfer : *listed) {
        rows.push_back({{"hour", transfer.hour},
                        {"in", transfer.transfer_in_bytes},
                        {"out", transfer.transfer_out_bytes}});
    }
    co_return control::ZettaControlResult<json>(std::move(rows));
}

liquid::runtime::task<control::ZettaControlResult<json>>
build_rows(std::shared_ptr<control::WorkspaceRecords> records,
           std::string customer_id,
           std::int64_t since_us) {
    auto listed = control::settled(
        co_await control::list_metered_builds(*records, std::move(customer_id), since_us));
    if (!listed) {
        co_return control::ZettaControlResult<json>(std::unexpected(listed.error()));
    }
    json rows = json::array();
    for (const control::MeteredBuild& build : *listed) {
        rows.push_back({{"result_id", build.result_id},
                        {"hour", build.hour},
                        {"seconds", build.compute_seconds}});
    }
    co_return control::ZettaControlResult<json>(std::move(rows));
}

std::expected<json, std::string> answer_of(WorkspaceRead& read) {
    if (!read.value) {
        return std::unexpected(read.value.error().message);
    }
    if (!*read.value) {
        return std::unexpected(read.value->error().message);
    }
    return std::move(**read.value);
}

// Whole seconds from `started` to `ended`, rounded up as metering rounds a build.
std::optional<std::uint64_t> elapsed_seconds(std::string_view started, std::string_view ended) {
    const auto begin = utc_ms(started);
    const auto end = utc_ms(ended);
    if (!begin || !end || *end < *begin) {
        return std::nullopt;
    }
    const auto milliseconds = static_cast<std::uint64_t>(*end - *begin);
    return milliseconds / 1000 + (milliseconds % 1000 == 0 ? 0 : 1);
}

// ------------------------------------------------------------------------------------------------
// Scheduled resources (lake#643, docs/api/metering.md#resources): what a workload is scheduled
// with, as the Orama operator reports it in workload_resources, times how long it ran.

using ResourceKey = std::pair<std::string, std::string>; // object kind, object id
using ResourceRows = std::map<ResourceKey, control::WorkloadResources>;

constexpr std::uint64_t mib = 1024 * 1024;

bool resource_dimension(std::string_view name) {
    return name == "cpu" || name == "ram" || name == "gpu" || name == "vram";
}

std::uint64_t capped(unsigned __int128 value) {
    return value > std::numeric_limits<std::uint64_t>::max()
               ? std::numeric_limits<std::uint64_t>::max()
               : static_cast<std::uint64_t>(value);
}

// `amount` units held for `seconds`, in units of `unit` (rounded up): millicores times seconds
// are mCPU-seconds; bytes times seconds over a MiB are MiB-seconds.
std::uint64_t held_for(std::uint64_t amount, std::uint64_t seconds, std::uint64_t unit = 1) {
    const unsigned __int128 product =
        static_cast<unsigned __int128>(amount) * static_cast<unsigned __int128>(seconds);
    return capped((product + unit - 1) / unit);
}

// What `row` used over `seconds`, one increment per resource dimension; GPU and GPU memory carry
// the GPU class when the operator knows it.
std::vector<UsageRecorder::Increment>
resource_increments(const control::WorkloadResources& row, std::uint64_t seconds, Labels labels) {
    std::vector<UsageRecorder::Increment> out;
    if (seconds == 0) {
        return out;
    }
    out.push_back({"cpu", held_for(row.cpu_millis, seconds), labels});
    out.push_back({"ram", held_for(row.memory_bytes, seconds, mib), labels});
    if (row.gpus > 0) {
        Labels gpu = labels;
        if (!row.gpu_class.empty()) {
            gpu["class"] = row.gpu_class;
        }
        out.push_back({"gpu", held_for(static_cast<std::uint64_t>(row.gpus), seconds), gpu});
        out.push_back({"vram", held_for(row.vram_bytes, seconds, mib), std::move(gpu)});
    }
    return out;
}

std::expected<ResourceRows, std::string> resource_rows(control::Store& store) {
    const auto rows = store.workload_resources();
    if (!rows) {
        return std::unexpected(rows.error().message);
    }
    ResourceRows out;
    for (const control::WorkloadResources& row : *rows) {
        out.emplace(ResourceKey{row.object_kind, row.object_id}, row);
    }
    return out;
}

const control::WorkloadResources*
resources_of(const ResourceRows& rows, std::string_view kind, const std::string& id) {
    const auto found = rows.find(ResourceKey{std::string(kind), id});
    return found == rows.end() ? nullptr : &found->second;
}

bool finished(std::string_view phase) {
    return phase == "Succeeded" || phase == "Failed" || phase == "Cancelled";
}

// The kinds of Zetta's storage_usage lake records, and the `lake_storage` kind each becomes
// (lake#658). Datasets keep their name from before, `workspace_data`.
const std::vector<std::pair<std::string_view, std::string_view>>& engine_storage_kinds() {
    static const std::vector<std::pair<std::string_view, std::string_view>> kinds{
        {"datasets", "workspace_data"},
        {"models", "models"},
        {"indexes", "indexes"},
        {"runs", "runs"},
        {"streams", "streams"},
        {"files", "files"},
        {"buckets", "buckets"},
    };
    return kinds;
}

// Whether a `lake_storage` kind is held on a workspace engine's own volume: billed as the
// volume, so shown as engine_storage and never as storage (decision 61, lake#663).
bool engine_held(std::string_view kind) {
    for (const auto& [engine, name] : engine_storage_kinds()) {
        if (name == kind) {
            return true;
        }
    }
    return false;
}

// A workload charged once for how long it ran (lake#658): an image build, a job-mode agent launch,
// an agent cron run or a workspace methodology run (lake#655) that has finished. It ran from its request (a cron run: its start) to its
// final status, so time queued is included for builds and launches, as for build time.
struct FinishedJob {
    std::string row_kind;   // its workload_resources kind: build, agent_launch, agent_cron_run or
                            // methodology_run
    std::string usage_kind; // its kind in the usage series: build, agent or methodology
    std::string id, workspace_id, started, ended;
};

struct FinishedJobs {
    std::vector<FinishedJob> jobs;
    std::vector<std::string> problems;
};

// The account's finished jobs in `workspaces`.
FinishedJobs finished_jobs(control::Store& store,
                           const std::string& customer_id,
                           const std::set<std::string>& workspaces) {
    FinishedJobs out;
    if (const auto builds = store.list_builds(customer_id)) {
        for (const control::Build& build : *builds) {
            if (workspaces.contains(build.workspace_id) && finished(build.phase)) {
                out.jobs.push_back({"build",
                                    "build",
                                    build.id,
                                    build.workspace_id,
                                    build.created_at,
                                    build.updated_at});
            }
        }
    } else {
        out.problems.push_back("Builds: " + builds.error().message);
    }
    if (const auto launches = store.list_agent_launches(customer_id, "", 10000)) {
        for (const control::AgentLaunch& launch : *launches) {
            if (workspaces.contains(launch.workspace_id) && launch.mode == "job" &&
                finished(launch.phase) && !launch.status_updated_at.empty()) {
                out.jobs.push_back({"agent_launch",
                                    "agent",
                                    launch.id,
                                    launch.workspace_id,
                                    launch.created_at,
                                    launch.status_updated_at});
            }
        }
    } else {
        out.problems.push_back("Agent launches: " + launches.error().message);
    }
    if (const auto runs = store.list_agent_cron_runs(customer_id, "", 10000)) {
        for (const control::AgentLaunch& run : *runs) {
            if (workspaces.contains(run.workspace_id) && finished(run.phase) &&
                !run.status_updated_at.empty()) {
                out.jobs.push_back({"agent_cron_run",
                                    "agent",
                                    run.id,
                                    run.workspace_id,
                                    run.created_at,
                                    run.status_updated_at});
            }
        }
    } else {
        out.problems.push_back("Agent cron runs: " + runs.error().message);
    }
    // A workspace methodology's run (lake#655) ran from its sandbox pod's start to its finish, as
    // the operator reports them; the stage and collect pods around it are not charged.
    for (const std::string& workspace_id : workspaces) {
        const auto methodology_runs = store.list_methodology_runs(customer_id, workspace_id, "", 1000);
        if (!methodology_runs) {
            out.problems.push_back("Methodology runs: " + methodology_runs.error().message);
            continue;
        }
        for (const control::MethodologyRun& run : *methodology_runs) {
            if (!run.started_at.empty() && !run.finished_at.empty()) {
                out.jobs.push_back({"methodology_run",
                                    "methodology",
                                    run.id,
                                    run.workspace_id,
                                    run.started_at,
                                    run.finished_at});
            }
        }
    }
    return out;
}

// The usage series keeps 400 days; a job that ended before them is not charged.
constexpr std::int64_t series_retention_ms = 400 * day_ms;
// At most this many jobs are charged per workspace in one sample, so a first sample after a long
// gap spreads its writes over the following ones.
constexpr std::size_t jobs_per_sample = 500;

std::chrono::system_clock::time_point time_of(std::int64_t ms) {
    return std::chrono::system_clock::time_point{std::chrono::milliseconds(ms)};
}

// Charges every finished job that has its resources reported and was not charged before: its
// CPU, RAM, GPU and GPU memory for how long it ran, in the hour it ended, after recording
// `if_absent` in its workspace that it was charged (lake_metering_job_records). A job whose
// record was written but whose usage was lost (a crash before the flush) is not charged again:
// at most once, never twice.
liquid::runtime::task<void> charge_finished_jobs(ApiContext& context,
                                                 const std::string& customer_id,
                                                 const std::vector<FinishedJob>& jobs,
                                                 const ResourceRows& resources,
                                                 std::chrono::system_clock::time_point at,
                                                 UsageSampleReport& report) {
    const std::int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
    const std::int64_t horizon_ms = now_ms - series_retention_ms;
    struct Candidate {
        const FinishedJob* job;
        const control::WorkloadResources* row;
        std::int64_t ended_ms;
        std::uint64_t seconds;
    };
    std::map<std::string, std::vector<Candidate>> by_workspace;
    for (const FinishedJob& job : jobs) {
        const auto* row = resources_of(resources, job.row_kind, job.id);
        const auto ended = utc_ms(job.ended);
        const auto ran = elapsed_seconds(job.started, job.ended);
        if (row == nullptr || !ended || !ran || *ended < horizon_ms || *ended > now_ms) {
            continue;
        }
        by_workspace[job.workspace_id].push_back({&job, row, *ended, *ran});
    }
    for (auto& [workspace_id, candidates] : by_workspace) {
        auto records = workspace_records(context, customer_id, workspace_id);
        if (!records) {
            report.unmeasured.push_back("Finished jobs of workspace " + workspace_id + ": " +
                                        records.error().message);
            continue;
        }
        auto charged = control::settled(
            co_await control::list_metered_jobs(*records, customer_id, horizon_ms * 1000));
        if (!charged) {
            report.unmeasured.push_back("Finished jobs of workspace " + workspace_id + ": " +
                                        charged.error().message);
            continue;
        }
        std::set<std::string> seen;
        for (const control::MeteredJob& job : *charged) {
            seen.insert(job.kind + ":" + job.object_id);
        }
        std::ranges::stable_sort(candidates, {}, &Candidate::ended_ms);
        std::size_t written = 0;
        for (const Candidate& candidate : candidates) {
            const FinishedJob& job = *candidate.job;
            if (seen.contains(job.row_kind + ":" + job.id)) {
                continue;
            }
            if (written == jobs_per_sample) {
                break;
            }
            ++written;
            auto recorded = control::settled(co_await control::record_metered_job(
                *records,
                {customer_id,
                 job.row_kind,
                 job.id,
                 utc_text(floor_to(candidate.ended_ms, hour_ms)),
                 candidate.seconds}));
            if (!recorded) {
                report.unmeasured.push_back("Job " + job.id + ": " + recorded.error().message);
                continue;
            }
            if (!*recorded) {
                continue; // another lake charged it first
            }
            auto increments =
                resource_increments(*candidate.row,
                                    candidate.seconds,
                                    {{"kind", job.usage_kind}, {"object_id", job.id}});
            if (increments.empty()) {
                continue;
            }
            report.increments += increments.size();
            context.usage_recorder->add_together(
                workspace_id, std::move(increments), time_of(candidate.ended_ms));
        }
    }
    co_return {};
}

// The running workloads of the chosen workspaces the operator has not reported the resources of,
// by name, and the finished builds, agent jobs and cron runs of [from, to) without them: their
// CPU, RAM, GPU and VRAM are not counted (usage_over_time's not_measured).
std::vector<std::string> unreported_workloads(control::Store& store,
                                              const std::string& customer_id,
                                              const std::vector<control::Workspace>& workspaces,
                                              const ResourceRows& rows,
                                              std::int64_t from_ms,
                                              std::int64_t to_ms) {
    std::set<std::string> chosen, engines;
    std::vector<std::string> names;
    for (const control::Workspace& workspace : workspaces) {
        chosen.insert(workspace.id);
        if (!workspace.zetta_service_id.empty()) {
            engines.insert(workspace.zetta_service_id);
        }
        if (workspace.desired == "running" && !resources_of(rows, "workspace", workspace.id) &&
            !resources_of(rows, "service", workspace.zetta_service_id)) {
            names.push_back("the engine of workspace " + workspace.name);
        }
    }
    if (const auto services = store.list_services(customer_id)) {
        for (const control::ManagedService& service : *services) {
            if (chosen.contains(service.workspace_id) && !engines.contains(service.id) &&
                service.desired == "present" && !resources_of(rows, "service", service.id)) {
                names.push_back("service " + service.name);
            }
        }
    }
    if (const auto apps = store.list_web_apps(customer_id)) {
        for (const control::WebApp& app : *apps) {
            if (chosen.contains(app.workspace_id) && app.desired == "running" &&
                !resources_of(rows, "app", app.id)) {
                names.push_back("app " + app.name);
            }
        }
    }
    if (const auto launches = store.list_agent_launches(customer_id, "", 10000)) {
        for (const control::AgentLaunch& launch : *launches) {
            if (chosen.contains(launch.workspace_id) && launch.mode == "service" &&
                launch.desired == "present" && !resources_of(rows, "agent_launch", launch.id)) {
                names.push_back("agent " +
                                (launch.agent_name.empty() ? launch.agent_id : launch.agent_name));
            }
        }
    }
    std::size_t jobs = 0;
    for (const FinishedJob& job : finished_jobs(store, customer_id, chosen).jobs) {
        const auto ended = utc_ms(job.ended);
        if (ended && *ended >= from_ms && *ended < to_ms &&
            !resources_of(rows, job.row_kind, job.id)) {
            ++jobs;
        }
    }
    if (jobs > 0) {
        names.push_back(std::format("{} finished build{}, agent job{} and cron run{}",
                                    jobs,
                                    jobs == 1 ? "" : "s",
                                    jobs == 1 ? "" : "s",
                                    jobs == 1 ? "" : "s"));
    }
    return names;
}

// What usage_over_time needs about a request: the account, its chosen live workspaces, the
// dimensions and the periods.
struct UsageQuery {
    std::string customer_id;
    std::vector<control::Workspace> workspaces;
    std::set<std::string> dimensions;
    Periods periods;
};

// The reading itself, shared by usage_over_time and usage_limits.
struct UsageReading {
    json dimensions = json::array();
    json workspaces = json::array();
    json not_measured = json::array();
    std::map<std::string, double> totals; // dimension -> total
};

liquid::runtime::task<UsageReading> read_usage(ApiContext& context, const UsageQuery& query) {
    UsageReading reading;
    Accumulator accumulator(query.periods);
    const Periods& periods = query.periods;
    const auto wants = [&](std::string_view dimension) {
        return query.dimensions.contains(std::string(dimension));
    };
    const bool wants_resources = wants("cpu") || wants("ram") || wants("gpu") || wants("vram");
    const bool wants_series = wants("calls") || wants("predictions") || wants("searches") ||
                              wants("speech") || wants("model_tokens") || wants("instances") ||
                              wants("volumes") || wants("transfer") || wants_resources;
    const std::int64_t from_ms = periods.starts.empty() ? 0 : periods.starts.front();
    const std::int64_t to_ms = periods.end;
    const std::int64_t since_us = from_ms * 1000;

    // Five reads a workspace, all at once: the counter, the readings, runs, transfer, builds.
    constexpr std::size_t reads = 5;
    std::deque<ApiContext> contexts;
    std::vector<boost::capy::io_task<WorkspaceRead>> pending;
    pending.reserve(query.workspaces.size() * reads);
    for (const control::Workspace& workspace : query.workspaces) {
        contexts.push_back(context);
        ApiContext& bound = contexts.back();
        bound.workspace_id = workspace.id;
        const json window{{"from_ms", from_ms}, {"to_ms", to_ms}, {"step_ms", periods.step_ms()}};
        if (wants_series) {
            json input = window;
            input["metric"] = std::string(UsageRecorder::usage_metric);
            input["group_by"] = json::array(
                {"dimension", "kind", "agent", "launch_id", "model", "object_id", "class"});
            pending.push_back(complete_read(query_series(bound, query.customer_id, input)));
        } else {
            pending.push_back(complete_read(nothing()));
        }
        if (wants("storage") || wants("engine_storage")) {
            json input = window;
            input["metric"] = std::string(UsageRecorder::storage_metric);
            input["group_by"] = json::array({"kind"});
            input["aggregate"] = "last";
            pending.push_back(complete_read(query_series(bound, query.customer_id, input)));
        } else {
            pending.push_back(complete_read(nothing()));
        }
        if (wants("run_compute")) {
            pending.push_back(complete_read(
                call_workspace(bound, query.customer_id, "list_runs", true, json::object())));
        } else {
            pending.push_back(complete_read(nothing()));
        }
        if (wants("transfer") || wants("build_compute")) {
            auto opened = workspace_records(context, query.customer_id, workspace.id);
            if (!opened) {
                pending.push_back(complete_read(refused(opened.error())));
                pending.push_back(complete_read(refused(opened.error())));
            } else {
                auto records = std::make_shared<control::WorkspaceRecords>(std::move(*opened));
                pending.push_back(complete_read(
                    wants("transfer") ? transfer_rows(records, query.customer_id, since_us)
                                      : nothing()));
                pending.push_back(complete_read(
                    wants("build_compute") ? build_rows(records, query.customer_id, since_us)
                                           : nothing()));
            }
        } else {
            pending.push_back(complete_read(nothing()));
            pending.push_back(complete_read(nothing()));
        }
    }
    std::vector<WorkspaceRead> answers;
    std::error_code wait_error;
    if (!pending.empty()) {
        auto [error, done] = co_await liquid::runtime::when_all(std::move(pending));
        wait_error = error;
        answers = std::move(done);
    }
    std::map<std::string, std::string> names;
    for (std::size_t index = 0; index < query.workspaces.size(); ++index) {
        const control::Workspace& workspace = query.workspaces[index];
        names[workspace.id] = workspace.name;
        std::vector<std::string> problems;
        if (wait_error) {
            problems.push_back(wait_error.message());
        } else {
            WorkspaceRead* read = &answers[index * reads];
            const auto fold_series =
                [&](WorkspaceRead& one, std::string_view what, const std::string& fixed_dimension) {
                    auto answer = answer_of(one);
                    if (!answer) {
                        problems.push_back(std::string(what) + ": " + answer.error());
                        return;
                    }
                    if (answer->is_null()) {
                        return;
                    }
                    for (const json& series : answer->value("series", json::array())) {
                        Labels labels;
                        for (const auto& [name, value] :
                             series.value("labels", json::object()).items()) {
                            if (value.is_string()) {
                                labels[name] = value.get<std::string>();
                            }
                        }
                        std::string dimension =
                            fixed_dimension.empty() ? labels["dimension"] : fixed_dimension;
                        // What the engine holds on its own volume is engine_storage, never
                        // billed storage (decision 61, lake#663).
                        if (dimension == "storage" && engine_held(labels["kind"])) {
                            dimension = "engine_storage";
                        }
                        if (!wants(dimension)) {
                            continue;
                        }
                        for (const json& point : series.value("points", json::array())) {
                            accumulator.add(dimension,
                                            workspace.id,
                                            labels,
                                            point.value("at_ms", std::int64_t{0}),
                                            point.value("value", 0.0));
                        }
                    }
                };
            fold_series(read[0], "usage series", "");
            fold_series(read[1], "storage readings", "storage");
            if (wants("run_compute")) {
                if (auto runs = answer_of(read[2])) {
                    for (const json& run : runs->value("runs", json::array())) {
                        if (!run.is_object()) {
                            continue;
                        }
                        const std::string ended = run.value("updated", std::string());
                        const auto seconds =
                            elapsed_seconds(run.value("created", std::string()), ended);
                        const auto at = utc_ms(ended);
                        if (seconds && at) {
                            accumulator.add("run_compute",
                                            workspace.id,
                                            {{"kind", "run"}},
                                            *at,
                                            static_cast<double>(*seconds));
                        }
                    }
                } else {
                    problems.push_back("runs: " + runs.error());
                }
            }
            if (wants("transfer")) {
                if (auto rows = answer_of(read[3])) {
                    for (const json& row : *rows) {
                        const auto at = utc_ms(row.value("hour", std::string()));
                        if (!at) {
                            continue;
                        }
                        accumulator.add(
                            "transfer", workspace.id, {{"kind", "in"}}, *at, row.value("in", 0.0));
                        accumulator.add("transfer",
                                        workspace.id,
                                        {{"kind", "out"}},
                                        *at,
                                        row.value("out", 0.0));
                    }
                } else {
                    problems.push_back("repository transfer: " + rows.error());
                }
            }
            if (wants("build_compute")) {
                if (auto rows = answer_of(read[4])) {
                    std::set<std::string> counted;
                    for (const json& row : *rows) {
                        const auto at = utc_ms(row.value("hour", std::string()));
                        if (!at || !counted.insert(row.value("result_id", std::string())).second) {
                            continue;
                        }
                        accumulator.add("build_compute",
                                        workspace.id,
                                        {{"kind", "repository"}},
                                        *at,
                                        row.value("seconds", 0.0));
                    }
                } else {
                    problems.push_back("repository builds: " + rows.error());
                }
            }
        }
        json entry{{"workspace_id", workspace.id},
                   {"name", workspace.name},
                   {"measured", problems.empty()},
                   {"message", ""}};
        if (!problems.empty()) {
            std::string message;
            for (const std::string& problem : problems) {
                message += (message.empty() ? "" : "; ") + problem;
            }
            entry["message"] = message;
            reading.not_measured.push_back("Workspace " + workspace.name + ": " + message);
        }
        reading.workspaces.push_back(std::move(entry));
    }
    // Image builds are the operator's rows: from request to the final status, in the window they
    // finished in, in the build's workspace.
    if (wants("build_compute") && context.store) {
        std::set<std::string> chosen;
        for (const control::Workspace& workspace : query.workspaces) {
            chosen.insert(workspace.id);
        }
        if (const auto builds = context.store->list_builds(query.customer_id)) {
            for (const control::Build& build : *builds) {
                if (!chosen.contains(build.workspace_id) ||
                    (build.phase != "Succeeded" && build.phase != "Failed" &&
                     build.phase != "Cancelled")) {
                    continue;
                }
                const auto seconds = elapsed_seconds(build.created_at, build.updated_at);
                const auto at = utc_ms(build.updated_at);
                if (seconds && at) {
                    accumulator.add("build_compute",
                                    build.workspace_id,
                                    {{"kind", "image"}},
                                    *at,
                                    static_cast<double>(*seconds));
                }
            }
        } else {
            reading.not_measured.push_back("Image build time: " + builds.error().message);
        }
    }

    for (const UsageDimension& dimension : usage_dimensions()) {
        if (!wants(dimension.name)) {
            continue;
        }
        const bool gauge = dimension.aggregation == "last";
        std::vector<const RawSeries*> mine;
        for (const auto& [key, series] : accumulator.series()) {
            if (series.dimension == dimension.name) {
                mine.push_back(&series);
            }
        }
        std::vector<std::optional<double>> all(periods.size());
        for (const RawSeries* series : mine) {
            merge_into(all, series->line);
        }
        const double total = total_of(all, gauge);
        reading.totals[std::string(dimension.name)] = total;
        // `billed` (lake#658): whether the price list may rate it. Build, run and instance time
        // are kept as detail and never billed: compute is billed as CPU, RAM, GPU and VRAM.
        json entry{{"dimension", dimension.name},
                   {"title", dimension.title},
                   {"unit", dimension.unit},
                   {"aggregation", dimension.aggregation},
                   {"billed", dimension.priced},
                   {"total", total},
                   {"points", points_json(all, gauge)}};
        entry["by_workspace"] = breakdown(
            mine,
            gauge,
            periods.size(),
            [](const RawSeries& series) { return series.workspace_id; },
            [&names](const RawSeries& series) {
                return json{{"workspace_id", series.workspace_id},
                            {"name", names[series.workspace_id]}};
            });
        entry["by_kind"] = breakdown(mine, gauge, periods.size(), [](const RawSeries& series) {
            return label(series, "kind");
        });
        if (dimension.name == "model_tokens") {
            entry["by_agent"] = breakdown(mine, gauge, periods.size(), [](const RawSeries& series) {
                return label(series, "agent");
            });
            entry["by_launch"] = breakdown(
                mine,
                gauge,
                periods.size(),
                [](const RawSeries& series) { return label(series, "launch_id"); },
                [](const RawSeries& series) { return json{{"agent", label(series, "agent")}}; });
            entry["by_model"] = breakdown(mine, gauge, periods.size(), [](const RawSeries& series) {
                return label(series, "model");
            });
        }
        if (dimension.name == "instances" || dimension.name == "volumes" ||
            dimension.name == "predictions" || dimension.name == "searches" ||
            resource_dimension(dimension.name)) {
            entry["by_object"] = breakdown(
                mine,
                gauge,
                periods.size(),
                [](const RawSeries& series) { return label(series, "object_id"); },
                [](const RawSeries& series) { return json{{"kind", label(series, "kind")}}; });
        }
        if (dimension.classes) {
            entry["by_class"] = breakdown(mine, gauge, periods.size(), [](const RawSeries& series) {
                return label(series, "class");
            });
            // Usage without a class (a GPU the operator could not name) is the `""` class.
            double classed = 0;
            for (const json& row : entry["by_class"]) {
                classed += row.at("total").get<double>();
            }
            if (total - classed > 0.5) {
                std::vector<std::optional<double>> unclassed(periods.size());
                for (const RawSeries* series : mine) {
                    if (label(*series, "class").empty()) {
                        merge_into(unclassed, series->line);
                    }
                }
                entry["by_class"].push_back({{"key", ""},
                                             {"total", total - classed},
                                             {"points", points_json(unclassed, gauge)}});
            }
        }
        reading.dimensions.push_back(std::move(entry));
    }

    // What this reading cannot include, measured or not.
    if (wants_resources && context.store) {
        if (auto rows = resource_rows(*context.store)) {
            const auto names = unreported_workloads(
                *context.store, query.customer_id, query.workspaces, *rows, from_ms, to_ms);
            if (!names.empty()) {
                std::string list;
                for (std::size_t index = 0; index < names.size() && index < 8; ++index) {
                    list += (index == 0 ? "" : ", ") + names[index];
                }
                if (names.size() > 8) {
                    list += std::format(" and {} more", names.size() - 8);
                }
                reading.not_measured.push_back(
                    "CPU, RAM, GPU and GPU memory of " + list +
                    ": the Orama operator has not reported the resources it schedules them with, "
                    "so they are not counted (lake#643).");
            }
        } else {
            reading.not_measured.push_back(
                "CPU, RAM, GPU and GPU memory: the resources the Orama operator reports could not "
                "be read: " +
                rows.error());
        }
    }
    // Model tokens are counted by the model gateway as each call is answered (lake#248).
    // Speech is measured for every transcript and dictation (lake#613, lake#658).
    if (wants("engine_storage")) {
        // A workspace sampled in the window whose engine reported no models (its Zetta has no
        // storage_usage yet): its models, indexes, run artifacts, topics, files and buckets are
        // not in what its engine holds (lake#658).
        std::set<std::string> sampled, reported;
        for (const auto& [key, series] : accumulator.series()) {
            if (series.dimension != "engine_storage") {
                continue;
            }
            sampled.insert(series.workspace_id);
            if (label(series, "kind") == "models") {
                reported.insert(series.workspace_id);
            }
        }
        std::string unreported;
        for (const control::Workspace& workspace : query.workspaces) {
            if (sampled.contains(workspace.id) && !reported.contains(workspace.id)) {
                unreported += (unreported.empty() ? "" : ", ") + workspace.name;
            }
        }
        if (!unreported.empty()) {
            reading.not_measured.push_back(
                "Model files, indexes, run artifacts, topics, files and buckets inside workspace " +
                unreported +
                ": its engine does not report their size yet (it needs a Zetta with "
                "storage_usage), so only its datasets are shown (lake#658).");
        }
    }
    if (wants("transfer")) {
        reading.not_measured.push_back(
            "Transfer of apps through the ingress: the Orama operator does not report it yet "
            "(lake#661).");
    }
    co_return reading;
}

// The account, chosen workspaces, dimensions and periods of a usage_over_time input.
std::expected<UsageQuery, ApiResult> usage_query(const json& input, ApiContext& context) {
    UsageQuery query;
    query.customer_id = input.at("customer_id");
    const auto rows = context.store->list_workspaces(query.customer_id);
    if (!rows) {
        return std::unexpected(ApiResult::failure(rows.error().code, rows.error().message));
    }
    std::map<std::string, control::Workspace> live;
    for (const control::Workspace& row : *rows) {
        if (row.desired != "deleted") {
            live.emplace(row.id, row);
        }
    }
    if (input.contains("workspace_ids")) {
        const json& ids = input.at("workspace_ids");
        std::set<std::string> seen;
        for (std::size_t index = 0; index < ids.size(); ++index) {
            const std::string id = ids[index].get<std::string>();
            const auto found = live.find(id);
            if (found == live.end()) {
                return std::unexpected(ApiResult::failure(
                    "invalid_input",
                    "workspace " + id + " is not a live workspace of this account",
                    "/workspace_ids/" + std::to_string(index)));
            }
            if (seen.insert(id).second) {
                query.workspaces.push_back(found->second);
            }
        }
    } else {
        for (const auto& [id, row] : live) {
            query.workspaces.push_back(row);
        }
        std::ranges::stable_sort(query.workspaces, [](const auto& left, const auto& right) {
            return left.name < right.name;
        });
    }
    if (input.contains("dimensions")) {
        const json& names = input.at("dimensions");
        for (std::size_t index = 0; index < names.size(); ++index) {
            const std::string name = names[index].get<std::string>();
            if (!find_usage_dimension(name)) {
                return std::unexpected(ApiResult::failure("invalid_input",
                                                          "unknown usage dimension " + name,
                                                          "/dimensions/" + std::to_string(index)));
            }
            query.dimensions.insert(name);
        }
    } else {
        for (const UsageDimension& dimension : usage_dimensions()) {
            query.dimensions.insert(std::string(dimension.name));
        }
    }
    const auto from = utc_ms(input.at("from").get<std::string>());
    if (!from) {
        return std::unexpected(
            ApiResult::failure("invalid_input", "from must be an RFC 3339 UTC time", "/from"));
    }
    const auto to = utc_ms(input.at("to").get<std::string>());
    if (!to) {
        return std::unexpected(
            ApiResult::failure("invalid_input", "to must be an RFC 3339 UTC time", "/to"));
    }
    if (*to <= *from) {
        return std::unexpected(ApiResult::failure("invalid_input", "to must be after from", "/to"));
    }
    const auto granularity = granularity_of(input.at("granularity").get<std::string>());
    if (!granularity) {
        return std::unexpected(ApiResult::failure(
            "invalid_input", "granularity must be hour, day or month", "/granularity"));
    }
    auto periods = periods_of(*from, *to, *granularity);
    if (!periods) {
        return std::unexpected(periods.error());
    }
    query.periods = std::move(*periods);
    return query;
}

std::expected<std::string, ApiError> customer_object(const json& input, ApiContext& context) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const auto object = context.store->find_policy_object("customer", input.at("customer_id"));
    if (!object) {
        return std::unexpected(
            ApiError{object.error().code, object.error().message, "/customer_id"});
    }
    return object->id;
}

json dimension_names() {
    json names = json::array();
    for (const UsageDimension& dimension : usage_dimensions()) {
        names.push_back(dimension.name);
    }
    return names;
}

// ------------------------------------------------------------------------------------------------
// Limits.

constexpr int default_warn_percent = 80;

// The limits of an account with this month's use of each, and storage's against its quota.
liquid::runtime::task<ApiResult> limits_of(const std::string& customer_id, ApiContext& context) {
    const auto stored = context.store->usage_limits(customer_id);
    if (!stored) {
        co_return ApiResult::failure(stored.error().code, stored.error().message);
    }
    const auto quota = context.store->storage_quota(customer_id);
    if (!quota) {
        co_return ApiResult::failure(quota.error().code, quota.error().message);
    }
    std::map<std::string, control::UsageLimit> by_dimension;
    for (const control::UsageLimit& limit : *stored) {
        by_dimension[limit.dimension] = limit;
    }
    const std::uint64_t quota_bytes = quota->value_or(context.metering.storage_quota_bytes);

    // This UTC month's use of every counted dimension that has a limit.
    const std::int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
    const std::int64_t month = period_floor(now_ms, Granularity::month);
    UsageQuery query;
    query.customer_id = customer_id;
    for (const auto& [name, limit] : by_dimension) {
        if (name != "storage") {
            query.dimensions.insert(name);
        }
    }
    UsageReading reading;
    if (!query.dimensions.empty() && context.workspaces) {
        if (const auto rows = context.store->list_workspaces(customer_id)) {
            for (const control::Workspace& row : *rows) {
                if (row.desired != "deleted") {
                    query.workspaces.push_back(row);
                }
            }
        }
        query.periods =
            *periods_of(month, period_after(month, Granularity::month), Granularity::month);
        auto read = co_await read_usage(context, query);
        if (!read) {
            co_return ApiResult::failure("store_error", read.error().message);
        }
        reading = std::move(*read);
    }
    std::optional<std::uint64_t> repository_bytes;
    if (quota_bytes != 0 && context.sync_store) {
        const auto measured = co_await blocking_call(
            [&] { return context.sync_store->unique_stored_bytes(customer_id); },
            [](std::string message) -> SyncStoreResult<std::uint64_t> {
                return std::unexpected(SyncStoreError{"store_unavailable", std::move(message)});
            });
        if (measured) {
            repository_bytes = *measured;
        }
    }

    json limits = json::array();
    json warnings = json::array();
    const auto describe = [&](const std::string& name,
                              std::uint64_t limit,
                              int warn_percent,
                              std::optional<double> used,
                              bool enforced,
                              std::string_view period) {
        const UsageDimension* dimension = find_usage_dimension(name);
        json row{{"dimension", name},
                 {"title", dimension ? dimension->title : std::string_view(name)},
                 {"unit", dimension ? dimension->unit : std::string_view("")},
                 {"limit", limit},
                 {"warn_percent", warn_percent},
                 {"enforced", enforced},
                 {"period", period},
                 {"used", used ? json(*used) : json(nullptr)},
                 {"percent", nullptr},
                 {"state", used ? "ok" : "not_measured"}};
        if (used && limit > 0) {
            const double percent = *used * 100.0 / static_cast<double>(limit);
            row["percent"] = percent;
            if (percent >= 100.0) {
                row["state"] = "over";
                warnings.push_back(
                    std::format("{} is over its limit.", row["title"].get<std::string>()));
            } else if (percent >= warn_percent) {
                row["state"] = "warning";
                warnings.push_back(std::format(
                    "{} is at {:.0f}% of its limit.", row["title"].get<std::string>(), percent));
            }
        }
        limits.push_back(std::move(row));
    };
    if (quota_bytes != 0) {
        const auto found = by_dimension.find("storage");
        describe("storage",
                 quota_bytes,
                 found == by_dimension.end() ? default_warn_percent : found->second.warn_percent,
                 repository_bytes ? std::optional<double>(static_cast<double>(*repository_bytes))
                                  : std::nullopt,
                 true,
                 "now");
    }
    for (const auto& [name, limit] : by_dimension) {
        if (name == "storage") {
            continue;
        }
        const auto used = reading.totals.find(name);
        describe(name,
                 limit.limit,
                 limit.warn_percent,
                 used == reading.totals.end() ? std::nullopt : std::optional<double>(used->second),
                 // The model gateway refuses model calls past this one (lake#248).
                 name == "model_tokens",
                 "month");
    }
    co_return ApiResult::success({{"customer_id", customer_id},
                                  {"month", utc_text(month)},
                                  {"limits", std::move(limits)},
                                  {"warnings", std::move(warnings)},
                                  {"not_measured", std::move(reading.not_measured)}});
}

// ------------------------------------------------------------------------------------------------
// Model tokens, reported by the agent that spent them.

// The task, launch or schedule a credential belongs to (registry.cpp acting_agent), or why the
// caller may not report usage.
struct Reporter {
    std::string workspace_id;
    std::string agent;
    std::string launch_id;
    std::string customer_id;
};

std::expected<Reporter, ApiResult> reporter_of(const json& input, ApiContext& context) {
    const auto id_after = [&](std::string_view prefix) -> std::string {
        return context.credential_session_id.starts_with(prefix)
                   ? context.credential_session_id.substr(prefix.size())
                   : std::string();
    };
    const auto refused = [] {
        return ApiResult::failure("unauthorized",
                                  "only an agent's own task, launch, schedule or bus credential "
                                  "may report its model usage");
    };
    if (!context.store) {
        return std::unexpected(
            ApiResult::failure("store_unavailable", "no control-plane store is configured"));
    }
    if (context.credential_kind == "agent_service" && !context.agent_service.empty()) {
        const std::string task_id = input.value("task_id", std::string());
        const auto task = context.store->get_agent_task(task_id);
        if (task_id.empty() || !task || task->agent != context.agent_service) {
            return std::unexpected(
                ApiResult::failure("conflict", "the task is not one of this agent's", "/task_id"));
        }
        return Reporter{task->workspace_id, task->agent, task->id, task->customer_id};
    }
    if (context.credential_kind == "agent_task") {
        const auto task = context.store->get_agent_task(id_after("task:"));
        if (!task) {
            return std::unexpected(refused());
        }
        return Reporter{task->workspace_id, task->agent, task->id, task->customer_id};
    }
    if (context.credential_kind == "agent_launch") {
        const auto launch = context.store->get_agent_launch(id_after("launch:"));
        if (!launch) {
            return std::unexpected(refused());
        }
        return Reporter{launch->workspace_id,
                        launch->agent_name.empty() ? launch->agent_id : launch->agent_name,
                        launch->id,
                        launch->customer_id};
    }
    if (context.credential_kind == "agent_schedule") {
        const auto schedule = context.store->get_agent_cron_schedule(id_after("schedule:"));
        if (!schedule) {
            return std::unexpected(refused());
        }
        return Reporter{schedule->workspace_id,
                        schedule->agent_name.empty() ? schedule->agent_id : schedule->agent_name,
                        schedule->id,
                        schedule->customer_id};
    }
    return std::unexpected(refused());
}

std::expected<std::string, ApiError> agent_object(const json&, ApiContext&) {
    return std::string("agent_usage");
}

// ------------------------------------------------------------------------------------------------
// The model gateway (lake#248, docs/api/model-gateway.md).

// Who a model call is made for: an agent's own task, launch or schedule credential, never a
// person's key or a library agent's bus credential (which names no turn).
std::expected<Reporter, ApiResult> model_caller(const json& input, ApiContext& context) {
    if (!context.authorized) {
        return std::unexpected(ApiResult::failure(
            "unauthenticated",
            "a model call needs the agent's task, launch or schedule credential as its key"));
    }
    if (context.credential_kind != "agent_task" && context.credential_kind != "agent_launch" &&
        context.credential_kind != "agent_schedule") {
        return std::unexpected(ApiResult::failure(
            "unauthorized",
            "only an agent's own task, launch or schedule credential may call a model through "
            "Lake"));
    }
    return reporter_of(input, context);
}

// An account's monthly model-token limit and this month's use, as this process last read them.
// Reading the use reads every workspace of the account, so it is read at most once a minute; the
// tokens this process counts in between are added to it. Each replica keeps its own, so a limit
// is exceeded by at most about a minute of every replica's calls.
struct ModelTokenAllowance {
    std::uint64_t limit = 0; // 0: no limit
    double used = 0;
    std::chrono::steady_clock::time_point read_at;
};
constexpr std::chrono::seconds allowance_refresh{60};

std::mutex& allowance_mutex() {
    static std::mutex mutex;
    return mutex;
}
std::map<std::string, ModelTokenAllowance>& allowances() {
    static std::map<std::string, ModelTokenAllowance> by_customer;
    return by_customer;
}

std::optional<ApiResult> over_model_limit(const ModelTokenAllowance& allowance) {
    if (allowance.limit == 0 || allowance.used < static_cast<double>(allowance.limit)) {
        return std::nullopt;
    }
    return ApiResult::failure(
        "usage_limit_reached",
        std::format("This account has used its monthly limit of {} model tokens. An administrator "
                    "of the account raises or removes it with set_usage_limit (Usage in the "
                    "Lake Console).",
                    allowance.limit));
}

// The refusal when the account's model tokens are at or over its monthly limit, if they are.
liquid::runtime::task<std::optional<ApiResult>> model_limit_refusal(const std::string& customer_id,
                                                                    ApiContext& context) {
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(allowance_mutex());
        const auto found = allowances().find(customer_id);
        if (found != allowances().end() && now - found->second.read_at < allowance_refresh) {
            co_return over_model_limit(found->second);
        }
    }
    const auto stored = context.store->usage_limits(customer_id);
    if (!stored) {
        co_return ApiResult::failure("store_unavailable",
                                     "the account's usage limits could not be read: " +
                                         stored.error().message);
    }
    ModelTokenAllowance allowance{.read_at = now};
    for (const control::UsageLimit& limit : *stored) {
        if (limit.dimension == "model_tokens") {
            allowance.limit = limit.limit;
        }
    }
    if (allowance.limit > 0 && context.workspaces) {
        const std::int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count();
        const std::int64_t month = period_floor(now_ms, Granularity::month);
        UsageQuery query;
        query.customer_id = customer_id;
        query.dimensions.insert("model_tokens");
        if (const auto rows = context.store->list_workspaces(customer_id)) {
            for (const control::Workspace& row : *rows) {
                if (row.desired != "deleted") {
                    query.workspaces.push_back(row);
                }
            }
        }
        query.periods =
            *periods_of(month, period_after(month, Granularity::month), Granularity::month);
        auto read = co_await read_usage(context, query);
        if (!read) {
            co_return ApiResult::failure("store_unavailable",
                                         "this month's model tokens could not be read: " +
                                             read.error().message);
        }
        if (const auto total = read->totals.find("model_tokens"); total != read->totals.end()) {
            allowance.used = total->second;
        }
    }
    std::lock_guard lock(allowance_mutex());
    allowances()[customer_id] = allowance;
    co_return over_model_limit(allowance);
}

void count_model_tokens(const std::string& customer_id, std::uint64_t tokens) {
    std::lock_guard lock(allowance_mutex());
    if (const auto found = allowances().find(customer_id); found != allowances().end()) {
        found->second.used += static_cast<double>(tokens);
    }
}

liquid::runtime::task<ApiResult> model_completion(const json& input, ApiContext& context) {
    if (!context.store) {
        co_return ApiResult::failure("store_unavailable", "no control-plane store is configured");
    }
    auto caller = model_caller(input, context);
    if (!caller) {
        co_return caller.error();
    }
    if (caller->workspace_id.empty()) {
        co_return ApiResult::failure("conflict", "this run belongs to no workspace");
    }
    const std::string endpoint = input.at("endpoint");
    context.audit_workspace_id = caller->workspace_id;
    context.audit_object = "agent_usage/" + caller->launch_id;
    // What was asked of which model and what it cost; never the prompt or the answer.
    context.audit_details = {{"endpoint", endpoint}};
    auto prepared = prepare_model_request(endpoint, input.at("request"));
    if (!prepared) {
        co_return ApiResult::failure(
            prepared.error().code, prepared.error().message, prepared.error().path);
    }
    const std::string requested = prepared->at("model");
    const auto stream_field = prepared->find("stream");
    const bool stream = stream_field != prepared->end() && stream_field->is_boolean() &&
                        stream_field->get<bool>();
    context.audit_details["model"] = requested;
    context.audit_details["streamed"] = stream;
    if (!context.model_provider) {
        co_return ApiResult::failure("not_supported",
                                     "No model provider is configured on this Lake; an "
                                     "administrator sets LAKE_OPENAI_API_KEY.");
    }
    auto refusal = co_await model_limit_refusal(caller->customer_id, context);
    if (!refusal) {
        co_return ApiResult::failure("store_error", refusal.error().message);
    }
    if (*refusal) {
        context.audit_details["refused"] = "usage_limit";
        co_return std::move(**refusal);
    }
    ModelProviderRequest request{endpoint, prepared->dump(), stream};
    // A stream the caller reads as it arrives (the `/openai/v1/*` route, lake#665): each piece goes
    // on to the caller, and the usage is read from the events as they pass.
    const bool relayed = stream && static_cast<bool>(context.model_stream);
    auto passing = std::make_shared<ModelStreamUsage>();
    if (relayed) {
        request.on_body = [passing, sink = context.model_stream](std::string_view piece) {
            passing->feed(piece);
            return sink(piece);
        };
    }
    auto ran = co_await run_blocking(
        [provider = context.model_provider, request]() { return provider(request); });
    if (!ran) {
        co_return ApiResult::failure("busy", ran.error().message);
    }
    const ModelProviderResponse& response = *ran;
    context.audit_details["provider_status"] = response.status;
    if (!response.error.empty() || response.status < 200 || response.status >= 300) {
        const ApiError failure =
            model_provider_failure(response.status, response.body, response.error);
        co_return ApiResult::failure(failure.code, failure.message);
    }
    if (relayed) {
        passing->finish();
    }
    const ModelUsage usage = relayed ? passing->usage() : model_usage(response.body, stream);
    const std::string model = usage.model.empty() ? requested : usage.model;
    const bool recorded = usage.reported && context.usage_recorder != nullptr;
    if (recorded) {
        const Labels labels{
            {"agent", caller->agent}, {"launch_id", caller->launch_id}, {"model", model}};
        Labels in = labels, out = labels;
        in["kind"] = "input";
        out["kind"] = "output";
        std::vector<UsageRecorder::Increment> increments;
        increments.push_back({"model_tokens", usage.input_tokens, std::move(in)});
        increments.push_back({"model_tokens", usage.output_tokens, std::move(out)});
        context.usage_recorder->add_together(caller->workspace_id, std::move(increments));
    }
    count_model_tokens(caller->customer_id, usage.input_tokens + usage.output_tokens);
    context.audit_details["answered_model"] = model;
    context.audit_details["input_tokens"] = usage.input_tokens;
    context.audit_details["output_tokens"] = usage.output_tokens;
    context.audit_details["usage_reported"] = usage.reported;
    json out{{"workspace_id", caller->workspace_id},
             {"agent", caller->agent},
             {"launch_id", caller->launch_id},
             {"model", model},
             {"input_tokens", usage.input_tokens},
             {"output_tokens", usage.output_tokens},
             {"usage_reported", usage.reported},
             {"recorded", recorded},
             {"streamed", stream}};
    if (relayed) {
        // The caller already has the stream, piece by piece.
    } else if (stream) {
        out["events"] = response.body;
    } else {
        json answer = json::parse(response.body, nullptr, false);
        if (!answer.is_object()) {
            co_return ApiResult::failure("engine_error",
                                         "the model provider answered without a JSON object");
        }
        out["response"] = std::move(answer);
    }
    co_return ApiResult::success(std::move(out));
}

} // namespace

void record_workspace_transfer(const ApiContext& context,
                               const std::string& workspace_id,
                               std::uint64_t in,
                               std::uint64_t out) {
    if (context.usage_recorder != nullptr) {
        context.usage_recorder->add_transfer(workspace_id, in, out);
    }
}

// ------------------------------------------------------------------------------------------------
// The owner loop's sample.

liquid::runtime::task<UsageSampleReport> sample_usage(ApiContext& context,
                                                      std::chrono::seconds elapsed,
                                                      std::chrono::system_clock::time_point at) {
    UsageSampleReport report;
    UsageRecorder* recorder = context.usage_recorder;
    if (!context.store || !recorder) {
        co_return report;
    }
    const auto customers = context.store->list_customers(100000);
    if (!customers) {
        report.unmeasured.push_back("Accounts: " + customers.error().message);
        co_return report;
    }
    const std::uint64_t seconds =
        static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed.count()));
    const auto sample =
        [&](const std::string& workspace_id, std::string_view kind, std::uint64_t bytes) {
            recorder->sample(workspace_id, kind, bytes, at);
            ++report.readings;
        };
    // The resources the Orama operator reports for every workload (lake#643), read once a sample.
    std::optional<ResourceRows> resources;
    if (auto rows = resource_rows(*context.store)) {
        resources = std::move(*rows);
    } else {
        report.unmeasured.push_back("Workload resources: " + rows.error());
    }
    const auto spend = [&](const std::string& workspace_id,
                           std::string_view dimension,
                           std::uint64_t value,
                           Labels labels) {
        if (value == 0) {
            return;
        }
        recorder->add(workspace_id, dimension, value, std::move(labels), at);
        ++report.increments;
    };
    for (const control::Customer& customer : *customers) {
        const auto rows = context.store->list_workspaces(customer.id);
        if (!rows) {
            report.unmeasured.push_back("Workspaces of " + customer.id + ": " +
                                        rows.error().message);
            continue;
        }
        std::vector<control::Workspace> live;
        std::set<std::string> live_ids;
        for (const control::Workspace& row : *rows) {
            if (row.desired != "deleted") {
                live.push_back(row);
                live_ids.insert(row.id);
            }
        }
        if (live.empty()) {
            continue;
        }
        report.workspaces += live.size();

        // Repository storage. One workspace holds all of its space's repositories, so the space's
        // store is its own; with several, each repository's unique bytes go to its workspace.
        if (context.sync_store) {
            if (live.size() == 1) {
                if (const auto bytes = context.sync_store->unique_stored_bytes(customer.id)) {
                    sample(live.front().id, "repositories", *bytes);
                } else {
                    report.unmeasured.push_back("Repository storage of " + customer.id + ": " +
                                                bytes.error().message);
                }
            } else if (const auto repositories =
                           context.store->list_repositories(customer.id, 10000)) {
                std::map<std::string, std::uint64_t> bytes;
                std::set<std::string> failed;
                for (const control::Workspace& workspace : live) {
                    bytes[workspace.id] = 0;
                }
                for (const control::Repository& repository : *repositories) {
                    if (!live_ids.contains(repository.workspace_id) ||
                        !repository.deleted_at.empty()) {
                        continue;
                    }
                    const auto summary =
                        context.sync_store->repository_storage(customer.id, repository.id);
                    if (summary) {
                        bytes[repository.workspace_id] += summary->unique_bytes;
                    } else if (summary.error().code != "not_found") {
                        failed.insert(repository.workspace_id);
                    }
                }
                for (const auto& [workspace_id, total] : bytes) {
                    if (failed.contains(workspace_id)) {
                        report.unmeasured.push_back("Repository storage of workspace " +
                                                    workspace_id);
                    } else {
                        sample(workspace_id, "repositories", total);
                    }
                }
            }
        }

        // Container images, by the workspace each image is routed to.
        if (context.workspaces) {
            auto read = co_await tenant_image_bytes(context, customer.id);
            const ImageBytes images = read ? std::move(*read) : ImageBytes{};
            for (const control::Workspace& workspace : live) {
                if (!read || images.unmeasured_workspaces.contains(workspace.id)) {
                    report.unmeasured.push_back("Container images of workspace " + workspace.id);
                    continue;
                }
                const auto found = images.workspace_bytes.find(workspace.id);
                sample(workspace.id,
                       "images",
                       found == images.workspace_bytes.end() ? 0 : found->second);
            }
        }

        // Each workspace's own data: datasets, transcripts and workspace files.
        for (const control::Workspace& workspace : live) {
            if (!context.workspaces) {
                break;
            }
            ApiContext bound = context;
            bound.workspace_id = workspace.id;
            auto datasets = control::settled(
                co_await call_workspace(bound, customer.id, "list_datasets", true, json::object()));
            if (!datasets) {
                report.unmeasured.push_back("Workspace " + workspace.name + ": " +
                                            datasets.error().message);
                continue; // a workspace that is not ready has nothing else to read either
            }
            std::uint64_t data_bytes = 0;
            for (const json& dataset : datasets->value("datasets", json::array())) {
                if (dataset.is_object()) {
                    const json size = dataset.value("size_bytes", json(0));
                    if (size.is_number_integer() || size.is_number_unsigned()) {
                        data_bytes += static_cast<std::uint64_t>(
                            std::max<std::int64_t>(0, size.get<std::int64_t>()));
                    }
                }
            }
            // Everything else the engine holds (lake#658): its models, indexes, run artifacts,
            // topics, files and buckets, from Zetta's storage_usage; its datasets from there too,
            // whole databases rather than row chunks. An engine without the capability reports
            // only the datasets above, and usage_over_time says what it cannot include.
            auto stored = control::settled(
                co_await call_workspace(bound, customer.id, "storage_usage", true, json::object()));
            if (stored && stored->is_object() && stored->contains("kinds") &&
                stored->at("kinds").is_object()) {
                const auto bytes_of = [](const json& value) -> std::uint64_t {
                    return value.is_number_unsigned() ? value.get<std::uint64_t>()
                           : value.is_number_integer()
                               ? static_cast<std::uint64_t>(
                                     std::max<std::int64_t>(0, value.get<std::int64_t>()))
                               : 0;
                };
                const json kinds = stored->value("kinds", json::object());
                for (const auto& [kind, name] : engine_storage_kinds()) {
                    if (kind == "buckets") {
                        continue;
                    }
                    sample(workspace.id, name, bytes_of(kinds.value(std::string(kind), json(0))));
                }
                // The workspace's own buckets, never lake's: the store and workspace files are
                // `repositories` and `drive_files` already.
                std::uint64_t buckets = 0;
                for (const json& bucket : stored->value("buckets", json::array())) {
                    const std::string name = bucket.value("bucket", std::string());
                    if (name != control::workspace_store_bucket &&
                        name != control::drive_files_bucket) {
                        buckets += bytes_of(bucket.value("bytes", json(0)));
                    }
                }
                sample(workspace.id, "buckets", buckets);
            } else {
                report.unmeasured.push_back(
                    "Workspace " + workspace.name + " engine storage: " +
                    (stored ? std::string("the engine reports no storage_usage")
                            : stored.error().message));
                sample(workspace.id, "workspace_data", data_bytes);
            }
            auto records = workspace_records(context, customer.id, workspace.id);
            if (!records) {
                report.unmeasured.push_back("Workspace " + workspace.name +
                                            " records: " + records.error().message);
                continue;
            }
            auto transcripts =
                control::settled(co_await control::list_transcripts(*records, customer.id));
            if (transcripts) {
                std::uint64_t text_bytes = 0;
                for (const control::Transcript& transcript : *transcripts) {
                    text_bytes += transcript.text.size();
                }
                sample(workspace.id, "transcripts", text_bytes);
            } else {
                report.unmeasured.push_back("Transcripts of workspace " + workspace.name + ": " +
                                            transcripts.error().message);
            }
            auto drives = control::settled(co_await control::list_drives(*records, customer.id));
            if (!drives) {
                report.unmeasured.push_back("Drives of workspace " + workspace.name + ": " +
                                            drives.error().message);
                continue;
            }
            std::uint64_t file_bytes = 0;
            bool files_read = true;
            for (const control::Drive& drive : *drives) {
                if (drive.storage != "files") {
                    continue;
                }
                auto entries = control::settled(
                    co_await control::list_drive_entries(*records, customer.id, drive.id));
                if (!entries) {
                    files_read = false;
                    report.unmeasured.push_back("Drive " + drive.name + ": " +
                                                entries.error().message);
                    break;
                }
                for (const control::DriveEntry& entry : *entries) {
                    if (entry.kind == "file" && entry.state == "ready") {
                        file_bytes += entry.size;
                    }
                }
            }
            if (files_read) {
                sample(workspace.id, "drive_files", file_bytes);
            }
        }

        // Instance and volume time, from what the Orama operator reports running.
        if (const auto services = context.store->list_services(customer.id)) {
            for (const control::ManagedService& service : *services) {
                if (!live_ids.contains(service.workspace_id) || service.desired != "present" ||
                    service.phase != "Ready") {
                    continue;
                }
                const auto instances = static_cast<std::uint64_t>(std::max(service.instances, 0));
                spend(service.workspace_id,
                      "instances",
                      seconds * instances,
                      {{"kind", service.kind}, {"object_id", service.id}});
                spend(service.workspace_id,
                      "volumes",
                      seconds * instances *
                          static_cast<std::uint64_t>(std::max(service.storage_gi, 0)),
                      {{"kind", "service"}, {"object_id", service.id}});
            }
        } else {
            report.unmeasured.push_back("Services of " + customer.id + ": " +
                                        services.error().message);
        }
        if (const auto stores = context.store->list_stores(customer.id)) {
            for (const control::ManagedStore& store : *stores) {
                if (!live_ids.contains(store.workspace_id) || store.desired != "present" ||
                    store.phase != "Ready") {
                    continue;
                }
                spend(store.workspace_id,
                      "volumes",
                      seconds * static_cast<std::uint64_t>(std::max(store.size_gi, 0)),
                      {{"kind", "store"}, {"object_id", store.id}});
            }
        } else {
            report.unmeasured.push_back("Stores of " + customer.id + ": " + stores.error().message);
        }
        if (const auto apps = context.store->list_web_apps(customer.id)) {
            for (const control::WebApp& app : *apps) {
                if (!live_ids.contains(app.workspace_id) || app.desired != "running" ||
                    !app.ready) {
                    continue;
                }
                spend(app.workspace_id,
                      "instances",
                      seconds * static_cast<std::uint64_t>(std::max(app.replicas, 0)),
                      {{"kind", "app"}, {"object_id", app.id}});
            }
        } else {
            report.unmeasured.push_back("Apps of " + customer.id + ": " + apps.error().message);
        }

        // Scheduled resources (lake#643): CPU, RAM, GPU and GPU memory of every workload, from
        // the requests (or limits) the operator schedules it with. A running workload counts the
        // seconds since the last sample; a finished build, agent job or cron run counts once, for
        // how long it ran, in the hour it ended (lake#658).
        if (resources) {
            const auto charge = [&](const std::string& workspace_id,
                                    const control::WorkloadResources& row,
                                    std::uint64_t held,
                                    Labels labels) {
                auto increments = resource_increments(row, held, std::move(labels));
                if (increments.empty()) {
                    return;
                }
                report.increments += increments.size();
                recorder->add_together(workspace_id, std::move(increments), at);
            };
            const auto running = [&](std::string_view kind,
                                     const std::string& id) -> const control::WorkloadResources* {
                const control::WorkloadResources* row = resources_of(*resources, kind, id);
                return row != nullptr && row->pods > 0 ? row : nullptr;
            };
            std::set<std::string> engines;
            for (const control::Workspace& workspace : live) {
                engines.insert(workspace.zetta_service_id);
                if (workspace.desired != "running") {
                    continue;
                }
                const control::WorkloadResources* row = running("workspace", workspace.id);
                if (row == nullptr && !workspace.zetta_service_id.empty()) {
                    row = running("service", workspace.zetta_service_id);
                }
                if (row != nullptr) {
                    charge(workspace.id,
                           *row,
                           seconds,
                           {{"kind", "workspace"}, {"object_id", workspace.id}});
                }
            }
            if (const auto services = context.store->list_services(customer.id)) {
                for (const control::ManagedService& service : *services) {
                    if (!live_ids.contains(service.workspace_id) || engines.contains(service.id) ||
                        service.desired != "present") {
                        continue;
                    }
                    if (const auto* row = running("service", service.id)) {
                        charge(service.workspace_id,
                               *row,
                               seconds,
                               {{"kind", service.kind}, {"object_id", service.id}});
                    }
                }
            }
            if (const auto stores = context.store->list_stores(customer.id)) {
                for (const control::ManagedStore& store : *stores) {
                    if (!live_ids.contains(store.workspace_id) || store.desired != "present") {
                        continue;
                    }
                    if (const auto* row = running("store", store.id)) {
                        charge(store.workspace_id,
                               *row,
                               seconds,
                               {{"kind", "store"}, {"object_id", store.id}});
                    }
                }
            }
            if (const auto apps = context.store->list_web_apps(customer.id)) {
                for (const control::WebApp& app : *apps) {
                    if (!live_ids.contains(app.workspace_id) || app.desired == "deleted") {
                        continue;
                    }
                    if (const auto* row = running("app", app.id)) {
                        charge(app.workspace_id,
                               *row,
                               seconds,
                               {{"kind", "app"}, {"object_id", app.id}});
                    }
                }
            }
            if (const auto launches = context.store->list_agent_launches(customer.id, "", 10000)) {
                for (const control::AgentLaunch& launch : *launches) {
                    if (!live_ids.contains(launch.workspace_id) || launch.mode != "service" ||
                        launch.desired != "present") {
                        continue;
                    }
                    if (const auto* row = running("agent_launch", launch.id)) {
                        charge(launch.workspace_id,
                               *row,
                               seconds,
                               {{"kind", "agent"}, {"object_id", launch.id}});
                    }
                }
            } else {
                report.unmeasured.push_back("Agent launches of " + customer.id + ": " +
                                            launches.error().message);
            }
            // Builds, job launches and cron runs (lake#658): each finished one is charged once,
            // whenever lake first finds it finished with its resources reported.
            auto jobs = finished_jobs(*context.store, customer.id, live_ids);
            for (const std::string& problem : jobs.problems) {
                report.unmeasured.push_back(problem + " of " + customer.id);
            }
            static_cast<void>(co_await charge_finished_jobs(
                context, customer.id, jobs.jobs, *resources, at, report));
        }

        // Repository build results finished since a little before the last sample, for their
        // workspace records (a result already recorded is written once only).
        const auto since =
            std::chrono::floor<std::chrono::hours>(at - 2 * elapsed - std::chrono::hours(1));
        const std::string since_hour = std::format("{:%Y-%m-%dT%H:00:00Z}", since);
        if (const auto queued = queue_repository_builds(context, customer.id, since_hour);
            !queued) {
            report.unmeasured.push_back("Repository builds of " + customer.id + ": " +
                                        queued.error());
        }
    }
    co_return report;
}

void register_usage_capabilities(Registry& registry) {
    const json dimensions_schema{{"type", "array"},
                                 {"items", {{"type", "string"}, {"enum", dimension_names()}}}};
    registry.add(Capability{
        .name = "usage_over_time",
        .title = "Usage over time",
        .description =
            "Usage per dimension over a period, by hour, day or month, added up across the chosen "
            "workspaces of one account (all of them when `workspace_ids` is left out), with each "
            "dimension's total, a value per period, and breakdowns by workspace and by kind (model "
            "tokens also by agent, launch and model; instances and volumes by service, store or "
            "app; CPU, RAM, GPU, GPU memory, predictions and searches by object). `billed` says "
            "whether the price list bills the dimension; build, run and instance time are detail "
            "and never billed, and so is engine_storage, what a workspace engine holds on its "
            "own volume, which is billed as the volume (`volumes`). Read from each workspace's own Zetta. `from` is rounded down and "
            "`to` up to whole periods; an hour granularity covers at most 100 days.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"workspace_ids", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                           {"dimensions", dimensions_schema},
                           {"from", {{"type", "string"}, {"description", "RFC 3339 UTC"}}},
                           {"to", {{"type", "string"}, {"description", "RFC 3339 UTC, exclusive"}}},
                           {"granularity",
                            {{"type", "string"},
                             {"enum", json::array({"hour", "day", "month"})}}}}},
                         {"required", json::array({"customer_id", "from", "to", "granularity"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"customer_id", {{"type", "string"}}},
                            {"from", {{"type", "string"}}},
                            {"to", {{"type", "string"}}},
                            {"granularity", {{"type", "string"}}},
                            {"periods", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                            {"dimensions", {{"type", "array"}, {"items", {{"type", "object"}}}}},
                            {"workspaces", {{"type", "array"}, {"items", {{"type", "object"}}}}},
                            {"not_measured",
                             {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
                          {"required",
                           json::array({"customer_id",
                                        "from",
                                        "to",
                                        "granularity",
                                        "periods",
                                        "dimensions",
                                        "workspaces",
                                        "not_measured"})}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .resolves_operator_audit_target = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return ApiResult::failure("store_unavailable",
                                             "no control-plane store is configured");
            }
            auto query = usage_query(input, context);
            if (!query) {
                co_return query.error();
            }
            UsageReading reading;
            if (context.workspaces) {
                auto read = co_await read_usage(context, *query);
                if (!read) {
                    co_return ApiResult::failure("store_error", read.error().message);
                }
                reading = std::move(*read);
            } else {
                reading.not_measured.push_back("Usage over time: this Lake has no workspaces.");
            }
            json periods = json::array();
            for (const std::int64_t start : query->periods.starts) {
                periods.push_back(utc_text(start));
            }
            const std::string granularity = input.at("granularity");
            context.audit_customer_id = query->customer_id;
            context.audit_kind = "metering.usage_over_time.read";
            context.audit_object = "customer/" + query->customer_id + "/metering";
            context.audit_details = {{"granularity", granularity},
                                     {"workspaces", query->workspaces.size()},
                                     {"dimensions", query->dimensions.size()}};
            co_return ApiResult::success({{"customer_id", query->customer_id},
                                          {"from", utc_text(query->periods.starts.front())},
                                          {"to", utc_text(query->periods.end)},
                                          {"granularity", granularity},
                                          {"periods", std::move(periods)},
                                          {"dimensions", std::move(reading.dimensions)},
                                          {"workspaces", std::move(reading.workspaces)},
                                          {"not_measured", std::move(reading.not_measured)}});
        }});
    registry.add(Capability{
        .name = "usage_limits",
        .title = "Usage limits",
        .description =
            "The account's usage limits with what is used now: storage against the enforced "
            "repository storage quota, and every other limited dimension against this UTC "
            "month's use. `state` is ok, warning (from `warn_percent`), over or not_measured; "
            "only storage is enforced.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"customer_id", {{"type", "string"}}}}},
                         {"required", json::array({"customer_id"})},
                         {"additionalProperties", false}},
        .output_schema =
            {{"type", "object"},
             {"properties",
              {{"customer_id", {{"type", "string"}}},
               {"month", {{"type", "string"}}},
               {"limits", {{"type", "array"}, {"items", {{"type", "object"}}}}},
               {"warnings", {{"type", "array"}, {"items", {{"type", "string"}}}}},
               {"not_measured", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
             {"required",
              json::array({"customer_id", "month", "limits", "warnings", "not_measured"})}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .resolves_operator_audit_target = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return ApiResult::failure("store_unavailable",
                                             "no control-plane store is configured");
            }
            const std::string customer_id = input.at("customer_id");
            auto completed = co_await limits_of(customer_id, context);
            if (!completed) {
                co_return ApiResult::failure("store_error", completed.error().message);
            }
            ApiResult result = std::move(*completed);
            if (result.ok) {
                context.audit_customer_id = customer_id;
                context.audit_kind = "metering.limits.read";
                context.audit_object = "customer/" + customer_id + "/metering";
                context.audit_details = {{"warnings", result.value["warnings"].size()}};
            }
            co_return result;
        }});
    registry.add(Capability{
        .name = "set_usage_limit",
        .title = "Set a usage limit",
        .description =
            "Set the account's limit on one dimension of usage, in the dimension's unit; 0 removes "
            "it. Storage's limit is the repository storage quota (set_storage_quota), enforced on "
            "new repository writes; every other limit is per UTC calendar month and warns from "
            "`warn_percent` (default 80) without refusing anything.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"dimension", {{"type", "string"}, {"enum", dimension_names()}}},
                           {"limit",
                            {{"type", "integer"},
                             {"minimum", 0},
                             {"maximum", std::numeric_limits<std::int64_t>::max()}}},
                           {"warn_percent",
                            {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}}}},
                         {"required", json::array({"customer_id", "dimension", "limit"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"customer_id", {{"type", "string"}}},
                            {"dimension", {{"type", "string"}}},
                            {"limit", {{"type", "integer"}}},
                            {"warn_percent", {{"type", "integer"}}}}},
                          {"required",
                           json::array({"customer_id", "dimension", "limit", "warn_percent"})}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .resolves_operator_audit_target = true,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return ApiResult::failure("store_unavailable",
                                          "no control-plane store is configured");
            }
            const std::string customer_id = input.at("customer_id");
            control::UsageLimit limit{customer_id,
                                      input.at("dimension"),
                                      input.at("limit").get<std::uint64_t>(),
                                      input.value("warn_percent", default_warn_percent),
                                      {}};
            if (limit.dimension == "storage") {
                const auto quota = context.store->set_storage_quota(customer_id, limit.limit);
                if (!quota) {
                    return ApiResult::failure(quota.error().code, quota.error().message);
                }
            }
            const auto stored = context.store->set_usage_limit(limit);
            if (!stored) {
                return ApiResult::failure(stored.error().code, stored.error().message);
            }
            context.audit_customer_id = customer_id;
            context.audit_kind = "metering.limit.set";
            context.audit_object = "customer/" + customer_id + "/metering";
            context.audit_details = {{"dimension", limit.dimension},
                                     {"limit", limit.limit},
                                     {"warn_percent", limit.warn_percent}};
            return ApiResult::success({{"customer_id", customer_id},
                                       {"dimension", limit.dimension},
                                       {"limit", limit.limit},
                                       {"warn_percent", limit.warn_percent}});
        }});
    registry.add(Capability{
        .name = "record_model_usage",
        .title = "Report model tokens",
        .description =
            "An agent reports the model tokens one of its turns or runs spent. Only an agent's own "
            "task, launch or schedule credential may call it, or a library agent's bus credential "
            "naming its task in `task_id`. Lake adds the tokens to the usage of the workspace the "
            "task, launch or schedule runs in, labelled with the agent, the run and the model.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"model_name",
                            {{"type", "string"},
                             {"minLength", 1},
                             {"maxLength", 128},
                             {"description", "the language model's name, such as gpt-5"}}},
                           {"input_tokens", {{"type", "integer"}, {"minimum", 0}}},
                           {"output_tokens", {{"type", "integer"}, {"minimum", 0}}},
                           {"task_id", {{"type", "string"}}}}},
                         {"required", json::array({"model_name", "input_tokens", "output_tokens"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"workspace_id", {{"type", "string"}}},
                            {"agent", {{"type", "string"}}},
                            {"launch_id", {{"type", "string"}}},
                            {"recorded", {{"type", "boolean"}}}}}},
        .kind = CapabilityKind::Action,
        // Authorized by the agent's own credential, which the handler checks; like the agent bus.
        .requires_auth = false,
        .object = "agent_usage",
        .action = "write",
        .resolve_object = agent_object,
        .handler = [](const json& input, ApiContext& context) {
            auto reporter = reporter_of(input, context);
            if (!reporter) {
                return reporter.error();
            }
            if (reporter->workspace_id.empty()) {
                return ApiResult::failure("conflict", "this run belongs to no workspace");
            }
            const std::string model = input.at("model_name");
            const auto input_tokens = input.at("input_tokens").get<std::uint64_t>();
            const auto output_tokens = input.at("output_tokens").get<std::uint64_t>();
            const bool recorded = context.usage_recorder != nullptr;
            if (recorded) {
                const Labels labels{{"agent", reporter->agent},
                                    {"launch_id", reporter->launch_id},
                                    {"model", model}};
                std::vector<UsageRecorder::Increment> increments;
                Labels in = labels, out = labels;
                in["kind"] = "input";
                out["kind"] = "output";
                increments.push_back({"model_tokens", input_tokens, std::move(in)});
                increments.push_back({"model_tokens", output_tokens, std::move(out)});
                context.usage_recorder->add_together(reporter->workspace_id, std::move(increments));
            }
            context.audit_workspace_id = reporter->workspace_id;
            context.audit_object = "agent_usage/" + reporter->launch_id;
            context.audit_details = {
                {"model", model}, {"input_tokens", input_tokens}, {"output_tokens", output_tokens}};
            return ApiResult::success({{"workspace_id", reporter->workspace_id},
                                       {"agent", reporter->agent},
                                       {"launch_id", reporter->launch_id},
                                       {"recorded", recorded}});
        }});
    registry.add(Capability{
        .name = "model_completion",
        .title = "Call a model through Lake",
        .description =
            "The model gateway: an agent's model call, sent to the model provider with Lake's own "
            "key and counted as `model_tokens` in the workspace its task, launch or schedule runs "
            "in. Only an agent's own task, launch or schedule credential may call it. `endpoint` "
            "is the OpenAI API path (`chat/completions`, `responses` or `embeddings`) and "
            "`request` its JSON body. Agents normally reach it as the OpenAI-compatible "
            "`POST /openai/v1/<endpoint>`, with the OpenAI SDK's base URL set to "
            "`<lake>/openai/v1` and the credential as its API key. Refused with "
            "`usage_limit_reached` once the account's monthly `model_tokens` limit is used up.",
        .input_schema =
            {{"type", "object"},
             {"properties",
              {{"endpoint",
                {{"type", "string"},
                 {"enum", json::array({"chat/completions", "responses", "embeddings"})},
                 {"description", "the OpenAI API path the request is for"}}},
               {"request",
                {{"type", "object"},
                 {"description",
                  "the OpenAI request body: `model` and the endpoint's own fields, such as "
                  "`messages` and `tools`; `stream: true` answers the whole event stream"}}}}},
             {"required", json::array({"endpoint", "request"})},
             {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"workspace_id", {{"type", "string"}}},
                            {"agent", {{"type", "string"}}},
                            {"launch_id", {{"type", "string"}}},
                            {"model", {{"type", "string"}}},
                            {"input_tokens", {{"type", "integer"}}},
                            {"output_tokens", {{"type", "integer"}}},
                            {"usage_reported", {{"type", "boolean"}}},
                            {"recorded", {{"type", "boolean"}}},
                            {"streamed", {{"type", "boolean"}}},
                            {"response", {{"type", "object"}}},
                            {"events",
                             {{"type", "string"},
                              {"description",
                               "the provider's whole event stream, when the request streamed "
                               "(the `/openai/v1/*` route relays it event by event instead)"}}}}},
                          {"required",
                           json::array({"workspace_id",
                                        "agent",
                                        "launch_id",
                                        "model",
                                        "input_tokens",
                                        "output_tokens",
                                        "usage_reported",
                                        "recorded",
                                        "streamed"})}},
        .kind = CapabilityKind::Action,
        // Authorized by the agent's own credential, which the handler checks; like
        // record_model_usage.
        .requires_auth = false,
        .object = "agent_usage",
        .action = "write",
        .resolve_object = agent_object,
        .async_handler = model_completion});
}

} // namespace lake::api
