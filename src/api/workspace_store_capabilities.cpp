// move_workspace_store (lake#380, decision 54 S1-S3, docs/api/workspace-store.md): the operator
// moves one workspace's repositories from its space's liquid store on lake's volume into the
// workspace's own store in its workspace Zetta, one workspace at a time. `plan` lists what moves,
// `copy` copies each repository with liquid's repository::transfer (liquid#328; a rerun copies only
// what is missing), `verify` compares every repository in both stores, and `switch` holds the
// repository requests, copies what arrived since, verifies again and records
// workspaces.store_moved_at, after which the workspace's repositories are served from its store.
// The space's store is never written by the move and stays on the volume.

#include <string>
#include <vector>

#include "lake/api/capabilities.hpp"
#include "lake/control/store.hpp"

namespace lake::api {
namespace {

using json = nlohmann::json;

std::expected<std::string, ApiError> move_object(const json&, ApiContext&) {
    return std::unexpected(ApiError{"unauthorized", "operator credential is required", {}});
}

json transfer_json(const WorkspaceStoreTransfer& transfer) {
    json differences = json::array();
    for (const auto& [subject, reason] : transfer.differences) {
        differences.push_back({{"subject", subject}, {"reason", reason}});
    }
    return {{"repository_id", transfer.repository_id},
            {"absent", transfer.absent},
            {"complete", transfer.complete},
            {"objects", transfer.objects},
            {"objects_present", transfer.objects_present},
            {"objects_written", transfer.objects_written},
            {"bytes_written", transfer.bytes_written},
            {"state_written", transfer.state_written},
            {"leases_written", transfer.leases_written},
            {"missing", transfer.missing},
            {"differences", std::move(differences)}};
}

ApiResult sync_failure(const SyncStoreError& error) {
    return ApiResult::failure(error.code.empty() ? "store_error" : error.code, error.message);
}

ApiResult move_workspace_store(const json& input, ApiContext& context) {
    if (!context.operator_credential) {
        return ApiResult::failure("unauthorized", "operator credential is required");
    }
    if (!context.store) {
        return ApiResult::failure("store_unavailable", "no control-plane store is configured");
    }
    if (!context.sync_store || !context.workspaces) {
        return ApiResult::failure("store_unavailable",
                                  "workspace stores are not configured on this lake");
    }
    const std::string workspace_id = input.at("workspace_id");
    const std::string mode = input.at("mode");
    context.audit_kind = "workspace_store.move";
    context.audit_details = {{"workspace_id", workspace_id}, {"mode", mode}};
    const auto workspace = context.store->get_workspace(workspace_id);
    if (!workspace) {
        return ApiResult::failure(workspace.error().code, workspace.error().message);
    }
    if (workspace->desired == "deleted") {
        return ApiResult::failure("conflict", "the workspace is deleted");
    }
    const std::string& customer_id = workspace->customer_id;
    const auto rows = context.store->list_repositories(customer_id, 100000);
    if (!rows) {
        return ApiResult::failure(rows.error().code, rows.error().message);
    }
    std::vector<control::Repository> repositories;
    json unassigned = json::array();
    for (const control::Repository& repository : *rows) {
        if (repository.workspace_id == workspace_id) {
            repositories.push_back(repository);
        } else if (repository.workspace_id.empty()) {
            unassigned.push_back({{"id", repository.id}, {"name", repository.name}});
        }
    }
    json output = {{"workspace_id", workspace_id},
                   {"customer_id", customer_id},
                   {"mode", mode},
                   {"store_moved_at", workspace->store_moved_at.empty()
                                          ? json(nullptr)
                                          : json(workspace->store_moved_at)},
                   {"unassigned", unassigned}};
    const bool blocked = !unassigned.empty();
    if (mode == "plan") {
        json listed = json::array();
        for (const control::Repository& repository : repositories) {
            json row = {{"id", repository.id},
                        {"name", repository.name},
                        {"state", repository.deleted_at.empty() ? "live" : "tombstoned"}};
            const auto storage = context.sync_store->repository_storage(customer_id, repository.id);
            if (storage) {
                row["logical_bytes"] = storage->logical_bytes;
                row["unique_bytes"] = storage->unique_bytes;
            } else {
                row["logical_bytes"] = nullptr;
                row["unique_bytes"] = nullptr;
                row["size_error"] = storage.error().message;
            }
            listed.push_back(std::move(row));
        }
        output["repositories"] = std::move(listed);
        output["blocked"] = blocked;
        return ApiResult::success(std::move(output));
    }
    if (blocked) {
        return ApiResult::failure(
            "conflict", "the space has repositories without a workspace; assign them first "
                        "(plan lists them under unassigned)");
    }
    if (mode == "switch") {
        if (!workspace->store_moved_at.empty()) {
            output["switched"] = true;
            output["repositories"] = json::array();
            return ApiResult::success(std::move(output));
        }
        std::string moved_at;
        // The repositories are listed while the switch holds the workspace's requests and
        // creations, so one created meanwhile is not missed.
        const auto list = [&]() -> SyncStoreResult<std::vector<std::string>> {
            const auto current = context.store->list_repositories(customer_id, 100000, workspace_id);
            if (!current) {
                return std::unexpected(SyncStoreError{current.error().code, current.error().message});
            }
            std::vector<std::string> identifiers;
            for (const control::Repository& repository : *current) {
                identifiers.push_back(repository.id);
            }
            return identifiers;
        };
        auto switched = context.sync_store->switch_workspace_store(
            customer_id, workspace_id, list, [&]() -> SyncStoreResult<void> {
                const auto marked = context.store->mark_workspace_store_moved(workspace_id);
                if (!marked) {
                    return std::unexpected(
                        SyncStoreError{marked.error().code, marked.error().message});
                }
                moved_at = marked->store_moved_at;
                return {};
            });
        if (!switched) {
            return sync_failure(switched.error());
        }
        json transfers = json::array();
        for (const WorkspaceStoreTransfer& transfer : *switched) {
            transfers.push_back(transfer_json(transfer));
        }
        output["repositories"] = std::move(transfers);
        output["switched"] = true;
        output["store_moved_at"] = moved_at;
        return ApiResult::success(std::move(output));
    }
    if (!workspace->store_moved_at.empty() && mode == "copy") {
        return ApiResult::failure("conflict",
                                  "the workspace is already served from its own store");
    }
    const bool verify = mode == "verify";
    json transfers = json::array();
    bool complete = true;
    for (const control::Repository& repository : repositories) {
        auto transfer = context.sync_store->transfer_to_workspace_store(
            customer_id, repository.id, workspace_id, verify);
        if (!transfer) {
            return sync_failure(transfer.error());
        }
        complete = complete && transfer->complete;
        transfers.push_back(transfer_json(*transfer));
    }
    output["repositories"] = std::move(transfers);
    output["complete"] = complete;
    return ApiResult::success(std::move(output));
}

} // namespace

void register_workspace_store_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "move_workspace_store",
        .title = "Move a workspace's repositories into its own store",
        .description =
            "Operator only (lake#380). Moves one workspace's repositories from its space's liquid "
            "store on lake's volume into the workspace's own encrypted store in its workspace "
            "Zetta. `plan` lists the repositories and their sizes and any repository of the space "
            "without a workspace, which blocks the move; `copy` copies each repository (a rerun "
            "copies only what is missing); `verify` compares every repository in both stores; "
            "`switch` holds repository requests, copies what arrived since, verifies again and "
            "serves the workspace's repositories from its store from then on. The space's store "
            "is never written and is not removed.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"workspace_id", {{"type", "string"}}},
                           {"mode",
                            {{"type", "string"},
                             {"enum", json::array({"plan", "copy", "verify", "switch"})}}}}},
                         {"required", json::array({"workspace_id", "mode"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "workspace store move",
        .action = "admin",
        .resolve_object = move_object,
        .handler = move_workspace_store});
}

} // namespace lake::api
