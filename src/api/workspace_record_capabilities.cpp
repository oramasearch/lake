// The one-shot copy of a retired control-store table into the owning workspace Zettas
// (docs/architecture/workspace-records.md, lake#370). One operator capability serves every
// collection that moves: each follow-up adds its collection to `copy_collections` and a copier.
// A collection leaves the list when the migration that drops its source lands. Three remain:
// audit_events (lake#317), the central events about a workspace's data, which stay in the central
// chain and are never dropped; oci_tags (lake#383), the tenant tags of the instance registry,
// whose source is files rather than a table; and oci_blobs (lake#383 phase B), the tenant blobs
// the instance registry still holds, streamed into each workspace's liquid store. Every other
// collection was copied and verified on dev and its source dropped: ask_messages by migration 0050 (lake#483); opportunities,
// stream_owners, drive_share_links, the application tables, metering, forge_records, the secrets
// tables, the platform tables and the content columns of auth_applications, repositories and
// agent tasks by migration 0070.

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <openssl/evp.h>

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

constexpr std::array<std::string_view, 3> copy_collections = {"audit_events", "oci_tags",
                                                              "oci_blobs"};

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



// The central audit events about a workspace's data, recorded before the audit split (lake#317,
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
                                     "this Lake has no registry storage to copy images from");
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

// SHA-256 over bytes fed in pieces, as `sha256:<hex>`.
class Sha256Stream {
public:
    Sha256Stream() : context_(EVP_MD_CTX_new(), &EVP_MD_CTX_free) {
        ok_ = context_ && EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) == 1;
    }
    void update(std::span<const std::byte> bytes) {
        ok_ = ok_ && EVP_DigestUpdate(context_.get(), bytes.data(), bytes.size()) == 1;
    }
    std::optional<std::string> finish() {
        std::array<unsigned char, 32> digest{};
        unsigned int size = 0;
        if (!ok_ || EVP_DigestFinal_ex(context_.get(), digest.data(), &size) != 1 ||
            size != digest.size()) {
            return std::nullopt;
        }
        std::string output = "sha256:";
        for (const unsigned char byte : digest) {
            output += std::format("{:02x}", byte);
        }
        return output;
    }

private:
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_;
    bool ok_ = false;
};

constexpr std::size_t blob_window = 4 * 1024 * 1024;

// `sha256:<hex>` of a file of the instance registry, read in windows.
SyncStoreResult<std::string> file_sha256(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(SyncStoreError{"io", "could not open " + path.string()});
    }
    Sha256Stream hash;
    std::vector<char> window(blob_window);
    while (input) {
        input.read(window.data(), static_cast<std::streamsize>(window.size()));
        const std::streamsize read = input.gcount();
        if (read > 0) {
            hash.update(std::as_bytes(std::span(window.data(), static_cast<std::size_t>(read))));
        }
    }
    const auto digest = input.bad() ? std::nullopt : hash.finish();
    if (!digest) {
        return std::unexpected(SyncStoreError{"io", "could not read " + path.string()});
    }
    return *digest;
}

// Whether the workspace store holds exactly the blob's bytes: the object read back in windows
// (never whole), its SHA-256 and its size compared with the record's digest and size.
SyncStoreResult<bool> store_holds_blob(SyncStore& store, const std::string& customer_id,
                                       const std::string& workspace_id,
                                       const control::OciBlob& blob) {
    Sha256Stream hash;
    std::vector<std::byte> window(blob_window);
    std::uint64_t offset = 0;
    for (;;) {
        auto read = store.read_workspace_blob(customer_id, workspace_id, blob.object, offset,
                                              window, false);
        if (!read && read.error().code == "not_found") {
            return false;
        }
        if (!read) {
            return std::unexpected(read.error());
        }
        if (*read == 0) {
            break;
        }
        hash.update(std::span(window).first(static_cast<std::size_t>(*read)));
        offset += *read;
        if (offset > blob.size) {
            return false;
        }
    }
    return offset == blob.size && hash.finish() == blob.digest;
}

