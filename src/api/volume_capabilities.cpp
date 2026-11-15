// SPDX-License-Identifier: MIT

// The operator's volumes (migration 0069, operator#259, operator#260,
// docs/api/volumes.md): set_workspace_storage grows a workspace Zetta's volumes,
// request_volume_snapshot asks for a snapshot now, and volume_snapshots reads every snapshot
// policy with what the operator last reported. Each is desired state or an observation in lake's
// PostgreSQL; the operator acts on it and never talks to a workspace Zetta. They are
// for the operator credential and the instance administrator only, and their audit events stay in
// the central log: volumes are control plane, not a workspace's data.

#include "lake/api/capabilities.hpp"

#include <expected>
#include <string>

#include "lake/control/zetta_workspace.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

bool instance_administrator(const ApiContext& context) {
    return context.operator_credential || context.acting_identity == "admin";
}

// Only the operator and the instance administrator, who skip object authorization, reach the
// handler; anyone else is refused before it.
std::expected<std::string, ApiError> administrator_only(const json&, ApiContext&) {
    return std::unexpected(
        ApiError{"unauthorized", "the operator or the instance administrator is required", {}});
}

ApiResult refused() {
    return ApiResult::failure("unauthorized",
                              "the operator or the instance administrator is required");
}

// A lookup that failed because the store could not answer is store_unavailable, never a caller's
// mistake; only an unknown or malformed id is invalid_input.
bool unknown_id(const control::StoreError& error) {
    return error.code == "invalid_input" || error.code == "not_found";
}

ApiResult store_down(const control::StoreError&) {
    return ApiResult::failure("store_unavailable", "the control-plane store did not answer");
}

ApiResult no_store() {
    return ApiResult::failure("store_unavailable",
                              "no control-plane store is configured (LAKE_CONTROL_DB_URI)");
}

json snapshot_policy_json(const control::VolumeSnapshotPolicy& policy) {
    json value = {{"target", policy.target},
                  {"schedule", policy.schedule},
                  {"keep", policy.keep},
                  {"requested_at", policy.requested_at},
                  {"created_at", policy.created_at},
                  {"updated_at", policy.updated_at},
                  {"status", nullptr}};
    if (policy.status) {
        value["status"] = {{"snapshot_name", policy.status->snapshot_name},
                           {"ready_to_use", policy.status->ready_to_use},
                           {"snapshot_at", policy.status->snapshot_at},
                           {"message", policy.status->message},
                           {"updated_at", policy.status->updated_at}};
    }
    return value;
}

json snapshot_policy_schema() {
    return {{"type", "object"},
            {"properties",
             {{"target", {{"type", "string"}}},
              {"schedule", {{"type", "string"}}},
              {"keep", {{"type", "integer"}}},
              {"requested_at", {{"type", "string"}}},
              {"created_at", {{"type", "string"}}},
              {"updated_at", {{"type", "string"}}},
              {"status", {{"type", json::array({"object", "null"})}}}}}};
}

} // namespace

