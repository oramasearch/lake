#include "lake/api/capabilities.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "lake/api/metering_recorder.hpp"
#include "lake/api/sync_store.hpp"
#include "lake/control/builds.hpp"
#include "lake/control/metering.hpp"
#include "lake/control/store.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

ApiResult store_error(const control::StoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

ApiResult sync_error(const SyncStoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

std::optional<std::string> hour_of(const RepositoryArtifact& artifact) {
    const std::string& timestamp =
        artifact.finished_at.empty() ? artifact.observed_at : artifact.finished_at;
    if (timestamp.size() < 13 || timestamp[4] != '-' || timestamp[7] != '-' ||
        timestamp[10] != 'T') {
        return std::nullopt;
    }
    return timestamp.substr(0, 13) + ":00:00Z";
}

std::uint64_t compute_seconds(const RepositoryArtifact& artifact) {
    const auto& timing = artifact.timing;
    const std::uint64_t milliseconds =
        timing.sandbox_ms + timing.exec_ms + timing.hash_ms + timing.store_ms;
    return milliseconds / 1000 + (milliseconds % 1000 == 0 ? 0 : 1);
}

std::uint64_t
priced_units(std::uint64_t units, std::uint64_t units_per_price, std::uint64_t micros) {
    using Wide = unsigned __int128;
    const Wide numerator = static_cast<Wide>(units) * micros;
    const Wide value = numerator / units_per_price + (numerator % units_per_price == 0 ? 0 : 1);
    return value > std::numeric_limits<std::uint64_t>::max()
               ? std::numeric_limits<std::uint64_t>::max()
               : static_cast<std::uint64_t>(value);
}

std::uint64_t add_saturated(std::uint64_t left, std::uint64_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left + right;
}

std::uint64_t multiply_saturated(std::uint64_t left, std::uint64_t right) {
    using Wide = unsigned __int128;
    const Wide value = static_cast<Wide>(left) * right;
    return value > std::numeric_limits<std::uint64_t>::max()
               ? std::numeric_limits<std::uint64_t>::max()
               : static_cast<std::uint64_t>(value);
}

// One hour of repository usage.
struct HourUsage {
    std::uint64_t transfer_in_bytes = 0;
    std::uint64_t transfer_out_bytes = 0;
    std::uint64_t compute_seconds = 0;
};

// Repository storage has no hourly history (it is measured live, lake#348), so an hour prices
// transfer and build time only. This keeps the configured prices; it decides no billing.
std::uint64_t cost_of(const HourUsage& usage, const MeteringConfig& pricing) {
    constexpr std::uint64_t gib = std::uint64_t{1} << 30U;
    std::uint64_t cost = priced_units(usage.transfer_in_bytes, gib, pricing.transfer_in_gib_micros);
    cost = add_saturated(
        cost, priced_units(usage.transfer_out_bytes, gib, pricing.transfer_out_gib_micros));
    return add_saturated(cost,
                         multiply_saturated(usage.compute_seconds, pricing.compute_second_micros));
}

// Milliseconds since the Unix epoch for "YYYY-MM-DDTHH:MM:SS[.fff][Z]" (a space may replace the
// T). Lake and Zetta both write UTC; anything after the fraction is ignored.
std::optional<std::int64_t> unix_milliseconds(std::string_view text) {
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
    std::int64_t milliseconds = 0;
    if (text.size() > 20 && text[19] == '.') {
        std::int64_t scale = 100;
        for (std::size_t index = 20; index < text.size() && scale > 0; ++index) {
            if (text[index] < '0' || text[index] > '9') {
                break;
            }
            milliseconds += (text[index] - '0') * scale;
            scale /= 10;
        }
    }
    const auto days = std::chrono::sys_days{date}.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(days).count() +
           ((*hour * 60 + *minute) * 60 + *second) * std::int64_t{1000} + milliseconds;
}

// The reporting window from `from`/`to` (inclusive UTC hours), in epoch milliseconds.
struct Window {
    std::optional<std::int64_t> from;
    std::optional<std::int64_t> to; // exclusive: the end of the `to` hour

    bool contains(std::int64_t at) const {
        return (!from || at >= *from) && (!to || at < *to);
    }
};

Window window_of(const json& input) {
    Window window;
    window.from = unix_milliseconds(input.value("from", ""));
    if (const auto to = unix_milliseconds(input.value("to", ""))) {
        window.to = *to + 3'600'000;
    }
    return window;
}

// Whole seconds from `started` to `ended`, each result rounded up like repository builds. Absent
// or reversed timestamps count nothing.
std::optional<std::uint64_t>
elapsed_seconds(std::string_view started, std::string_view ended, const Window& window) {
    const auto begin = unix_milliseconds(started);
    const auto end = unix_milliseconds(ended);
    if (!begin || !end || *end < *begin || !window.contains(*end)) {
        return std::nullopt;
    }
    const auto milliseconds = static_cast<std::uint64_t>(*end - *begin);
    return milliseconds / 1000 + (milliseconds % 1000 == 0 ? 0 : 1);
}

// Image builds the operator has finished, from request to the final status write. The build row
// carries no start time, so time spent queued is included (docs/api/metering.md).
json image_build_usage(const std::vector<control::Build>& builds, const Window& window) {
    std::uint64_t seconds = 0;
    std::uint64_t count = 0;
    for (const control::Build& build : builds) {
        if (build.phase != "Succeeded" && build.phase != "Failed" && build.phase != "Cancelled") {
            continue;
        }
        if (const auto elapsed = elapsed_seconds(build.created_at, build.updated_at, window)) {
            seconds = add_saturated(seconds, *elapsed);
            ++count;
        }
    }
    return {{"seconds", seconds}, {"count", count}};
}

struct WorkspaceRead {
    liquid::result<control::ZettaControlResult<json>> value;
};

boost::capy::io_task<WorkspaceRead>
complete_workspace_read(liquid::runtime::task<control::ZettaControlResult<json>> task) {
    auto result = co_await std::move(task);
    co_return boost::capy::io_result<WorkspaceRead>{{}, {std::move(result)}};
}

// The answer of one workspace read, or why it has none.
std::expected<json, std::string> workspace_answer(WorkspaceRead& read) {
    if (!read.value) {
        return std::unexpected(read.value.error().message);
    }
    if (!*read.value) {
        return std::unexpected(read.value->error().message);
    }
    return std::move(**read.value);
}

// What one workspace's metering records hold for the space, as JSON for the concurrent read.
liquid::runtime::task<control::ZettaControlResult<json>>
transfer_records(std::shared_ptr<control::WorkspaceRecords> records,
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
                        {"transfer_in_bytes", transfer.transfer_in_bytes},
                        {"transfer_out_bytes", transfer.transfer_out_bytes}});
    }
    co_return control::ZettaControlResult<json>(std::move(rows));
}

