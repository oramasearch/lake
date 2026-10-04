// The one-shot copy of a retired control-store table into the owning workspace Zettas
// (docs/architecture/workspace-records.md, lake#370). One operator capability serves every
// collection that moves: each follow-up adds its collection to `copy_collections` and a copier
// (opportunities, lake#376; stream_owners, lake#379; drive_share_links, lake#350;
// auth_app_webhook_events and auth_app_webhook_deliveries, lake#356 and lake#357; metering_usage
// and metering_build_records, lake#348 and lake#349; forge_records, lake#341; auth_app_users and
// auth_app_organizations, lake#353 and lake#354; auth_app_signing_keys and auth_app_webhooks,
// lake#352 and lake#355; the five secrets tables, lake#328-#332; dynamic leases, lake#362;
// auth_applications, the settings of each application, lake#351; platform_environments and
// platform_mirror, lake#358 and lake#359; agent_tasks, the content of each agent task, lake#374
// and lake#375; audit_events, the central events about a workspace's data, lake#317;
// repositories, the description of each repository, lake#315 part 2; oci_tags, the tenant tags
// of the instance registry and the manifests they name, lake#383). A collection
// leaves the list when the migration that drops its table lands (ask_messages, lake#370: copied
// and verified on dev, dropped by migration 0050, lake#483).

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "lake/api/audit_routing.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/images.hpp"
#include "lake/api/workspace_deletion.hpp"
#include "lake/control/agent_task_records.hpp"
#include "lake/control/application_authorization_codes.hpp"
#include "lake/control/application_user_records.hpp"
#include "lake/control/application_credential_records.hpp"
#include "lake/control/application_settings_records.hpp"
#include "lake/control/application_webhook_records.hpp"
#include "lake/control/drive_share_links.hpp"
#include "lake/control/forge_records.hpp"
#include "lake/control/metering.hpp"
#include "lake/control/oci_records.hpp"
#include "lake/control/opportunities.hpp"
#include "lake/control/platform_records.hpp"
#include "lake/control/repository_records.hpp"
#include "lake/control/secret_records.hpp"
#include "lake/control/store.hpp"
#include "lake/control/stream_owners.hpp"
#include "lake/control/workspace_audit.hpp"
#include "secret_records_api.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

using json = nlohmann::json;

constexpr std::array<std::string_view, 26> copy_collections = {"opportunities",
                                                               "stream_owners",
                                                               "drive_share_links",
                                                               "auth_app_webhook_events",
                                                               "auth_app_webhook_deliveries",
                                                               "metering_usage",
                                                               "metering_build_records",
                                                               "forge_records",
                                                               "auth_app_users",
                                                               "auth_app_organizations",
                                                               "auth_app_signing_keys",
                                                               "auth_app_webhooks",
                                                               "secrets_mounts",
                                                               "secrets_data_keys",
                                                               "secrets_encryption_keys",
                                                               "secrets_entries",
                                                               "secrets_versions",
                                                               "secrets_dynamic_leases",
                                                               "auth_applications",
                                                               "platform_environments",
                                                               "platform_mirror",
                                                               "auth_app_authorization_codes",
                                                               "agent_tasks",
                                                               "audit_events",
                                                               "repositories",
                                                               "oci_tags"};

struct WorkspaceTally {
    std::string customer_id;
    std::size_t rows = 0, copied = 0, already_present = 0, verified = 0;
    json missing = json::array(), mismatched = json::array();
    std::string error;
};

// The answer of one page of a copy, the same for every collection.
// `blocking`: whether the unresolved rows keep the page from being complete (by default, any).
ApiResult copy_result(std::string_view collection,
                      const std::string& mode,
                      std::size_t rows,
                      int limit,
                      const std::string& last_id,
                      json unresolved,
                      const std::map<std::string, WorkspaceTally>& tallies,
                      std::optional<bool> blocking = std::nullopt) {
    json workspaces = json::array();
    std::size_t copied = 0, already_present = 0, verified = 0, failed = 0;
    json missing = json::array(), mismatched = json::array();
    for (const auto& [workspace_id, tally] : tallies) {
        copied += tally.copied;
        already_present += tally.already_present;
        verified += tally.verified;
        failed += tally.error.empty() ? 0 : 1;
        for (const auto& id : tally.missing) {
            missing.push_back(id);
        }
        for (const auto& id : tally.mismatched) {
            mismatched.push_back(id);
        }
        json entry{{"workspace_id", workspace_id}, {"customer_id", tally.customer_id},
                   {"rows", tally.rows}};
        if (mode == "copy") {
            entry["copied"] = tally.copied;
            entry["already_present"] = tally.already_present;
        } else if (mode == "verify") {
            entry["verified"] = tally.verified;
            entry["missing"] = tally.missing;
            entry["mismatched"] = tally.mismatched;
        }
        if (!tally.error.empty()) {
            entry["error"] = tally.error;
        }
        workspaces.push_back(std::move(entry));
    }
    const bool all_assigned = blocking ? !*blocking : unresolved.empty();
    json value{{"collection", std::string(collection)},
               {"mode", mode},
               {"rows", rows},
               {"next_after", last_id.empty() || static_cast<int>(rows) < limit
                                  ? json(nullptr)
                                  : json(last_id)},
               {"assigned", rows - unresolved.size()},
               {"unresolved", std::move(unresolved)},
               {"workspaces", std::move(workspaces)},
               {"failed_workspaces", failed}};
    if (mode == "copy") {
        value["copied"] = copied;
        value["already_present"] = already_present;
    } else if (mode == "verify") {
        value["verified"] = verified;
        value["missing"] = std::move(missing);
        value["mismatched"] = std::move(mismatched);
        // Complete: every row of this page has one owner and is in its workspace, unchanged. Only
        // then may a later migration drop what this page covered.
        value["complete"] = all_assigned && failed == 0 && value["missing"].empty() &&
                            value["mismatched"].empty();
    }
    return ApiResult::success(std::move(value));
}

bool same_opportunity(const control::Opportunity& stored, const control::Opportunity& source) {
    const bool same_scan_row =
        stored.customer_id == source.customer_id && stored.task_id == source.task_id &&
        stored.rank == source.rank && stored.intent == source.intent &&
        stored.solution == source.solution && stored.methodology == source.methodology &&
        stored.why == source.why && stored.payload == source.payload && stored.at_us == source.at_us;
    // Lake writes only the workspace record after the cutover, so a status the person changed
    // since the copy (a later lake revision) is theirs, not a difference.
    return same_scan_row && (stored.lake_revision > 1 ||
                             (stored.status == source.status && stored.project == source.project));
}

// One page of opportunities: assignments first (so rows an old replica wrote after migration 0046
// are owned too), then per workspace one listing of the space's Opportunities and the missing
// appends. Each row is keyed by its scan and rank (control::opportunity_id), the key a settle of
// that scan writes, so an old scan read again after the copy is not saved twice.
liquid::runtime::task<ApiResult> copy_opportunities(ApiContext& context, const std::string& mode,
                                                    const std::string& after, int limit) {
    if (auto assigned = context.store->assign_legacy_opportunities(); !assigned) {
        co_return ApiResult::failure(assigned.error().code, assigned.error().message);
    }
    auto rows = context.store->legacy_opportunities(after, limit);
    if (!rows) {
        co_return ApiResult::failure(rows.error().code, rows.error().message);
    }
    json unresolved = json::array();
    std::map<std::string, std::vector<const control::LegacyOpportunity*>> by_workspace;
    for (const control::LegacyOpportunity& row : *rows) {
        if (row.workspace_id.empty() || control::opportunity_problem(row.opportunity)) {
            unresolved.push_back({{"id", row.opportunity.id},
                                  {"customer_id", row.opportunity.customer_id},
                                  {"outcome", row.workspace_id.empty() ? row.outcome
                                                                       : std::string("invalid")}});
        } else {
            by_workspace[row.workspace_id].push_back(&row);
        }
    }
    std::map<std::string, WorkspaceTally> tallies;
    for (const auto& [workspace_id, owned] : by_workspace) {
        WorkspaceTally& tally = tallies[workspace_id];
        tally.customer_id = owned.front()->opportunity.customer_id;
        tally.rows = owned.size();
        if (mode == "plan") {
            continue;
        }
        auto records = workspace_records(context, tally.customer_id, workspace_id);
        if (!records) {
            tally.error = records.error().code + ": " + records.error().message;
            continue;
        }
        auto present = control::settled(
            co_await control::list_opportunities(*records, tally.customer_id));
        if (!present) {
            tally.error = present.error().code + ": " + present.error().message;
            continue;
        }
        std::map<std::string, const control::Opportunity*> stored;
        for (const control::Opportunity& opportunity : *present) {
            stored.emplace(opportunity.id, &opportunity);
        }
        control::Opportunities opportunities(*records);
        for (const control::LegacyOpportunity* row : owned) {
            const std::string key =
                control::opportunity_id(row->opportunity.task_id, row->opportunity.rank);
            const auto found = stored.find(key);
            if (mode == "verify") {
                if (found == stored.end()) {
                    tally.missing.push_back(row->opportunity.id);
                } else if (!same_opportunity(*found->second, row->opportunity)) {
                    tally.mismatched.push_back(row->opportunity.id);
                } else {
                    ++tally.verified;
                }
                continue;
            }
            if (found != stored.end()) {
                ++tally.already_present;
                continue;
            }
            auto appended = control::settled(co_await opportunities.append(
                row->opportunity, control::RecordWrite{key, row->opportunity.at_us}));
            if (!appended) {
                if (appended.error().code == "conflict") {
                    ++tally.already_present; // a settle or another copy wrote it meanwhile
                    continue;
                }
                tally.error = appended.error().code + ": " + appended.error().message;
                break;
            }
            ++tally.copied;
        }
    }
    co_return copy_result("opportunities", mode, rows->size(), limit,
                          rows->empty() ? std::string() : rows->back().opportunity.id,
                          std::move(unresolved), tallies);
}

// One page of stream_owners (lake#379): assignments first, then per workspace one get_record per
// topic and, when copying, one `if_absent` put of the missing creators. Rows whose workspace was
// deleted are listed under `unresolved` but do not keep the page from being complete: their topic
// went with that workspace incarnation, so nothing is lost when the table is dropped.
liquid::runtime::task<ApiResult> copy_stream_owners(ApiContext& context, const std::string& mode,
                                                    const std::string& after, int limit) {
    if (auto assigned = context.store->assign_legacy_stream_owners(); !assigned) {
        co_return ApiResult::failure(assigned.error().code, assigned.error().message);
    }
    auto rows = context.store->legacy_stream_owners(after, limit);
    if (!rows) {
        co_return ApiResult::failure(rows.error().code, rows.error().message);
    }
    json unresolved = json::array();
    bool blocking = false;
    std::map<std::string, std::vector<const control::LegacyStreamOwner*>> by_workspace;
    for (const control::LegacyStreamOwner& row : *rows) {
        if (row.workspace_id.empty()) {
            unresolved.push_back({{"id", row.id},
                                  {"customer_id", row.owner.customer_id},
                                  {"workspace_id", row.owner.workspace_id},
                                  {"outcome", row.outcome}});
            blocking = blocking || row.outcome != "workspace_deleted";
        } else {
            by_workspace[row.workspace_id].push_back(&row);
        }
    }
    std::map<std::string, WorkspaceTally> tallies;
    for (const auto& [workspace_id, owned] : by_workspace) {
        WorkspaceTally& tally = tallies[workspace_id];
        tally.customer_id = owned.front()->owner.customer_id;
        tally.rows = owned.size();
        if (mode == "plan") {
            continue;
        }
        auto records = workspace_records(context, tally.customer_id, workspace_id);
        if (!records) {
            tally.error = records.error().code + ": " + records.error().message;
            continue;
        }
        for (const control::LegacyStreamOwner* row : owned) {
            auto creator = control::settled(
                co_await control::stream_creator(*records, row->owner.customer_id, row->owner.topic));
            if (!creator) {
                tally.error = creator.error().code + ": " + creator.error().message;
                break;
            }
            if (mode == "verify") {
                if (creator->empty()) {
                    tally.missing.push_back(row->id);
                } else if (*creator != row->owner.created_by) {
                    tally.mismatched.push_back(row->id);
                } else {
                    ++tally.verified;
                }
                continue;
            }
            if (!creator->empty()) {
                ++tally.already_present;
                continue;
            }
            auto claimed =
                control::settled(co_await control::claim_stream_owner(*records, row->owner));
            if (!claimed && claimed.error().code == "conflict") {
                // Claimed meanwhile (a concurrent copy, or a record that does not open); verify
                // compares it.
                ++tally.already_present;
                continue;
            }
            if (!claimed) {
                tally.error = claimed.error().code + ": " + claimed.error().message;
                break;
            }
            ++tally.copied;
        }
    }
    json workspaces = json::array();
    std::size_t copied = 0, already_present = 0, verified = 0, failed = 0;
    json missing = json::array(), mismatched = json::array();
    for (const auto& [workspace_id, tally] : tallies) {
        copied += tally.copied;
        already_present += tally.already_present;
        verified += tally.verified;
        failed += tally.error.empty() ? 0 : 1;
        for (const auto& id : tally.missing) {
            missing.push_back(id);
        }
        for (const auto& id : tally.mismatched) {
            mismatched.push_back(id);
        }
        json entry{{"workspace_id", workspace_id}, {"customer_id", tally.customer_id},
                   {"rows", tally.rows}};
        if (mode == "copy") {
            entry["copied"] = tally.copied;
            entry["already_present"] = tally.already_present;
        } else if (mode == "verify") {
            entry["verified"] = tally.verified;
            entry["missing"] = tally.missing;
            entry["mismatched"] = tally.mismatched;
        }
        if (!tally.error.empty()) {
            entry["error"] = tally.error;
        }
        workspaces.push_back(std::move(entry));
    }
    json value{{"collection", "stream_owners"},
               {"mode", mode},
               {"rows", rows->size()},
               {"next_after", rows->empty() || static_cast<int>(rows->size()) < limit
                                  ? json(nullptr)
                                  : json(rows->back().id)},
               {"assigned", rows->size() - unresolved.size()},
               {"unresolved", std::move(unresolved)},
               {"workspaces", std::move(workspaces)},
               {"failed_workspaces", failed}};
    if (mode == "copy") {
        value["copied"] = copied;
        value["already_present"] = already_present;
    } else if (mode == "verify") {
        value["verified"] = verified;
        value["missing"] = std::move(missing);
        value["mismatched"] = std::move(mismatched);
        value["complete"] = !blocking && failed == 0 && value["missing"].empty() &&
                            value["mismatched"].empty();
    }
    co_return ApiResult::success(std::move(value));
}