void register_volume_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "set_workspace_storage",
        .title = "Set workspace storage",
        .description =
            "Operator or instance administrator only. Grows each volume of a workspace's Zetta to "
            "`storage_gi` GiB. The operator expands the existing claims in place and reports "
            "`storage_gi_observed`, `storage_phase` and `storage_message` on the workspace. "
            "Volumes never shrink: a size below the current one is invalid_input.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"workspace_id", {{"type", "string"}}},
                           {"storage_gi",
                            {{"type", "integer"},
                             {"minimum", 1},
                             {"maximum", control::max_workspace_storage_gi}}}}},
                         {"required", json::array({"workspace_id", "storage_gi"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "workspace volume",
        .action = "admin",
        .resolve_object = administrator_only,
        .handler = [](const json& input, ApiContext& context) {
            if (!instance_administrator(context)) {
                return refused();
            }
            if (!context.store) {
                return no_store();
            }
            const std::string workspace_id = input.at("workspace_id");
            const auto storage_gi = input.at("storage_gi").get<std::int32_t>();
            context.audit_object = "workspace/" + workspace_id + "/storage";
            context.audit_details = {{"workspace_id", workspace_id}, {"storage_gi", storage_gi}};
            const auto before = context.store->get_workspace(workspace_id);
            if (!before && !unknown_id(before.error())) {
                return store_down(before.error());
            }
            if (!before || before->desired == "deleted") {
                return ApiResult::failure("invalid_input", "workspace_id is unknown",
                                          "/workspace_id");
            }
            context.audit_customer_id = before->customer_id;
            context.audit_details["previous_storage_gi"] = before->storage_gi;
            if (storage_gi < before->storage_gi) {
                return ApiResult::failure("invalid_input",
                                          "workspace storage never decreases: " +
                                              std::to_string(storage_gi) + " GiB is below " +
                                              std::to_string(before->storage_gi) + " GiB",
                                          "/storage_gi");
            }
            const auto updated = context.store->set_workspace_storage(workspace_id, storage_gi);
            if (!updated) {
                if (!unknown_id(updated.error())) {
                    return store_down(updated.error());
                }
                return ApiResult::failure("invalid_input", updated.error().message, "/storage_gi");
            }
            return ApiResult::success(control::workspace_json(*updated, true));
        }});
    registry.add(Capability{
        .name = "request_volume_snapshot",
        .title = "Request volume snapshot",
        .description =
            "Operator or instance administrator only. Asks the operator for a CSI snapshot of "
            "`target` now: `store` (Lake's store volume) or a workspace id (each volume of its "
            "Zetta). Optional `schedule` (five cron fields, UTC) and `keep` change the target's "
            "policy. Poll volume_snapshots until the target's status shows a snapshot taken after "
            "`requested_at` that is ready to use.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"target", {{"type", "string"}}},
                           {"schedule", {{"type", "string"}}},
                           {"keep",
                            {{"type", "integer"},
                             {"minimum", 1},
                             {"maximum", control::max_volume_snapshots_kept}}}}},
                         {"required", json::array({"target"})},
                         {"additionalProperties", false}},
        .output_schema = snapshot_policy_schema(),
        .kind = CapabilityKind::Action,
        .object = "volume snapshot",
        .action = "admin",
        .resolve_object = administrator_only,
        .handler = [](const json& input, ApiContext& context) {
            if (!instance_administrator(context)) {
                return refused();
            }
            if (!context.store) {
                return no_store();
            }
            control::VolumeSnapshotRequest request{.target = input.at("target")};
            context.audit_object = "volume/" + request.target + "/snapshot";
            context.audit_details = {{"target", request.target}};
            if (request.target != "store") {
                const auto workspace = context.store->get_workspace(request.target);
                if (!workspace && !unknown_id(workspace.error())) {
                    return store_down(workspace.error());
                }
                if (!workspace || workspace->desired == "deleted") {
                    return ApiResult::failure("invalid_input",
                                              "target must be store or a live workspace id",
                                              "/target");
                }
                context.audit_customer_id = workspace->customer_id;
            }
            if (input.contains("schedule")) {
                request.schedule = input.at("schedule").get<std::string>();
                if (!control::valid_snapshot_schedule(*request.schedule)) {
                    return ApiResult::failure(
                        "invalid_input", "schedule must be five cron fields", "/schedule");
                }
                context.audit_details["schedule"] = *request.schedule;
            }
            if (input.contains("keep")) {
                request.keep = input.at("keep").get<std::int32_t>();
                context.audit_details["keep"] = *request.keep;
            }
            const auto policy = context.store->request_volume_snapshot(request);
            if (!policy) {
                if (!unknown_id(policy.error())) {
                    return store_down(policy.error());
                }
                return ApiResult::failure("invalid_input", policy.error().message);
            }
            context.audit_details["requested_at"] = policy->requested_at;
            return ApiResult::success(snapshot_policy_json(*policy));
        }});
    registry.add(Capability{
        .name = "volume_snapshots",
        .title = "Volume snapshots",
        .description =
            "Operator or instance administrator only. Every volume snapshot policy (`store` "
            "first, then each workspace) with the operator's status: the last snapshot's name, "
            "whether it is ready to use, when it was taken, and why not. With `target`, that "
            "target's policy alone.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"target", {{"type", "string"}}}}},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"policies", {{"type", "array"}, {"items", snapshot_policy_schema()}}}}}},
        .kind = CapabilityKind::Read,
        .object = "volume snapshot",
        .action = "read",
        .resolve_object = administrator_only,
        .handler = [](const json& input, ApiContext& context) {
            if (!instance_administrator(context)) {
                return refused();
            }
            if (!context.store) {
                return no_store();
            }
            const std::string target = input.value("target", std::string());
            const auto policies = context.store->volume_snapshots();
            if (!policies) {
                return store_down(policies.error());
            }
            json values = json::array();
            for (const control::VolumeSnapshotPolicy& policy : *policies) {
                if (target.empty() || policy.target == target) {
                    values.push_back(snapshot_policy_json(policy));
                }
            }
            if (!target.empty() && values.empty()) {
                return ApiResult::failure("invalid_input", "target has no snapshot policy",
                                          "/target");
            }
            return ApiResult::success({{"policies", std::move(values)}});
        }});
}

} // namespace lake::api