liquid::runtime::task<control::ZettaControlResult<json>>
build_records(std::shared_ptr<control::WorkspaceRecords> records,
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
                        {"compute_seconds", build.compute_seconds}});
    }
    co_return control::ZettaControlResult<json>(std::move(rows));
}

liquid::runtime::task<control::ZettaControlResult<json>> refused(control::ZettaControlError error) {
    co_return control::ZettaControlResult<json>(std::unexpected(std::move(error)));
}

// Each live workspace's measurements and its repository usage records.
struct WorkspaceUsage {
    json entries = json::array();              // the summary's `workspaces[]`
    json transfer = json::array();             // every transfer record of every workspace read
    json builds = json::array();               // every build record of every workspace read
    std::set<std::string> recorded_workspaces; // workspaces whose build records were read
    std::vector<std::string> unrecorded;       // why a workspace's records could not be read
};

// Stored dataset bytes and recorded run time in each of the customer's workspace Zettas, and the
// repository transfer and build records lake keeps there (lake#348, lake#349). The four reads of
// every workspace run concurrently, in one round. Nothing read here is copied into lake's control
// store.
liquid::runtime::task<WorkspaceUsage>
workspace_usage(ApiContext& context, const std::string& customer_id, const Window& window) {
    WorkspaceUsage usage;
    if (!context.workspaces || !context.store) {
        co_return usage;
    }
    const auto rows = context.store->list_workspaces(customer_id);
    if (!rows) {
        co_return usage;
    }
    std::vector<control::Workspace> live;
    for (const control::Workspace& row : *rows) {
        if (row.desired != "deleted") {
            live.push_back(row);
        }
    }
    constexpr std::size_t reads_per_workspace = 4;
    const std::int64_t since_us = window.from ? *window.from * 1000 : 0;
    std::vector<ApiContext> contexts;
    contexts.reserve(live.size() * 2);
    std::vector<boost::capy::io_task<WorkspaceRead>> pending;
    pending.reserve(live.size() * reads_per_workspace);
    for (const control::Workspace& row : live) {
        for (const char* capability : {"list_datasets", "list_runs"}) {
            contexts.push_back(context);
            contexts.back().workspace_id = row.id;
            pending.push_back(complete_workspace_read(
                call_workspace(contexts.back(), customer_id, capability, true, json::object())));
        }
        auto opened = workspace_records(context, customer_id, row.id);
        if (!opened) {
            pending.push_back(complete_workspace_read(refused(opened.error())));
            pending.push_back(complete_workspace_read(refused(opened.error())));
            continue;
        }
        auto records = std::make_shared<control::WorkspaceRecords>(std::move(*opened));
        pending.push_back(
            complete_workspace_read(transfer_records(records, customer_id, since_us)));
        pending.push_back(complete_workspace_read(build_records(records, customer_id, since_us)));
    }
    if (pending.empty()) {
        co_return usage;
    }
    auto [wait_error, reads] = co_await liquid::runtime::when_all(std::move(pending));
    for (std::size_t index = 0; index < live.size(); ++index) {
        const control::Workspace& row = live[index];
        json entry{{"workspace_id", row.id},
                   {"name", row.name},
                   {"measured", false},
                   {"data_bytes", 0},
                   {"datasets", 0},
                   {"run_seconds", 0},
                   {"runs", 0},
                   {"message", ""}};
        if (wait_error) {
            entry["message"] = wait_error.message();
            usage.unrecorded.push_back("Repository transfer and builds in workspace " + row.name +
                                       ": " + wait_error.message());
            usage.entries.push_back(std::move(entry));
            continue;
        }
        WorkspaceRead* read = &reads[index * reads_per_workspace];
        auto transfer = workspace_answer(read[2]);
        auto builds = workspace_answer(read[3]);
        if (transfer && builds) {
            for (json& record : *transfer) {
                usage.transfer.push_back(std::move(record));
            }
            for (json& record : *builds) {
                usage.builds.push_back(std::move(record));
            }
            usage.recorded_workspaces.insert(row.id);
        } else {
            usage.unrecorded.push_back("Repository transfer and builds in workspace " + row.name +
                                       ": " + (!transfer ? transfer.error() : builds.error()));
        }
        auto datasets = workspace_answer(read[0]);
        auto runs = workspace_answer(read[1]);
        if (!datasets || !runs) {
            entry["message"] = !datasets ? datasets.error() : runs.error();
            usage.entries.push_back(std::move(entry));
            continue;
        }
        std::uint64_t data_bytes = 0;
        std::uint64_t dataset_count = 0;
        for (const json& dataset : datasets->value("datasets", json::array())) {
            if (!dataset.is_object()) {
                continue;
            }
            const json size = dataset.value("size_bytes", json(0));
            data_bytes = add_saturated(data_bytes,
                                       size.is_number_unsigned() || size.is_number_integer()
                                           ? static_cast<std::uint64_t>(std::max<std::int64_t>(
                                                 0, size.get<std::int64_t>()))
                                           : 0);
            ++dataset_count;
        }
        std::uint64_t run_seconds = 0;
        std::uint64_t run_count = 0;
        for (const json& run : runs->value("runs", json::array())) {
            if (!run.is_object()) {
                continue;
            }
            if (const auto elapsed = elapsed_seconds(run.value("created", std::string()),
                                                     run.value("updated", std::string()),
                                                     window)) {
                run_seconds = add_saturated(run_seconds, *elapsed);
                ++run_count;
            }
        }
        entry["measured"] = true;
        entry["data_bytes"] = data_bytes;
        entry["datasets"] = dataset_count;
        entry["run_seconds"] = run_seconds;
        entry["runs"] = run_count;
        usage.entries.push_back(std::move(entry));
    }
    co_return usage;
}