bool same_link(const control::DriveShareLink& stored, const control::DriveShareLink& source) {
    return stored.repository_id == source.repository_id && stored.object_id == source.object_id &&
           stored.node_id == source.node_id && stored.path == source.path &&
           stored.role == source.role && stored.token_hash == source.token_hash &&
           stored.expires_at == source.expires_at && stored.revoked_at == source.revoked_at &&
           stored.created_by_taap_id == source.created_by_taap_id && stored.at_us == source.at_us;
}

// One page of drive_share_links (lake#350): assignments and routes first, then per workspace one
// keyed read per link and the missing appends. Each copy keeps the row's id, so the link's
// `share-link:<id>` grant and its route keep naming it.
liquid::runtime::task<ApiResult> copy_drive_share_links(ApiContext& context,
                                                        const std::string& mode,
                                                        const std::string& after, int limit) {
    if (auto assigned = context.store->assign_legacy_drive_share_links(); !assigned) {
        co_return ApiResult::failure(assigned.error().code, assigned.error().message);
    }
    auto rows = context.store->legacy_drive_share_links(after, limit);
    if (!rows) {
        co_return ApiResult::failure(rows.error().code, rows.error().message);
    }
    json unresolved = json::array();
    std::map<std::string, std::vector<const control::LegacyDriveShareLink*>> by_workspace;
    for (const control::LegacyDriveShareLink& row : *rows) {
        if (row.workspace_id.empty()) {
            unresolved.push_back({{"id", row.link.id},
                                  {"customer_id", row.customer_id},
                                  {"outcome", row.outcome}});
        } else {
            by_workspace[row.workspace_id].push_back(&row);
        }
    }
    const auto collection = control::DriveShareLinkCodec::collection;
    std::map<std::string, WorkspaceTally> tallies;
    for (const auto& [workspace_id, owned] : by_workspace) {
        WorkspaceTally& tally = tallies[workspace_id];
        tally.customer_id = owned.front()->customer_id;
        tally.rows = owned.size();
        if (mode == "plan") {
            continue;
        }
        auto records = workspace_records(context, tally.customer_id, workspace_id);
        if (!records) {
            tally.error = records.error().code + ": " + records.error().message;
            continue;
        }
        control::DriveShareLinks links(*records);
        for (const control::LegacyDriveShareLink* row : owned) {
            auto present = control::settled(co_await records->get(collection, row->link.id));
            if (!present) {
                tally.error = present.error().code + ": " + present.error().message;
                break;
            }
            const auto stored =
                *present ? control::DriveShareLinkCodec::decode(**present) : std::nullopt;
            if (mode == "verify") {
                if (!stored) {
                    tally.missing.push_back(row->link.id);
                } else if (!same_link(*stored, row->link)) {
                    tally.mismatched.push_back(row->link.id);
                } else {
                    ++tally.verified;
                }
                continue;
            }
            if (*present) {
                ++tally.already_present;
                continue;
            }
            auto appended = control::settled(co_await links.append(
                row->link, control::RecordWrite{row->link.id, row->link.at_us}));
            if (!appended && appended.error().code == "conflict") {
                ++tally.already_present; // a concurrent copy; verify compares it
                continue;
            }
            if (!appended) {
                tally.error = appended.error().code + ": " + appended.error().message;
                break;
            }
            ++tally.copied;
        }
    }
    co_return copy_result("drive_share_links", mode, rows->size(), limit,
                          rows->empty() ? std::string() : rows->back().link.id,
                          std::move(unresolved), tallies);
}

// One page of a retired application table (lake#356, lake#357): assignments first, then per
// workspace and application one read of what is there and the missing writes. `Row` is a
// LegacyApplicationWebhookEvent or LegacyApplicationWebhookDelivery; `Ops` reads the
// application's records already in the workspace, compares, and writes one row.
template <class Row, class Ops>
liquid::runtime::task<ApiResult> copy_application_rows(ApiContext& context, std::string_view name,
                                                       const std::string& mode,
                                                       const std::vector<Row>& rows, int limit,
                                                       Ops ops) {
    json unresolved = json::array();
    // A row whose application's workspace was deleted went with it and does not block completion.
    bool blocking = false;
    std::map<std::string, std::map<std::string, std::vector<const Row*>>> by_workspace;
    for (const Row& row : rows) {
        if (row.workspace_id.empty()) {
            blocking = blocking || row.outcome != "workspace_deleted";
            unresolved.push_back({{"id", ops.id(row)},
                                  {"customer_id", row.customer_id},
                                  {"outcome", row.outcome}});
            blocking = blocking || row.outcome != "workspace_deleted";
        } else {
            by_workspace[row.workspace_id][ops.application_id(row)].push_back(&row);
        }
    }
    std::map<std::string, WorkspaceTally> tallies;
    for (const auto& [workspace_id, applications] : by_workspace) {
        WorkspaceTally& tally = tallies[workspace_id];
        tally.customer_id = applications.begin()->second.front()->customer_id;
        for (const auto& [application_id, owned] : applications) {
            tally.rows += owned.size();
        }
        if (mode == "plan") {
            continue;
        }
        auto records = workspace_records(context, tally.customer_id, workspace_id);
        if (!records) {
            tally.error = records.error().code + ": " + records.error().message;
            continue;
        }
        for (const auto& [application_id, owned] : applications) {
            // One read per application, shared by its rows.
            auto present = control::settled(co_await ops.present(*records, application_id));
            if (!present) {
                tally.error = present.error().code + ": " + present.error().message;
                break;
            }
            for (const Row* row : owned) {
                const auto found = present->find(ops.id(*row));
                if (mode == "verify") {
                    if (found == present->end()) {
                        tally.missing.push_back(ops.id(*row));
                    } else if (!ops.same(found->second, *row)) {
                        tally.mismatched.push_back(ops.id(*row));
                    } else {
                        ++tally.verified;
                    }
                    continue;
                }
                if (found != present->end()) {
                    ++tally.already_present;
                    continue;
                }
                auto written = control::settled(co_await ops.write(*records, *row));
                if (!written && written.error().code == "conflict") {
                    ++tally.already_present; // a concurrent copy; verify compares it
                    continue;
                }
                if (!written) {
                    tally.error = written.error().code + ": " + written.error().message;
                    break;
                }
                ++tally.copied;
            }
            if (!tally.error.empty()) {
                break;
            }
        }
    }
    co_return copy_result(name,
                          mode,
                          rows.size(),
                          limit,
                          rows.empty() ? std::string() : ops.id(rows.back()),
                          std::move(unresolved),
                          tallies,
                          blocking);
}

struct EventCopy {
    using Present = std::map<std::string, control::ApplicationWebhookEvent>;
    static std::string id(const control::LegacyApplicationWebhookEvent& row) {
        return row.event.id;
    }
    static std::string application_id(const control::LegacyApplicationWebhookEvent& row) {
        return row.event.application_id;
    }
    static liquid::runtime::task<control::ZettaControlResult<Present>>
    present(control::WorkspaceRecords& records, std::string application_id) {
        auto events = control::settled(
            co_await control::list_application_webhook_events(records, application_id, 0));
        if (!events) {
            co_return control::ZettaControlResult<Present>(std::unexpected(events.error()));
        }
        Present present;
        for (auto& event : *events) {
            const std::string key = event.id;
            present.emplace(key, std::move(event));
        }
        co_return control::ZettaControlResult<Present>(std::move(present));
    }
    static bool same(const control::ApplicationWebhookEvent& stored,
                     const control::LegacyApplicationWebhookEvent& row) {
        return stored.application_id == row.event.application_id &&
               stored.event == row.event.event && stored.payload == row.event.payload &&
               stored.at_us == row.event.at_us;
    }
    static liquid::runtime::task<control::ZettaControlResult<void>>
    write(control::WorkspaceRecords& records, const control::LegacyApplicationWebhookEvent& row) {
        control::ApplicationWebhookEvents events(records);
        auto appended = control::settled(co_await events.append(
            row.event, control::RecordWrite{row.event.id, row.event.at_us}));
        if (!appended) {
            co_return control::ZettaControlResult<void>(std::unexpected(appended.error()));
        }
        co_return control::ZettaControlResult<void>();
    }
};

struct DeliveryCopy {
    using Present = std::map<std::string, control::ApplicationWebhookDelivery>;
    static std::string id(const control::LegacyApplicationWebhookDelivery& row) {
        return row.delivery.id;
    }
    static std::string application_id(const control::LegacyApplicationWebhookDelivery& row) {
        return row.delivery.application_id;
    }
    static liquid::runtime::task<control::ZettaControlResult<Present>>
    present(control::WorkspaceRecords& records, std::string application_id) {
        auto deliveries = control::settled(
            co_await control::list_application_webhook_deliveries(records, application_id, 0));
        if (!deliveries) {
            co_return control::ZettaControlResult<Present>(std::unexpected(deliveries.error()));
        }
        Present present;
        for (auto& delivery : *deliveries) {
            const std::string key = delivery.id;
            present.emplace(key, std::move(delivery));
        }
        co_return control::ZettaControlResult<Present>(std::move(present));
    }
    static bool same(const control::ApplicationWebhookDelivery& stored,
                     const control::LegacyApplicationWebhookDelivery& row) {
        const auto& source = row.delivery;
        return stored.application_id == source.application_id &&
               stored.event_id == source.event_id && stored.webhook_id == source.webhook_id &&
               stored.attempt == source.attempt && stored.status == source.status &&
               stored.signature == source.signature && stored.error == source.error &&
               !stored.pending && stored.at_us == source.at_us;
    }
    static liquid::runtime::task<control::ZettaControlResult<void>>
    write(control::WorkspaceRecords& records, const control::LegacyApplicationWebhookDelivery& row) {
        // A finished attempt, written only where no record of that attempt exists.
        control::ApplicationWebhookDelivery delivery = row.delivery;
        delivery.pending = false;
        auto claimed = control::settled(
            co_await control::claim_application_webhook_delivery(records, std::move(delivery)));
        if (!claimed) {
            co_return control::ZettaControlResult<void>(std::unexpected(claimed.error()));
        }
        co_return control::ZettaControlResult<void>();
    }
};

liquid::runtime::task<ApiResult> copy_application_webhook_events(ApiContext& context,
                                                                 const std::string& mode,
                                                                 const std::string& after,
                                                                 int limit) {
    if (auto assigned = context.store->assign_legacy_application_webhook_events(); !assigned) {
        co_return ApiResult::failure(assigned.error().code, assigned.error().message);
    }
    auto rows = context.store->legacy_application_webhook_events(after, limit);
    if (!rows) {
        co_return ApiResult::failure(rows.error().code, rows.error().message);
    }
    co_return co_await copy_application_rows(context, "auth_app_webhook_events", mode, *rows,
                                             limit, EventCopy{});
}

