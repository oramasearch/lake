// move_workspace_store (lake#380, decision 54 S1-S3, docs/api/workspace-store.md): the operator
// moves one workspace's repositories from its space's liquid store on lake's volume into the
// workspace's own store in its workspace Zetta, one workspace at a time. `plan` lists what moves,
// `copy` copies each repository with liquid's repository::transfer (liquid#328; a rerun copies only
// what is missing), `verify` compares every repository in both stores, and `switch` holds the
// repository requests, copies what arrived since, verifies again and records
// workspaces.store_moved_at, after which the workspace's repositories are served from its store.
// The space's store is never written by the move and stays on the volume. `provision` repairs a
// workspace switched with no repository before a switch created its store (lake#383 phase B): it
// creates the store only for a workspace that never held anything.
//
// remove_space_store then removes a space's old store (<root>/<space>/store and
// <root>/<space>/repositories) from lake's volume, once every workspace of the space is switched
// and no repository of the space lacks a workspace. A dry run reports the paths and bytes.

#include <chrono>
#include <cstdio>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "lake/api/capabilities.hpp"
#include "lake/control/oci_records.hpp"
#include "lake/control/store.hpp"
#include "lake/search/index.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

// Every repository of a space (or of one of its workspaces). list_repositories has no cursor, so
// the limit grows until a page comes back short: nothing is cut off at a fixed size.
control::Result<std::vector<control::Repository>>
all_repositories(control::Store& store, const std::string& customer_id,
                 const std::string& workspace_id = {}) {
    for (int limit = 1000;; limit = limit > (1 << 28) ? std::numeric_limits<int>::max() : limit * 8) {
        auto page = store.list_repositories(customer_id, limit, workspace_id);
        if (!page || static_cast<long long>(page->size()) < limit ||
            limit == std::numeric_limits<int>::max()) {
            return page;
        }
    }
}

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
                                  "workspace stores are not configured on this Lake");
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
    const auto rows = all_repositories(*context.store, customer_id);
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
            "conflict", "the account has repositories without a workspace; assign them first "
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
            const auto current = all_repositories(*context.store, customer_id, workspace_id);
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
        // lake#381: the space's index on lake's volume keeps nothing of the switched workspace;
        // its own index is rebuilt from its store on lake's maintenance thread, never inside a
        // request.
        if (context.search) {
            context.search->schedule_rebuild(customer_id, workspace_id);
            const auto dropped = context.search->drop_switched(customer_id);
            if (dropped) {
                output["space_index_repositories_dropped"] = *dropped;
            } else {
                output["space_index_error"] = dropped.error().message;
            }
        }
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