// The legacy tenant tags (written to the instance registry before lake#383) that are not in their
// workspace yet, with the manifest and blob digests they need from the instance registry.
// `complete`: every legacy tag of a space that still exists was found in its routed workspace. A
// tag of a deleted space (`unknown_space` in oci_tags) can never be copied or pulled, so it does
// not hold completion back, and its digests count as needed only where a record (`recorded`)
// references them too. The control-store reads and the registry's files are read on the blocking
// pool; only the tag lookups in the workspaces are awaited here.
struct UncopiedLegacy {
    bool complete = true;
    std::set<std::string> digests;
};
liquid::runtime::task<UncopiedLegacy> uncopied_legacy_digests(ApiContext& context,
                                                              const WorkspaceContent& content,
                                                              const std::set<std::string>& recorded) {
    UncopiedLegacy output;
    if (!content.legacy_image_tags) {
        co_return output;
    }
    struct Legacy {
        std::string customer_id, name, tag, workspace_id;
        bool unknown_space = false;
        std::set<std::string> digests;
    };
    auto listed = co_await blocking_call(
        [&]() -> SyncStoreResult<std::vector<Legacy>> {
            std::vector<Legacy> legacy;
            std::map<std::string, std::optional<control::Customer>> customers;
            std::string after;
            for (;;) {
                const std::vector<LegacyImageTag> page = content.legacy_image_tags(after, 1000);
                for (const LegacyImageTag& row : page) {
                    Legacy item{.tag = row.tag};
                    item.digests.insert(row.digest);
                    if (content.read_blob) {
                        const auto document = content.read_blob(row.digest);
                        const json manifest =
                            document ? json::parse(*document, nullptr, false) : json();
                        if (manifest.is_object()) {
                            if (const auto references = manifest_digests(manifest)) {
                                item.digests.insert(references->begin(), references->end());
                            }
                        }
                    }
                    const auto repository = tenant_repository(row.repository);
                    auto& customer = customers[repository ? repository->space : std::string()];
                    if (repository && !customer) {
                        customer = space_customer(*context.store, repository->space);
                    }
                    if (!repository || !customer) {
                        item.unknown_space = true;
                    } else {
                        item.customer_id = customer->id;
                        item.name = repository->name;
                        if (const auto route =
                                context.store->get_oci_repository_route(customer->id, item.name)) {
                            item.workspace_id = route->workspace_id;
                        }
                    }
                    legacy.push_back(std::move(item));
                }
                if (page.size() < 1000) {
                    break;
                }
                after = page.back().repository + ":" + page.back().tag;
            }
            return legacy;
        },
        [](std::string message) -> SyncStoreResult<std::vector<Legacy>> {
            return std::unexpected(SyncStoreError{"store_unavailable", std::move(message)});
        });
    if (!listed) {
        // Nothing known about the legacy tags: treat them as not copied.
        output.complete = false;
        co_return output;
    }
    std::map<std::string, std::optional<control::WorkspaceRecords>> records_of;
    for (const Legacy& item : *listed) {
        if (item.unknown_space) {
            for (const std::string& digest : item.digests) {
                if (recorded.contains(digest)) {
                    output.digests.insert(digest);
                }
            }
            continue;
        }
        bool copied = false;
        if (!item.workspace_id.empty()) {
            auto [slot, fresh] = records_of.try_emplace(item.workspace_id);
            if (fresh) {
                if (auto opened = workspace_records(context, item.customer_id, item.workspace_id)) {
                    slot->second.emplace(std::move(*opened));
                }
            }
            if (slot->second) {
                auto tag = control::settled(co_await control::get_oci_tag(
                    *slot->second, item.customer_id, item.name, item.tag));
                copied = tag && *tag;
            }
        }
        if (!copied) {
            output.complete = false;
            output.digests.insert(item.digests.begin(), item.digests.end());
        }
    }
    co_return output;
}