liquid::runtime::task<ApiResult> copy_application_webhook_deliveries(ApiContext& context,
                                                                     const std::string& mode,
                                                                     const std::string& after,
                                                                     int limit) {
    if (auto assigned = context.store->assign_legacy_application_webhook_events(); !assigned) {
        co_return ApiResult::failure(assigned.error().code, assigned.error().message);
    }
    auto rows = context.store->legacy_application_webhook_deliveries(after, limit);
    if (!rows) {
        co_return ApiResult::failure(rows.error().code, rows.error().message);
    }
    co_return co_await copy_application_rows(context, "auth_app_webhook_deliveries", mode, *rows,
                                             limit, DeliveryCopy{});
}

// Takes the claim of a unique value for a copied user or organization: written if absent, or
// already held by it. Held by another is an error the operator resolves, never a takeover.
liquid::runtime::task<control::ZettaControlResult<void>>
claim_for_copy(control::WorkspaceRecords& records,
               control::RecordCollection collection,
               std::string application_id,
               std::string_view kind,
               std::string value,
               std::string holder) {
    const std::string key =
        control::application_claim_key(records, collection, application_id, kind, value);
    auto written = control::settled(co_await control::write_application_claim(
        records, collection, application_id, key, holder));
    if (written) {
        co_return control::ZettaControlResult<void>();
    }
    if (written.error().code != "conflict") {
        co_return control::ZettaControlResult<void>(std::unexpected(written.error()));
    }
    auto claim =
        control::settled(co_await control::read_application_claim(records, collection, key));
    if (!claim) {
        co_return control::ZettaControlResult<void>(std::unexpected(claim.error()));
    }
    if (*claim && (*claim)->holder == holder) {
        co_return control::ZettaControlResult<void>();
    }
    // Not `conflict`, which the copy counts as a row already present: the row cannot be copied.
    co_return control::ZettaControlResult<void>(std::unexpected(control::ZettaControlError{
        "store_error",
        std::string(kind) + " " + value + " of application " + application_id + " is held by " +
            (*claim ? (*claim)->holder : std::string("nobody"))}));
}

struct UserCopy {
    using Present = std::map<std::string, control::ApplicationUser>;
    static std::string id(const control::LegacyApplicationUser& row) {
        return row.id;
    }
    static std::string application_id(const control::LegacyApplicationUser& row) {
        return row.user.application_id;
    }
    static liquid::runtime::task<control::ZettaControlResult<Present>>
    present(control::WorkspaceRecords& records, std::string application_id) {
        auto users = control::settled(
            co_await control::list_application_users(records, application_id, "", 0));
        if (!users) {
            co_return control::ZettaControlResult<Present>(std::unexpected(users.error()));
        }
        Present present;
        for (auto& user : *users) {
            const std::string key =
                control::application_user_row_id(user.application_id, user.user_id);
            present.emplace(key, std::move(user));
        }
        co_return control::ZettaControlResult<Present>(std::move(present));
    }
    // A status someone changed in the workspace since the copy (a later lake revision) is theirs.
    static bool same(const control::ApplicationUser& stored,
                     const control::LegacyApplicationUser& row) {
        const auto& source = row.user;
        return stored.application_id == source.application_id && stored.user_id == source.user_id &&
               stored.taap_id == source.taap_id && stored.email == source.email &&
               stored.display_name == source.display_name && stored.own_pool == source.own_pool &&
               stored.at_us == source.at_us &&
               (stored.lake_revision > 1 || stored.status == source.status);
    }
    static liquid::runtime::task<control::ZettaControlResult<void>>
    write(control::WorkspaceRecords& records, const control::LegacyApplicationUser& row) {
        const control::ApplicationUser& user = row.user;
        if (user.own_pool && !user.email.empty()) {
            auto claimed =
                control::settled(co_await claim_for_copy(records,
                                                         control::ApplicationUserCodec::collection,
                                                         user.application_id,
                                                         control::email_claim,
                                                         user.email,
                                                         user.user_id));
            if (!claimed) {
                co_return claimed;
            }
        }
        auto written = control::settled(co_await control::create_application_user(records, user));
        if (!written) {
            co_return control::ZettaControlResult<void>(std::unexpected(written.error()));
        }
        co_return control::ZettaControlResult<void>();
    }
};

struct OrganizationCopy {
    using Present = std::map<std::string, control::ApplicationOrganization>;
    static std::string id(const control::LegacyApplicationOrganization& row) {
        return row.organization.id;
    }
    static std::string application_id(const control::LegacyApplicationOrganization& row) {
        return row.organization.application_id;
    }
    static liquid::runtime::task<control::ZettaControlResult<Present>>
    present(control::WorkspaceRecords& records, std::string application_id) {
        auto organizations = control::settled(
            co_await control::list_application_organizations(records, application_id, 0));
        if (!organizations) {
            co_return control::ZettaControlResult<Present>(std::unexpected(organizations.error()));
        }
        Present present;
        for (auto& organization : *organizations) {
            const std::string key = organization.id;
            present.emplace(key, std::move(organization));
        }
        co_return control::ZettaControlResult<Present>(std::move(present));
    }
    // A name or metadata someone changed in the workspace since the copy is theirs.
    static bool same(const control::ApplicationOrganization& stored,
                     const control::LegacyApplicationOrganization& row) {
        const auto& source = row.organization;
        return stored.application_id == source.application_id && stored.taap_id == source.taap_id &&
               stored.at_us == source.at_us &&
               (stored.lake_revision > 1 ||
                (stored.name == source.name && stored.metadata == source.metadata));
    }
    static liquid::runtime::task<control::ZettaControlResult<void>>
    write(control::WorkspaceRecords& records, const control::LegacyApplicationOrganization& row) {
        const control::ApplicationOrganization& organization = row.organization;
        const auto collection = control::ApplicationOrganizationCodec::collection;
        auto taap = control::settled(co_await claim_for_copy(records,
                                                             collection,
                                                             organization.application_id,
                                                             control::taap_claim,
                                                             organization.taap_id,
                                                             organization.id));
        if (!taap) {
            co_return taap;
        }
        auto name = control::settled(co_await claim_for_copy(records,
                                                             collection,
                                                             organization.application_id,
                                                             control::name_claim,
                                                             organization.name,
                                                             organization.id));
        if (!name) {
            co_return name;
        }
        auto written = control::settled(
            co_await control::put_new_application_organization(records, organization));
        if (!written) {
            co_return control::ZettaControlResult<void>(std::unexpected(written.error()));
        }
        co_return control::ZettaControlResult<void>();
    }
};

liquid::runtime::task<ApiResult> copy_application_users(ApiContext& context,
                                                        const std::string& mode,
                                                        const std::string& after,
                                                        int limit) {
    if (auto assigned = context.store->assign_legacy_application_directory(); !assigned) {
        co_return ApiResult::failure(assigned.error().code, assigned.error().message);
    }
    auto rows = context.store->legacy_application_users(after, limit);
    if (!rows) {
        co_return ApiResult::failure(rows.error().code, rows.error().message);
    }
    co_return co_await copy_application_rows(
        context, "auth_app_users", mode, *rows, limit, UserCopy{});
}

liquid::runtime::task<ApiResult> copy_application_organizations(ApiContext& context,
                                                                const std::string& mode,
                                                                const std::string& after,
                                                                int limit) {
    if (auto assigned = context.store->assign_legacy_application_directory(); !assigned) {
        co_return ApiResult::failure(assigned.error().code, assigned.error().message);
    }
    auto rows = context.store->legacy_application_organizations(after, limit);
    if (!rows) {
        co_return ApiResult::failure(rows.error().code, rows.error().message);
    }
    co_return co_await copy_application_rows(
        context, "auth_app_organizations", mode, *rows, limit, OrganizationCopy{});
}

// An application's key ring (lake#352): one record per application, keyed by it. The envelope of
// each private key is copied as the table held it, never opened. A ring a rotation has changed
// since the copy (a later lake revision) still holds every copied key, retired.
struct SigningKeyCopy {
    using Present = std::map<std::string, control::ApplicationSigningKeyRing>;
    static std::string id(const control::LegacyApplicationSigningKeyRing& row) {
        return row.ring.application_id;
    }
    static std::string application_id(const control::LegacyApplicationSigningKeyRing& row) {
        return row.ring.application_id;
    }
    static liquid::runtime::task<control::ZettaControlResult<Present>>
    present(control::WorkspaceRecords& records, std::string application_id) {
        auto ring = control::settled(
            co_await control::get_application_signing_keys(records, application_id));
        if (!ring) {
            co_return control::ZettaControlResult<Present>(std::unexpected(ring.error()));
        }
        Present present;
        if (*ring) {
            present.emplace(application_id, std::move(**ring));
        }
        co_return control::ZettaControlResult<Present>(std::move(present));
    }
    static bool same(const control::ApplicationSigningKeyRing& stored,
                     const control::LegacyApplicationSigningKeyRing& row) {
        const bool rotated = stored.lake_revision > 1;
        for (const control::ApplicationSigningKey& source : row.ring.keys) {
            const auto found = std::ranges::find_if(stored.keys, [&](const auto& key) {
                return key.version == source.version;
            });
            if (found == stored.keys.end() || found->kid != source.kid ||
                found->public_key != source.public_key ||
                found->encrypted_seed != source.encrypted_seed) {
                return false;
            }
            if (!rotated && (found->current != source.current ||
                             found->created_at != source.created_at ||
                             found->retired_at != source.retired_at)) {
                return false;
            }
        }
        return rotated || stored.keys.size() == row.ring.keys.size();
    }
    static liquid::runtime::task<control::ZettaControlResult<void>>
    write(control::WorkspaceRecords& records, const control::LegacyApplicationSigningKeyRing& row) {
        auto created = control::settled(
            co_await control::create_application_signing_keys(records, row.ring));
        if (!created) {
            co_return control::ZettaControlResult<void>(std::unexpected(created.error()));
        }
        co_return control::ZettaControlResult<void>();
    }
};

// An application's webhook (lake#355), keyed by its id, with its secret's envelope as the table
// held it.
struct WebhookCopy {
    using Present = std::map<std::string, control::ApplicationWebhook>;
    static std::string id(const control::LegacyApplicationWebhook& row) {
        return row.webhook.id;
    }
    static std::string application_id(const control::LegacyApplicationWebhook& row) {
        return row.webhook.application_id;
    }
    static liquid::runtime::task<control::ZettaControlResult<Present>>
    present(control::WorkspaceRecords& records, std::string application_id) {
        auto webhooks = control::settled(
            co_await control::list_application_webhooks(records, application_id));
        if (!webhooks) {
            co_return control::ZettaControlResult<Present>(std::unexpected(webhooks.error()));
        }
        Present present;
        for (auto& webhook : *webhooks) {
            const std::string key = webhook.id;
            present.emplace(key, std::move(webhook));
        }
        co_return control::ZettaControlResult<Present>(std::move(present));
    }
    static bool same(const control::ApplicationWebhook& stored,
                     const control::LegacyApplicationWebhook& row) {
        const auto& source = row.webhook;
        return stored.application_id == source.application_id && stored.url == source.url &&
               stored.events == source.events && stored.enabled == source.enabled &&
               stored.encrypted_secret == source.encrypted_secret && stored.at_us == source.at_us;
    }
    static liquid::runtime::task<control::ZettaControlResult<void>>
    write(control::WorkspaceRecords& records, const control::LegacyApplicationWebhook& row) {
        auto created =
            control::settled(co_await control::create_application_webhook(records, row.webhook));
        if (!created) {
            co_return control::ZettaControlResult<void>(std::unexpected(created.error()));
        }
        co_return control::ZettaControlResult<void>();
    }
};

// An application's settings (lake#351): one record per application, keyed by it, with the
// instant its routing row was created. A theme changed since the copy (a later lake revision) is
// accepted; the name, origins and redirect URIs are never changed after registration.
struct SettingsCopy {
    using Present = std::map<std::string, control::ApplicationSettings>;
    static std::string id(const control::LegacyApplicationSettings& row) {
        return row.settings.application_id;
    }
    static std::string application_id(const control::LegacyApplicationSettings& row) {
        return row.settings.application_id;
    }
    // Any space's record under the application's key; `same` compares the space.
    static liquid::runtime::task<control::ZettaControlResult<Present>>
    present(control::WorkspaceRecords& records, std::string application_id) {
        auto settings = control::settled(
            co_await control::get_application_settings(records, application_id, {}));
        if (!settings) {
            co_return control::ZettaControlResult<Present>(std::unexpected(settings.error()));
        }
        Present present;
        if (*settings) {
            present.emplace(application_id, std::move(**settings));
        }
        co_return control::ZettaControlResult<Present>(std::move(present));
    }
    static bool same(const control::ApplicationSettings& stored,
                     const control::LegacyApplicationSettings& row) {
        const auto& source = row.settings;
        const bool changed = stored.lake_revision > 1;
        return stored.customer_id == source.customer_id && stored.name == source.name &&
               stored.allowed_origins == source.allowed_origins &&
               stored.redirect_uris == source.redirect_uris && stored.at_us == source.at_us &&
               (changed || stored.theme == source.theme);
    }
    static liquid::runtime::task<control::ZettaControlResult<void>>
    write(control::WorkspaceRecords& records, const control::LegacyApplicationSettings& row) {
        auto created =
            control::settled(co_await control::create_application_settings(records, row.settings));
        if (!created) {
            co_return control::ZettaControlResult<void>(std::unexpected(created.error()));
        }
        co_return control::ZettaControlResult<void>();
    }
};