bool is_uuid(const std::string& value) {
    static const std::regex pattern(
        "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
    return std::regex_match(value, pattern);
}

// What still needs the space's store: a live workspace not switched yet, a repository without a
// workspace (live or tombstoned), and a live repository of a deleted workspace (never moved, since
// a deleted workspace is not switched). Empty when the store may go.
SyncStoreResult<std::vector<std::string>> space_store_blockers(ApiContext& context,
                                                               const std::string& customer_id) {
    const auto workspaces = context.store->list_workspaces(customer_id);
    if (!workspaces) {
        return std::unexpected(SyncStoreError{workspaces.error().code, workspaces.error().message});
    }
    const auto repositories = all_repositories(*context.store, customer_id);
    if (!repositories) {
        return std::unexpected(SyncStoreError{repositories.error().code, repositories.error().message});
    }
    std::vector<std::string> blockers;
    std::map<std::string, const control::Workspace*> by_id;
    for (const control::Workspace& workspace : *workspaces) {
        by_id.emplace(workspace.id, &workspace);
        if (workspace.desired != "deleted" && workspace.store_moved_at.empty()) {
            blockers.push_back("workspace " + workspace.id + " (" + workspace.name +
                               ") is not switched to its own store");
        }
    }
    for (const control::Repository& repository : *repositories) {
        if (repository.workspace_id.empty()) {
            blockers.push_back("repository " + repository.id + " (" + repository.name +
                               ") has no workspace");
            continue;
        }
        const auto found = by_id.find(repository.workspace_id);
        const bool deleted = found == by_id.end() || found->second->desired == "deleted";
        if (deleted && repository.deleted_at.empty()) {
            blockers.push_back("repository " + repository.id + " (" + repository.name +
                               ") is live in a deleted workspace");
        }
    }
    return blockers;
}

ApiResult remove_space_store(const json& input, ApiContext& context) {
    if (!context.operator_credential) {
        return ApiResult::failure("unauthorized", "operator credential is required");
    }
    if (!context.store) {
        return ApiResult::failure("store_unavailable", "no control-plane store is configured");
    }
    const std::string customer_id = input.at("customer_id");
    const bool dry_run = input.value("dry_run", false);
    context.audit_kind = "workspace_store.remove_space_store";
    context.audit_details = {{"customer_id", customer_id}, {"dry_run", dry_run}};
    // The space id names a directory directly under the store root: only a UUID, never the
    // instance's own directory.
    if (!is_uuid(customer_id) || customer_id == context.node_name) {
        return ApiResult::failure("invalid_input", "customer_id must be an account's UUID", "/customer_id");
    }
    if (const auto customer = context.store->get_customer(customer_id); !customer) {
        return ApiResult::failure(customer.error().code, customer.error().message, "/customer_id");
    }
    // Checked first without any hold (so a refusal never waits on the space's lane), then again
    // under the hold, where a workspace created meanwhile is seen.
    const auto check = [&]() -> SyncStoreResult<void> {
        auto found = space_store_blockers(context, customer_id);
        if (!found) {
            return std::unexpected(found.error());
        }
        const std::vector<std::string>& blockers = *found;
        if (!blockers.empty()) {
            std::string message = "the account's store is still needed; nothing was removed:";
            for (std::size_t index = 0; index < blockers.size() && index < 20; ++index) {
                message += (index == 0 ? " " : "; ") + blockers[index];
            }
            if (blockers.size() > 20) {
                message += "; and " + std::to_string(blockers.size() - 20) + " more";
            }
            return std::unexpected(SyncStoreError{"conflict", message});
        }
        return {};
    };
    if (auto checked = check(); !checked) {
        return sync_failure(checked.error());
    }
    if (!context.sync_store) {
        return ApiResult::failure("store_unavailable",
                                  "account stores are not configured on this Lake");
    }
    auto removal = context.sync_store->remove_space_store(customer_id, dry_run, check);
    if (!removal) {
        return sync_failure(removal.error());
    }
    json directories = json::array();
    for (const SpaceStoreDirectory& directory : removal->directories) {
        directories.push_back({{"path", directory.path},
                               {"exists", directory.exists},
                               {"bytes", directory.bytes},
                               {"files", directory.files}});
    }
    context.audit_details["bytes"] = removal->bytes;
    context.audit_details["removed"] = removal->removed;
    return ApiResult::success({{"customer_id", customer_id},
                               {"dry_run", dry_run},
                               {"removed", removal->removed},
                               {"bytes", removal->bytes},
                               {"directories", std::move(directories)}});
}

// A control-store timestamp (`YYYY-MM-DDTHH:MM:SS...` or PostgreSQL's `YYYY-MM-DD HH:MM:SS...`,
// both UTC) to the second; nullopt for an empty or malformed one.
std::optional<std::chrono::sys_seconds> instant_of(const std::string& text) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    char separator = 0;
    if (text.size() < 19 ||
        std::sscanf(text.c_str(), "%4d-%2d-%2d%c%2d:%2d:%2d", &year, &month, &day, &separator,
                    &hour, &minute, &second) != 7 ||
        (separator != 'T' && separator != ' ')) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year(year), std::chrono::month(month),
                                           std::chrono::day(day)};
    if (!date.ok()) {
        return std::nullopt;
    }
    return std::chrono::sys_days(date) + std::chrono::hours(hour) + std::chrono::minutes(minute) +
           std::chrono::seconds(second);
}

