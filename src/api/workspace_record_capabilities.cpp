// The one-shot copy of a retired control-store table into the owning workspace Zettas
// (docs/architecture/workspace-records.md, lake#370). One operator capability serves every
// collection that moves: each follow-up adds its collection to `copy_collections` and a copier.

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "lake/api/capabilities.hpp"
#include "lake/control/ask_messages.hpp"
#include "lake/control/store.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

using json = nlohmann::json;

constexpr std::array<std::string_view, 1> copy_collections = {"ask_messages"};

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
    json value{{"collection", "ask_messages"},
               {"mode", mode},
               {"rows", rows->size()},
               {"next_after", rows->empty() || static_cast<int>(rows->size()) < limit
                                  ? json(nullptr)
                                  : json(rows->back().message.id)},
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
        // Complete: every row of this page has one owner and is in its workspace, unchanged. Only
        // then may a later migration drop what this page covered.
        value["complete"] = all_assigned && failed == 0 && value["missing"].empty() &&
                            value["mismatched"].empty();
    }
    co_return ApiResult::success(std::move(value));
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
            co_return co_await copy_ask_messages(context, mode, input.value("after", std::string()),
                                                 input.value("limit", 1000));
        }});
}

} // namespace lake::api