liquid::runtime::task<ApiResult> copy_application_settings(ApiContext& context,
                                                           const std::string& mode,
                                                           const std::string& after, int limit) {
    if (auto assigned = context.store->assign_legacy_application_settings(); !assigned) {
        co_return ApiResult::failure(assigned.error().code, assigned.error().message);
    }
    auto rows = context.store->legacy_application_settings(after, limit);
    if (!rows) {
        co_return ApiResult::failure(rows.error().code, rows.error().message);
    }
    co_return co_await copy_application_rows(context, "auth_applications", mode, *rows, limit,
                                             SettingsCopy{});
}

// A repository's description (lake#315 part 2): one settings record per repository, keyed by it,
// with the instant its row was created. A description changed since the copy (a later lake
// revision) is accepted.
struct RepositoryDescriptionCopy {
    using Present = std::map<std::string, control::RepositorySettings>;
    static std::string id(const control::LegacyRepositoryDescription& row) {
        return row.settings.repository_id;
    }
    // copy_application_rows groups by this key: one read per repository.
    static std::string application_id(const control::LegacyRepositoryDescription& row) {
        return row.settings.repository_id;
    }
    // Any space's record under the repository's key; `same` compares the space.
    static liquid::runtime::task<control::ZettaControlResult<Present>>
    present(control::WorkspaceRecords& records, std::string repository_id) {
        auto settings = control::settled(
            co_await control::get_repository_settings(records, repository_id, {}));
        if (!settings) {
            co_return control::ZettaControlResult<Present>(std::unexpected(settings.error()));
        }
        Present present;
        if (*settings) {
            present.emplace(repository_id, std::move(**settings));
        }
        co_return control::ZettaControlResult<Present>(std::move(present));
    }
    static bool same(const control::RepositorySettings& stored,
                     const control::LegacyRepositoryDescription& row) {
        const auto& source = row.settings;
        return stored.customer_id == source.customer_id && stored.at_us == source.at_us &&
               (stored.lake_revision > 1 || stored.description == source.description);
    }
    static liquid::runtime::task<control::ZettaControlResult<void>>
    write(control::WorkspaceRecords& records, const control::LegacyRepositoryDescription& row) {
        auto created =
            control::settled(co_await control::create_repository_settings(records, row.settings));
        if (!created) {
            co_return control::ZettaControlResult<void>(std::unexpected(created.error()));
        }
        co_return control::ZettaControlResult<void>();
    }
};

liquid::runtime::task<ApiResult> copy_repository_descriptions(ApiContext& context,
                                                              const std::string& mode,
                                                              const std::string& after,
                                                              int limit) {
    if (auto assigned = context.store->assign_legacy_repository_descriptions(); !assigned) {
        co_return ApiResult::failure(assigned.error().code, assigned.error().message);
    }
    auto rows = context.store->legacy_repository_descriptions(after, limit);
    if (!rows) {
        co_return ApiResult::failure(rows.error().code, rows.error().message);
    }
    co_return co_await copy_application_rows(context, "repositories", mode, *rows, limit,
                                             RepositoryDescriptionCopy{});
}

liquid::runtime::task<ApiResult> copy_application_signing_keys(ApiContext& context,
                                                               const std::string& mode,
                                                               const std::string& after,
                                                               int limit) {
    if (auto assigned = context.store->assign_legacy_application_credentials(); !assigned) {
        co_return ApiResult::failure(assigned.error().code, assigned.error().message);
    }
    auto rows = context.store->legacy_application_signing_keys(after, limit);
    if (!rows) {
        co_return ApiResult::failure(rows.error().code, rows.error().message);
    }
    co_return co_await copy_application_rows(context, "auth_app_signing_keys", mode, *rows, limit,
                                             SigningKeyCopy{});
}

liquid::runtime::task<ApiResult> copy_application_webhooks(ApiContext& context,
                                                           const std::string& mode,
                                                           const std::string& after, int limit) {
    if (auto assigned = context.store->assign_legacy_application_credentials(); !assigned) {
        co_return ApiResult::failure(assigned.error().code, assigned.error().message);
    }
    auto rows = context.store->legacy_application_webhooks(after, limit);
    if (!rows) {
        co_return ApiResult::failure(rows.error().code, rows.error().message);
    }
    co_return co_await copy_application_rows(context, "auth_app_webhooks", mode, *rows, limit,
                                             WebhookCopy{});
}

bool same_authorization_code(const control::ApplicationAuthorizationCode& stored,
                             const control::ApplicationAuthorizationCode& source) {
    return stored.application_id == source.application_id && stored.user_id == source.user_id &&
           stored.redirect_uri == source.redirect_uri &&
           stored.code_challenge == source.code_challenge && stored.nonce == source.nonce &&
           stored.expires_us == source.expires_us && stored.at_us == source.at_us;
}

// One page of the live codes of the retired auth_app_authorization_codes table (lake#363). Each is
// written into its application's workspace under the blind key lake redeems it by, with the instant
// it was issued and the instant it expires, so a copied code is redeemable for no longer than it
// was. A code is recorded as copied (outcome `copied`) before it is written, and a copied code is
// never written again: a code redeemed in the workspace after the copy is gone for good, so a
// rerun cannot make it redeemable twice. For the same reason verify counts a copied code that is no
// longer in the workspace (redeemed, or expired there) as verified; one present must be unchanged.
liquid::runtime::task<ApiResult> copy_application_authorization_codes(ApiContext& context,
                                                                      const std::string& mode,
                                                                      const std::string& after,
                                                                      int limit) {
    using Row = control::LegacyApplicationAuthorizationCode;
    if (auto assigned = context.store->assign_legacy_application_authorization_codes();
        !assigned) {
        co_return ApiResult::failure(assigned.error().code, assigned.error().message);
    }
    auto rows = context.store->legacy_application_authorization_codes(after, limit);
    if (!rows) {
        co_return ApiResult::failure(rows.error().code, rows.error().message);
    }
    json unresolved = json::array();
    // A code whose application's workspace was deleted went with it and does not block.
    bool blocking = false;
    std::map<std::string, std::map<std::string, std::vector<const Row*>>> by_workspace;
    for (const Row& row : *rows) {
        if (row.workspace_id.empty()) {
            blocking = blocking || row.outcome != "workspace_deleted";
            unresolved.push_back({{"id", row.code.code_hash},
                                  {"customer_id", row.customer_id},
                                  {"outcome", row.outcome}});
        } else {
            by_workspace[row.workspace_id][row.code.application_id].push_back(&row);
        }
    }
    std::map<std::string, WorkspaceTally> tallies;
    for (const auto& [workspace_id, applications] : by_workspace) {
        WorkspaceTally& tally = tallies[workspace_id];
        tally.customer_id = applications.begin()->second.front()->customer_id;
        for (const auto& [application_id, owned] : applications) {
            tally.rows += owned.size();
        }
        if (mode == "plan") {
            continue;
        }
        auto records = workspace_records(context, tally.customer_id, workspace_id);
        if (!records) {
            tally.error = records.error().code + ": " + records.error().message;
            continue;
        }
        for (const auto& [application_id, owned] : applications) {
            // One read per application, shared by its codes.
            auto codes = control::settled(
                co_await control::list_application_authorization_codes(*records, application_id));
            if (!codes) {
                tally.error = codes.error().code + ": " + codes.error().message;
                break;
            }
            std::map<std::string, control::ApplicationAuthorizationCode> present;
            for (auto& code : *codes) {
                const std::string hash = code.code_hash;
                present.emplace(hash, std::move(code));
            }
            for (const Row* row : owned) {
                const bool copied = row->outcome == "copied";
                const auto found = present.find(row->code.code_hash);
                if (mode == "verify") {
                    if (found != present.end()) {
                        if (same_authorization_code(found->second, row->code)) {
                            ++tally.verified;
                        } else {
                            tally.mismatched.push_back(row->code.code_hash);
                        }
                    } else if (copied) {
                        ++tally.verified; // redeemed or expired in the workspace since the copy
                    } else {
                        tally.missing.push_back(row->code.code_hash);
                    }
                    continue;
                }
                if (found != present.end() || copied) {
                    ++tally.already_present;
                    continue;
                }
                if (auto marked = context.store->record_legacy_application_authorization_code_copied(
                        row->code.code_hash);
                    !marked) {
                    tally.error = marked.error().code + ": " + marked.error().message;
                    break;
                }
                auto written = control::settled(
                    co_await control::issue_application_authorization_code(*records, row->code));
                if (!written && written.error().code == "conflict") {
                    ++tally.already_present; // a concurrent copy; verify compares it
                    continue;
                }
                if (!written) {
                    tally.error = written.error().code + ": " + written.error().message;
                    break;
                }
                ++tally.copied;
            }
            if (!tally.error.empty()) {
                break;
            }
        }
    }
    co_return copy_result("auth_app_authorization_codes", mode, rows->size(), limit,
                          rows->empty() ? std::string() : rows->back().code.code_hash,
                          std::move(unresolved), tallies, blocking);
}

// The rows of one retired metering table grouped by the workspace that owns them. Rows whose
// workspace was deleted are reported but do not block completion: their records went with it.
template <class Row>
std::map<std::string, std::vector<const Row*>>
by_owner(const std::vector<Row>& rows,
         json& unresolved,
         bool& blocking,
         const std::string& (*customer)(const Row&)) {
    std::map<std::string, std::vector<const Row*>> owned;
    for (const Row& row : rows) {
        if (row.workspace_id.empty()) {
            unresolved.push_back(
                {{"id", row.id}, {"customer_id", customer(row)}, {"outcome", row.outcome}});
            blocking = blocking || row.outcome != "workspace_deleted";
        } else {
            owned[row.workspace_id].push_back(&row);
        }
    }
    return owned;
}

const std::string& usage_customer(const control::LegacyMeteringUsage& row) {
    return row.usage.customer_id;
}
const std::string& build_customer(const control::LegacyMeteredBuild& row) {
    return row.build.customer_id;
}

// One page of metering_usage (lake#348): assignments first, then per workspace one listing of the
// space's transfer records and an `if_absent` put of each missing row under its legacy id. Only
// transfer moves: the hourly storage gauge is retired (storage is measured live), and compute is
// in the build records.
liquid::runtime::task<ApiResult> copy_metering_usage(ApiContext& context,
                                                     const std::string& mode,
                                                     const std::string& after,
                                                     int limit) {
    if (auto assigned = context.store->assign_legacy_metering(); !assigned) {
        co_return ApiResult::failure(assigned.error().code, assigned.error().message);
    }
    auto rows = context.store->legacy_metering_usage(after, limit);
    if (!rows) {
        co_return ApiResult::failure(rows.error().code, rows.error().message);
    }
    json unresolved = json::array();
    bool blocking = false;
    const auto owned = by_owner(*rows, unresolved, blocking, &usage_customer);
    std::map<std::string, WorkspaceTally> tallies;
    for (const auto& [workspace_id, owned_rows] : owned) {
        WorkspaceTally& tally = tallies[workspace_id];
        tally.customer_id = owned_rows.front()->usage.customer_id;
        tally.rows = owned_rows.size();
        if (mode == "plan") {
            continue;
        }
        auto records = workspace_records(context, tally.customer_id, workspace_id);
        if (!records) {
            tally.error = records.error().code + ": " + records.error().message;
            continue;
        }
        auto present =
            control::settled(co_await control::list_metered_transfer(*records, tally.customer_id));
        if (!present) {
            tally.error = present.error().code + ": " + present.error().message;
            continue;
        }
        std::map<std::string, control::MeteredTransfer> stored;
        for (control::MeteredTransfer& transfer : *present) {
            const std::string id = transfer.id;
            stored.emplace(id, std::move(transfer));
        }
        for (const control::LegacyMeteringUsage* row : owned_rows) {
            const auto found = stored.find(row->id);
            if (mode == "verify") {
                if (found == stored.end()) {
                    tally.missing.push_back(row->id);
                } else if (found->second.hour != row->usage.hour ||
                           found->second.transfer_in_bytes != row->usage.transfer_in_bytes ||
                           found->second.transfer_out_bytes != row->usage.transfer_out_bytes) {
                    tally.mismatched.push_back(row->id);
                } else {
                    ++tally.verified;
                }
                continue;
            }
            if (found != stored.end()) {
                ++tally.already_present;
                continue;
            }
            auto copied = control::settled(co_await control::copy_metered_transfer(*records, *row));
            if (!copied) {
                tally.error = copied.error().code + ": " + copied.error().message;
                break;
            }
            ++(*copied ? tally.copied : tally.already_present);
        }
    }
    co_return copy_result("metering_usage",
                          mode,
                          rows->size(),
                          limit,
                          rows->empty() ? std::string() : rows->back().id,
                          std::move(unresolved),
                          tallies,
                          blocking);
}