// `provision`: a workspace switched by a lake that did not yet create the store of a workspace
// without repositories has no store, and a switched workspace's store is never created implicitly.
// This creates it only when the workspace never held anything: it is switched, no repository row
// (live or tombstoned) names it, no image blob record names a store object, and its bucket is
// absent or empty (a row tombstoned with its retention ended at its deletion is a creation the
// missing store refused, and does not count). Otherwise `conflict` names what exists, so a wiped store of a workspace that
// held data is still never recreated empty. A store that already opens is left as it is.
liquid::runtime::task<ApiResult> provision_workspace_store(const json& input, ApiContext& context) {
    if (!context.operator_credential) {
        co_return ApiResult::failure("unauthorized", "operator credential is required");
    }
    if (!context.store) {
        co_return ApiResult::failure("store_unavailable", "no control-plane store is configured");
    }
    if (!context.sync_store || !context.workspaces) {
        co_return ApiResult::failure("store_unavailable",
                                     "workspace stores are not configured on this Lake");
    }
    const std::string workspace_id = input.at("workspace_id");
    context.audit_kind = "workspace_store.move";
    context.audit_details = {{"workspace_id", workspace_id}, {"mode", "provision"}};
    // The control-store reads run on the blocking pool, off the server thread.
    struct Rows {
        control::Workspace workspace;
        std::vector<control::Repository> repositories;
    };
    auto read = co_await blocking_call(
        [&]() -> std::expected<Rows, ApiResult> {
            auto workspace = context.store->get_workspace(workspace_id);
            if (!workspace) {
                return std::unexpected(
                    ApiResult::failure(workspace.error().code, workspace.error().message));
            }
            auto listed = all_repositories(*context.store, workspace->customer_id,
                                                           workspace_id);
            if (!listed) {
                return std::unexpected(ApiResult::failure(listed.error().code, listed.error().message));
            }
            return Rows{std::move(*workspace), std::move(*listed)};
        },
        [](std::string message) -> std::expected<Rows, ApiResult> {
            return std::unexpected(ApiResult::failure("store_unavailable", std::move(message)));
        });
    if (!read) {
        co_return read.error();
    }
    const control::Workspace& workspace = read->workspace;
    if (workspace.desired == "deleted") {
        co_return ApiResult::failure("conflict", "the workspace is deleted");
    }
    if (workspace.store_moved_at.empty()) {
        co_return ApiResult::failure(
            "conflict", "the workspace is not switched; its store is created by copy and switch");
    }
    const std::string customer_id = workspace.customer_id;
    const std::vector<control::Repository>* repositories = &read->repositories;
    // A row tombstoned with its retention ended at its deletion is a creation the missing store
    // refused (the creation rolls its row back so): it never held content. Any other row, live or
    // tombstoned, means the store may have held data.
    json names = json::array();
    json refused_creations = json::array();
    for (const control::Repository& repository : *repositories) {
        const auto deleted = instant_of(repository.deleted_at);
        const auto retained = instant_of(repository.retention_until);
        if (deleted && retained && *retained <= *deleted + std::chrono::seconds(2)) {
            refused_creations.push_back(repository.id);
            continue;
        }
        names.push_back(repository.id + " (" + repository.name +
                        (repository.deleted_at.empty() ? ", live)" : ", tombstoned)"));
    }
    if (!names.empty()) {
        co_return ApiResult::failure(
            "conflict", "the workspace has repositories, so its store may have held data and is "
                        "not recreated empty: " + names.dump());
    }
    auto records = workspace_records(context, customer_id, workspace_id);
    if (!records) {
        co_return ApiResult::failure(records.error().code, records.error().message);
    }
    auto blobs = control::settled(co_await control::list_oci_blobs(*records, customer_id));
    if (!blobs) {
        co_return ApiResult::failure(blobs.error().code, blobs.error().message);
    }
    std::size_t stored = 0;
    for (const control::OciBlob& blob : *blobs) {
        stored += blob.object.empty() ? 0 : 1;
    }
    if (stored != 0) {
        co_return ApiResult::failure(
            "conflict", "the workspace records " + std::to_string(stored) +
                            " image blobs in its store, so its store held data and is not "
                            "recreated empty");
    }
    auto provisioned = co_await blocking_call(
        [&] { return context.sync_store->provision_workspace_store(customer_id, workspace_id); },
        [](std::string message) -> SyncStoreResult<WorkspaceStoreProvision> {
            return std::unexpected(SyncStoreError{"store_unavailable", std::move(message)});
        });
    if (!provisioned) {
        co_return sync_failure(provisioned.error());
    }
    context.audit_details["created"] = provisioned->created;
    co_return ApiResult::success({{"workspace_id", workspace_id},
                                  {"customer_id", customer_id},
                                  {"mode", "provision"},
                                  {"store_moved_at", workspace.store_moved_at},
                                  {"created", provisioned->created},
                                  {"refused_creations", std::move(refused_creations)},
                                  {"objects", provisioned->objects}});
}

} // namespace

