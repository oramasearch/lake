// The one-shot copy of a retired control-store table into the owning workspace Zettas
// (docs/architecture/workspace-records.md, lake#370). One operator capability serves every
// collection that moves: each follow-up adds its collection to `copy_collections` and a copier.
// A collection leaves the list when the migration that drops its source lands. Two remain:
// audit_events (lake#317), the central events about a workspace's data, which stay in the central
// chain and are never dropped, and oci_tags (lake#383), the tenant tags of the instance registry,
// whose source is files rather than a table. Every other collection was copied and verified on
// dev and its source dropped: ask_messages by migration 0050 (lake#483); opportunities,
// stream_owners, drive_share_links, the application tables, metering, forge_records, the secrets
// tables, the platform tables and the content columns of auth_applications, repositories and
// agent tasks by migration 0070.

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
#include "lake/control/oci_records.hpp"
#include "lake/control/store.hpp"
#include "lake/control/workspace_audit.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

using json = nlohmann::json;

constexpr std::array<std::string_view, 2> copy_collections = {"audit_events", "oci_tags"};

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
            if (input.at("collection") == "audit_events") {
                co_return co_await copy_audit_events(context, mode, after, limit);
            }
            co_return co_await copy_oci_tags(context, mode, after, limit);
        }});
}

} // namespace lake::api