// One page of metering_build_records (lake#349): per workspace one listing of the space's build
// records and an `if_absent` put of each missing result under the key a usage read also writes,
// so a result recorded since the cutover is not counted twice.
liquid::runtime::task<ApiResult> copy_metering_build_records(ApiContext& context,
                                                             const std::string& mode,
                                                             const std::string& after,
                                                             int limit) {
    if (auto assigned = context.store->assign_legacy_metering(); !assigned) {
        co_return ApiResult::failure(assigned.error().code, assigned.error().message);
    }
    auto rows = context.store->legacy_metering_build_records(after, limit);
    if (!rows) {
        co_return ApiResult::failure(rows.error().code, rows.error().message);
    }
    json unresolved = json::array();
    bool blocking = false;
    const auto owned = by_owner(*rows, unresolved, blocking, &build_customer);
    std::map<std::string, WorkspaceTally> tallies;
    for (const auto& [workspace_id, owned_rows] : owned) {
        WorkspaceTally& tally = tallies[workspace_id];
        tally.customer_id = owned_rows.front()->build.customer_id;
        tally.rows = owned_rows.size();
        if (mode == "plan") {
            continue;
        }
        auto records = workspace_records(context, tally.customer_id, workspace_id);
        if (!records) {
            tally.error = records.error().code + ": " + records.error().message;
            continue;
        }
        auto present =
            control::settled(co_await control::list_metered_builds(*records, tally.customer_id));
        if (!present) {
            tally.error = present.error().code + ": " + present.error().message;
            continue;
        }
        std::map<std::string, control::MeteredBuild> stored;
        for (control::MeteredBuild& build : *present) {
            const std::string id = build.result_id;
            stored.emplace(id, std::move(build));
        }
        for (const control::LegacyMeteredBuild* row : owned_rows) {
            const auto found = stored.find(row->build.result_id);
            if (mode == "verify") {
                if (found == stored.end()) {
                    tally.missing.push_back(row->id);
                } else if (found->second.hour != row->build.hour ||
                           found->second.compute_seconds != row->build.compute_seconds) {
                    tally.mismatched.push_back(row->id);
                } else {
                    ++tally.verified;
                }
                continue;
            }
            if (found != stored.end()) {
                ++tally.already_present;
                continue;
            }
            auto copied =
                control::settled(co_await control::record_metered_build(*records, row->build));
            if (!copied) {
                tally.error = copied.error().code + ": " + copied.error().message;
                break;
            }
            ++(*copied ? tally.copied : tally.already_present);
        }
    }
    co_return copy_result("metering_build_records",
                          mode,
                          rows->size(),
                          limit,
                          rows->empty() ? std::string() : rows->back().id,
                          std::move(unresolved),
                          tallies,
                          blocking);
}

// A forge record in its workspace is the row it was copied from: same repository, kind and
// creation instant, and the same value unless someone has changed it there since (lake revision
// above 1), which the copy accepts.
bool same_forge_record(const control::ForgeRecord& stored, const control::ForgeRecord& source) {
    return stored.repository_id == source.repository_id && stored.kind == source.kind &&
           stored.at_us == source.at_us &&
           (stored.lake_revision > 1 || stored.value == source.value);
}

// One page of forge_records (lake#341): assignments first, then per workspace one keyed read per
// row and the missing appends. Each copy keeps the row's id and creation instant, so references
// between records (a comment's issue_id, a delivery's webhook_id) still hold.
liquid::runtime::task<ApiResult> copy_forge_records(ApiContext& context, const std::string& mode,
                                                    const std::string& after, int limit) {
    if (auto assigned = context.store->assign_legacy_forge_records(); !assigned) {
        co_return ApiResult::failure(assigned.error().code, assigned.error().message);
    }
    auto rows = context.store->legacy_forge_records(after, limit);
    if (!rows) {
        co_return ApiResult::failure(rows.error().code, rows.error().message);
    }
    json unresolved = json::array();
    bool blocking = false;
    std::map<std::string, std::vector<const control::LegacyForgeRecord*>> by_workspace;
    for (const control::LegacyForgeRecord& row : *rows) {
        if (row.workspace_id.empty()) {
            unresolved.push_back({{"id", row.record.id},
                                  {"repository_id", row.record.repository_id},
                                  {"customer_id", row.customer_id},
                                  {"outcome", row.outcome}});
            blocking = blocking || row.outcome != "workspace_deleted";
        } else {
            by_workspace[row.workspace_id].push_back(&row);
        }
    }
    const auto collection = control::ForgeRecordCodec::collection;
    std::map<std::string, WorkspaceTally> tallies;
    for (const auto& [workspace_id, owned] : by_workspace) {
        WorkspaceTally& tally = tallies[workspace_id];
        tally.customer_id = owned.front()->customer_id;
        tally.rows = owned.size();
        if (mode == "plan") {
            continue;
        }
        auto records = workspace_records(context, tally.customer_id, workspace_id);
        if (!records) {
            tally.error = records.error().code + ": " + records.error().message;
            continue;
        }
        control::ForgeRecords forge(*records);
        for (const control::LegacyForgeRecord* row : owned) {
            auto present = control::settled(co_await records->get(collection, row->record.id));
            if (!present) {
                tally.error = present.error().code + ": " + present.error().message;
                break;
            }
            const auto stored =
                *present ? control::ForgeRecordCodec::decode(**present) : std::nullopt;
            if (mode == "verify") {
                if (!stored) {
                    tally.missing.push_back(row->record.id);
                } else if (!same_forge_record(*stored, row->record)) {
                    tally.mismatched.push_back(row->record.id);
                } else {
                    ++tally.verified;
                }
                continue;
            }
            if (*present) {
                ++tally.already_present;
                continue;
            }
            auto appended = control::settled(co_await forge.append(
                row->record, control::RecordWrite{row->record.id, row->record.at_us}));
            if (!appended && appended.error().code == "conflict") {
                ++tally.already_present; // a concurrent copy; verify compares it
                continue;
            }
            if (!appended) {
                tally.error = appended.error().code + ": " + appended.error().message;
                break;
            }
            ++tally.copied;
        }
    }
    co_return copy_result("forge_records", mode, rows->size(), limit,
                          rows->empty() ? std::string() : rows->back().record.id,
                          std::move(unresolved), tallies, blocking);
}

// The retired platform_mirror table (lake#359): each row goes to the workspace of the repository
// it names, under platform_mirror_id(kind, key), the key the sync writes. Per workspace, one
// listing per repository. A row the sync has since replaced by a newer operator-control row is
// verified, not mismatched. Rows of a deleted workspace, or naming no repository lake has, are
// reported and do not hold the copy back. Copy this collection before platform_environments.
liquid::runtime::task<ApiResult> copy_platform_mirror(ApiContext& context, const std::string& mode,
                                                      const std::string& after, int limit) {
    if (auto assigned = context.store->assign_legacy_platform_mirror(); !assigned) {
        co_return ApiResult::failure(assigned.error().code, assigned.error().message);
    }
    auto rows = context.store->legacy_platform_mirror(after, limit);
    if (!rows) {
        co_return ApiResult::failure(rows.error().code, rows.error().message);
    }
    json unresolved = json::array();
    bool blocking = false;
    std::map<std::string, std::map<std::string, std::vector<const control::LegacyPlatformMirrorRow*>>>
        by_workspace; // workspace -> repository -> rows
    for (const control::LegacyPlatformMirrorRow& row : *rows) {
        const bool valid = row.row.value.is_object() && control::is_platform_mirror_kind(row.row.kind);
        if (row.workspace_id.empty() || !valid) {
            const std::string outcome = valid ? row.outcome : std::string("invalid");
            unresolved.push_back({{"id", row.id},
                                  {"kind", row.row.kind},
                                  {"repository_id", row.repository_id},
                                  {"customer_id", row.customer_id},
                                  {"outcome", outcome}});
            blocking = blocking ||
                       (outcome != "workspace_deleted" && outcome != "unknown_repository");
        } else {
            by_workspace[row.workspace_id][row.repository_id].push_back(&row);
        }
    }
    std::map<std::string, WorkspaceTally> tallies;
    for (const auto& [workspace_id, repositories] : by_workspace) {
        WorkspaceTally& tally = tallies[workspace_id];
        tally.customer_id = repositories.begin()->second.front()->customer_id;
        for (const auto& [repository_id, owned] : repositories) {
            tally.rows += owned.size();
        }
        if (mode == "plan") {
            continue;
        }
        auto records = workspace_records(context, tally.customer_id, workspace_id);
        if (!records) {
            tally.error = records.error().code + ": " + records.error().message;
            continue;
        }
        for (const auto& [repository_id, owned] : repositories) {
            auto present = control::settled(
                co_await control::list_platform_mirror_records(*records, repository_id));
            if (!present) {
                tally.error = present.error().code + ": " + present.error().message;
                break;
            }
            std::map<std::string, json> stored;
            for (const control::PlatformMirrorRow& row : *present) {
                stored.emplace(control::platform_mirror_id(row.kind, row.key), row.value);
            }
            for (const control::LegacyPlatformMirrorRow* row : owned) {
                const auto found = stored.find(row->id);
                if (mode == "verify") {
                    if (found == stored.end()) {
                        tally.missing.push_back(row->id);
                    } else if (found->second != row->row.value &&
                               !control::platform_row_is_newer(found->second, row->row.value)) {
                        tally.mismatched.push_back(row->id);
                    } else {
                        ++tally.verified;
                    }
                    continue;
                }
                if (found != stored.end()) {
                    ++tally.already_present;
                    continue;
                }
                auto written = control::settled(co_await control::put_platform_mirror_record(
                    *records, repository_id, row->row, row->at_us));
                if (!written) {
                    tally.error = written.error().code + ": " + written.error().message;
                    break;
                }
                if (*written) {
                    ++tally.copied;
                } else {
                    ++tally.already_present; // the sync mirrored it, or a newer row, first
                }
            }
            if (!tally.error.empty()) {
                break;
            }
        }
    }
    co_return copy_result("platform_mirror", mode, rows->size(), limit,
                          rows->empty() ? std::string() : rows->back().id, std::move(unresolved),
                          tallies, blocking);
}

// The retired platform_environments table (lake#358): each environment goes to its repository's
// workspace under its derived id, platform_environment_id(repository, name), with the policy
// object it had (migration 0060 gave that object the same external id) and its creation instant.
// An environment the sync created first, with the same policy object, counts as present.
liquid::runtime::task<ApiResult> copy_platform_environments(ApiContext& context,
                                                            const std::string& mode,
                                                            const std::string& after, int limit) {
    if (auto assigned = context.store->assign_legacy_platform_environments(); !assigned) {
        co_return ApiResult::failure(assigned.error().code, assigned.error().message);
    }
    auto rows = context.store->legacy_platform_environments(after, limit);
    if (!rows) {
        co_return ApiResult::failure(rows.error().code, rows.error().message);
    }
    json unresolved = json::array();
    bool blocking = false;
    std::map<std::string, std::vector<const control::LegacyPlatformEnvironment*>> by_workspace;
    for (const control::LegacyPlatformEnvironment& row : *rows) {
        if (row.workspace_id.empty()) {
            unresolved.push_back({{"id", row.id},
                                  {"repository_id", row.environment.repository_id},
                                  {"customer_id", row.customer_id},
                                  {"outcome", row.outcome}});
            blocking = blocking || row.outcome != "workspace_deleted";
        } else {
            by_workspace[row.workspace_id].push_back(&row);
        }
    }
    std::map<std::string, WorkspaceTally> tallies;
    for (const auto& [workspace_id, owned] : by_workspace) {
        WorkspaceTally& tally = tallies[workspace_id];
        tally.customer_id = owned.front()->customer_id;
        tally.rows = owned.size();
        if (mode == "plan") {
            continue;
        }
        auto records = workspace_records(context, tally.customer_id, workspace_id);
        if (!records) {
            tally.error = records.error().code + ": " + records.error().message;
            continue;
        }
        for (const control::LegacyPlatformEnvironment* row : owned) {
            const control::PlatformEnvironment& source = row->environment;
            auto present = control::settled(co_await control::get_platform_environment_record(
                *records, source.repository_id, source.name));
            if (!present) {
                tally.error = present.error().code + ": " + present.error().message;
                break;
            }
            if (mode == "verify") {
                if (!*present) {
                    tally.missing.push_back(row->id);
                } else if ((*present)->object_id != source.object_id) {
                    tally.mismatched.push_back(row->id);
                } else {
                    ++tally.verified;
                }
                continue;
            }
            if (*present) {
                ++tally.already_present;
                continue;
            }
            auto written = control::settled(
                co_await control::put_platform_environment_record(*records, source));
            if (!written) {
                tally.error = written.error().code + ": " + written.error().message;
                break;
            }
            // A concurrent writer's record (another policy object) answers instead of ours;
            // verify reports it.
            if (written->object_id == source.object_id) {
                ++tally.copied;
            } else {
                ++tally.already_present;
            }
        }
    }
    co_return copy_result("platform_environments", mode, rows->size(), limit,
                          rows->empty() ? std::string() : rows->back().id, std::move(unresolved),
                          tallies, blocking);
}

