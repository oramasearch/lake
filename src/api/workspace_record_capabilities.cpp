// The one-shot copy of a retired control-store table into the owning workspace Zettas
// (docs/architecture/workspace-records.md, lake#370). One operator capability serves every
// collection that moves: each follow-up adds its collection to `copy_collections` and a copier
// (ask_messages, lake#370; opportunities, lake#376; stream_owners, lake#379;
// drive_share_links, lake#350).

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "lake/api/capabilities.hpp"
#include "lake/control/ask_messages.hpp"
#include "lake/control/drive_share_links.hpp"
#include "lake/control/opportunities.hpp"
#include "lake/control/store.hpp"
#include "lake/control/stream_owners.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

using json = nlohmann::json;

constexpr std::array<std::string_view, 4> copy_collections = {"ask_messages", "opportunities",
                                                             "stream_owners", "drive_share_links"};

struct WorkspaceTally {
    std::string customer_id;
    std::size_t rows = 0, copied = 0, already_present = 0, verified = 0;
    json missing = json::array(), mismatched = json::array();
    std::string error;
};

bool same_message(const control::AskMessage& stored, const control::AskMessage& source) {
    return stored.conversation_id == source.conversation_id &&
           stored.customer_id == source.customer_id && stored.user_id == source.user_id &&
           stored.role == source.role && stored.content == source.content &&
           stored.payload == source.payload && stored.at_us == source.at_us;
}

// The answer of one page of a copy, the same for every collection.
ApiResult copy_result(std::string_view collection, const std::string& mode, std::size_t rows,
                      int limit, const std::string& last_id, json unresolved,
                      const std::map<std::string, WorkspaceTally>& tallies) {
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
    const bool all_assigned = unresolved.empty();
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

// One page of ask_messages: assignments first (so rows an old replica wrote after migration 0045
// are owned too), then per workspace and conversation, one read and the missing appends.
liquid::runtime::task<ApiResult> copy_ask_messages(ApiContext& context, const std::string& mode,
                                                   const std::string& after, int limit) {
    if (auto assigned = context.store->assign_legacy_ask_messages(); !assigned) {
        co_return ApiResult::failure(assigned.error().code, assigned.error().message);
    }
    auto rows = context.store->legacy_ask_messages(after, limit);
    if (!rows) {
        co_return ApiResult::failure(rows.error().code, rows.error().message);
    }
    json unresolved = json::array();
    std::map<std::string, std::vector<const control::LegacyAskMessage*>> by_workspace;
    for (const control::LegacyAskMessage& row : *rows) {
        if (row.workspace_id.empty()) {
            unresolved.push_back({{"id", row.message.id},
                                  {"customer_id", row.message.customer_id},
                                  {"outcome", row.outcome}});
        } else {
            by_workspace[row.workspace_id].push_back(&row);
        }
    }
    std::map<std::string, WorkspaceTally> tallies;
    for (const auto& [workspace_id, owned] : by_workspace) {
        WorkspaceTally& tally = tallies[workspace_id];
        tally.customer_id = owned.front()->message.customer_id;
        tally.rows = owned.size();
        if (mode == "plan") {
            continue;
        }
        auto records = workspace_records(context, tally.customer_id, workspace_id);
        if (!records) {
            tally.error = records.error().code + ": " + records.error().message;
            continue;
        }
        control::AskMessages messages(*records);
        // One read per conversation, shared by the rows of that conversation.
        std::map<std::string, std::vector<const control::LegacyAskMessage*>> by_lookup;
        for (const control::LegacyAskMessage* row : owned) {
            by_lookup[control::AskMessageCodec::lookup(row->message)].push_back(row);
        }
        for (const auto& [lookup, conversation] : by_lookup) {
            auto present = control::settled(co_await messages.list(lookup));
            if (!present) {
                tally.error = present.error().code + ": " + present.error().message;
                break;
            }
            std::map<std::string, const control::AskMessage*> stored;
            for (const control::AskMessage& message : *present) {
                stored.emplace(message.id, &message);
            }
            for (const control::LegacyAskMessage* row : conversation) {
                const auto found = stored.find(row->message.id);
                if (mode == "verify") {
                    if (found == stored.end()) {
                        tally.missing.push_back(row->message.id);
                    } else if (!same_message(*found->second, row->message)) {
                        tally.mismatched.push_back(row->message.id);
                    } else {
                        ++tally.verified;
                    }
                    continue;
                }
                if (found != stored.end()) {
                    ++tally.already_present;
                    continue;
                }
                auto appended = control::settled(co_await messages.append(
                    row->message, control::RecordWrite{row->message.id, row->message.at_us}));
                if (!appended && appended.error().code == "conflict") {
                    // The key is already there (a concurrent copy); verify compares it.
                    ++tally.already_present;
                    continue;
                }
                if (!appended) {
                    tally.error = appended.error().code + ": " + appended.error().message;
                    break;
                }
                ++tally.copied;
            }
            if (!tally.error.empty()) {
                break;
            }
        }
    }
    co_return copy_result("ask_messages", mode, rows->size(), limit,
                          rows->empty() ? std::string() : rows->back().message.id,
                          std::move(unresolved), tallies);
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
            if (input.at("collection") == "stream_owners") {
                co_return co_await copy_stream_owners(context, mode, after, limit);
            }
            co_return co_await copy_ask_messages(context, mode, after, limit);
        }});
}

} // namespace lake::api
