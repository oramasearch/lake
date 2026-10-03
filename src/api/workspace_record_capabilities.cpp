// The one-shot copy of a retired control-store table into the owning workspace Zettas
// (docs/architecture/workspace-records.md, lake#370). One operator capability serves every
// collection that moves: each follow-up adds its collection to `copy_collections` and a copier
// (opportunities, lake#376; stream_owners, lake#379; drive_share_links, lake#350;
// auth_app_webhook_events and auth_app_webhook_deliveries, lake#356 and lake#357; metering_usage
// and metering_build_records, lake#348 and lake#349; forge_records, lake#341; auth_app_users and
// auth_app_organizations, lake#353 and lake#354). A collection
// leaves the list when the migration that drops its table lands (ask_messages, lake#370: copied
// and verified on dev, dropped by migration 0050, lake#483).

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "lake/api/capabilities.hpp"
#include "lake/control/application_user_records.hpp"
#include "lake/control/application_webhook_records.hpp"
#include "lake/control/drive_share_links.hpp"
#include "lake/control/forge_records.hpp"
#include "lake/control/metering.hpp"
#include "lake/control/opportunities.hpp"
#include "lake/control/store.hpp"
#include "lake/control/stream_owners.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

using json = nlohmann::json;

constexpr std::array<std::string_view, 10> copy_collections = {"opportunities",
                                                               "stream_owners",
                                                               "drive_share_links",
                                                               "auth_app_webhook_events",
                                                               "auth_app_webhook_deliveries",
                                                               "metering_usage",
                                                               "metering_build_records",
                                                               "forge_records",
                                                               "auth_app_users",
                                                               "auth_app_organizations"};

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
            if (input.at("collection") == "auth_app_users") {
                co_return co_await copy_application_users(context, mode, after, limit);
            }
            if (input.at("collection") == "auth_app_organizations") {
                co_return co_await copy_application_organizations(context, mode, after, limit);
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
            if (input.at("collection") == "auth_app_webhook_deliveries") {
                co_return co_await copy_application_webhook_deliveries(context, mode, after,
                                                                       limit);
            }
            co_return co_await copy_stream_owners(context, mode, after, limit);
        }});
}

} // namespace lake::api