// Every build result of the space's repositories, each with its repository's workspace (empty for
// a repository migration 0051 left without one), read from the repositories on every request.
struct RepositoryBuild {
    std::string workspace_id;
    control::MeteredBuild build;
};

std::expected<std::vector<RepositoryBuild>, ApiResult> repository_builds(
    const std::string& customer_id, ApiContext& context, std::size_t& without_workspace) {
    const auto repositories = context.store->list_repositories(customer_id, 1000);
    if (!repositories) {
        return std::unexpected(store_error(repositories.error()));
    }
    std::vector<RepositoryBuild> builds;
    for (const control::Repository& repository : *repositories) {
        if (repository.workspace_id.empty()) {
            ++without_workspace;
        }
        RepositoryArtifactFilter filter;
        filter.limit = 1000;
        for (;;) {
            const auto page =
                context.sync_store->repository_artifacts(customer_id, repository.id, filter);
            if (!page) {
                return std::unexpected(sync_error(page.error()));
            }
            for (const RepositoryArtifact& artifact : page->items) {
                const auto hour = hour_of(artifact);
                if (!hour) {
                    continue;
                }
                builds.push_back({repository.workspace_id,
                                  {.customer_id = customer_id,
                                   .result_id = artifact.id,
                                   .hour = *hour,
                                   .compute_seconds = compute_seconds(artifact),
                                   .repository_id = repository.id}});
            }
            if (page->done || page->next_cursor.empty()) {
                break;
            }
            filter.cursor = page->next_cursor;
        }
    }
    return builds;
}