// The tenant blobs the instance registry still holds (lake#383 phase B): each
// lake_oci_blobs record of each workspace an image route names, ordered by `<workspace>/<digest>`.
// A record that already names a store object is in place. One that names none (written before
// phase B, or while its workspace was served from its space's store) is copied: `copy` checks the
// instance registry's file against the digest, streams it into the workspace's liquid store and
// names the object in the record (conditionally on the revision read), after which the blob is
// read only from the store. Unresolved: `workspace_not_switched` (the workspace has no store yet;
// move_workspace_store first), `blob_missing` (the registry no longer holds the bytes) and
// `blob_corrupt` (they do not hash to the digest); each blocks completion. `verify` reads every
// stored object back in windows and compares its SHA-256 and size with the record, and lists in
// `instance_paths` the registry files that verified copies leave behind: removable later (with
// approval), except a digest the instance's own repositories use or a record not yet copied still
// needs. Nothing is ever removed from the instance registry here.
liquid::runtime::task<ApiResult> copy_oci_blobs(ApiContext& context, const std::string& mode,
                                                const std::string& after, int limit) {
    const WorkspaceContent* content = context.workspace_content;
    if (content == nullptr || !content->blob_file || !content->instance_digests ||
        context.sync_store == nullptr) {
        co_return ApiResult::failure("store_unavailable",
                                     "this Lake has no registry storage to copy image blobs from");
    }
    auto customers = context.store->list_customers(1'000'000);
    if (!customers) {
        co_return ApiResult::failure(customers.error().code, customers.error().message);
    }
    std::map<std::string, std::string> owners; // workspace -> space
    for (const control::Customer& customer : *customers) {
        auto routes = context.store->list_oci_repository_routes(customer.id, {});
        if (!routes) {
            co_return ApiResult::failure(routes.error().code, routes.error().message);
        }
        for (const control::OciRepositoryRoute& route : *routes) {
            owners.emplace(route.workspace_id, customer.id);
        }
    }
    struct Row {
        std::string id, workspace_id, customer_id;
        control::OciBlob blob;
        bool switched = false;
    };
    std::vector<Row> rows;
    std::map<std::string, WorkspaceTally> tallies;
    std::map<std::string, control::WorkspaceRecords> opened;
    bool listed_all = true; // every routed workspace's records were read
    for (const auto& [workspace_id, customer_id] : owners) {
        auto workspace = context.store->get_workspace(workspace_id);
        auto records = workspace_records(context, customer_id, workspace_id);
        if (!workspace || !records) {
            listed_all = false;
            WorkspaceTally& tally = tallies[workspace_id];
            tally.customer_id = customer_id;
            tally.error = !workspace ? workspace.error().code + ": " + workspace.error().message
                                     : records.error().code + ": " + records.error().message;
            continue;
        }
        auto blobs = control::settled(co_await control::list_oci_blobs(*records, customer_id));
        if (!blobs) {
            listed_all = false;
            WorkspaceTally& tally = tallies[workspace_id];
            tally.customer_id = customer_id;
            tally.error = blobs.error().code + ": " + blobs.error().message;
            continue;
        }
        for (control::OciBlob& blob : *blobs) {
            rows.push_back({workspace_id + "/" + blob.digest, workspace_id, customer_id,
                            std::move(blob), !workspace->store_moved_at.empty()});
        }
        opened.emplace(workspace_id, std::move(*records));
    }
    std::ranges::sort(rows, {}, &Row::id);
    // What a record without an object still reads from the instance registry, on any page.
    std::set<std::string> still_needed;
    std::set<std::string> recorded;
    for (const Row& row : rows) {
        recorded.insert(row.blob.digest);
        if (row.blob.object.empty()) {
            still_needed.insert(row.blob.digest);
        }
    }
    std::erase_if(rows, [&](const Row& row) { return !(row.id > after); });
    if (static_cast<int>(rows.size()) > limit) {
        rows.resize(static_cast<std::size_t>(limit));
    }
    json unresolved = json::array();
    std::map<std::string, std::vector<Row*>> by_workspace;
    for (Row& row : rows) {
        if (row.blob.object.empty() && !row.switched) {
            unresolved.push_back({{"id", row.id}, {"customer_id", row.customer_id},
                                  {"outcome", "workspace_not_switched"}});
        } else if (row.blob.object.empty() && !content->blob_file(row.blob.digest)) {
            unresolved.push_back(
                {{"id", row.id}, {"customer_id", row.customer_id}, {"outcome", "blob_missing"}});
        } else {
            by_workspace[row.workspace_id].push_back(&row);
        }
    }
    // For `instance_paths`: what the instance's own repositories use, and whether every tag the
    // instance registry still holds for a tenant repository is in its workspace (oci_tags
    // complete); the manifest and blobs of one that is not are still needed.
    std::set<std::string> instance;
    bool tags_complete = true;
    if (mode == "verify") {
        auto digests = co_await blocking_call(
            [&] { return SyncStoreResult<std::set<std::string>>(content->instance_digests()); },
            [](std::string message) -> SyncStoreResult<std::set<std::string>> {
                return std::unexpected(SyncStoreError{"store_unavailable", std::move(message)});
            });
        if (!digests) {
            co_return ApiResult::failure(digests.error().code, digests.error().message);
        }
        instance = std::move(*digests);
        auto uncopied = co_await uncopied_legacy_digests(context, *content, recorded);
        if (!uncopied) {
            co_return ApiResult::failure("store_error", uncopied.error().message);
        }
        tags_complete = uncopied->complete;
        still_needed.insert(uncopied->digests.begin(), uncopied->digests.end());
    }
    std::set<std::string> instance_paths;
    std::size_t rooted_now = 0;
    for (const auto& [workspace_id, owned] : by_workspace) {
        WorkspaceTally& tally = tallies[workspace_id];
        tally.customer_id = owned.front()->customer_id;
        tally.rows = owned.size();
        if (mode == "plan") {
            continue;
        }
        control::WorkspaceRecords& records = opened.at(workspace_id);
        for (Row* row : owned) {
            const control::OciBlob& blob = row->blob;
            if (mode == "verify") {
                if (blob.object.empty()) {
                    tally.missing.push_back(row->id);
                    continue;
                }
                auto held = co_await blocking_call(
                    [&] {
                        return store_holds_blob(*context.sync_store, row->customer_id,
                                                workspace_id, blob);
                    },
                    [](std::string message) -> SyncStoreResult<bool> {
                        return std::unexpected(SyncStoreError{"store_unavailable", std::move(message)});
                    });
                if (!held) {
                    tally.error = held.error().code + ": " + held.error().message;
                    break;
                }
                if (!*held) {
                    tally.mismatched.push_back(row->id);
                    continue;
                }
                ++tally.verified;
                if (const auto file = content->blob_file(blob.digest);
                    file && !instance.contains(blob.digest) && !still_needed.contains(blob.digest)) {
                    instance_paths.insert(file->string());
                }
                continue;
            }
            if (!blob.object.empty()) {
                // In place. One stored before lake rooted image blobs is rooted now, once.
                if (!blob.rooted) {
                    auto rooted = co_await blocking_call(
                        [&] {
                            return context.sync_store->root_workspace_blob(row->customer_id,
                                                                           workspace_id, blob.object);
                        },
                        [](std::string message) -> SyncStoreResult<void> {
                            return std::unexpected(SyncStoreError{"store_unavailable", std::move(message)});
                        });
                    if (!rooted) {
                        tally.error = rooted.error().code + ": " + rooted.error().message;
                        break;
                    }
                    control::OciBlob marked = blob;
                    marked.rooted = true;
                    auto recorded =
                        control::settled(co_await control::store_oci_blob(records, std::move(marked)));
                    if (!recorded) {
                        tally.error = recorded.error().code + ": " + recorded.error().message;
                        break;
                    }
                    ++rooted_now;
                }
                ++tally.already_present;
                continue;
            }
            const auto file = content->blob_file(blob.digest);
            if (!file) {
                unresolved.push_back(
                    {{"id", row->id}, {"customer_id", row->customer_id}, {"outcome", "blob_missing"}});
                continue;
            }
            auto stored = co_await blocking_call(
                [&]() -> SyncStoreResult<WorkspaceBlob> {
                    const auto actual = file_sha256(*file);
                    if (!actual) {
                        return std::unexpected(actual.error());
                    }
                    if (*actual != blob.digest) {
                        return std::unexpected(
                            SyncStoreError{"invalid_digest", "the registry's bytes are not the digest"});
                    }
                    return context.sync_store->put_workspace_blob(row->customer_id, workspace_id,
                                                                  *file, false);
                },
                [](std::string message) -> SyncStoreResult<WorkspaceBlob> {
                    return std::unexpected(SyncStoreError{"store_unavailable", std::move(message)});
                });
            if (!stored && stored.error().code == "invalid_digest") {
                unresolved.push_back(
                    {{"id", row->id}, {"customer_id", row->customer_id}, {"outcome", "blob_corrupt"}});
                continue;
            }
            if (!stored) {
                tally.error = stored.error().code + ": " + stored.error().message;
                break;
            }
            auto recorded = control::settled(co_await control::store_oci_blob(
                records, {.customer_id = row->customer_id,
                          .digest = blob.digest,
                          .size = stored->size,
                          .object = stored->object,
                          .rooted = stored->rooted}));
            if (!recorded) {
                tally.error = recorded.error().code + ": " + recorded.error().message;
                break;
            }
            ++(recorded->object == stored->object ? tally.copied : tally.already_present);
        }
    }
    ApiResult result = copy_result("oci_blobs", mode, rows.size(), limit,
                                   rows.empty() ? std::string() : rows.back().id,
                                   std::move(unresolved), tallies);
    if (result.ok && mode == "copy") {
        result.value["rooted"] = rooted_now; // objects already in place given their liquid root
    }
    if (result.ok && mode == "verify") {
        // Advisory, and only when nothing could have been missed: every routed workspace's records
        // were read and every legacy tenant tag is in its workspace. The removal step re-checks
        // each path against the records and the instance's repositories of its own time.
        if (listed_all && tags_complete) {
            result.value["instance_paths"] = instance_paths;
        } else {
            result.value["instance_paths"] = nullptr;
            result.value["instance_paths_withheld"] =
                !listed_all ? "a routed workspace's image blob records could not be read"
                            : "the instance registry still holds tenant tags not copied into their "
                              "workspaces (copy_workspace_records oci_tags)";
        }
    }
    co_return result;
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
                                             "workspaces are not configured on this Lake");
            }
            const std::string mode = input.at("mode");
            context.audit_kind = "workspace_records.copy";
            context.audit_details = {{"collection", input.at("collection")}, {"mode", mode}};
            const std::string after = input.value("after", std::string());
            const int limit = input.value("limit", 1000);
            if (input.at("collection") == "audit_events") {
                co_return co_await copy_audit_events(context, mode, after, limit);
            }
            if (input.at("collection") == "oci_blobs") {
                co_return co_await copy_oci_blobs(context, mode, after, limit);
            }
            co_return co_await copy_oci_tags(context, mode, after, limit);
        }});
}

} // namespace lake::api