// The retired secrets tables (lake#328-#332): every row goes to the workspace of its mount's route
// (secret_mount_routes, migration 0057), under the key a live call derives for it, so a copied
// secret, key or data key is found exactly as one written after the cutover. A row whose mount has
// no route is unresolved and blocks completion; one whose mount's workspace was deleted went with
// it and does not. `Ops` reads what is in the workspace, compares and writes one row; a write that
// finds the key taken counts as already present, and verify compares it.
template <class Row, class Ops>
liquid::runtime::task<ApiResult> copy_secret_rows(ApiContext& context, std::string_view name,
                                                  const std::string& mode,
                                                  const std::vector<Row>& rows, int limit,
                                                  Ops ops) {
    json unresolved = json::array();
    bool blocking = false;
    std::map<std::string, std::vector<const Row*>> by_workspace;
    for (const Row& row : rows) {
        const control::LegacySecretOwner& owner = ops.owner(row);
        if (owner.workspace_id.empty()) {
            unresolved.push_back({{"id", ops.id(row)},
                                  {"mount_id", owner.mount_id},
                                  {"customer_id", owner.customer_id},
                                  {"outcome", owner.outcome}});
            blocking = blocking || owner.outcome != "workspace_deleted";
        } else {
            by_workspace[owner.workspace_id].push_back(&row);
        }
    }
    std::map<std::string, WorkspaceTally> tallies;
    for (const auto& [workspace_id, owned] : by_workspace) {
        WorkspaceTally& tally = tallies[workspace_id];
        tally.customer_id = ops.owner(*owned.front()).customer_id;
        tally.rows = owned.size();
        if (mode == "plan") {
            continue;
        }
        auto records = workspace_records(context, tally.customer_id, workspace_id);
        if (!records) {
            tally.error = records.error().code + ": " + records.error().message;
            continue;
        }
        for (const Row* row : owned) {
            auto present = control::settled(co_await ops.present(*records, *row));
            if (!present) {
                tally.error = present.error().code + ": " + present.error().message;
                break;
            }
            if (mode == "verify") {
                if (!*present) {
                    tally.missing.push_back(ops.id(*row));
                } else if (!ops.same(context, **present, *row)) {
                    tally.mismatched.push_back(ops.id(*row));
                } else {
                    ++tally.verified;
                }
                continue;
            }
            if (*present) {
                ++tally.already_present;
                continue;
            }
            auto written = control::settled(co_await ops.write(context, *records, *row));
            if (!written) {
                tally.error = written.error().code + ": " + written.error().message;
                break;
            }
            ++(*written ? tally.copied : tally.already_present);
        }
    }
    co_return copy_result(name, mode, rows.size(), limit,
                          rows.empty() ? std::string() : ops.id(rows.back()),
                          std::move(unresolved), tallies, blocking);
}

using PresentRecord = control::ZettaControlResult<std::optional<control::StoredRecord>>;
using Copied = control::ZettaControlResult<bool>;

// A record written `if_absent`: true when this copy wrote it, false when the key was taken.
liquid::runtime::task<Copied> put_absent(control::WorkspaceRecords& records,
                                         control::RecordCollection collection, std::string id,
                                         std::string lookup, json body, std::int64_t at_us) {
    auto written = control::settled(co_await records.put(
        collection, std::move(id), std::move(lookup), std::move(body),
        control::RecordCondition{.if_absent = true}, 1, at_us));
    if (!written && written.error().code == "conflict") {
        co_return Copied(false);
    }
    if (!written) {
        co_return Copied(std::unexpected(written.error()));
    }
    co_return Copied(true);
}

liquid::runtime::task<PresentRecord> present_record(control::WorkspaceRecords& records,
                                                    control::RecordCollection collection,
                                                    std::string id) {
    co_return control::settled(co_await records.get(collection, std::move(id)));
}

struct MountCopy {
    static std::string id(const control::LegacySecretMount& row) {
        return row.mount.id;
    }
    static const control::LegacySecretOwner& owner(const control::LegacySecretMount& row) {
        return row.owner;
    }
    static liquid::runtime::task<PresentRecord> present(control::WorkspaceRecords& records,
                                                        const control::LegacySecretMount& row) {
        return present_record(records, control::SecretMountCodec::collection, row.mount.id);
    }
    static bool same(const ApiContext&, const control::StoredRecord& stored,
                     const control::LegacySecretMount& row) {
        const auto mount = control::SecretMountCodec::decode(stored);
        return mount && mount->customer_id == row.mount.customer_id &&
               mount->organization_id == row.mount.organization_id &&
               mount->workspace_id == row.owner.workspace_id && mount->name == row.mount.name &&
               mount->description == row.mount.description && mount->at_us == row.mount.at_us;
    }
    // The name claim first, then the mount: a name a mount made after the cutover already holds is
    // reported rather than given to a second mount.
    static liquid::runtime::task<Copied> write(ApiContext&, control::WorkspaceRecords& records,
                                               const control::LegacySecretMount& row) {
        const auto collection = control::SecretMountCodec::collection;
        const std::string claim_key = control::secret_mount_name_key(
            records, row.mount.organization_id, row.mount.name);
        auto claimed = control::settled(co_await put_absent(
            records, collection, claim_key,
            control::secret_mount_name_lookup(row.mount.organization_id),
            control::secret_mount_name_claim(row.mount.id), row.mount.at_us));
        if (!claimed) {
            co_return Copied(std::unexpected(claimed.error()));
        }
        if (!*claimed) {
            auto holder = control::settled(co_await records.get(collection, claim_key));
            if (!holder) {
                co_return Copied(std::unexpected(holder.error()));
            }
            const auto mount =
                *holder ? control::secret_mount_name_claimed(**holder) : std::nullopt;
            if (mount != row.mount.id) {
                co_return Copied(std::unexpected(control::ZettaControlError{
                    "conflict", "mount " + row.mount.id + ": its name is used by mount " +
                                    mount.value_or("?") + " in this workspace"}));
            }
        }
        control::SecretMount mount = row.mount;
        mount.workspace_id = row.owner.workspace_id;
        co_return control::settled(co_await put_absent(
            records, collection, mount.id, control::SecretMountCodec::lookup(mount.customer_id),
            control::SecretMountCodec::encode(mount), mount.at_us));
    }
};

struct EntryCopy {
    static std::string id(const control::LegacySecretEntry& row) {
        return row.id;
    }
    static const control::LegacySecretOwner& owner(const control::LegacySecretEntry& row) {
        return row.owner;
    }
    static liquid::runtime::task<PresentRecord> present(control::WorkspaceRecords& records,
                                                        const control::LegacySecretEntry& row) {
        return present_record(
            records, control::SecretEntryCodec::collection,
            control::SecretEntryCodec::key(records, row.owner.mount_id, row.name));
    }
    // A put, delete or metadata change since the copy (a later lake revision) is accepted.
    static bool same(const ApiContext&, const control::StoredRecord& stored,
                     const control::LegacySecretEntry& row) {
        const auto entry = control::SecretEntryCodec::decode(stored);
        return entry && entry->mount_id == row.owner.mount_id && entry->name == row.name &&
               entry->at_us == row.at_us &&
               (entry->lake_revision > 1 || (entry->current_version == row.current_version &&
                                              entry->custom_metadata == row.custom_metadata));
    }
    static liquid::runtime::task<Copied> write(ApiContext&, control::WorkspaceRecords& records,
                                               const control::LegacySecretEntry& row) {
        control::SecretEntry entry{row.owner.mount_id, row.name, row.current_version,
                                   row.custom_metadata};
        entry.updated_at = row.updated_at;
        return put_absent(records, control::SecretEntryCodec::collection,
                          control::SecretEntryCodec::key(records, row.owner.mount_id, row.name),
                          control::SecretEntryCodec::lookup(row.owner.mount_id),
                          control::SecretEntryCodec::encode(entry), row.at_us);
    }
};

struct VersionCopy {
    static std::string id(const control::LegacySecretVersion& row) {
        return row.id;
    }
    static const control::LegacySecretOwner& owner(const control::LegacySecretVersion& row) {
        return row.owner;
    }
    static std::string key(control::WorkspaceRecords& records,
                           const control::LegacySecretVersion& row) {
        return control::SecretVersionCodec::key(records, row.owner.mount_id, row.name,
                                                row.version);
    }
    static liquid::runtime::task<PresentRecord> present(control::WorkspaceRecords& records,
                                                        const control::LegacySecretVersion& row) {
        return present_record(records, control::SecretVersionCodec::collection, key(records, row));
    }
    // The same ciphertext and lifecycle, or a delete, undelete or destroy made since the copy.
    static bool same(const ApiContext&, const control::StoredRecord& stored,
                     const control::LegacySecretVersion& row) {
        const auto version = control::SecretVersionCodec::decode(stored);
        return version && version->mount_id == row.owner.mount_id && version->name == row.name &&
               version->version == row.version && version->at_us == row.at_us &&
               (version->lake_revision > 1 ||
                (version->ciphertext.value_or("") == row.ciphertext &&
                 version->deleted_at == row.deleted_at &&
                 version->destroyed_at == row.destroyed_at));
    }
    static liquid::runtime::task<Copied> write(ApiContext&, control::WorkspaceRecords& records,
                                               const control::LegacySecretVersion& row) {
        control::SecretVersionRecord version;
        version.mount_id = row.owner.mount_id;
        version.name = row.name;
        version.version = row.version;
        if (row.destroyed_at.empty()) {
            version.ciphertext = row.ciphertext;
        }
        version.deleted_at = row.deleted_at;
        version.destroyed_at = row.destroyed_at;
        return put_absent(records, control::SecretVersionCodec::collection, key(records, row),
                          control::SecretVersionCodec::lookup(row.owner.mount_id, row.name),
                          control::SecretVersionCodec::encode(version), row.at_us);
    }
};

struct EncryptionKeyCopy {
    static std::string id(const control::LegacyEncryptionKey& row) {
        return row.key.id;
    }
    static const control::LegacySecretOwner& owner(const control::LegacyEncryptionKey& row) {
        return row.owner;
    }
    static liquid::runtime::task<PresentRecord> present(control::WorkspaceRecords& records,
                                                        const control::LegacyEncryptionKey& row) {
        return present_record(
            records, control::EncryptionKeyCodec::collection,
            control::EncryptionKeyCodec::key(records, row.key.mount_id, row.key.name));
    }
    // A rotation since the copy (a later lake revision) is accepted.
    static bool same(const ApiContext&, const control::StoredRecord& stored,
                     const control::LegacyEncryptionKey& row) {
        const auto key = control::EncryptionKeyCodec::decode(stored);
        return key && key->key.id == row.key.id && key->key.mount_id == row.key.mount_id &&
               key->key.name == row.key.name && key->key.derived_from == row.key.derived_from &&
               key->at_us == row.at_us &&
               (key->lake_revision > 1 || key->key.current_version == row.key.current_version);
    }
    static liquid::runtime::task<Copied> write(ApiContext&, control::WorkspaceRecords& records,
                                               const control::LegacyEncryptionKey& row) {
        return put_absent(
            records, control::EncryptionKeyCodec::collection,
            control::EncryptionKeyCodec::key(records, row.key.mount_id, row.key.name),
            control::EncryptionKeyCodec::lookup(row.key.mount_id),
            control::EncryptionKeyCodec::encode(row.key), row.at_us);
    }
};