bool in_window(const std::string& hour, const Window& window) {
    const auto at_us = control::metering_hour_us(hour);
    return at_us && window.contains(*at_us / 1000);
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

} // namespace

void register_metering_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "set_storage_quota",
        .title = "Set customer storage quota",
        .description =
            "Set the customer byte quota enforced for new repository writes; zero disables it.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"quota_bytes", {{"type", "integer"}, {"minimum", 0}}}}},
                         {"required", json::array({"customer_id", "quota_bytes"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"customer_id", {{"type", "string"}}},
                            {"quota_bytes", {{"type", "integer"}}}}},
                          {"required", json::array({"customer_id", "quota_bytes"})}},
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
            const std::uint64_t quota_bytes = input.at("quota_bytes");
            const auto quota =
                context.store->set_storage_quota(input.at("customer_id"), quota_bytes);
            if (!quota) {
                return store_error(quota.error());
            }
            context.audit_customer_id = input.at("customer_id");
            context.audit_kind = "metering.quota.set";
            context.audit_object =
                "customer/" + input.at("customer_id").get<std::string>() + "/metering";
            context.audit_details = {{"quota_bytes", *quota}};
            return ApiResult::success(
                {{"customer_id", input.at("customer_id")}, {"quota_bytes", *quota}});
        }});
    registry.add(Capability{
        .name = "metering_usage",
        .title = "Customer metering usage",
        .description =
            "Return hourly repository usage, current storage and compute across repositories, "
            "images and workspaces, the storage quota, and configured pricing.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"from", {{"type", "string"}}},
                           {"to", {{"type", "string"}}},
                           {"limit", {{"type", "integer"}}}}},
                         {"required", json::array({"customer_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"customer_id", {{"type", "string"}}},
                            {"currency", {{"type", "string"}}},
                            {"quota_bytes", {{"type", "integer"}}},
                            {"over_quota", {{"type", "boolean"}}},
                            {"cost_micros", {{"type", "integer"}}},
                            {"pricing", {{"type", "object"}}},
                            {"summary", {{"type", "object"}}},
                            {"items", {{"type", "array"}}}}},
                          {"required",
                           json::array({"customer_id",
                                        "currency",
                                        "quota_bytes",
                                        "over_quota",
                                        "cost_micros",
                                        "pricing",
                                        "summary",
                                        "items"})}},
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
            if (!context.sync_store) {
                co_return ApiResult::failure("store_unavailable",
                                             "no repository store is configured");
            }
            const std::string customer_id = input.at("customer_id");
            const auto configured_quota = context.store->storage_quota(customer_id);
            if (!configured_quota) {
                co_return store_error(configured_quota.error());
            }
            const std::uint64_t quota_bytes =
                configured_quota->value_or(context.metering.storage_quota_bytes);
            const auto stored_bytes = context.sync_store->unique_stored_bytes(customer_id);
            if (!stored_bytes) {
                co_return sync_error(stored_bytes.error());
            }
            // Repository usage: transfer and build records from each workspace, this process's
            // transfer not yet written, and every build result the repositories hold now. A
            // result not yet recorded counts here and is queued for its workspace.
            const Window window = window_of(input);
            std::size_t without_workspace = 0;
            auto repository_results = repository_builds(customer_id, context, without_workspace);
            if (!repository_results) {
                co_return repository_results.error();
            }
            auto read = co_await workspace_usage(context, customer_id, window);
            if (!read) {
                co_return ApiResult::failure("store_error", read.error().message);
            }
            WorkspaceUsage measured = std::move(*read);
            std::map<std::string, HourUsage> hours;
            const auto add_transfer =
                [&](const std::string& hour, std::uint64_t in, std::uint64_t out) {
                    if (!in_window(hour, window)) {
                        return;
                    }
                    HourUsage& usage = hours[hour];
                    usage.transfer_in_bytes = add_saturated(usage.transfer_in_bytes, in);
                    usage.transfer_out_bytes = add_saturated(usage.transfer_out_bytes, out);
                };
            for (const json& record : measured.transfer) {
                add_transfer(record.value("hour", std::string()),
                             record.value("transfer_in_bytes", std::uint64_t{0}),
                             record.value("transfer_out_bytes", std::uint64_t{0}));
            }
            if (context.metering_recorder) {
                for (const PendingTransfer& transfer :
                     context.metering_recorder->pending_transfers(customer_id)) {
                    add_transfer(
                        transfer.hour, transfer.transfer_in_bytes, transfer.transfer_out_bytes);
                }
            }
            std::set<std::string> counted;
            const auto add_build = [&](const std::string& result_id,
                                       const std::string& hour,
                                       std::uint64_t seconds) {
                if (!in_window(hour, window) || !counted.insert(result_id).second) {
                    return false;
                }
                hours[hour].compute_seconds = add_saturated(hours[hour].compute_seconds, seconds);
                return true;
            };
            for (const json& record : measured.builds) {
                add_build(record.value("result_id", std::string()),
                          record.value("hour", std::string()),
                          record.value("compute_seconds", std::uint64_t{0}));
            }
            for (RepositoryBuild& result : *repository_results) {
                if (add_build(
                        result.build.result_id, result.build.hour, result.build.compute_seconds) &&
                    context.metering_recorder &&
                    measured.recorded_workspaces.contains(result.workspace_id)) {
                    context.metering_recorder->add_build(result.workspace_id,
                                                         std::move(result.build));
                }
            }
            const int limit = std::clamp(input.value("limit", 744), 1, 10000);
            json items = json::array();
            std::uint64_t cost_micros = 0;
            std::uint64_t repository_build_seconds = 0;
            for (const auto& [hour, row] : hours) {
                if (static_cast<int>(items.size()) >= limit) {
                    break;
                }
                const std::uint64_t row_cost = cost_of(row, context.metering);
                cost_micros = add_saturated(cost_micros, row_cost);
                repository_build_seconds =
                    add_saturated(repository_build_seconds, row.compute_seconds);
                items.push_back({{"hour", hour},
                                 {"transfer_in_bytes", row.transfer_in_bytes},
                                 {"transfer_out_bytes", row.transfer_out_bytes},
                                 {"compute_seconds", row.compute_seconds},
                                 {"cost_micros", row_cost}});
            }
            const bool over_quota = quota_bytes != 0 && *stored_bytes >= quota_bytes;

            // What repository usage leaves out, measured now from where it lives.
            json not_measured = json::array();
            not_measured.push_back(
                "Repository storage over time: only the current size is measured, so it is not "
                "in the hourly table or the estimated charge.");
            if (!context.metering_recorder) {
                not_measured.push_back(
                    "Repository transfer: this lake has no workspaces to record it in.");
            }
            if (without_workspace > 0) {
                not_measured.push_back(std::format(
                    "Repository transfer of {} {} without a workspace: not recorded until a "
                    "workspace is chosen.",
                    without_workspace,
                    without_workspace == 1 ? "repository" : "repositories"));
            }
            for (const std::string& line : measured.unrecorded) {
                not_measured.push_back(line);
            }
            std::optional<std::uint64_t> image_bytes;
            const auto customer = context.store->get_customer(customer_id);
            if (context.registry_space_bytes && customer) {
                image_bytes = context.registry_space_bytes(customer->name);
            }
            if (!image_bytes) {
                not_measured.push_back(
                    "Container images: this lake has no image registry storage.");
            }
            json image_builds{{"seconds", 0}, {"count", 0}};
            if (const auto builds = context.store->list_builds(customer_id)) {
                image_builds = image_build_usage(*builds, window);
            } else {
                not_measured.push_back("Image build time: " + builds.error().message);
            }
            json workspaces = std::move(measured.entries);
            std::uint64_t workspace_bytes = 0;
            std::uint64_t run_seconds = 0;
            std::uint64_t run_count = 0;
            for (const json& workspace : workspaces) {
                if (!workspace.value("measured", false)) {
                    not_measured.push_back("Workspace " + workspace.value("name", std::string()) +
                                           ": " + workspace.value("message", std::string()));
                    continue;
                }
                workspace_bytes =
                    add_saturated(workspace_bytes, workspace.value("data_bytes", std::uint64_t{0}));
                run_seconds =
                    add_saturated(run_seconds, workspace.value("run_seconds", std::uint64_t{0}));
                run_count += workspace.value("runs", std::uint64_t{0});
            }
            if (!context.workspaces) {
                not_measured.push_back("Workspace data and runs: this lake has no workspaces.");
            }
            if (!workspaces.empty()) {
                not_measured.push_back(
                    "Model files and indexes inside a workspace: the workspace does not report "
                    "their size yet.");
            }
            const std::uint64_t image_bytes_value = image_bytes.value_or(0);
            const std::uint64_t total_bytes =
                add_saturated(add_saturated(*stored_bytes, image_bytes_value), workspace_bytes);
            const std::uint64_t image_build_seconds =
                image_builds.value("seconds", std::uint64_t{0});
            const std::uint64_t total_seconds = add_saturated(
                add_saturated(repository_build_seconds, image_build_seconds), run_seconds);
            const bool priced = context.metering.transfer_in_gib_micros != 0 ||
                                context.metering.transfer_out_gib_micros != 0 ||
                                context.metering.compute_second_micros != 0;
            json summary{{"storage",
                          {{"repositories_bytes", *stored_bytes},
                           {"images_bytes", image_bytes ? json(*image_bytes) : json(nullptr)},
                           {"workspace_data_bytes", workspace_bytes},
                           {"total_bytes", total_bytes}}},
                         {"compute",
                          {{"repository_build_seconds", repository_build_seconds},
                           {"image_build_seconds", image_build_seconds},
                           {"image_builds", image_builds.value("count", std::uint64_t{0})},
                           {"workspace_run_seconds", run_seconds},
                           {"workspace_runs", run_count},
                           {"total_seconds", total_seconds}}},
                         {"workspaces", std::move(workspaces)},
                         {"not_measured", std::move(not_measured)},
                         {"priced", priced}};

            context.audit_customer_id = customer_id;
            context.audit_kind = "metering.usage.read";
            context.audit_object = "customer/" + customer_id + "/metering";
            context.audit_details = {{"stored_bytes", *stored_bytes},
                                     {"total_bytes", total_bytes},
                                     {"over_quota", over_quota}};
            co_return ApiResult::success(
                {{"customer_id", customer_id},
                 {"currency", context.metering.currency},
                 {"quota_bytes", quota_bytes},
                 {"over_quota", over_quota},
                 {"warning",
                  over_quota
                      ? "Storage quota exceeded; new writes are throttled while existing content "
                        "remains complete and readable."
                      : ""},
                 {"cost_micros", cost_micros},
                 {"pricing",
                  {{"storage_gib_hour_micros", context.metering.storage_gib_hour_micros},
                   {"transfer_in_gib_micros", context.metering.transfer_in_gib_micros},
                   {"transfer_out_gib_micros", context.metering.transfer_out_gib_micros},
                   {"compute_second_micros", context.metering.compute_second_micros}}},
                 {"summary", std::move(summary)},
                 {"items", std::move(items)}});
        }});
}

} // namespace lake::api