void register_workspace_store_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "remove_space_store",
        .title = "Remove an account's old store from Lake's volume",
        .description =
            "Operator only (lake#380). Removes an account's liquid store from Lake's volume "
            "(`<LAKE_STORE_ROOT>/<customer_id>/store` and `<customer_id>/repositories`) once every "
            "live workspace of the account is switched to its own store, no repository of the "
            "account lacks "
            "a workspace and no live repository belongs to a deleted workspace; otherwise "
            "`conflict`, removing nothing. It refuses the account store's requests and waits for "
            "the running ones, closes the store and deletes the two directories, never anything "
            "else. `dry_run` reports the directories and bytes without removing or closing "
            "anything. An account store is created again, empty, if a workspace of the account that is "
            "not switched later uses it.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"dry_run", {{"type", "boolean"}}}}},
                         {"required", json::array({"customer_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "space store removal",
        .action = "admin",
        .resolve_object = move_object,
        .handler = remove_space_store});

    registry.add(Capability{
        .name = "move_workspace_store",
        .title = "Move a workspace's repositories into its own store",
        .description =
            "Operator only (lake#380). Moves one workspace's repositories from its account's liquid "
            "store on Lake's volume into the workspace's own encrypted store in its workspace "
            "Zetta. `plan` lists the repositories and their sizes and any repository of the account "
            "without a workspace, which blocks the move; `copy` copies each repository (a rerun "
            "copies only what is missing); `verify` compares every repository in both stores; "
            "`switch` holds repository requests, copies what arrived since, verifies again and "
            "serves the workspace's repositories from its store from then on. The account's store "
            "is never written and is not removed. `provision` creates the store of a switched "
            "workspace that never held anything (no repository, no stored image blob, an absent "
            "or empty bucket) and refuses with conflict otherwise; repeated, it changes nothing.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"workspace_id", {{"type", "string"}}},
                           {"mode",
                            {{"type", "string"},
                             {"enum",
                              json::array({"plan", "copy", "verify", "switch", "provision"})}}}}},
                         {"required", json::array({"workspace_id", "mode"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "workspace store move",
        .action = "admin",
        .resolve_object = move_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (input.at("mode") == "provision") {
                co_return co_await provision_workspace_store(input, context);
            }
            // Every other mode waits on repository stores: on the blocking pool (lake#380).
            co_return co_await blocking_call(
                [&] { return move_workspace_store(input, context); },
                [](std::string message) {
                    return ApiResult::failure("store_unavailable", std::move(message));
                });
        }});
}

} // namespace lake::api