struct DataKeyCopy {
    static std::string id(const control::LegacySecretDataKey& row) {
        return row.id;
    }
    static const control::LegacySecretOwner& owner(const control::LegacySecretDataKey& row) {
        return row.owner;
    }
    static std::string key(control::WorkspaceRecords& records,
                           const control::LegacySecretDataKey& row) {
        return control::SecretDataKeyCodec::key(records, row.owner_kind, row.owner_id,
                                                row.key_version);
    }
    static liquid::runtime::task<PresentRecord> present(control::WorkspaceRecords& records,
                                                        const control::LegacySecretDataKey& row) {
        return present_record(records, control::SecretDataKeyCodec::collection, key(records, row));
    }
    static control::SecretDataKey value(const control::LegacySecretDataKey& row) {
        return {row.owner.customer_id, row.owner_kind, row.owner_id, row.key_version, row.wrapped};
    }
    // The same key: both wraps are opened and the raw keys compared, since a KEK rotation rewraps
    // the row and the record separately.
    static bool same(const ApiContext& context, const control::StoredRecord& stored,
                     const control::LegacySecretDataKey& row) {
        const auto key = control::SecretDataKeyCodec::decode(stored);
        if (!key || key->customer_id != row.owner.customer_id ||
            key->owner_kind != row.owner_kind || key->owner_id != row.owner_id ||
            key->key_version != row.key_version) {
            return false;
        }
        auto copied = unwrap_secret_data_key(context, *key);
        auto source = unwrap_secret_data_key(context, value(row));
        const bool equal = copied && source && *copied == *source;
        if (copied) {
            std::ranges::fill(*copied, '\0');
        }
        if (source) {
            std::ranges::fill(*source, '\0');
        }
        return equal;
    }
    // The row's wrap is copied as it is; if a rotation made another KEK version current since the
    // row was read, the record is rewrapped by it before the copy answers.
    static liquid::runtime::task<Copied> write(ApiContext& context,
                                               control::WorkspaceRecords& records,
                                               const control::LegacySecretDataKey& row) {
        const std::string id = key(records, row);
        auto written = control::settled(co_await put_absent(
            records, control::SecretDataKeyCodec::collection, id,
            control::SecretDataKeyCodec::lookup(row.owner.customer_id),
            control::SecretDataKeyCodec::encode(value(row)), row.at_us));
        if (!written || !*written) {
            co_return written;
        }
        auto settled = control::settled(co_await rewrap_secret_data_key(context, records, id));
        if (!settled) {
            co_return Copied(std::unexpected(settled.error()));
        }
        co_return Copied(true);
    }
};

// A dynamic lease (lake#362) follows its mount. Its route is recorded before its record, as a new
// lease's is, so the expiry loop finds every copied lease; the route is due when the lease
// expires, or at once for a revoked lease, whose sealed value the loop then erases.
struct LeaseCopy {
    static std::string id(const control::LegacyDynamicSecretLease& row) {
        return row.id;
    }
    static const control::LegacySecretOwner& owner(const control::LegacyDynamicSecretLease& row) {
        return row.owner;
    }
    static liquid::runtime::task<PresentRecord>
    present(control::WorkspaceRecords& records, const control::LegacyDynamicSecretLease& row) {
        return present_record(records, control::SecretLeaseCodec::collection, row.id);
    }
    // The same lease, or one renewed, revoked or expired since the copy (a later lake revision).
    static bool same(const ApiContext&, const control::StoredRecord& stored,
                     const control::LegacyDynamicSecretLease& row) {
        const auto lease = control::SecretLeaseCodec::decode(stored);
        return lease && lease->mount_id == row.owner.mount_id && lease->role == row.role &&
               lease->at_us == row.at_us &&
               (lease->lake_revision > 1 ||
                (lease->ciphertext.value_or("") == row.encrypted_value &&
                 lease->expires_us == row.expires_us && lease->revoked_at == row.revoked_at &&
                 lease->renewals == row.renewals));
    }
    static liquid::runtime::task<Copied> write(ApiContext& context,
                                               control::WorkspaceRecords& records,
                                               const control::LegacyDynamicSecretLease& row) {
        auto routed = context.store->record_secret_lease_route(
            {row.id, row.owner.mount_id, row.owner.customer_id, row.owner.workspace_id,
             row.revoked_at.empty() ? row.expires_us : row.revoked_us});
        if (!routed) {
            co_return Copied(std::unexpected(
                control::ZettaControlError{routed.error().code, routed.error().message}));
        }
        control::SecretLeaseRecord lease;
        lease.id = row.id;
        lease.mount_id = row.owner.mount_id;
        lease.role = row.role;
        lease.ciphertext = row.encrypted_value;
        lease.expires_us = row.expires_us;
        lease.revoked_at = row.revoked_at;
        lease.renewals = row.renewals;
        co_return control::settled(co_await put_absent(
            records, control::SecretLeaseCodec::collection, row.id,
            control::SecretLeaseCodec::lookup(row.owner.mount_id),
            control::SecretLeaseCodec::encode(lease), row.at_us));
    }
};

// One page of a retired secrets table, after recording each legacy mount's owner and route.
liquid::runtime::task<ApiResult> copy_secrets(ApiContext& context, const std::string& collection,
                                              const std::string& mode, const std::string& after,
                                              int limit) {
    if (auto assigned = context.store->assign_legacy_secrets(); !assigned) {
        co_return ApiResult::failure(assigned.error().code, assigned.error().message);
    }
    const auto failed = [](const control::StoreError& error) {
        return ApiResult::failure(error.code, error.message);
    };
    if (collection == "secrets_mounts") {
        auto rows = context.store->legacy_secret_mounts(after, limit);
        if (!rows) {
            co_return failed(rows.error());
        }
        co_return co_await copy_secret_rows(context, collection, mode, *rows, limit, MountCopy{});
    }
    if (collection == "secrets_entries") {
        auto rows = context.store->legacy_secret_entries(after, limit);
        if (!rows) {
            co_return failed(rows.error());
        }
        co_return co_await copy_secret_rows(context, collection, mode, *rows, limit, EntryCopy{});
    }
    if (collection == "secrets_versions") {
        auto rows = context.store->legacy_secret_versions(after, limit);
        if (!rows) {
            co_return failed(rows.error());
        }
        co_return co_await copy_secret_rows(context, collection, mode, *rows, limit,
                                            VersionCopy{});
    }
    if (collection == "secrets_encryption_keys") {
        auto rows = context.store->legacy_encryption_keys(after, limit);
        if (!rows) {
            co_return failed(rows.error());
        }
        co_return co_await copy_secret_rows(context, collection, mode, *rows, limit,
                                            EncryptionKeyCopy{});
    }
    if (collection == "secrets_dynamic_leases") {
        auto rows = context.store->legacy_dynamic_secret_leases(after, limit);
        if (!rows) {
            co_return failed(rows.error());
        }
        co_return co_await copy_secret_rows(context, collection, mode, *rows, limit, LeaseCopy{});
    }
    auto rows = context.store->legacy_secret_data_keys(after, limit);
    if (!rows) {
        co_return failed(rows.error());
    }
    co_return co_await copy_secret_rows(context, collection, mode, *rows, limit, DataKeyCopy{});
}

// The content of each agent task (lake#374, lake#375): one record per task in its own workspace,
// keyed by the task, with the task's creation instant, holding the retired body, message, result
// and finished phase. A task whose events were recorded after the deploy but before the copy has a
// record without a request: the copy gives it the request and keeps what the events wrote, and
// verify accepts any record changed since (a later lake revision) whose request matches.
bool same_agent_task(const control::AgentTaskContent& stored, const control::LegacyAgentTask& row) {
    const control::AgentTask& source = row.task;
    const std::string phase =
        source.phase == "Succeeded" || source.phase == "Failed" ? source.phase : std::string();
    const bool changed = stored.lake_revision > 1;
    return stored.customer_id == source.customer_id && stored.request == source.body &&
           (changed || (stored.message == source.message && stored.result == source.result &&
                        stored.phase == phase && stored.at_us == row.at_us));
}

liquid::runtime::task<ApiResult> copy_agent_tasks(ApiContext& context, const std::string& mode,
                                                  const std::string& after, int limit) {
    if (auto assigned = context.store->assign_legacy_agent_tasks(); !assigned) {
        co_return ApiResult::failure(assigned.error().code, assigned.error().message);
    }
    auto rows = context.store->legacy_agent_tasks(after, limit);
    if (!rows) {
        co_return ApiResult::failure(rows.error().code, rows.error().message);
    }
    json unresolved = json::array();
    // A task whose workspace was deleted went with it and does not block completion.
    bool blocking = false;
    std::map<std::string, std::vector<const control::LegacyAgentTask*>> by_workspace;
    for (const control::LegacyAgentTask& row : *rows) {
        if (row.workspace_id.empty()) {
            blocking = blocking || row.outcome != "workspace_deleted";
            unresolved.push_back({{"id", row.task.id},
                                  {"customer_id", row.task.customer_id},
                                  {"outcome", row.outcome}});
        } else {
            by_workspace[row.workspace_id].push_back(&row);
        }
    }
    std::map<std::string, WorkspaceTally> tallies;
    for (const auto& [workspace_id, owned] : by_workspace) {
        WorkspaceTally& tally = tallies[workspace_id];
        tally.customer_id = owned.front()->task.customer_id;
        tally.rows = owned.size();
        if (mode == "plan") {
            continue;
        }
        auto records = workspace_records(context, tally.customer_id, workspace_id);
        if (!records) {
            tally.error = records.error().code + ": " + records.error().message;
            continue;
        }
        for (const control::LegacyAgentTask* row : owned) {
            const control::AgentTask& source = row->task;
            // Any space's record under the task's key: verify compares the space.
            auto present = control::settled(
                co_await control::get_agent_task_content(*records, source.id, {}));
            if (!present) {
                tally.error = present.error().code + ": " + present.error().message;
                break;
            }
            if (mode == "verify") {
                if (!*present) {
                    tally.missing.push_back(source.id);
                } else if (!same_agent_task(**present, *row)) {
                    tally.mismatched.push_back(source.id);
                } else {
                    ++tally.verified;
                }
                continue;
            }
            if (*present && !(*present)->request.is_null()) {
                ++tally.already_present;
                continue;
            }
            if (*present) {
                // Events recorded before the copy: add the request, keep what they wrote.
                auto filled = control::settled(co_await control::fill_agent_task_request(
                    *records, source.id, source.customer_id, source.body));
                if (!filled) {
                    tally.error = filled.error().code + ": " + filled.error().message;
                    break;
                }
                ++tally.copied;
                continue;
            }
            const bool finished = source.phase == "Succeeded" || source.phase == "Failed";
            auto written = control::settled(co_await control::create_agent_task_content(
                *records, {.task_id = source.id,
                           .customer_id = source.customer_id,
                           .agent = source.agent,
                           .request = source.body,
                           .message = source.message,
                           .result = source.result,
                           .phase = finished ? source.phase : std::string(),
                           .at_us = row->at_us}));
            if (!written && written.error().code == "conflict") {
                ++tally.already_present; // a concurrent copy or event; verify compares it
                continue;
            }
            if (!written) {
                tally.error = written.error().code + ": " + written.error().message;
                break;
            }
            ++tally.copied;
        }
    }
    co_return copy_result("agent_tasks", mode, rows->size(), limit,
                          rows->empty() ? std::string() : rows->back().task.id,
                          std::move(unresolved), tallies, blocking);
}

// The central audit events about a workspace's data, recorded before decision 52 (lake#317,
// migration 0063): each goes to its workspace's audit as it is, under its id, keeping its
// signature and central predecessor, so the central chain still holds its place. An event whose
// workspace was deleted stays central and does not hold the copy back; nothing needs selecting,
// since an event no workspace can be found for is not assigned at all. Verify compares the copy
// with the central row and checks its signature.
liquid::runtime::task<ApiResult> copy_audit_events(ApiContext& context, const std::string& mode,
                                                   const std::string& after, int limit) {
    if (auto assigned = context.store->assign_legacy_audit_events(workspace_audit_actions());
        !assigned) {
        co_return ApiResult::failure(assigned.error().code, assigned.error().message);
    }
    auto rows = context.store->legacy_audit_events(after, limit);
    if (!rows) {
        co_return ApiResult::failure(rows.error().code, rows.error().message);
    }
    json unresolved = json::array();
    std::map<std::string, std::vector<const control::LegacyAuditEvent*>> by_workspace;
    for (const control::LegacyAuditEvent& row : *rows) {
        if (row.workspace_id.empty()) {
            unresolved.push_back({{"id", row.event.id},
                                  {"customer_id", row.customer_id},
                                  {"outcome", row.outcome}});
        } else {
            by_workspace[row.workspace_id].push_back(&row);
        }
    }
    std::map<std::string, WorkspaceTally> tallies;
    for (const auto& [workspace_id, owned] : by_workspace) {
        WorkspaceTally& tally = tallies[workspace_id];
        tally.customer_id = owned.front()->customer_id;
        tally.rows = owned.size();
        if (mode == "plan") {
            continue;
        }
        auto records = workspace_records(context, tally.customer_id, workspace_id);
        if (!records) {
            tally.error = records.error().code + ": " + records.error().message;
            continue;
        }
        for (const control::LegacyAuditEvent* row : owned) {
            auto present =
                control::settled(co_await control::get_copied_audit_event(*records, row->event.id));
            if (!present) {
                tally.error = present.error().code + ": " + present.error().message;
                break;
            }
            if (mode == "verify") {
                if (!*present) {
                    tally.missing.push_back(row->event.id);
                } else if (audit::to_json((*present)->event) != audit::to_json(row->event) ||
                           !audit::verify_event(*context.store, (*present)->event)) {
                    tally.mismatched.push_back(row->event.id);
                } else {
                    ++tally.verified;
                }
                continue;
            }
            if (*present) {
                ++tally.already_present;
                continue;
            }
            auto copied =
                control::settled(co_await control::copy_central_audit_event(*records, row->event));
            if (!copied) {
                tally.error = copied.error().code + ": " + copied.error().message;
                break;
            }
            ++(*copied ? tally.copied : tally.already_present);
        }
    }
    co_return copy_result("audit_events", mode, rows->size(), limit,
                          rows->empty() ? std::string() : rows->back().event.id,
                          std::move(unresolved), tallies, false);
}

