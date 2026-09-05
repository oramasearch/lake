#include "lake/api/capabilities.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>

#include "lake/api/sync_store.hpp"
#include "lake/control/store.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

ApiResult store_error(const control::StoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

ApiResult sync_error(const SyncStoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

std::string current_hour() {
    return std::format("{:%FT%H}:00:00Z",
                       std::chrono::floor<std::chrono::hours>(std::chrono::system_clock::now()));
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

std::uint64_t cost_of(const control::MeteringUsage& usage, const MeteringConfig& pricing) {
    constexpr std::uint64_t gib = std::uint64_t{1} << 30U;
    std::uint64_t cost = priced_units(usage.stored_bytes, gib, pricing.storage_gib_hour_micros);
    cost = add_saturated(
        cost, priced_units(usage.transfer_in_bytes, gib, pricing.transfer_in_gib_micros));
    cost = add_saturated(
        cost, priced_units(usage.transfer_out_bytes, gib, pricing.transfer_out_gib_micros));
    return add_saturated(cost,
                         multiply_saturated(usage.compute_seconds, pricing.compute_second_micros));
}

ApiResult reconcile_builds(const std::string& customer_id, ApiContext& context) {
    const auto repositories = context.store->list_repositories(customer_id, 1000);
    if (!repositories) {
        return store_error(repositories.error());
    }
    for (const control::Repository& repository : *repositories) {
        RepositoryArtifactFilter filter;
        filter.limit = 1000;
        for (;;) {
            const auto page =
                context.sync_store->repository_artifacts(customer_id, repository.id, filter);
            if (!page) {
                return sync_error(page.error());
            }
            for (const RepositoryArtifact& artifact : page->items) {
                const auto hour = hour_of(artifact);
                if (!hour) {
                    continue;
                }
                const auto recorded = context.store->record_metered_build(
                    {.customer_id = customer_id,
                     .result_id = artifact.id,
                     .hour = *hour,
                     .compute_seconds = compute_seconds(artifact)});
                if (!recorded) {
                    return store_error(recorded.error());
                }
            }
            if (page->done || page->next_cursor.empty()) {
                break;
            }
            filter.cursor = page->next_cursor;
        }
    }
    return ApiResult::success(json::object());
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
        .name = "metering_usage",
        .title = "Customer metering usage",
        .description = "Return hourly storage, transfer, compute, quota, and configured pricing.",
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
                            {"items", {{"type", "array"}}}}},
                          {"required",
                           json::array({"customer_id",
                                        "currency",
                                        "quota_bytes",
                                        "over_quota",
                                        "cost_micros",
                                        "pricing",
                                        "items"})}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .resolves_operator_audit_target = true,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return ApiResult::failure("store_unavailable",
                                          "no control-plane store is configured");
            }
            if (!context.sync_store) {
                return ApiResult::failure("store_unavailable", "no repository store is configured");
            }
            const std::string customer_id = input.at("customer_id");
            const auto stored_bytes = context.sync_store->unique_stored_bytes(customer_id);
            if (!stored_bytes) {
                return sync_error(stored_bytes.error());
            }
            const auto current =
                context.store->upsert_metering_usage({.customer_id = customer_id,
                                                      .hour = current_hour(),
                                                      .stored_bytes = *stored_bytes});
            if (!current) {
                return store_error(current.error());
            }
            const ApiResult reconciled = reconcile_builds(customer_id, context);
            if (!reconciled.ok) {
                return reconciled;
            }
            const int limit = std::clamp(input.value("limit", 744), 1, 10000);
            const auto usage = context.store->list_metering_usage(
                customer_id, input.value("from", ""), input.value("to", ""), limit);
            if (!usage) {
                return store_error(usage.error());
            }
            json items = json::array();
            std::uint64_t cost_micros = 0;
            for (const control::MeteringUsage& row : *usage) {
                const std::uint64_t row_cost = cost_of(row, context.metering);
                cost_micros = add_saturated(cost_micros, row_cost);
                items.push_back({{"hour", row.hour},
                                 {"stored_bytes", row.stored_bytes},
                                 {"transfer_in_bytes", row.transfer_in_bytes},
                                 {"transfer_out_bytes", row.transfer_out_bytes},
                                 {"compute_seconds", row.compute_seconds},
                                 {"cost_micros", row_cost}});
            }
            const bool over_quota = context.metering.storage_quota_bytes != 0 &&
                                    *stored_bytes >= context.metering.storage_quota_bytes;
            context.audit_customer_id = customer_id;
            context.audit_kind = "metering.usage.read";
            context.audit_object = "customer/" + customer_id + "/metering";
            context.audit_details = {{"stored_bytes", *stored_bytes}, {"over_quota", over_quota}};
            return ApiResult::success(
                {{"customer_id", customer_id},
                 {"currency", context.metering.currency},
                 {"quota_bytes", context.metering.storage_quota_bytes},
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
                 {"items", std::move(items)}});
        }});
}

} // namespace lake::api