// The instance registry's tenant tags and the manifests they name (lake#383 phase A): each tag
// goes to the workspace its repository is routed to, as a manifest record (`if_absent`, keyed by
// digest), a blob record for each blob the manifest references, and a tag record (`if_absent`,
// keyed by repository and tag, marked `copied`). Each run finds the owner of each repository
// (legacy_image_owner); only `copy` writes the route of an unrouted one, so `plan` and `verify`
// change nothing. A repository without an owner is unresolved (`needs_selection` until an operator
// calls create_image_repository, `invalid_name`), as is a tag whose manifest or blobs the registry
// no longer holds (`blob_missing`); `unknown_space` does not block completion. Verify reads each tag
// back: one the copy wrote must name the same manifest with the same bytes, unless a later push
// moved it; one a push wrote (after the cutover, before the copy reached it) is newer and is kept.
// Blobs stay in the instance registry until phase B. Rows are ordered by `<repository>:<tag>`.
liquid::runtime::task<ApiResult> copy_oci_tags(ApiContext& context, const std::string& mode,
                                               const std::string& after, int limit) {
    const WorkspaceContent* content = context.workspace_content;
    if (content == nullptr || !content->legacy_image_tags || !content->read_blob ||
        !content->blob_size) {
        co_return ApiResult::failure("store_unavailable",
                                     "this lake has no registry storage to copy images from");
    }
    const std::vector<LegacyImageTag> rows = content->legacy_image_tags(after, limit);
    json unresolved = json::array();
    struct Owned {
        const LegacyImageTag* row;
        std::string customer_id;
        std::string name;
        std::string document;
        std::vector<control::OciReference> references;
    };
    std::map<std::string, std::vector<Owned>> by_workspace;
    std::map<std::string, std::optional<control::Customer>> customers;
    std::map<std::pair<std::string, std::string>, ImageOwner> owners;
    const bool write = mode == "copy";
    for (const LegacyImageTag& row : rows) {
        const std::string id = row.repository + ":" + row.tag;
        const auto repository = tenant_repository(row.repository);
        auto& customer = customers[repository ? repository->space : std::string()];
        if (repository && !customer) {
            customer = space_customer(*context.store, repository->space);
        }
        if (!repository || !customer) {
            unresolved.push_back({{"id", id}, {"outcome", "unknown_space"}});
            continue;
        }
        auto [owner, fresh] = owners.try_emplace({customer->id, repository->name});
        if (fresh) {
            owner->second =
                legacy_image_owner(*context.store, *customer, repository->name, write);
        }
        if (owner->second.workspace_id.empty()) {
            unresolved.push_back(
                {{"id", id}, {"customer_id", customer->id}, {"outcome", owner->second.outcome}});
            continue;
        }
        // The manifest and every blob it references must still be in the registry: a tag copied
        // without them could never be pulled, and its sizes would be wrong.
        const auto bytes = content->read_blob(row.digest);
        const json document = bytes ? json::parse(*bytes, nullptr, false) : json();
        auto digests = document.is_object()
                           ? manifest_digests(document)
                           : std::expected<std::vector<std::string>, std::string>(
                                 std::unexpected("missing"));
        std::vector<control::OciReference> references;
        bool missing = !digests;
        for (const std::string& digest : digests ? *digests : std::vector<std::string>{}) {
            const auto size = content->blob_size(digest);
            missing = missing || !size;
            references.push_back({digest, size.value_or(0)});
        }
        if (missing) {
            unresolved.push_back(
                {{"id", id}, {"customer_id", customer->id}, {"outcome", "blob_missing"}});
            continue;
        }
        by_workspace[owner->second.workspace_id].push_back(
            {&row, customer->id, repository->name, *bytes, std::move(references)});
    }
    std::map<std::string, WorkspaceTally> tallies;
    for (auto& [workspace_id, owned] : by_workspace) {
        WorkspaceTally& tally = tallies[workspace_id];
        tally.customer_id = owned.front().customer_id;
        tally.rows = owned.size();
        if (mode == "plan") {
            continue;
        }
        auto records = workspace_records(context, tally.customer_id, workspace_id);
        if (!records) {
            tally.error = records.error().code + ": " + records.error().message;
            continue;
        }
        for (Owned& item : owned) {
            const LegacyImageTag& row = *item.row;
            const std::string id = row.repository + ":" + row.tag;
            auto tag = control::settled(
                co_await control::get_oci_tag(*records, item.customer_id, item.name, row.tag));
            if (!tag) {
                tally.error = tag.error().code + ": " + tag.error().message;
                break;
            }
            if (mode == "verify") {
                if (!*tag) {
                    tally.missing.push_back(id);
                    continue;
                }
                // A push wrote it, or moved it since the copy: newer than the registry's pointer.
                if (!(*tag)->copied || (*tag)->lake_revision > 1) {
                    ++tally.verified;
                    continue;
                }
                auto manifest = control::settled(
                    co_await control::get_oci_manifest(*records, item.customer_id, row.digest));
                if (!manifest) {
                    tally.error = manifest.error().code + ": " + manifest.error().message;
                    break;
                }
                if ((*tag)->digest == row.digest && *manifest &&
                    (*manifest)->document == item.document) {
                    ++tally.verified;
                } else {
                    tally.mismatched.push_back(id);
                }
                continue;
            }
            if (*tag) {
                ++tally.already_present;
                continue;
            }
            bool failed = false;
            for (const control::OciReference& reference : item.references) {
                auto recorded = control::settled(co_await control::record_oci_blob(
                    *records, {item.customer_id, reference.digest, reference.size}));
                if (!recorded) {
                    tally.error = recorded.error().code + ": " + recorded.error().message;
                    failed = true;
                    break;
                }
            }
            if (failed) {
                break;
            }
            auto manifest = control::settled(co_await control::put_oci_manifest(
                *records, {.customer_id = item.customer_id,
                           .digest = row.digest,
                           .content_type = row.content_type,
                           .document = item.document,
                           .references = item.references}));
            if (!manifest) {
                tally.error = manifest.error().code + ": " + manifest.error().message;
                break;
            }
            auto written = control::settled(co_await control::create_oci_tag(
                *records, {.customer_id = item.customer_id,
                           .repository = item.name,
                           .tag = row.tag,
                           .digest = row.digest,
                           .content_type = row.content_type,
                           .manifest_size = item.document.size(),
                           .references = item.references,
                           .copied = true}));
            if (!written && written.error().code == "conflict") {
                ++tally.already_present; // a concurrent copy or push; verify compares it
                continue;
            }
            if (!written) {
                tally.error = written.error().code + ": " + written.error().message;
                break;
            }
            ++tally.copied;
        }
    }
    // A tag of a space that no longer exists has nowhere to go and does not hold the copy back.
    const bool blocking = std::ranges::any_of(unresolved, [](const json& item) {
        return item.value("outcome", std::string()) != "unknown_space";
    });
    co_return copy_result("oci_tags", mode, rows.size(), limit,
                          rows.empty() ? std::string() : rows.back().repository + ":" + rows.back().tag,
                          std::move(unresolved), tallies, blocking);
}

// Only the operator copies, and the operator bypasses policy; anyone else is refused here, before
// a policy lookup on an object no grant can name.
std::expected<std::string, ApiError> records_object(const json&, ApiContext&) {
    return std::unexpected(ApiError{"unauthorized", "operator credential is required", {}});
}

} // namespace

void register_workspace_record_capabilities(Registry& registry) {
    json collections = json::array();
    for (const std::string_view name : copy_collections) {
        collections.push_back(name);
    }
    registry.add(Capability{
        .name = "copy_workspace_records",
        .title = "Copy retired records into workspaces",
        .description =
            "Operator only. The one-shot move of a retired control-store table into the "
            "workspace Zetta that owns each row (docs/architecture/workspace-records.md): `plan` "
            "reports each row's recorded owner, `copy` writes the missing rows (rerunning copies "
            "nothing twice), and `verify` reads every assigned row back and compares it. Rows "
            "without one owner are listed under `unresolved` and never copied. Pages by row id "
            "with `after` and `limit`; `next_after` is null on the last page.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"collection", {{"type", "string"}, {"enum", collections}}},
                           {"mode",
                            {{"type", "string"}, {"enum", json::array({"plan", "copy", "verify"})}}},
                           {"after", {{"type", "string"}}},
                           {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}}}}},
                         {"required", json::array({"collection", "mode"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "workspace record copy",
        .action = "admin",
        .resolve_object = records_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.operator_credential) {
                co_return ApiResult::failure("unauthorized", "operator credential is required");
            }
            if (!context.store) {
                co_return ApiResult::failure("store_unavailable",
                                             "no control-plane store is configured");
            }
            if (!context.workspaces) {
                co_return ApiResult::failure("store_unavailable",
                                             "workspaces are not configured on this lake");
            }
            const std::string mode = input.at("mode");
            context.audit_kind = "workspace_records.copy";
            context.audit_details = {{"collection", input.at("collection")}, {"mode", mode}};
            const std::string after = input.value("after", std::string());
            const int limit = input.value("limit", 1000);
            if (input.at("collection") == "opportunities") {
                co_return co_await copy_opportunities(context, mode, after, limit);
            }
            if (input.at("collection") == "drive_share_links") {
                co_return co_await copy_drive_share_links(context, mode, after, limit);
            }
            if (input.at("collection") == "forge_records") {
                co_return co_await copy_forge_records(context, mode, after, limit);
            }
            if (input.at("collection") == "audit_events") {
                co_return co_await copy_audit_events(context, mode, after, limit);
            }
            if (input.at("collection") == "platform_mirror") {
                co_return co_await copy_platform_mirror(context, mode, after, limit);
            }
            if (input.at("collection") == "platform_environments") {
                co_return co_await copy_platform_environments(context, mode, after, limit);
            }
            if (input.at("collection") == "auth_app_users") {
                co_return co_await copy_application_users(context, mode, after, limit);
            }
            if (input.at("collection") == "auth_app_organizations") {
                co_return co_await copy_application_organizations(context, mode, after, limit);
            }
            if (const std::string collection = input.at("collection");
                collection.starts_with("secrets_")) {
                co_return co_await copy_secrets(context, collection, mode, after, limit);
            }
            if (input.at("collection") == "auth_app_webhook_events") {
                co_return co_await copy_application_webhook_events(context, mode, after, limit);
            }
            if (input.at("collection") == "metering_usage") {
                co_return co_await copy_metering_usage(context, mode, after, limit);
            }
            if (input.at("collection") == "metering_build_records") {
                co_return co_await copy_metering_build_records(context, mode, after, limit);
            }
            if (input.at("collection") == "auth_applications") {
                co_return co_await copy_application_settings(context, mode, after, limit);
            }
            if (input.at("collection") == "repositories") {
                co_return co_await copy_repository_descriptions(context, mode, after, limit);
            }
            if (input.at("collection") == "oci_tags") {
                co_return co_await copy_oci_tags(context, mode, after, limit);
            }
            if (input.at("collection") == "agent_tasks") {
                co_return co_await copy_agent_tasks(context, mode, after, limit);
            }
            if (input.at("collection") == "auth_app_signing_keys") {
                co_return co_await copy_application_signing_keys(context, mode, after, limit);
            }
            if (input.at("collection") == "auth_app_authorization_codes") {
                co_return co_await copy_application_authorization_codes(context, mode, after,
                                                                        limit);
            }
            if (input.at("collection") == "auth_app_webhooks") {
                co_return co_await copy_application_webhooks(context, mode, after, limit);
            }
            if (input.at("collection") == "auth_app_webhook_deliveries") {
                co_return co_await copy_application_webhook_deliveries(context, mode, after,
                                                                       limit);
            }
            co_return co_await copy_stream_owners(context, mode, after, limit);
        }});
}

} // namespace lake::api
