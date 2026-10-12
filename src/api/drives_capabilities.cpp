// Drives (docs/api/drives.md, lake#561): a workspace's drives, each a sealed `lake_drives` record
// in its workspace Zetta that keeps its files in a repository, as workspace files, or as shortcuts
// only (virtual), or that browses one of the workspace's bucket or filesystem stores in place
// (store, lake#656). Workspace files are `lake_drive_entries` records and, for each file, one
// sealed object in the workspace Zetta's `lake_files` bucket (drive_files.hpp). A repository drive
// is browsed and written with the drive_* capabilities of drive_capabilities.cpp.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <openssl/evp.h>

#include "lake/api/capabilities.hpp"
#include "lake/control/drive_files.hpp"
#include "lake/control/drives.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/store.hpp"
#include "lake/control/store_contents.hpp"
#include "lake/control/transcripts.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

using json = nlohmann::json;

// The most bytes one append_drive_upload chunk, one upload_drive_entries call or one
// read_drive_file answer carries (decoded); REST bodies are at most 64 MiB with base64.
constexpr std::uint64_t max_chunk_bytes = 8 * 1024 * 1024;
constexpr std::uint64_t max_inline_bytes = 16 * 1024 * 1024;
// The largest file one upload may declare.
constexpr std::uint64_t max_file_bytes = 64ULL * 1024 * 1024 * 1024;
constexpr std::size_t default_recent = 20;

std::int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

ApiResult failed(const control::ZettaControlError& error, std::string path = {}) {
    return ApiResult::failure(error.code, error.message, error.path.empty() ? path : error.path);
}

ApiResult called(std::expected<ApiResult, liquid::error> completed) {
    return completed ? std::move(*completed)
                     : ApiResult::failure("store_error", completed.error().message);
}

std::string actor(const ApiContext& context) {
    return context.acting_user_id.empty() ? context.acting_identity : context.acting_user_id;
}

// Another capability, as the same person, authorized and validated by the registry.
liquid::runtime::task<ApiResult> call_as(ApiContext& context, std::string name, json input) {
    if (context.registry == nullptr) {
        co_return ApiResult::failure("store_unavailable", "no registry to call " + name);
    }
    ApiContext inner = context;
    inner.audit_kind.clear();
    inner.audit_details = json::object();
    co_return called(co_await context.registry->call_async(name, input, inner));
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

// Read on the space, for the list reads, which authorize themselves as every list_ read does.
std::optional<ApiResult> authorize_space_read(const json& input, ApiContext& context) {
    const auto object = customer_object(input, context);
    if (!object) {
        return ApiResult::failure(object.error().code, object.error().message, object.error().path);
    }
    if (context.acting_identity == "admin") {
        return std::nullopt;
    }
    const auto decision = control::authorize(
        *context.store, context.acting_identity, *object, "read", context.authorization_cache);
    if (!decision) {
        return ApiResult::failure(decision.error().code, decision.error().message);
    }
    if (!decision->allowed) {
        return ApiResult::failure("unauthorized", "principal is not allowed to read the customer");
    }
    return std::nullopt;
}

// Whether the caller may read a repository: its policy, as the drive_* reads decide it.
bool may_read_repository(ApiContext& context, const std::string& repository_id) {
    if (context.acting_identity == "admin" || context.operator_credential) {
        return true;
    }
    const auto object = context.store->find_policy_object("repository", repository_id);
    if (!object) {
        return false;
    }
    const auto decision = control::authorize(
        *context.store, context.acting_identity, object->id, "read", context.authorization_cache);
    return decision && decision->allowed;
}

std::expected<std::vector<std::byte>, ApiError> decode_base64(std::string_view text,
                                                              std::string path) {
    if (text.empty()) {
        return std::vector<std::byte>{};
    }
    if (text.size() % 4 != 0) {
        return std::unexpected(ApiError{"invalid_input", "invalid base64 data", std::move(path)});
    }
    std::vector<std::byte> out((text.size() / 4) * 3);
    const int size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                     reinterpret_cast<const unsigned char*>(text.data()),
                                     static_cast<int>(text.size()));
    if (size < 0) {
        return std::unexpected(ApiError{"invalid_input", "invalid base64 data", std::move(path)});
    }
    std::size_t padding = 0;
    if (text.back() == '=') {
        ++padding;
    }
    if (text.size() > 1 && text[text.size() - 2] == '=') {
        ++padding;
    }
    out.resize(static_cast<std::size_t>(size) - padding);
    return out;
}

std::string bytes_text(const std::vector<std::byte>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::string encode_base64(std::string_view bytes) {
    std::string out((bytes.size() + 2) / 3 * 4, '\0');
    constexpr std::size_t block = 3 * 1024 * 1024;
    for (std::size_t offset = 0; offset < bytes.size(); offset += block) {
        const std::size_t length = std::min(block, bytes.size() - offset);
        EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data() + offset / 3 * 4),
                        reinterpret_cast<const unsigned char*>(bytes.data() + offset),
                        static_cast<int>(length));
    }
    return out;
}

// The media type a file name suggests, for a file uploaded without one.
std::string media_type_for(std::string_view name) {
    return control::drive_media_type(name);
}

bool valid_media_type(std::string_view type) {
    return !type.empty() && type.size() <= 255 && type.find('/') != std::string_view::npos &&
           std::ranges::all_of(type, [](char c) {
               const auto byte = static_cast<unsigned char>(c);
               return byte > 0x20 && byte < 0x7f && c != '"' && c != '\\';
           });
}

// The parts of a relative path ("Trip/Day 1/photo.jpg"), or nothing when one is not a name.
std::optional<std::vector<std::string>> path_parts(std::string_view path) {
    std::vector<std::string> parts;
    std::size_t offset = 0;
    while (offset <= path.size()) {
        const std::size_t slash = path.find('/', offset);
        const std::string_view part = path.substr(
            offset, slash == std::string_view::npos ? std::string_view::npos : slash - offset);
        if (!part.empty()) {
            if (!control::valid_drive_name(part)) {
                return std::nullopt;
            }
            parts.emplace_back(part);
        }
        if (slash == std::string_view::npos) {
            break;
        }
        offset = slash + 1;
    }
    if (parts.empty()) {
        return std::nullopt;
    }
    return parts;
}

// What a finished task of this file answered: a runtime failure is a store error. (A liquid task
// wraps its value in an expected, so the values below carry their own failure instead.)
template <class T> T awaited(std::expected<T, liquid::error> completed) {
    if (!completed) {
        T out{};
        out.failure = ApiResult::failure("store_error", completed.error().message);
        return out;
    }
    return std::move(*completed);
}

ApiResult called_as(std::expected<ApiResult, liquid::error> completed) {
    return completed ? std::move(*completed)
                     : ApiResult::failure("store_error", completed.error().message);
}

// ---- JSON ----

json drive_json(const control::Drive& drive) {
    json out{
        {"id", drive.id},
        {"name", drive.name},
        {"storage", drive.storage},
        {"repository_id", drive.repository_id.empty() ? json(nullptr) : json(drive.repository_id)},
        {"store_id", drive.store_id.empty() ? json(nullptr) : json(drive.store_id)},
        {"created_by", drive.created_by},
        {"created_at", drive.created_at}};
    return out;
}

json drive_schema() {
    return {
        {"type", "object"},
        {"properties",
         {{"id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"storage", {{"type", "string"}, {"enum", {"repository", "files", "virtual", "store"}}}},
          {"repository_id", {{"type", json::array({"string", "null"})}}},
          {"store_id", {{"type", json::array({"string", "null"})}}},
          {"created_by", {{"type", "string"}}},
          {"created_at", {{"type", "string"}}}}},
        {"required",
         json::array(
             {"id", "name", "storage", "repository_id", "store_id", "created_by", "created_at"})},
        {"additionalProperties", false}};
}

json entry_json(const control::DriveEntry& entry, const std::string& path) {
    json out{{"id", entry.id},
             {"drive_id", entry.drive_id},
             {"parent_id", entry.parent_id.empty() ? json(nullptr) : json(entry.parent_id)},
             {"kind", entry.kind},
             {"name", entry.name},
             {"path", path},
             {"created_by", entry.created_by},
             {"created_at", entry.created_at},
             {"modified_by", entry.modified_by},
             {"modified_at", control::record_instant(entry.modified_us)}};
    if (entry.kind == "file") {
        out["size"] = entry.size;
        out["content_type"] = entry.content_type;
        out["sha256"] = entry.sha256.empty() ? json(nullptr) : json(entry.sha256);
        out["state"] = entry.state;
    }
    if (entry.kind == "shortcut") {
        out["target"] = entry.target;
    }
    return out;
}

json entry_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"drive_id", {{"type", "string"}}},
              {"parent_id", {{"type", json::array({"string", "null"})}}},
              {"kind", {{"type", "string"}, {"enum", {"folder", "file", "shortcut"}}}},
              {"name", {{"type", "string"}}},
              {"path", {{"type", "string"}}},
              {"created_by", {{"type", "string"}}},
              {"created_at", {{"type", "string"}}},
              {"modified_by", {{"type", "string"}}},
              {"modified_at", {{"type", "string"}}},
              {"size", {{"type", "integer"}, {"minimum", 0}}},
              {"content_type", {{"type", "string"}}},
              {"sha256", {{"type", json::array({"string", "null"})}}},
              {"state", {{"type", "string"}, {"enum", {"uploading", "ready"}}}},
              {"target", {{"type", "object"}}},
              {"status", {{"type", "string"}, {"enum", {"ok", "broken", "unknown"}}}},
              {"reason", {{"type", "string"}}}}},
            {"required",
             json::array({"id",
                          "drive_id",
                          "parent_id",
                          "kind",
                          "name",
                          "path",
                          "created_by",
                          "created_at",
                          "modified_by",
                          "modified_at"})},
            {"additionalProperties", false}};
}

// An unfinished upload as list_drive_entries lists it: the entry, its age and whether it expired.
json upload_schema() {
    json schema = entry_schema();
    schema["properties"]["age_seconds"] = {{"type", "integer"}, {"minimum", 0}};
    schema["properties"]["expired"] = {{"type", "boolean"}};
    schema["required"].push_back("age_seconds");
    schema["required"].push_back("expired");
    return schema;
}

json target_schema() {
    return {{"type", "object"},
            {"description",
             "What the shortcut opens: {kind: repository, repository_id}, {kind: repository_path, "
             "repository_id, path, ref?}, {kind: drive_entry, drive_id, entry_id}, {kind: "
             "dataset|transcript|agent|model, id}, {kind: project, name} or {kind: url, url}."},
            {"properties",
             {{"kind",
               {{"type", "string"},
                {"enum",
                 {"repository",
                  "repository_path",
                  "drive_entry",
                  "dataset",
                  "transcript",
                  "project",
                  "agent",
                  "model",
                  "url"}}}},
              {"repository_id", {{"type", "string"}}},
              {"path", {{"type", "string"}}},
              {"ref", {{"type", "string"}}},
              {"drive_id", {{"type", "string"}}},
              {"entry_id", {{"type", "string"}}},
              {"id", {{"type", "string"}}},
              {"name", {{"type", "string"}}},
              {"url", {{"type", "string"}}}}},
            {"required", json::array({"kind"})},
            {"additionalProperties", false}};
}

json string_schema() {
    return {{"type", "string"}};
}

json workspace_input(json properties, json required) {
    properties["customer_id"] = string_schema();
    properties["workspace_id"] = string_schema();
    required.insert(required.begin(), "workspace_id");
    required.insert(required.begin(), "customer_id");
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

json object_of(json properties, json required) {
    json out{
        {"type", "object"}, {"properties", std::move(properties)}, {"additionalProperties", false}};
    out["required"] = std::move(required);
    return out;
}

// ---- Opening a drive ----

// A drive of the space in the workspace, with the workspace's records, or the failure to answer
// (not an expected: a liquid task already wraps its value in one).
struct Opened {
    std::optional<control::WorkspaceRecords> records;
    control::Drive drive;
    std::optional<ApiResult> failure;
};

Opened refusal(ApiResult failure) {
    Opened out;
    out.failure = std::move(failure);
    return out;
}

// Opens `input.drive_id` (or `field`) of the space in the workspace. `entries` refuses a
// repository drive, whose files are browsed with drive_browse.
liquid::runtime::task<Opened>
open_drive(const json& input, ApiContext& context, bool entries, std::string field = "drive_id") {
    const std::string customer_id = input.at("customer_id");
    auto records = workspace_records(context, customer_id, input.at("workspace_id"));
    if (!records) {
        co_return refusal(failed(records.error()));
    }
    const std::string id = input.at(field);
    auto found = control::settled(co_await control::get_drive(*records, customer_id, id));
    if (!found) {
        co_return refusal(failed(found.error()));
    }
    if (!*found) {
        co_return refusal(
            ApiResult::failure("not_found", "no such drive in this workspace", "/" + field));
    }
    if (entries && (*found)->storage == "repository") {
        co_return refusal(ApiResult::failure(
            "invalid_input",
            "a repository drive's files are browsed and written with drive_browse and the other "
            "drive_* capabilities, at its repository_id",
            "/" + field));
    }
    context.audit_details["drive_id"] = id;
    Opened out;
    out.records.emplace(std::move(*records));
    out.drive = std::move(**found);
    co_return out;
}

// The drive's entries as a tree, or the failure.
struct Read {
    std::optional<control::DriveTree> tree;
    std::optional<ApiResult> failure;
};

liquid::runtime::task<Read> read_tree(Opened& opened) {
    auto listed = control::settled(co_await control::list_drive_entries(
        *opened.records, opened.drive.customer_id, opened.drive.id));
    if (!listed) {
        co_return Read{std::nullopt, failed(listed.error())};
    }
    co_return Read{control::DriveTree(std::move(*listed)), std::nullopt};
}

// The `lake_files` bucket of the workspace, or the failure.
struct Files {
    std::optional<control::DriveFileStore> store;
    std::optional<ApiResult> failure;
};

Files file_store(ApiContext& context,
                 const std::string& customer_id,
                 const std::string& workspace_id) {
    if (!context.object_transport || !context.workspaces) {
        return {std::nullopt,
                ApiResult::failure("not_supported",
                                   "this Lake keeps no workspace files: it has no workspace Zetta "
                                   "object transport")};
    }
    const auto endpoint = context.workspaces->endpoint(customer_id, workspace_id);
    if (!endpoint) {
        return {std::nullopt, ApiResult::failure(endpoint.error().code, endpoint.error().message)};
    }
    auto cipher = control::DriveFileCipher::open(context.instance_kek, workspace_id);
    if (!cipher) {
        return {std::nullopt, failed(cipher.error())};
    }
    auto client = std::make_shared<control::ZettaObjectClient>(
        context.object_transport,
        control::ObjectEndpoint{endpoint->url, endpoint->token},
        std::string(control::drive_files_bucket));
    return {control::DriveFileStore(std::move(client), std::move(*cipher)), std::nullopt};
}

// A folder of the drive ("" for the top), or the failure.
std::optional<ApiResult>
folder_problem(const control::DriveTree& tree, const std::string& id, std::string field) {
    if (id.empty()) {
        return std::nullopt;
    }
    const control::DriveEntry* folder = tree.find(id);
    if (folder == nullptr || folder->state != "ready") {
        return ApiResult::failure("not_found", "no such folder in this drive", std::move(field));
    }
    if (folder->kind != "folder") {
        return ApiResult::failure("invalid_input", "this item is not a folder", std::move(field));
    }
    return std::nullopt;
}

// The folders of `parts` under `parent_id`, made where missing (as `mkdir -p`); answers the last
// one's id, or the failure.
struct Made {
    std::string folder_id;
    std::optional<ApiResult> failure;
};

liquid::runtime::task<Made> make_folders(Opened& opened,
                                         control::DriveTree& tree,
                                         std::string parent_id,
                                         const std::vector<std::string>& parts,
                                         const std::string& by,
                                         std::string field) {
    for (const std::string& name : parts) {
        const control::DriveEntry* existing = tree.child(parent_id, name);
        if (existing != nullptr) {
            if (existing->kind != "folder") {
                co_return Made{
                    {},
                    ApiResult::failure("conflict",
                                       "“" + name + "” is a " + existing->kind + ", not a folder",
                                       field)};
            }
            parent_id = existing->id;
            continue;
        }
        control::DriveEntry folder;
        folder.customer_id = opened.drive.customer_id;
        folder.drive_id = opened.drive.id;
        folder.parent_id = parent_id;
        folder.kind = "folder";
        folder.name = name;
        folder.created_by = by;
        auto stored = control::settled(
            co_await control::record_drive_entry(*opened.records, std::move(folder)));
        if (!stored) {
            co_return Made{{}, failed(stored.error(), field)};
        }
        parent_id = stored->id;
        tree.add(std::move(*stored));
    }
    co_return Made{std::move(parent_id), std::nullopt};
}

// ---- Shortcut status ----

struct Status {
    std::string status;
    std::string reason;
};

// Whether a shortcut's target is there, as far as lake can tell without asking the workspace
// engine: repositories, repository paths, drive items and transcripts are checked; datasets,
// projects, agents, models and web addresses are `unknown`. Trees are read once per repository
// and ref per call.
class ShortcutChecker {
public:
    ShortcutChecker(ApiContext& context,
                    control::WorkspaceRecords& records,
                    std::string customer_id)
        : context_(context), records_(records), customer_id_(std::move(customer_id)) {}

    liquid::runtime::task<Status> check(const json& target) {
        const std::string kind = target.value("kind", "");
        if (kind == "repository" || kind == "repository_path") {
            const std::string id = target.value("repository_id", "");
            const auto repository = context_.store->get_repository(id);
            if (!repository) {
                co_return repository.error().code == "not_found"
                    ? Status{"broken", "the repository is gone"}
                    : Status{"unknown", repository.error().message};
            }
            if (!repository->deleted_at.empty()) {
                co_return Status{"broken", "the repository was deleted"};
            }
            if (!may_read_repository(context_, id)) {
                co_return Status{"broken", "you cannot read the repository"};
            }
            if (kind == "repository") {
                co_return Status{"ok", ""};
            }
            const std::string ref = target.value("ref", "main");
            const std::string path = target.value("path", "");
            auto paths = co_await tree_paths(*repository, ref);
            if (!paths || !*paths) {
                co_return Status{"unknown", "the repository could not be read"};
            }
            co_return (*paths)->contains(path)
                ? Status{"ok", ""}
                : Status{"broken", "the path is not in the repository"};
        }
        if (kind == "drive_entry") {
            const std::string drive_id = target.value("drive_id", "");
            auto drive =
                control::settled(co_await control::get_drive(records_, customer_id_, drive_id));
            if (!drive) {
                co_return Status{"unknown", drive.error().message};
            }
            if (!*drive) {
                co_return Status{"broken", "the drive is gone"};
            }
            auto entry = control::settled(co_await control::get_drive_entry(
                records_, customer_id_, drive_id, target.value("entry_id", "")));
            if (!entry) {
                co_return Status{"unknown", entry.error().message};
            }
            co_return *entry && (*entry)->state == "ready" ? Status{"ok", ""}
                                                           : Status{"broken", "the item is gone"};
        }
        if (kind == "transcript") {
            auto found = control::settled(
                co_await control::get_transcript(records_, customer_id_, target.value("id", "")));
            if (!found) {
                co_return Status{"unknown", found.error().message};
            }
            co_return *found ? Status{"ok", ""} : Status{"broken", "the transcript is gone"};
        }
        co_return Status{"unknown", ""};
    }

private:
    liquid::runtime::task<std::optional<std::set<std::string>>>
    tree_paths(const control::Repository& repository, const std::string& ref) {
        const std::string key = repository.id + "\n" + ref;
        if (const auto found = trees_.find(key); found != trees_.end()) {
            co_return found->second;
        }
        std::optional<std::set<std::string>> paths;
        if (context_.sync_store) {
            SyncStore* store = context_.sync_store;
            auto read = co_await blocking_call(
                [store, repository, ref] {
                    return store->repository_tree(repository.customer_id, repository.id, ref);
                },
                [](std::string message) -> SyncStoreResult<std::vector<RepositoryTreeEntry>> {
                    return std::unexpected(SyncStoreError{"store_unavailable", std::move(message)});
                });
            if (read) {
                paths.emplace();
                for (const RepositoryTreeEntry& entry : *read) {
                    paths->insert(entry.path);
                }
            }
        }
        trees_[key] = paths;
        co_return paths;
    }

    ApiContext& context_;
    control::WorkspaceRecords& records_;
    std::string customer_id_;
    std::map<std::string, std::optional<std::set<std::string>>> trees_;
};

Status status_of(std::expected<Status, liquid::error> completed) {
    return completed ? std::move(*completed) : Status{"unknown", completed.error().message};
}

// ---- Drives ----

// ---- Store drives (lake#656) ----
//
// A drive with `storage: store` is one of the workspace's bucket or filesystem stores, browsed in
// place through the store's own provider (control::StoreContents, opened by the injected
// StoreBrowser with what the operator delivered for the store). Nothing is copied into Lake: an
// item's id is its path in the store, and the drive's record only names the store. Deleting the
// drive never deletes the store. Volumes are refused: their contents belong to the service that
// mounts them.

struct StoreOpened {
    std::shared_ptr<control::StoreContents> contents;
    std::optional<ApiResult> failure;
};

// The workspace's store `store_id`, Ready and delivered, opened by the store browser; or why not
// (at `field`).
StoreOpened open_store(ApiContext& context,
                       const std::string& customer_id,
                       const std::string& workspace_id,
                       const std::string& store_id,
                       const std::string& field) {
    const auto refuse = [&](std::string code, std::string message) {
        return StoreOpened{nullptr, ApiResult::failure(std::move(code), std::move(message), field)};
    };
    if (!context.workspaces) {
        return refuse("store_unavailable", "this Lake has no storage");
    }
    const auto found = context.workspaces->store(customer_id, store_id, workspace_id);
    if (!found) {
        return refuse(found.error().code == "not_found" ? "not_found" : found.error().code,
                      found.error().code == "not_found" ? "no such store in this workspace"
                                                        : found.error().message);
    }
    if (const std::string kind = control::store_kind_refusal(found->type); !kind.empty()) {
        return refuse("invalid_input", kind);
    }
    if (found->desired != "present" || found->phase != "Ready") {
        return refuse("workspace_not_ready", "the store is not ready");
    }
    const auto connection = context.workspaces->store_connection(*found);
    if (!connection) {
        return refuse("workspace_not_ready",
                      "the store is ready and its connection is still being delivered to Lake; "
                      "try again in a minute");
    }
    if (!context.store_browser) {
        return refuse("not_supported", control::store_browse_refusal(found->type, found->provider));
    }
    auto opened = context.store_browser(*found, *connection);
    if (!opened) {
        return refuse(opened.error().code, opened.error().message);
    }
    return {std::move(*opened), std::nullopt};
}

StoreOpened store_of(ApiContext& context, const control::Drive& drive, const json& input) {
    return open_store(
        context, drive.customer_id, input.at("workspace_id"), drive.store_id, "/drive_id");
}

json store_entry_json(const control::Drive& drive, const control::StoreItem& item) {
    const std::string parent = control::store_parent(item.path);
    const std::string name = control::store_name(item.path);
    json out{{"id", item.path},
             {"drive_id", drive.id},
             {"parent_id", parent.empty() ? json(nullptr) : json(parent)},
             {"kind", item.folder ? "folder" : "file"},
             {"name", name},
             {"path", item.path},
             {"created_by", ""},
             {"created_at", item.modified_at},
             {"modified_by", ""},
             {"modified_at", item.modified_at}};
    if (!item.folder) {
        out["size"] = item.size;
        out["content_type"] =
            item.content_type.empty() ? control::drive_media_type(name) : item.content_type;
        out["sha256"] = nullptr;
        out["state"] = "ready";
    }
    return out;
}

ApiResult store_failed(const control::ZettaControlError& error, std::string path) {
    return ApiResult::failure(error.code, error.message, error.path.empty() ? path : error.path);
}

// An item's path from `input.<field>` ("" is the top), or the refusal.
std::expected<std::string, ApiResult>
store_path(const json& input, std::string_view field, bool top_allowed) {
    const std::string path = input.value(std::string(field), std::string());
    if (!control::valid_store_path(path) || (path.empty() && !top_allowed)) {
        return std::unexpected(ApiResult::failure(
            "invalid_input",
            "a store path is names joined by \"/\", each 1 to 255 bytes with no backslash or "
            "control character",
            "/" + std::string(field)));
    }
    return path;
}

// The item at `path`, which must be there (and a folder, when `folder`).
struct StoreFound {
    std::optional<control::StoreItem> item;
    std::optional<ApiResult> failure;
};

liquid::runtime::task<StoreFound> store_find(control::StoreContents& contents,
                                             std::string path,
                                             std::string field,
                                             std::optional<bool> folder = std::nullopt) {
    auto found = control::settled(co_await contents.stat(path));
    if (!found) {
        co_return StoreFound{std::nullopt, store_failed(found.error(), field)};
    }
    if (!*found || (folder && (*found)->folder != *folder)) {
        co_return StoreFound{std::nullopt,
                             ApiResult::failure("not_found",
                                                folder && *folder
                                                    ? "no such folder in this drive"
                                                    : (folder ? "no such file in this drive"
                                                              : "no such item in this drive"),
                                                field)};
    }
    co_return StoreFound{std::move(**found), std::nullopt};
}

StoreFound awaited_found(std::expected<StoreFound, liquid::error> completed) {
    return completed ? std::move(*completed)
                     : StoreFound{std::nullopt,
                                  ApiResult::failure("store_error", completed.error().message)};
}

liquid::runtime::task<ApiResult>
store_list(const json& input, ApiContext& context, const control::Drive& drive) {
    StoreOpened store = store_of(context, drive, input);
    if (store.failure) {
        co_return *store.failure;
    }
    const std::string field = input.contains("folder_id") ? "folder_id" : "path";
    auto folder = store_path(input, field, true);
    if (!folder) {
        co_return folder.error();
    }
    if (!folder->empty()) {
        StoreFound found =
            awaited_found(co_await store_find(*store.contents, *folder, "/" + field, true));
        if (found.failure) {
            co_return *found.failure;
        }
    }
    auto listed = control::settled(co_await store.contents->list(*folder));
    if (!listed) {
        co_return store_failed(listed.error(), "/" + field);
    }
    json ancestors = json::array();
    std::string prefix;
    if (!folder->empty()) {
        std::size_t start = 0;
        while (true) {
            const std::size_t slash = folder->find('/', start);
            prefix = folder->substr(0, slash);
            ancestors.push_back({{"id", prefix}, {"name", folder->substr(start, slash - start)}});
            if (slash == std::string::npos) {
                break;
            }
            start = slash + 1;
        }
    }
    json items = json::array();
    for (const control::StoreItem& item : *listed) {
        items.push_back(store_entry_json(drive, item));
    }
    context.audit_details["folder_id"] = *folder;
    context.audit_details["store_id"] = drive.store_id;
    co_return ApiResult::success({{"drive", drive_json(drive)},
                                  {"folder_id", folder->empty() ? json(nullptr) : json(*folder)},
                                  {"path", *folder},
                                  {"ancestors", std::move(ancestors)},
                                  {"items", std::move(items)},
                                  {"uploads", json::array()}});
}

liquid::runtime::task<ApiResult>
store_get(const json& input, ApiContext& context, const control::Drive& drive) {
    StoreOpened store = store_of(context, drive, input);
    if (store.failure) {
        co_return *store.failure;
    }
    auto path = store_path(input, "id", false);
    if (!path) {
        co_return path.error();
    }
    StoreFound found = awaited_found(co_await store_find(*store.contents, *path, "/id"));
    if (found.failure) {
        co_return *found.failure;
    }
    context.audit_details["entry_id"] = *path;
    co_return ApiResult::success(store_entry_json(drive, *found.item));
}

liquid::runtime::task<ApiResult>
store_make_folder(const json& input, ApiContext& context, const control::Drive& drive) {
    StoreOpened store = store_of(context, drive, input);
    if (store.failure) {
        co_return *store.failure;
    }
    auto parent = store_path(input, "parent_id", true);
    if (!parent) {
        co_return parent.error();
    }
    auto relative = store_path(input, "path", false);
    if (!relative) {
        co_return relative.error();
    }
    const std::string path = control::join_store_path(*parent, *relative);
    // `mkdir -p`: a folder already there is answered as it is; a file on the way is a conflict.
    std::string level;
    for (std::size_t start = 0;;) {
        const std::size_t slash = path.find('/', start);
        level = path.substr(0, slash);
        auto at = control::settled(co_await store.contents->stat(level));
        if (!at) {
            co_return store_failed(at.error(), "/path");
        }
        if (*at && !(*at)->folder) {
            co_return ApiResult::failure(
                "conflict", "“" + level + "” is a file, not a folder", "/path");
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    auto made = control::settled(co_await store.contents->make_folder(path));
    if (!made) {
        co_return store_failed(made.error(), "/path");
    }
    StoreFound found = awaited_found(co_await store_find(*store.contents, path, "/path", true));
    if (found.failure) {
        co_return *found.failure;
    }
    context.audit_details["entry_id"] = path;
    co_return ApiResult::success(store_entry_json(drive, *found.item));
}

liquid::runtime::task<ApiResult>
store_move(const json& input, ApiContext& context, const control::Drive& drive) {
    StoreOpened store = store_of(context, drive, input);
    if (store.failure) {
        co_return *store.failure;
    }
    auto from = store_path(input, "id", false);
    if (!from) {
        co_return from.error();
    }
    StoreFound found = awaited_found(co_await store_find(*store.contents, *from, "/id"));
    if (found.failure) {
        co_return *found.failure;
    }
    const bool renaming = input.contains("name");
    std::string parent = control::store_parent(*from);
    if (input.contains("parent_id")) {
        auto chosen = store_path(input, "parent_id", true);
        if (!chosen) {
            co_return chosen.error();
        }
        parent = *chosen;
    }
    const std::string name =
        renaming ? input.at("name").get<std::string>() : control::store_name(*from);
    if (!control::valid_drive_name(name)) {
        co_return ApiResult::failure("invalid_input", "the new name is not valid", "/name");
    }
    if (parent == *from || parent.starts_with(*from + "/")) {
        co_return ApiResult::failure(
            "invalid_input", "a folder cannot move into itself", "/parent_id");
    }
    if (!parent.empty()) {
        StoreFound folder =
            awaited_found(co_await store_find(*store.contents, parent, "/parent_id", true));
        if (folder.failure) {
            co_return *folder.failure;
        }
    }
    const std::string to = control::join_store_path(parent, name);
    if (to != *from) {
        auto taken = control::settled(co_await store.contents->stat(to));
        if (!taken) {
            co_return store_failed(taken.error(), "/id");
        }
        if (*taken) {
            co_return ApiResult::failure("conflict",
                                         "the folder already has an item named “" + name + "”",
                                         renaming ? "/name" : "/parent_id");
        }
        auto moved = control::settled(co_await store.contents->move(*from, to));
        if (!moved) {
            co_return store_failed(moved.error(), "/id");
        }
    }
    StoreFound now = awaited_found(co_await store_find(*store.contents, to, "/id"));
    if (now.failure) {
        co_return *now.failure;
    }
    context.audit_details["entry_id"] = to;
    context.audit_details["moved_from"] = *from;
    co_return ApiResult::success(store_entry_json(drive, *now.item));
}

liquid::runtime::task<ApiResult>
store_remove(const json& input, ApiContext& context, const control::Drive& drive) {
    StoreOpened store = store_of(context, drive, input);
    if (store.failure) {
        co_return *store.failure;
    }
    auto path = store_path(input, "id", false);
    if (!path) {
        co_return path.error();
    }
    StoreFound found = awaited_found(co_await store_find(*store.contents, *path, "/id"));
    if (found.failure) {
        co_return *found.failure;
    }
    auto removed = control::settled(co_await store.contents->remove(*path));
    if (!removed) {
        co_return store_failed(removed.error(), "/id");
    }
    context.audit_details["entry_id"] = *path;
    context.audit_details["entries_removed"] = 1;
    co_return ApiResult::success({{"deleted", true}, {"entries_removed", 1}});
}

// "name (2).ext", "name (3).ext", … : the first name free in the store's folder.
liquid::runtime::task<control::ZettaControlResult<std::string>>
store_free_name(control::StoreContents& contents, std::string folder, std::string name) {
    const std::size_t dot = name.rfind('.');
    const bool extension = dot != std::string::npos && dot > 0;
    const std::string stem = extension ? name.substr(0, dot) : name;
    const std::string suffix = extension ? name.substr(dot) : std::string();
    for (int n = 2; n < 1000; ++n) {
        const std::string candidate = stem + " (" + std::to_string(n) + ")" + suffix;
        auto taken =
            control::settled(co_await contents.stat(control::join_store_path(folder, candidate)));
        if (!taken) {
            co_return control::ZettaControlResult<std::string>(std::unexpected(taken.error()));
        }
        if (!*taken) {
            co_return control::ZettaControlResult<std::string>(candidate);
        }
    }
    co_return control::ZettaControlResult<std::string>(
        std::unexpected(control::ZettaControlError{"conflict", "no free name for “" + name + "”"}));
}

liquid::runtime::task<ApiResult> store_upload(const json& input,
                                              ApiContext& context,
                                              const control::Drive& drive,
                                              std::vector<std::string> paths,
                                              std::vector<std::string> contents,
                                              std::uint64_t total) {
    StoreOpened store = store_of(context, drive, input);
    if (store.failure) {
        co_return *store.failure;
    }
    auto parent = store_path(input, "parent_id", true);
    if (!parent) {
        co_return parent.error();
    }
    if (!parent->empty()) {
        StoreFound folder =
            awaited_found(co_await store_find(*store.contents, *parent, "/parent_id", true));
        if (folder.failure) {
            co_return *folder.failure;
        }
    }
    const std::string on_conflict = input.value("on_conflict", "rename");
    const json& list = input.at("files");
    json items = json::array();
    for (std::size_t index = 0; index < paths.size(); ++index) {
        const std::string field = "/files/" + std::to_string(index);
        if (!control::valid_store_path(paths[index]) || paths[index].empty()) {
            co_return ApiResult::failure("invalid_input",
                                         "each name in the path is a valid file or folder name",
                                         field + "/path");
        }
        std::string path = control::join_store_path(*parent, paths[index]);
        auto taken = control::settled(co_await store.contents->stat(path));
        if (!taken) {
            co_return store_failed(taken.error(), field + "/path");
        }
        if (*taken) {
            if ((*taken)->folder || on_conflict == "refuse") {
                co_return ApiResult::failure("conflict",
                                             "the folder already has an item named “" +
                                                 control::store_name(path) + "”",
                                             field + "/path");
            }
            if (on_conflict == "rename") {
                auto free = control::settled(co_await store_free_name(
                    *store.contents, control::store_parent(path), control::store_name(path)));
                if (!free) {
                    co_return store_failed(free.error(), field + "/path");
                }
                path = control::join_store_path(control::store_parent(path), *free);
            }
        }
        std::string type = list.at(index).value("content_type", "");
        if (type.empty()) {
            type = control::drive_media_type(control::store_name(path));
        }
        auto written = control::settled(
            co_await store.contents->write(path, std::move(contents[index]), std::move(type)));
        if (!written) {
            co_return store_failed(written.error(), field + "/content");
        }
        items.push_back(store_entry_json(drive, *written));
    }
    context.audit_details["files"] = items.size();
    context.audit_details["bytes"] = total;
    context.audit_details["store_id"] = drive.store_id;
    co_return ApiResult::success({{"items", std::move(items)}});
}

liquid::runtime::task<ApiResult>
store_read(const json& input, ApiContext& context, const control::Drive& drive) {
    StoreOpened store = store_of(context, drive, input);
    if (store.failure) {
        co_return *store.failure;
    }
    auto path = store_path(input, "id", false);
    if (!path) {
        co_return path.error();
    }
    StoreFound found = awaited_found(co_await store_find(*store.contents, *path, "/id", false));
    if (found.failure) {
        co_return *found.failure;
    }
    const control::StoreItem& item = *found.item;
    const std::uint64_t offset = input.value("offset", std::uint64_t{0});
    if (offset > item.size) {
        co_return ApiResult::failure("invalid_input", "offset is past the file's end", "/offset");
    }
    const std::uint64_t length =
        std::min(input.value("length", item.size - offset), item.size - offset);
    if (length > max_inline_bytes) {
        co_return ApiResult::failure(
            "size_invalid",
            "one call reads at most 16 MiB: read the file in ranges (offset, length), or download "
            "it from /drive/files/download",
            "/length");
    }
    auto bytes = control::settled(co_await store.contents->read(*path, offset, length));
    if (!bytes) {
        co_return store_failed(bytes.error(), "/id");
    }
    const json entry = store_entry_json(drive, item);
    context.audit_details["entry_id"] = *path;
    context.audit_details["bytes"] = bytes->size();
    co_return ApiResult::success({{"id", *path},
                                  {"name", control::store_name(*path)},
                                  {"size", item.size},
                                  {"content_type", entry.at("content_type")},
                                  {"sha256", ""},
                                  {"offset", offset},
                                  {"content_base64", encode_base64(*bytes)}});
}

// A store drive's names for search_drives: every folder and file, walked from the top, at most
// `store_search_items` of them; why not, when the store cannot be read.
constexpr std::size_t store_search_items = 10'000;

struct StoreWalk {
    std::vector<control::StoreItem> items;
    std::string problem;
};

liquid::runtime::task<StoreWalk>
store_walk(ApiContext& context, const control::Drive& drive, const json& input) {
    StoreOpened store = store_of(context, drive, input);
    if (store.failure) {
        co_return StoreWalk{{}, store.failure->error.message};
    }
    StoreWalk out;
    std::vector<std::string> folders{""};
    while (!folders.empty() && out.items.size() < store_search_items) {
        const std::string folder = folders.back();
        folders.pop_back();
        auto listed = control::settled(co_await store.contents->list(folder));
        if (!listed) {
            co_return StoreWalk{{}, listed.error().message};
        }
        for (control::StoreItem& item : *listed) {
            if (item.folder) {
                folders.push_back(item.path);
            }
            out.items.push_back(std::move(item));
        }
    }
    if (out.items.size() > store_search_items) {
        out.items.resize(store_search_items);
    }
    co_return out;
}

StoreWalk awaited_walk(std::expected<StoreWalk, liquid::error> completed) {
    return completed ? std::move(*completed) : StoreWalk{{}, completed.error().message};
}

liquid::runtime::task<ApiResult> create_drive(const json& input, ApiContext& context) {
    if (!context.store) {
        co_return ApiResult::failure("store_unavailable", "workspaces are not configured");
    }
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    control::Drive drive;
    drive.customer_id = customer_id;
    drive.name = input.at("name");
    drive.storage = input.at("storage");
    drive.created_by = actor(context);
    if (!control::valid_drive_name(drive.name)) {
        co_return ApiResult::failure(
            "invalid_input",
            "a drive's name is 1 to 255 bytes with no slash, backslash or control character",
            "/name");
    }
    auto records = workspace_records(context, customer_id, workspace_id);
    if (!records) {
        co_return failed(records.error());
    }
    auto existing = control::settled(co_await control::list_drives(*records, customer_id));
    if (!existing) {
        co_return failed(existing.error());
    }
    if (std::ranges::any_of(
            *existing, [&](const control::Drive& other) { return other.name == drive.name; })) {
        co_return ApiResult::failure(
            "conflict", "this workspace already has a drive named “" + drive.name + "”", "/name");
    }
    bool created_repository = false;
    if (drive.storage == "repository") {
        const std::string repository_id = input.value("repository_id", std::string());
        if (!repository_id.empty()) {
            // An existing repository of this workspace that the person may read.
            const ApiResult got =
                called_as(co_await call_as(context, "get_repository", {{"id", repository_id}}));
            if (!got.ok) {
                co_return ApiResult::failure(got.error.code, got.error.message, "/repository_id");
            }
            if (got.value.value("workspace_id", "") != workspace_id ||
                got.value.value("customer_id", "") != customer_id) {
                co_return ApiResult::failure("invalid_input",
                                             "the repository belongs to another workspace",
                                             "/repository_id");
            }
            drive.repository_id = repository_id;
        } else {
            const std::string name = input.value("repository_name", std::string());
            if (name.empty()) {
                co_return ApiResult::failure(
                    "invalid_input",
                    "a repository drive names an existing repository_id or a repository_name to "
                    "create",
                    "/repository_name");
            }
            const ApiResult made = called_as(co_await call_as(
                context,
                "create_repository",
                {{"customer_id", customer_id}, {"workspace_id", workspace_id}, {"name", name}}));
            if (!made.ok) {
                co_return ApiResult::failure(
                    made.error.code, made.error.message, "/repository_name");
            }
            drive.repository_id = made.value.value("id", "");
            created_repository = true;
            // A new repository has no `main` until its first change, and every drive_* capability
            // resolves `main` first: the drive's first change is the empty-folder marker at its
            // top, which drive_browse never lists.
            if (!context.sync_store) {
                co_return ApiResult::failure("store_unavailable",
                                             "no repository store is configured");
            }
            SyncStore* store = context.sync_store;
            const std::string repository_id = drive.repository_id;
            auto committed = co_await blocking_call(
                [store, customer_id, repository_id] {
                    std::vector<RepositoryWriteOperation> operations{
                        {.kind = RepositoryWriteKind::Folder, .path = ""}};
                    return store->commit_repository_files(
                        customer_id, repository_id, "main", "Create drive", std::move(operations));
                },
                [](std::string message) -> SyncStoreResult<std::string> {
                    return std::unexpected(SyncStoreError{"store_unavailable", std::move(message)});
                });
            if (!committed) {
                co_return ApiResult::failure(committed.error().code,
                                             "the repository was created, but its first change "
                                             "failed: " +
                                                 committed.error().message);
            }
            context.audit_details["change"] = *committed;
        }
    } else if (input.contains("repository_id") || input.contains("repository_name")) {
        co_return ApiResult::failure(
            "invalid_input", "only a repository drive names a repository", "/repository_id");
    }
    if (drive.storage == "store") {
        // An existing bucket or filesystem store of this workspace, opened now so a store Lake
        // cannot browse is refused with the reason before any drive names it. Nothing is copied.
        const std::string store_id = input.value("store_id", std::string());
        if (store_id.empty()) {
            co_return ApiResult::failure(
                "invalid_input", "a store drive names one of the workspace's stores", "/store_id");
        }
        StoreOpened store = open_store(context, customer_id, workspace_id, store_id, "/store_id");
        if (store.failure) {
            co_return *store.failure;
        }
        drive.store_id = store_id;
    } else if (input.contains("store_id")) {
        co_return ApiResult::failure(
            "invalid_input", "only a store drive names a store", "/store_id");
    }
    auto stored = control::settled(co_await control::record_drive(*records, std::move(drive)));
    if (!stored) {
        co_return failed(stored.error());
    }
    context.audit_details["drive_id"] = stored->id;
    context.audit_details["storage"] = stored->storage;
    if (!stored->repository_id.empty()) {
        context.audit_details["repository_id"] = stored->repository_id;
        context.audit_details["repository_created"] = created_repository;
    }
    if (!stored->store_id.empty()) {
        context.audit_details["store_id"] = stored->store_id;
    }
    co_return ApiResult::success(drive_json(*stored));
}

liquid::runtime::task<ApiResult> list_drives(const json& input, ApiContext& context) {
    if (!context.store) {
        co_return ApiResult::failure("store_unavailable", "workspaces are not configured");
    }
    if (auto refused = authorize_space_read(input, context)) {
        co_return *refused;
    }
    const std::string customer_id = input.at("customer_id");
    auto records = workspace_records(context, customer_id, input.at("workspace_id"));
    if (!records) {
        co_return failed(records.error());
    }
    auto listed = control::settled(co_await control::list_drives(*records, customer_id));
    if (!listed) {
        co_return failed(listed.error());
    }
    json drives = json::array();
    for (const control::Drive& drive : *listed) {
        drives.push_back(drive_json(drive));
    }
    co_return ApiResult::success({{"drives", std::move(drives)}});
}

liquid::runtime::task<ApiResult> get_drive(const json& input, ApiContext& context) {
    Opened opened = awaited(co_await open_drive(input, context, false, "id"));
    if (opened.failure) {
        co_return *opened.failure;
    }
    co_return ApiResult::success(drive_json(opened.drive));
}

liquid::runtime::task<ApiResult> rename_drive(const json& input, ApiContext& context) {
    const std::string name = input.at("name");
    if (!control::valid_drive_name(name)) {
        co_return ApiResult::failure("invalid_input", "the drive's name is not valid", "/name");
    }
    Opened opened = awaited(co_await open_drive(input, context, false, "id"));
    if (opened.failure) {
        co_return *opened.failure;
    }
    auto drives =
        control::settled(co_await control::list_drives(*opened.records, opened.drive.customer_id));
    if (!drives) {
        co_return failed(drives.error());
    }
    if (std::ranges::any_of(*drives, [&](const control::Drive& other) {
            return other.name == name && other.id != opened.drive.id;
        })) {
        co_return ApiResult::failure(
            "conflict", "this workspace already has a drive named “" + name + "”", "/name");
    }
    auto renamed = control::settled(co_await control::rename_drive(
        *opened.records, opened.drive.customer_id, opened.drive.id, name));
    if (!renamed) {
        co_return failed(renamed.error(), "/id");
    }
    co_return ApiResult::success(drive_json(*renamed));
}

// Removes the entries (and their files' objects) given, deepest first. Answers how many went.
struct Removed {
    std::size_t count = 0;
    std::optional<ApiResult> failure;
};

liquid::runtime::task<Removed>
remove_entries(Opened& opened,
               ApiContext& context,
               const std::string& workspace_id,
               const std::vector<const control::DriveEntry*>& entries) {
    std::optional<Files> files;
    Removed out;
    for (const control::DriveEntry* entry : entries) {
        if (entry->kind == "file") {
            if (!files) {
                files = file_store(context, opened.drive.customer_id, workspace_id);
            }
            if (files->failure) {
                co_return Removed{out.count, files->failure};
            }
            auto removed = control::settled(co_await files->store->remove(entry->object_key));
            if (!removed) {
                co_return Removed{out.count, failed(removed.error())};
            }
        }
        auto removed = control::settled(co_await control::remove_drive_entry(
            *opened.records, opened.drive.customer_id, opened.drive.id, entry->id));
        if (!removed) {
            co_return Removed{out.count, failed(removed.error())};
        }
        ++out.count;
    }
    co_return out;
}

liquid::runtime::task<ApiResult> delete_drive(const json& input, ApiContext& context) {
    Opened opened = awaited(co_await open_drive(input, context, false, "id"));
    if (opened.failure) {
        co_return *opened.failure;
    }
    // The person types the drive's name: deleting a workspace-files drive deletes its files.
    if (input.at("confirm_name").get<std::string>() != opened.drive.name) {
        co_return ApiResult::failure(
            "invalid_input", "type the drive's name exactly to delete it", "/confirm_name");
    }
    std::size_t removed_entries = 0;
    // A repository drive's repository and a store drive's store stay: the drive only names them.
    if (opened.drive.storage != "repository" && opened.drive.storage != "store") {
        Read read = awaited(co_await read_tree(opened));
        if (read.failure) {
            co_return *read.failure;
        }
        std::vector<const control::DriveEntry*> all;
        for (const control::DriveEntry& entry : read.tree->entries()) {
            all.push_back(&entry);
        }
        // Children before parents, so a failure part way leaves no orphan.
        std::ranges::sort(all, [&](const control::DriveEntry* a, const control::DriveEntry* b) {
            return read.tree->ancestors(a->id).size() > read.tree->ancestors(b->id).size();
        });
        Removed removed =
            awaited(co_await remove_entries(opened, context, input.at("workspace_id"), all));
        if (removed.failure) {
            co_return *removed.failure;
        }
        removed_entries = removed.count;
    }
    auto removed = control::settled(
        co_await control::remove_drive(*opened.records, opened.drive.customer_id, opened.drive.id));
    if (!removed) {
        co_return failed(removed.error(), "/id");
    }
    context.audit_details["storage"] = opened.drive.storage;
    context.audit_details["entries_removed"] = removed_entries;
    json out{{"deleted", true}, {"entries_removed", removed_entries}};
    if (!opened.drive.repository_id.empty()) {
        // A repository drive only names its repository; the repository stays.
        out["repository_id"] = opened.drive.repository_id;
    }
    if (!opened.drive.store_id.empty()) {
        // A store drive only names its store; the store and its contents stay.
        out["store_id"] = opened.drive.store_id;
    }
    co_return ApiResult::success(std::move(out));
}

// ---- Entries ----

liquid::runtime::task<ApiResult> list_drive_entries(const json& input, ApiContext& context) {
    if (!context.store) {
        co_return ApiResult::failure("store_unavailable", "workspaces are not configured");
    }
    if (auto refused = authorize_space_read(input, context)) {
        co_return *refused;
    }
    Opened opened = awaited(co_await open_drive(input, context, true));
    if (opened.failure) {
        co_return *opened.failure;
    }
    if (opened.drive.storage == "store") {
        co_return called_as(co_await store_list(input, context, opened.drive));
    }
    Read read = awaited(co_await read_tree(opened));
    if (read.failure) {
        co_return *read.failure;
    }
    const control::DriveTree& tree = *read.tree;
    std::string folder_id = input.value("folder_id", std::string());
    if (folder_id.empty() && !input.value("path", std::string()).empty()) {
        const control::DriveEntry* found = tree.resolve("", input.at("path").get<std::string>());
        if (found == nullptr || found->kind != "folder") {
            co_return ApiResult::failure("not_found", "no such folder in this drive", "/path");
        }
        folder_id = found->id;
    }
    if (auto refused = folder_problem(tree, folder_id, "/folder_id")) {
        co_return *refused;
    }
    json ancestors = json::array();
    for (const control::DriveEntry* folder : tree.ancestors(folder_id)) {
        ancestors.push_back({{"id", folder->id}, {"name", folder->name}});
    }
    ShortcutChecker checker(context, *opened.records, opened.drive.customer_id);
    json items = json::array();
    const std::string base = tree.path(folder_id);
    for (const control::DriveEntry* entry : tree.children(folder_id)) {
        json item = entry_json(*entry, base.empty() ? entry->name : base + "/" + entry->name);
        if (entry->kind == "shortcut") {
            const Status status = status_of(co_await checker.check(entry->target));
            item["status"] = status.status;
            if (!status.reason.empty()) {
                item["reason"] = status.reason;
            }
        }
        items.push_back(std::move(item));
    }
    // Unfinished uploads are not items, but they hold their names until they finish or expire
    // (lake#588), so they are listed apart, with their age, for a person or client to finish or
    // delete them (delete_drive_entry by id).
    json uploads = json::array();
    const std::int64_t now = now_us();
    for (const control::DriveEntry* upload : tree.uploads(folder_id)) {
        json item = entry_json(*upload, base.empty() ? upload->name : base + "/" + upload->name);
        const std::int64_t age = std::max<std::int64_t>(0, now - upload->modified_us);
        item["age_seconds"] = age / 1'000'000;
        item["expired"] = control::drive_upload_expired(*upload, now);
        uploads.push_back(std::move(item));
    }
    context.audit_details["folder_id"] = folder_id;
    co_return ApiResult::success(
        {{"drive", drive_json(opened.drive)},
         {"folder_id", folder_id.empty() ? json(nullptr) : json(folder_id)},
         {"path", base},
         {"ancestors", std::move(ancestors)},
         {"items", std::move(items)},
         {"uploads", std::move(uploads)}});
}

liquid::runtime::task<ApiResult> get_drive_entry(const json& input, ApiContext& context) {
    Opened opened = awaited(co_await open_drive(input, context, true));
    if (opened.failure) {
        co_return *opened.failure;
    }
    if (opened.drive.storage == "store") {
        co_return called_as(co_await store_get(input, context, opened.drive));
    }
    Read read = awaited(co_await read_tree(opened));
    if (read.failure) {
        co_return *read.failure;
    }
    const control::DriveEntry* entry = read.tree->find(input.at("id").get<std::string>());
    if (entry == nullptr) {
        co_return ApiResult::failure("not_found", "no such item in this drive", "/id");
    }
    json out = entry_json(*entry, read.tree->path(entry->id));
    if (entry->kind == "shortcut") {
        ShortcutChecker checker(context, *opened.records, opened.drive.customer_id);
        const Status status = status_of(co_await checker.check(entry->target));
        out["status"] = status.status;
        if (!status.reason.empty()) {
            out["reason"] = status.reason;
        }
    }
    context.audit_details["entry_id"] = entry->id;
    co_return ApiResult::success(std::move(out));
}

liquid::runtime::task<ApiResult> create_drive_folder_entry(const json& input, ApiContext& context) {
    const auto parts = path_parts(input.at("path").get<std::string>());
    if (!parts) {
        co_return ApiResult::failure(
            "invalid_input",
            "each folder name is 1 to 255 bytes with no backslash or control character",
            "/path");
    }
    Opened opened = awaited(co_await open_drive(input, context, true));
    if (opened.failure) {
        co_return *opened.failure;
    }
    if (opened.drive.storage == "store") {
        co_return called_as(co_await store_make_folder(input, context, opened.drive));
    }
    Read read = awaited(co_await read_tree(opened));
    if (read.failure) {
        co_return *read.failure;
    }
    const std::string parent_id = input.value("parent_id", std::string());
    if (auto refused = folder_problem(*read.tree, parent_id, "/parent_id")) {
        co_return *refused;
    }
    // A folder that is already there is answered as it is; `mkdir -p` with a conflict check.
    Made made = awaited(
        co_await make_folders(opened, *read.tree, parent_id, *parts, actor(context), "/path"));
    if (made.failure) {
        co_return *made.failure;
    }
    const control::DriveEntry* folder = read.tree->find(made.folder_id);
    context.audit_details["entry_id"] = made.folder_id;
    co_return ApiResult::success(entry_json(*folder, read.tree->path(made.folder_id)));
}

liquid::runtime::task<ApiResult> create_drive_shortcut(const json& input, ApiContext& context) {
    const std::string name = input.at("name");
    if (!control::valid_drive_name(name)) {
        co_return ApiResult::failure("invalid_input", "the shortcut's name is not valid", "/name");
    }
    const json& target = input.at("target");
    if (auto problem = control::drive_shortcut_problem(target)) {
        co_return ApiResult::failure("invalid_input", *problem, "/target");
    }
    Opened opened = awaited(co_await open_drive(input, context, false));
    if (opened.failure) {
        co_return *opened.failure;
    }
    if (opened.drive.storage == "repository") {
        co_return ApiResult::failure(
            "invalid_input", "shortcuts live in a workspace-files or virtual drive", "/drive_id");
    }
    if (opened.drive.storage == "store") {
        co_return ApiResult::failure("invalid_input",
                                     "a store drive holds the store's own files; put shortcuts in "
                                     "a workspace-files or virtual drive",
                                     "/drive_id");
    }
    Read read = awaited(co_await read_tree(opened));
    if (read.failure) {
        co_return *read.failure;
    }
    const std::string parent_id = input.value("parent_id", std::string());
    if (auto refused = folder_problem(*read.tree, parent_id, "/parent_id")) {
        co_return *refused;
    }
    if (read.tree->child(parent_id, name) != nullptr) {
        co_return ApiResult::failure(
            "conflict", "this folder already has an item named “" + name + "”", "/name");
    }
    // A target lake can check must be there now; it may break later, and listings say so.
    ShortcutChecker checker(context, *opened.records, opened.drive.customer_id);
    const Status status = status_of(co_await checker.check(target));
    if (status.status == "broken") {
        co_return ApiResult::failure(
            "not_found", "the shortcut's target: " + status.reason, "/target");
    }
    control::DriveEntry entry;
    entry.customer_id = opened.drive.customer_id;
    entry.drive_id = opened.drive.id;
    entry.parent_id = parent_id;
    entry.kind = "shortcut";
    entry.name = name;
    entry.target = target;
    entry.created_by = actor(context);
    auto stored =
        control::settled(co_await control::record_drive_entry(*opened.records, std::move(entry)));
    if (!stored) {
        co_return failed(stored.error());
    }
    context.audit_details["entry_id"] = stored->id;
    context.audit_details["target_kind"] = target.value("kind", "");
    json out = entry_json(*stored,
                          read.tree->path(parent_id).empty()
                              ? stored->name
                              : read.tree->path(parent_id) + "/" + stored->name);
    out["status"] = status.status;
    co_return ApiResult::success(std::move(out));
}

liquid::runtime::task<ApiResult> move_drive_entry(const json& input, ApiContext& context) {
    const bool moving = input.contains("parent_id");
    const bool renaming = input.contains("name");
    if (!moving && !renaming) {
        co_return ApiResult::failure(
            "invalid_input", "send a new parent_id, a new name or both", "/name");
    }
    Opened opened = awaited(co_await open_drive(input, context, true));
    if (opened.failure) {
        co_return *opened.failure;
    }
    if (opened.drive.storage == "store") {
        co_return called_as(co_await store_move(input, context, opened.drive));
    }
    Read read = awaited(co_await read_tree(opened));
    if (read.failure) {
        co_return *read.failure;
    }
    const control::DriveTree& tree = *read.tree;
    const std::string id = input.at("id");
    const control::DriveEntry* entry = tree.find(id);
    if (entry == nullptr || entry->state != "ready") {
        co_return ApiResult::failure("not_found", "no such item in this drive", "/id");
    }
    const std::string parent_id =
        moving ? input.at("parent_id").get<std::string>() : entry->parent_id;
    const std::string name = renaming ? input.at("name").get<std::string>() : entry->name;
    if (!control::valid_drive_name(name)) {
        co_return ApiResult::failure("invalid_input", "the new name is not valid", "/name");
    }
    if (auto refused = folder_problem(tree, parent_id, "/parent_id")) {
        co_return *refused;
    }
    if (!parent_id.empty() && tree.within(parent_id, id)) {
        co_return ApiResult::failure(
            "invalid_input", "a folder cannot move into itself", "/parent_id");
    }
    const control::DriveEntry* taken = tree.child(parent_id, name);
    if (taken != nullptr && taken->id != id) {
        co_return ApiResult::failure("conflict",
                                     "the folder already has an item named “" + name + "”",
                                     renaming ? "/name" : "/parent_id");
    }
    const std::string by = actor(context);
    auto changed = control::settled(co_await control::change_drive_entry(
        *opened.records,
        opened.drive.customer_id,
        opened.drive.id,
        id,
        [&](control::DriveEntry& next) -> std::optional<control::ZettaControlError> {
            next.parent_id = parent_id;
            next.name = name;
            next.modified_by = by;
            next.modified_us = now_us();
            return std::nullopt;
        }));
    if (!changed) {
        co_return failed(changed.error(), "/id");
    }
    context.audit_details["entry_id"] = id;
    const std::string base = tree.path(parent_id);
    co_return ApiResult::success(entry_json(*changed, base.empty() ? name : base + "/" + name));
}

liquid::runtime::task<ApiResult> delete_drive_entry(const json& input, ApiContext& context) {
    Opened opened = awaited(co_await open_drive(input, context, true));
    if (opened.failure) {
        co_return *opened.failure;
    }
    if (opened.drive.storage == "store") {
        co_return called_as(co_await store_remove(input, context, opened.drive));
    }
    Read read = awaited(co_await read_tree(opened));
    if (read.failure) {
        co_return *read.failure;
    }
    const std::string id = input.at("id");
    if (read.tree->find(id) == nullptr) {
        co_return ApiResult::failure("not_found", "no such item in this drive", "/id");
    }
    Removed removed = awaited(
        co_await remove_entries(opened, context, input.at("workspace_id"), read.tree->subtree(id)));
    if (removed.failure) {
        co_return *removed.failure;
    }
    context.audit_details["entry_id"] = id;
    context.audit_details["entries_removed"] = removed.count;
    co_return ApiResult::success({{"deleted", true}, {"entries_removed", removed.count}});
}

// ---- Uploads ----

// Starts one file: its folders made where missing, then an uploading entry (never listed until
// finished). `on_conflict` decides what a file of the same name in the folder means: rename (the
// new file takes the first free "name (2).ext"), replace (it replaces that file when its upload
// finishes) or refuse.
struct Begun {
    std::optional<control::DriveEntry> entry;
    std::string path;
    std::optional<ApiResult> failure;
};

liquid::runtime::task<Begun> begin_file(Opened& opened,
                                        control::DriveTree& tree,
                                        control::DriveFileStore& store,
                                        std::string parent_id,
                                        const std::string& relative_path,
                                        std::uint64_t size,
                                        std::string content_type,
                                        const std::string& on_conflict,
                                        const std::string& by,
                                        const std::string& field) {
    auto parts = path_parts(relative_path);
    if (!parts) {
        co_return Begun{std::nullopt,
                        {},
                        ApiResult::failure("invalid_input",
                                           "each name in the path is 1 to 255 bytes with no "
                                           "backslash or control character",
                                           field)};
    }
    if (size > max_file_bytes) {
        co_return Begun{std::nullopt,
                        {},
                        ApiResult::failure("size_invalid", "a file is at most 64 GiB", "/size")};
    }
    std::string name = parts->back();
    parts->pop_back();
    Made made = awaited(co_await make_folders(opened, tree, parent_id, *parts, by, field));
    if (made.failure) {
        co_return Begun{std::nullopt, {}, made.failure};
    }
    parent_id = made.folder_id;
    // Expired uploads in the folder go first (lake#588), object before record, so an upload
    // abandoned a day ago no longer holds its name.
    std::vector<control::DriveEntry> expired;
    for (const control::DriveEntry* upload : tree.uploads(parent_id)) {
        if (control::drive_upload_expired(*upload, now_us())) {
            expired.push_back(*upload);
        }
    }
    for (const control::DriveEntry& upload : expired) {
        auto removed = control::settled(co_await store.remove(upload.object_key));
        if (!removed) {
            co_return Begun{std::nullopt, {}, failed(removed.error(), field)};
        }
        auto forgotten = control::settled(co_await control::remove_drive_entry(
            *opened.records, opened.drive.customer_id, opened.drive.id, upload.id));
        if (!forgotten) {
            co_return Begun{std::nullopt, {}, failed(forgotten.error(), field)};
        }
        tree.remove(upload.id);
    }
    std::string replaces;
    if (const control::DriveEntry* existing = tree.child(parent_id, name)) {
        if (on_conflict == "refuse") {
            co_return Begun{
                std::nullopt,
                {},
                ApiResult::failure(
                    "conflict", "the folder already has an item named “" + name + "”", field)};
        }
        if (on_conflict == "replace" && existing->kind == "file" && existing->state == "ready") {
            replaces = existing->id;
        } else {
            name = control::drive_free_name(tree, parent_id, name);
        }
    }
    if (content_type.empty()) {
        content_type = media_type_for(name);
    }
    control::DriveEntry entry;
    entry.customer_id = opened.drive.customer_id;
    entry.drive_id = opened.drive.id;
    entry.parent_id = parent_id;
    entry.kind = "file";
    entry.name = name;
    entry.size = size;
    entry.content_type = content_type;
    entry.object_key = control::new_drive_file_key();
    entry.state = "uploading";
    entry.replaces = replaces;
    entry.created_by = by;
    auto stored =
        control::settled(co_await control::record_drive_entry(*opened.records, std::move(entry)));
    if (!stored) {
        co_return Begun{std::nullopt, {}, failed(stored.error(), field)};
    }
    tree.add(*stored);
    const std::string base = tree.path(parent_id);
    co_return Begun{std::move(*stored), base.empty() ? name : base + "/" + name, std::nullopt};
}

// Finishes one file: reads its object back for the authoritative SHA-256 (and that it holds
// exactly the declared bytes), marks it ready, and removes the file it replaces.
struct Finished {
    std::optional<control::DriveEntry> entry;
    std::optional<ApiResult> failure;
};

liquid::runtime::task<Finished> finish_file(Opened& opened,
                                            control::DriveFileStore& store,
                                            const control::DriveEntry& entry,
                                            const std::string& expected_sha256,
                                            const std::string& by) {
    auto digest = control::settled(co_await store.digest(entry.object_key, entry.size));
    if (!digest) {
        if (digest.error().code == "conflict" || digest.error().code == "not_found") {
            co_return Finished{std::nullopt,
                               ApiResult::failure("conflict",
                                                  "the upload is not complete: send every chunk "
                                                  "of the file's " +
                                                      std::to_string(entry.size) + " bytes first",
                                                  "/id")};
        }
        co_return Finished{std::nullopt, failed(digest.error())};
    }
    if (!expected_sha256.empty() && expected_sha256 != *digest) {
        co_return Finished{std::nullopt,
                           ApiResult::failure("invalid_digest",
                                              "the file's bytes do not match the sha256 sent",
                                              "/sha256")};
    }
    const std::string sha256 = *digest;
    auto changed = control::settled(co_await control::change_drive_entry(
        *opened.records,
        opened.drive.customer_id,
        opened.drive.id,
        entry.id,
        [&](control::DriveEntry& next) -> std::optional<control::ZettaControlError> {
            if (next.state == "ready") {
                return std::nullopt; // finished already: the same bytes, the same digest
            }
            next.state = "ready";
            next.sha256 = sha256;
            next.replaces.clear();
            next.modified_by = by;
            next.modified_us = now_us();
            return std::nullopt;
        }));
    if (!changed) {
        co_return Finished{std::nullopt, failed(changed.error(), "/id")};
    }
    if (!entry.replaces.empty()) {
        // The file it replaces goes, if it is still the one of that name in that folder.
        auto old = control::settled(co_await control::get_drive_entry(
            *opened.records, opened.drive.customer_id, opened.drive.id, entry.replaces));
        if (old && *old && (*old)->parent_id == changed->parent_id &&
            (*old)->name == changed->name && (*old)->kind == "file") {
            auto removed = control::settled(co_await store.remove((*old)->object_key));
            if (removed) {
                static_cast<void>(control::settled(co_await control::remove_drive_entry(
                    *opened.records, opened.drive.customer_id, opened.drive.id, (*old)->id)));
            }
        }
    }
    co_return Finished{std::move(*changed), std::nullopt};
}

std::optional<ApiResult> files_drive_only(const Opened& opened) {
    if (opened.drive.storage == "store") {
        return ApiResult::failure("invalid_input",
                                  "a store drive takes upload_drive_entries (at most 16 MiB a "
                                  "call); chunked uploads are for workspace-files drives",
                                  "/drive_id");
    }
    if (opened.drive.storage != "files") {
        return ApiResult::failure("invalid_input",
                                  "a virtual drive holds folders and shortcuts only; upload into a "
                                  "workspace-files drive",
                                  "/drive_id");
    }
    return std::nullopt;
}

liquid::runtime::task<ApiResult> begin_drive_upload(const json& input, ApiContext& context) {
    const std::string content_type = input.value("content_type", std::string());
    if (!content_type.empty() && !valid_media_type(content_type)) {
        co_return ApiResult::failure(
            "invalid_input", "content_type is a media type such as text/csv", "/content_type");
    }
    Opened opened = awaited(co_await open_drive(input, context, true));
    if (opened.failure) {
        co_return *opened.failure;
    }
    if (auto refused = files_drive_only(opened)) {
        co_return *refused;
    }
    Files files = file_store(context, opened.drive.customer_id, input.at("workspace_id"));
    if (files.failure) {
        co_return *files.failure;
    }
    Read read = awaited(co_await read_tree(opened));
    if (read.failure) {
        co_return *read.failure;
    }
    const std::string parent_id = input.value("parent_id", std::string());
    if (auto refused = folder_problem(*read.tree, parent_id, "/parent_id")) {
        co_return *refused;
    }
    // The bucket is made before the entry, so a lake that cannot write files leaves no entry.
    auto bucket = control::settled(co_await files.store->ready());
    if (!bucket) {
        co_return failed(bucket.error());
    }
    Begun begun = awaited(co_await begin_file(opened,
                                              *read.tree,
                                              *files.store,
                                              parent_id,
                                              input.at("path"),
                                              input.at("size"),
                                              content_type,
                                              input.value("on_conflict", "rename"),
                                              actor(context),
                                              "/path"));
    if (begun.failure) {
        co_return *begun.failure;
    }
    context.audit_details["entry_id"] = begun.entry->id;
    context.audit_details["size"] = begun.entry->size;
    co_return ApiResult::success({{"entry", entry_json(*begun.entry, begun.path)},
                                  {"block_size", control::drive_file_block},
                                  {"max_chunk_bytes", max_chunk_bytes}});
}

// The uploading file of the drive, or the failure.
liquid::runtime::task<Finished> uploading(Opened& opened, const std::string& id) {
    auto found = control::settled(co_await control::get_drive_entry(
        *opened.records, opened.drive.customer_id, opened.drive.id, id));
    if (!found) {
        co_return Finished{std::nullopt, failed(found.error(), "/id")};
    }
    if (!*found || (*found)->kind != "file") {
        co_return Finished{std::nullopt,
                           ApiResult::failure("not_found", "no such upload in this drive", "/id")};
    }
    co_return Finished{std::move(**found), std::nullopt};
}

liquid::runtime::task<ApiResult> append_drive_upload(const json& input, ApiContext& context) {
    Opened opened = awaited(co_await open_drive(input, context, true));
    if (opened.failure) {
        co_return *opened.failure;
    }
    Finished found = awaited(co_await uploading(opened, input.at("id")));
    if (found.failure) {
        co_return *found.failure;
    }
    const control::DriveEntry& entry = *found.entry;
    if (entry.state != "uploading") {
        co_return ApiResult::failure("conflict", "this upload is finished", "/id");
    }
    if (control::drive_upload_expired(entry, now_us())) {
        co_return ApiResult::failure("conflict",
                                     "this upload expired: it was not finished within 24 hours of "
                                     "its start; begin it again",
                                     "/id");
    }
    auto content = decode_base64(input.at("content").get<std::string>(), "/content");
    if (!content) {
        co_return ApiResult::failure(
            content.error().code, content.error().message, content.error().path);
    }
    const std::uint64_t offset = input.at("offset");
    if (content->size() > max_chunk_bytes) {
        co_return ApiResult::failure("size_invalid", "a chunk is at most 8 MiB", "/content");
    }
    if (offset > entry.size || content->size() > entry.size - offset) {
        co_return ApiResult::failure(
            "invalid_input", "the chunk runs past the file's declared size", "/offset");
    }
    Files files = file_store(context, opened.drive.customer_id, input.at("workspace_id"));
    if (files.failure) {
        co_return *files.failure;
    }
    const bool last = offset + content->size() == entry.size;
    auto written = control::settled(
        co_await files.store->write(entry.object_key, offset, bytes_text(*content), last));
    if (!written) {
        if (written.error().code == "conflict") {
            co_return ApiResult::failure(
                "conflict",
                "this chunk does not continue the upload: send the chunks in order, each at the "
                "offset where the last one ended",
                "/offset");
        }
        co_return failed(written.error());
    }
    context.audit_details["entry_id"] = entry.id;
    record_workspace_transfer(context, input.at("workspace_id"), content->size(), 0);
    // What the object holds now, not where this chunk ended (lake#588): a chunk sent again after
    // later ones (an answer lost, a client starting over) answers how much the upload has, so a
    // client that trusts `complete` finishes instead of sending everything again.
    auto received = control::settled(co_await files.store->received(entry.object_key));
    if (!received) {
        co_return failed(received.error());
    }
    const std::uint64_t held = std::max<std::uint64_t>(*received, offset + content->size());
    co_return ApiResult::success(
        {{"received", held}, {"complete", held == entry.size}, {"size", entry.size}});
}

liquid::runtime::task<ApiResult> finish_drive_upload(const json& input, ApiContext& context) {
    Opened opened = awaited(co_await open_drive(input, context, true));
    if (opened.failure) {
        co_return *opened.failure;
    }
    Finished found = awaited(co_await uploading(opened, input.at("id")));
    if (found.failure) {
        co_return *found.failure;
    }
    Files files = file_store(context, opened.drive.customer_id, input.at("workspace_id"));
    if (files.failure) {
        co_return *files.failure;
    }
    if (control::drive_upload_expired(*found.entry, now_us())) {
        co_return ApiResult::failure("conflict",
                                     "this upload expired: it was not finished within 24 hours of "
                                     "its start; begin it again",
                                     "/id");
    }
    Finished finished = awaited(co_await finish_file(
        opened, *files.store, *found.entry, input.value("sha256", std::string()), actor(context)));
    if (finished.failure) {
        co_return *finished.failure;
    }
    Read read = awaited(co_await read_tree(opened));
    const std::string path = read.tree ? read.tree->path(finished.entry->id) : finished.entry->name;
    context.audit_details["entry_id"] = finished.entry->id;
    context.audit_details["sha256"] = finished.entry->sha256;
    context.audit_details["size"] = finished.entry->size;
    co_return ApiResult::success(entry_json(*finished.entry, path));
}

liquid::runtime::task<ApiResult> upload_drive_entries(const json& input, ApiContext& context) {
    const json& list = input.at("files");
    if (list.empty()) {
        co_return ApiResult::failure("invalid_input", "at least one file is required", "/files");
    }
    std::vector<std::string> contents;
    std::vector<std::string> paths;
    std::uint64_t total = 0;
    for (std::size_t index = 0; index < list.size(); ++index) {
        const std::string field = "/files/" + std::to_string(index);
        // One item shape for both upload calls (lake#587): `path`, or `name` for one file name.
        const json& item = list.at(index);
        if (item.contains("path") == item.contains("name")) {
            co_return ApiResult::failure("invalid_input",
                                         "send the file's `path` (or its `name`), not both",
                                         field + "/path");
        }
        if (item.contains("name") &&
            item.at("name").get<std::string>().find('/') != std::string::npos) {
            co_return ApiResult::failure("invalid_input",
                                         "a name is one file name; send folders in `path`",
                                         field + "/name");
        }
        paths.push_back(item.contains("path") ? item.at("path").get<std::string>()
                                              : item.at("name").get<std::string>());
        auto decoded = decode_base64(list.at(index).value("content", ""), field + "/content");
        if (!decoded) {
            co_return ApiResult::failure(
                decoded.error().code, decoded.error().message, decoded.error().path);
        }
        total += decoded->size();
        if (total > max_inline_bytes) {
            co_return ApiResult::failure(
                "size_invalid",
                "one call uploads at most 16 MiB; send larger files with begin_drive_upload",
                field + "/content");
        }
        const std::string type = list.at(index).value("content_type", "");
        if (!type.empty() && !valid_media_type(type)) {
            co_return ApiResult::failure(
                "invalid_input", "content_type is a media type", field + "/content_type");
        }
        contents.push_back(bytes_text(*decoded));
    }
    Opened opened = awaited(co_await open_drive(input, context, true));
    if (opened.failure) {
        co_return *opened.failure;
    }
    if (opened.drive.storage == "store") {
        co_return called_as(co_await store_upload(
            input, context, opened.drive, std::move(paths), std::move(contents), total));
    }
    if (auto refused = files_drive_only(opened)) {
        co_return *refused;
    }
    Files files = file_store(context, opened.drive.customer_id, input.at("workspace_id"));
    if (files.failure) {
        co_return *files.failure;
    }
    Read read = awaited(co_await read_tree(opened));
    if (read.failure) {
        co_return *read.failure;
    }
    const std::string parent_id = input.value("parent_id", std::string());
    if (auto refused = folder_problem(*read.tree, parent_id, "/parent_id")) {
        co_return *refused;
    }
    auto bucket = control::settled(co_await files.store->ready());
    if (!bucket) {
        co_return failed(bucket.error());
    }
    const std::string on_conflict = input.value("on_conflict", "rename");
    const std::string by = actor(context);
    json items = json::array();
    for (std::size_t index = 0; index < list.size(); ++index) {
        const std::string field = "/files/" + std::to_string(index);
        Begun begun = awaited(co_await begin_file(opened,
                                                  *read.tree,
                                                  *files.store,
                                                  parent_id,
                                                  paths[index],
                                                  contents[index].size(),
                                                  list.at(index).value("content_type", ""),
                                                  on_conflict,
                                                  by,
                                                  field + "/path"));
        if (begun.failure) {
            co_return *begun.failure;
        }
        auto written = control::settled(co_await files.store->write(
            begun.entry->object_key, 0, std::move(contents[index]), true));
        if (!written) {
            co_return failed(written.error(), field + "/content");
        }
        Finished finished =
            awaited(co_await finish_file(opened, *files.store, *begun.entry, "", by));
        if (finished.failure) {
            co_return *finished.failure;
        }
        items.push_back(entry_json(*finished.entry, begun.path));
    }
    context.audit_details["files"] = items.size();
    context.audit_details["bytes"] = total;
    record_workspace_transfer(context, input.at("workspace_id"), total, 0);
    co_return ApiResult::success({{"items", std::move(items)}});
}

liquid::runtime::task<ApiResult> read_drive_file(const json& input, ApiContext& context) {
    Opened opened = awaited(co_await open_drive(input, context, true));
    if (opened.failure) {
        co_return *opened.failure;
    }
    if (opened.drive.storage == "store") {
        co_return called_as(co_await store_read(input, context, opened.drive));
    }
    auto found = control::settled(co_await control::get_drive_entry(
        *opened.records, opened.drive.customer_id, opened.drive.id, input.at("id")));
    if (!found) {
        co_return failed(found.error(), "/id");
    }
    if (!*found || (*found)->state != "ready") {
        co_return ApiResult::failure("not_found", "no such item in this drive", "/id");
    }
    const control::DriveEntry& entry = **found;
    if (entry.kind != "file") {
        co_return ApiResult::failure("invalid_input", "this item is not a file", "/id");
    }
    const std::uint64_t offset = input.value("offset", std::uint64_t{0});
    if (offset > entry.size) {
        co_return ApiResult::failure("invalid_input", "offset is past the file's end", "/offset");
    }
    const std::uint64_t length =
        std::min(input.value("length", entry.size - offset), entry.size - offset);
    if (length > max_inline_bytes) {
        co_return ApiResult::failure(
            "size_invalid",
            "one call reads at most 16 MiB: read the file in ranges (offset, length), or download "
            "it from /drive/files/download",
            "/length");
    }
    Files files = file_store(context, opened.drive.customer_id, input.at("workspace_id"));
    if (files.failure) {
        co_return *files.failure;
    }
    auto bytes =
        control::settled(co_await files.store->read(entry.object_key, entry.size, offset, length));
    if (!bytes) {
        co_return failed(bytes.error());
    }
    context.audit_details["entry_id"] = entry.id;
    context.audit_details["bytes"] = bytes->size();
    record_workspace_transfer(context, input.at("workspace_id"), 0, bytes->size());
    co_return ApiResult::success({{"id", entry.id},
                                  {"name", entry.name},
                                  {"size", entry.size},
                                  {"content_type", entry.content_type},
                                  {"sha256", entry.sha256},
                                  {"offset", offset},
                                  {"content_base64", encode_base64(*bytes)}});
}

liquid::runtime::task<ApiResult> recent_drive_entries(const json& input, ApiContext& context) {
    if (!context.store) {
        co_return ApiResult::failure("store_unavailable", "workspaces are not configured");
    }
    if (auto refused = authorize_space_read(input, context)) {
        co_return *refused;
    }
    const std::string customer_id = input.at("customer_id");
    auto records = workspace_records(context, customer_id, input.at("workspace_id"));
    if (!records) {
        co_return failed(records.error());
    }
    auto drives = control::settled(co_await control::list_drives(*records, customer_id));
    if (!drives) {
        co_return failed(drives.error());
    }
    const std::size_t limit = input.value("limit", default_recent);
    struct Row {
        const control::Drive* drive;
        control::DriveEntry entry;
        std::string path;
    };
    std::vector<Row> rows;
    for (const control::Drive& drive : *drives) {
        if (drive.storage != "files") {
            continue;
        }
        auto listed =
            control::settled(co_await control::list_drive_entries(*records, customer_id, drive.id));
        if (!listed) {
            co_return failed(listed.error());
        }
        const control::DriveTree tree(std::move(*listed));
        for (const control::DriveEntry& entry : tree.entries()) {
            if (entry.kind == "file" && entry.state == "ready") {
                rows.push_back({&drive, entry, tree.path(entry.id)});
            }
        }
    }
    std::ranges::sort(rows, [](const Row& a, const Row& b) {
        return a.entry.modified_us != b.entry.modified_us
                   ? a.entry.modified_us > b.entry.modified_us
                   : a.entry.id < b.entry.id;
    });
    if (rows.size() > limit) {
        rows.resize(limit);
    }
    json items = json::array();
    for (const Row& row : rows) {
        items.push_back({{"drive", {{"id", row.drive->id}, {"name", row.drive->name}}},
                         {"entry", entry_json(row.entry, row.path)}});
    }
    co_return ApiResult::success({{"items", std::move(items)}});
}

// ---- A drive file as a dataset (lake#593) ----

// Loads a CSV or JSONL file of a drive (a workspace file by `id`, or a repository drive's file by
// its path in `id`) as a dataset of the workspace, through the same capabilities a person would
// call: read_drive_file or download_drive_file for the bytes (authorized and audited as reads of
// the drive), then the workspace's upload_dataset through workspace_act. Nothing is copied
// anywhere else, and at most 16 MiB is loaded, as upload_dataset takes.
liquid::runtime::task<ApiResult> load_drive_dataset(const json& input, ApiContext& context) {
    Opened opened = awaited(co_await open_drive(input, context, false));
    if (opened.failure) {
        co_return *opened.failure;
    }
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const std::string id = input.at("id");
    if (opened.drive.storage == "virtual") {
        co_return ApiResult::failure(
            "invalid_input",
            "a virtual drive holds shortcuts only; open the file where it lives",
            "/drive_id");
    }
    // The file's name decides its format, so a file that is neither CSV nor JSONL is refused
    // before its bytes are read.
    std::string name = id.substr(id.rfind('/') == std::string::npos ? 0 : id.rfind('/') + 1);
    if (opened.drive.storage == "files" || opened.drive.storage == "store") {
        ApiResult entry = called_as(co_await call_as(context,
                                                     "get_drive_entry",
                                                     {{"customer_id", customer_id},
                                                      {"workspace_id", workspace_id},
                                                      {"drive_id", opened.drive.id},
                                                      {"id", id}}));
        if (!entry.ok) {
            co_return ApiResult::failure(entry.error.code, entry.error.message, "/id");
        }
        if (entry.value.value("kind", "") != "file" || entry.value.value("state", "") != "ready") {
            co_return ApiResult::failure("not_found", "no such file in this drive", "/id");
        }
        name = entry.value.value("name", std::string());
        if (entry.value.value("size", std::uint64_t{0}) > max_inline_bytes) {
            co_return ApiResult::failure(
                "size_invalid", "a dataset is loaded from a file of at most 16 MiB", "/id");
        }
    }
    std::string format = input.value("format", std::string());
    if (format.empty()) {
        const std::string type = control::drive_media_type(name);
        format = type == "text/csv" ? "csv" : type == "application/x-ndjson" ? "jsonl" : "";
    }
    if (format != "csv" && format != "jsonl") {
        co_return ApiResult::failure("invalid_input",
                                     "a dataset is loaded from a CSV or JSONL file; name its "
                                     "`format` when its name does not say",
                                     "/format");
    }
    ApiResult read =
        opened.drive.storage == "repository"
            ? called_as(co_await call_as(
                  context,
                  "download_drive_file",
                  {{"repository_id", opened.drive.repository_id}, {"ref", "main"}, {"path", id}}))
            : called_as(co_await call_as(context,
                                         "read_drive_file",
                                         {{"customer_id", customer_id},
                                          {"workspace_id", workspace_id},
                                          {"drive_id", opened.drive.id},
                                          {"id", id}}));
    if (!read.ok) {
        co_return ApiResult::failure(read.error.code, read.error.message, "/id");
    }
    if (!read.value.contains("content_base64") || !read.value.at("content_base64").is_string() ||
        read.value.value("size", std::uint64_t{0}) > max_inline_bytes) {
        co_return ApiResult::failure(
            "size_invalid", "a dataset is loaded from a file of at most 16 MiB", "/id");
    }
    auto bytes = decode_base64(read.value.at("content_base64").get<std::string>(), "/id");
    if (!bytes) {
        co_return ApiResult::failure("corrupt", "the file's bytes did not decode", "/id");
    }
    json dataset{{"table_name", input.at("table_name")},
                 {"format", format},
                 {"content", bytes_text(*bytes)}};
    if (input.contains("if_exists")) {
        dataset["if_exists"] = input.at("if_exists");
    }
    ApiResult loaded = called_as(co_await call_as(context,
                                                  "workspace_act",
                                                  {{"customer_id", customer_id},
                                                   {"workspace_id", workspace_id},
                                                   {"capability", "upload_dataset"},
                                                   {"input", std::move(dataset)}}));
    if (!loaded.ok) {
        // upload_dataset's own fields are this call's fields of the same name.
        std::string path = loaded.error.path;
        if (path.starts_with("/input/")) {
            path = path.substr(6);
        }
        if (path == "/content") {
            path = "/id";
        }
        co_return ApiResult::failure(loaded.error.code, loaded.error.message, path);
    }
    json value = loaded.value.is_object() ? loaded.value : json{{"result", loaded.value}};
    value["source"] = {{"drive_id", opened.drive.id}, {"id", id}, {"name", name}};
    context.audit_details["drive_id"] = opened.drive.id;
    context.audit_details["id"] = id;
    context.audit_details["format"] = format;
    co_return ApiResult::success(std::move(value));
}

// ---- A drive file as a model (lake#654) ----

// Brings a model file of a drive into the workspace as a standalone model, as load_drive_dataset
// brings a table: the bytes are read as the caller (read_drive_file or download_drive_file, so
// the drive's authorization and audit apply), then the workspace engine's register_model keeps
// them through workspace_act. The file goes as `weights_base64`, so a binary model (ONNX, XGBoost's
// UBJSON) arrives intact; the engine reads ONNX tree ensembles, XGBoost and LightGBM models and its
// own weights format, and refuses any other (lake#681). At most 16 MiB.
liquid::runtime::task<ApiResult> load_drive_model(const json& input, ApiContext& context) {
    Opened opened = awaited(co_await open_drive(input, context, false));
    if (opened.failure) {
        co_return *opened.failure;
    }
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const std::string id = input.at("id");
    if (opened.drive.storage == "virtual") {
        co_return ApiResult::failure(
            "invalid_input",
            "a virtual drive holds shortcuts only; open the file where it lives",
            "/drive_id");
    }
    if (!input.contains("name") && !input.contains("version_of")) {
        co_return ApiResult::failure(
            "invalid_input", "name the model, or give version_of for a new version of one", "/name");
    }
    std::string name = id.substr(id.rfind('/') == std::string::npos ? 0 : id.rfind('/') + 1);
    if (opened.drive.storage == "files") {
        ApiResult entry = called_as(co_await call_as(context,
                                                     "get_drive_entry",
                                                     {{"customer_id", customer_id},
                                                      {"workspace_id", workspace_id},
                                                      {"drive_id", opened.drive.id},
                                                      {"id", id}}));
        if (!entry.ok) {
            co_return ApiResult::failure(entry.error.code, entry.error.message, "/id");
        }
        if (entry.value.value("kind", "") != "file" || entry.value.value("state", "") != "ready") {
            co_return ApiResult::failure("not_found", "no such file in this drive", "/id");
        }
        name = entry.value.value("name", std::string());
        if (entry.value.value("size", std::uint64_t{0}) > max_inline_bytes) {
            co_return ApiResult::failure(
                "size_invalid", "a model is brought from a file of at most 16 MiB", "/id");
        }
    }
    ApiResult read =
        opened.drive.storage == "repository"
            ? called_as(co_await call_as(
                  context,
                  "download_drive_file",
                  {{"repository_id", opened.drive.repository_id}, {"ref", "main"}, {"path", id}}))
            : called_as(co_await call_as(context,
                                         "read_drive_file",
                                         {{"customer_id", customer_id},
                                          {"workspace_id", workspace_id},
                                          {"drive_id", opened.drive.id},
                                          {"id", id}}));
    if (!read.ok) {
        co_return ApiResult::failure(read.error.code, read.error.message, "/id");
    }
    if (!read.value.contains("content_base64") || !read.value.at("content_base64").is_string() ||
        read.value.value("size", std::uint64_t{0}) > max_inline_bytes) {
        co_return ApiResult::failure(
            "size_invalid", "a model is brought from a file of at most 16 MiB", "/id");
    }
    auto bytes = decode_base64(read.value.at("content_base64").get<std::string>(), "/id");
    if (!bytes) {
        co_return ApiResult::failure("corrupt", "the file's bytes did not decode", "/id");
    }
    json model{{"weights_base64", read.value.at("content_base64")},
               {"source", "drive " + opened.drive.name + ": " + name}};
    for (const char* key : {"name", "version_of", "kind", "metrics", "format", "feature_names"}) {
        if (input.contains(key)) {
            model[key] = input.at(key);
        }
    }
    ApiResult brought = called_as(co_await call_as(context,
                                                   "workspace_act",
                                                   {{"customer_id", customer_id},
                                                    {"workspace_id", workspace_id},
                                                    {"capability", "register_model"},
                                                    {"input", std::move(model)}}));
    if (!brought.ok) {
        // register_model's own fields are this call's fields of the same name; the file is `id`.
        std::string path = brought.error.path;
        if (path.starts_with("/input/")) {
            path = path.substr(6);
        }
        if (path == "/weights" || path == "/weights_base64" || path == "/source") {
            path = "/id";
        }
        co_return ApiResult::failure(brought.error.code, brought.error.message, path);
    }
    json value = brought.value.is_object() ? brought.value : json{{"result", brought.value}};
    value["source"] = {{"drive_id", opened.drive.id}, {"id", id}, {"name", name}};
    context.audit_details["drive_id"] = opened.drive.id;
    context.audit_details["id"] = id;
    co_return ApiResult::success(std::move(value));
}

// ---- Search ----

constexpr std::size_t default_search = 50;

// ASCII letters in lower case; other bytes (UTF-8 included) compare as they are.
std::string folded(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return out;
}

std::string parent_of(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
}

std::string name_of(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

// A repository's tree at `main`, read off the server thread, or why it could not be read.
struct RepositoryPaths {
    std::vector<RepositoryTreeEntry> entries;
    std::string problem;
};

liquid::runtime::task<RepositoryPaths> repository_paths(ApiContext& context,
                                                        const std::string& repository_id) {
    if (!context.sync_store) {
        co_return RepositoryPaths{{}, "this Lake has no repository store"};
    }
    const auto repository = context.store->get_repository(repository_id);
    if (!repository) {
        co_return RepositoryPaths{{},
                                  repository.error().code == "not_found"
                                      ? std::string("its repository is gone")
                                      : repository.error().message};
    }
    if (!repository->deleted_at.empty()) {
        co_return RepositoryPaths{{}, "its repository was deleted"};
    }
    if (!may_read_repository(context, repository_id)) {
        co_return RepositoryPaths{{}, "you cannot read its repository"};
    }
    SyncStore* store = context.sync_store;
    const control::Repository found = *repository;
    auto read = co_await blocking_call(
        [store, found] { return store->repository_tree(found.customer_id, found.id, "main"); },
        [](std::string message) -> SyncStoreResult<std::vector<RepositoryTreeEntry>> {
            return std::unexpected(SyncStoreError{"store_unavailable", std::move(message)});
        });
    if (!read) {
        co_return RepositoryPaths{{}, read.error().message};
    }
    co_return RepositoryPaths{std::move(*read), {}};
}

RepositoryPaths awaited_paths(std::expected<RepositoryPaths, liquid::error> completed) {
    return completed ? std::move(*completed) : RepositoryPaths{{}, completed.error().message};
}

// Drive and item names that contain `q`, ignoring the case of A to Z, across the workspace's drives
// (or one): each drive's entries are read once, a repository drive's tree at `main` once.
liquid::runtime::task<ApiResult> search_drives(const json& input, ApiContext& context) {
    if (!context.store) {
        co_return ApiResult::failure("store_unavailable", "workspaces are not configured");
    }
    if (auto refused = authorize_space_read(input, context)) {
        co_return *refused;
    }
    const std::string needle = folded(input.at("q").get<std::string>());
    if (needle.empty()) {
        co_return ApiResult::failure("invalid_input", "type something to search for", "/q");
    }
    const std::string customer_id = input.at("customer_id");
    auto records = workspace_records(context, customer_id, input.at("workspace_id"));
    if (!records) {
        co_return failed(records.error());
    }
    auto drives = control::settled(co_await control::list_drives(*records, customer_id));
    if (!drives) {
        co_return failed(drives.error());
    }
    const std::string only = input.value("drive_id", std::string());
    if (!only.empty()) {
        std::erase_if(*drives, [&](const control::Drive& drive) { return drive.id != only; });
        if (drives->empty()) {
            co_return ApiResult::failure(
                "not_found", "no such drive in this workspace", "/drive_id");
        }
        context.audit_details["drive_id"] = only;
    }
    const std::size_t limit = input.value("limit", default_search);
    const auto matches = [&](std::string_view name) {
        return folded(name).find(needle) != std::string::npos;
    };
    const auto drive_ref = [](const control::Drive& drive) {
        return json{{"id", drive.id},
                    {"name", drive.name},
                    {"storage", drive.storage},
                    {"repository_id",
                     drive.repository_id.empty() ? json(nullptr) : json(drive.repository_id)},
                    {"store_id", drive.store_id.empty() ? json(nullptr) : json(drive.store_id)}};
    };

    // Every match is kept (bounded by what the drives hold) so the order does not depend on the
    // order the drives were read in; shortcuts are checked only for the matches answered.
    struct Hit {
        json item;
        const control::DriveEntry* shortcut = nullptr;
    };
    std::vector<Hit> hits;
    std::vector<control::DriveTree> trees;
    trees.reserve(drives->size());
    json skipped = json::array();
    for (const control::Drive& drive : *drives) {
        if (matches(drive.name)) {
            hits.push_back({{{"drive", drive_ref(drive)},
                             {"kind", "drive"},
                             {"id", nullptr},
                             {"parent_id", nullptr},
                             {"name", drive.name},
                             {"path", ""},
                             {"folder", ""}}});
        }
        if (drive.storage == "store") {
            StoreWalk walk = awaited_walk(co_await store_walk(context, drive, input));
            if (!walk.problem.empty()) {
                skipped.push_back(
                    {{"drive", drive_ref(drive)}, {"reason", std::move(walk.problem)}});
                continue;
            }
            for (const control::StoreItem& item : walk.items) {
                const std::string name = control::store_name(item.path);
                if (!matches(name)) {
                    continue;
                }
                const std::string folder = control::store_parent(item.path);
                json found{{"drive", drive_ref(drive)},
                           {"kind", item.folder ? "folder" : "file"},
                           {"id", item.path},
                           {"parent_id", folder.empty() ? json(nullptr) : json(folder)},
                           {"name", name},
                           {"path", item.path},
                           {"folder", folder}};
                if (!item.folder) {
                    found["size"] = item.size;
                    found["content_type"] = item.content_type.empty()
                                                ? control::drive_media_type(name)
                                                : item.content_type;
                    if (!item.modified_at.empty()) {
                        found["modified_at"] = item.modified_at;
                    }
                }
                hits.push_back({std::move(found)});
            }
            continue;
        }
        if (drive.storage == "repository") {
            RepositoryPaths read =
                awaited_paths(co_await repository_paths(context, drive.repository_id));
            if (!read.problem.empty()) {
                skipped.push_back(
                    {{"drive", drive_ref(drive)}, {"reason", std::move(read.problem)}});
                continue;
            }
            for (const RepositoryTreeEntry& entry : read.entries) {
                const std::string name = name_of(entry.path);
                if (entry.path.empty() || name == ".lake-folder" ||
                    (entry.kind != "tree" && entry.kind != "blob") || !matches(name)) {
                    continue;
                }
                json item{{"drive", drive_ref(drive)},
                          {"kind", entry.kind == "tree" ? "folder" : "file"},
                          {"id", nullptr},
                          {"parent_id", nullptr},
                          {"name", name},
                          {"path", entry.path},
                          {"folder", parent_of(entry.path)}};
                if (entry.kind == "blob") {
                    item["size"] = entry.size;
                }
                hits.push_back({std::move(item)});
            }
            continue;
        }
        auto listed =
            control::settled(co_await control::list_drive_entries(*records, customer_id, drive.id));
        if (!listed) {
            co_return failed(listed.error());
        }
        const control::DriveTree& tree = trees.emplace_back(std::move(*listed));
        for (const control::DriveEntry& entry : tree.entries()) {
            if (entry.state != "ready" || !matches(entry.name)) {
                continue;
            }
            json item{
                {"drive", drive_ref(drive)},
                {"kind", entry.kind},
                {"id", entry.id},
                {"parent_id", entry.parent_id.empty() ? json(nullptr) : json(entry.parent_id)},
                {"name", entry.name},
                {"path", tree.path(entry.id)},
                {"folder", tree.path(entry.parent_id)}};
            if (entry.kind == "file") {
                item["size"] = entry.size;
                item["content_type"] = entry.content_type;
                item["modified_at"] = control::record_instant(entry.modified_us);
            }
            if (entry.kind == "shortcut") {
                item["target"] = entry.target;
            }
            hits.push_back({std::move(item), entry.kind == "shortcut" ? &entry : nullptr});
        }
    }
    const auto rank = [](const json& item) { return item.at("kind") == "drive" ? 0 : 1; };
    std::ranges::stable_sort(hits, [&](const Hit& a, const Hit& b) {
        const auto key = [&](const Hit& hit) {
            return std::tuple(rank(hit.item),
                              hit.item.at("drive").at("name").get<std::string>(),
                              hit.item.at("path").get<std::string>());
        };
        return key(a) < key(b);
    });
    const bool truncated = hits.size() > limit;
    if (truncated) {
        hits.resize(limit);
    }
    ShortcutChecker checker(context, *records, customer_id);
    json items = json::array();
    for (Hit& hit : hits) {
        if (hit.shortcut != nullptr) {
            const Status status = status_of(co_await checker.check(hit.shortcut->target));
            hit.item["status"] = status.status;
            if (!status.reason.empty()) {
                hit.item["reason"] = status.reason;
            }
        }
        items.push_back(std::move(hit.item));
    }
    context.audit_details["matches"] = items.size();
    co_return ApiResult::success(
        {{"items", std::move(items)}, {"truncated", truncated}, {"skipped", std::move(skipped)}});
}

} // namespace

void register_drives_capabilities(Registry& registry) {
    const auto action = [&](std::string name,
                            std::string title,
                            std::string description,
                            json input,
                            json output,
                            AsyncCapabilityHandler handler) {
        registry.add(Capability{.name = std::move(name),
                                .title = std::move(title),
                                .description = std::move(description),
                                .input_schema = std::move(input),
                                .output_schema = std::move(output),
                                .kind = CapabilityKind::Action,
                                .object = "customer",
                                .action = "write",
                                .resolve_object = customer_object,
                                .async_handler = std::move(handler)});
    };
    const auto read = [&](std::string name,
                          std::string title,
                          std::string description,
                          json input,
                          json output,
                          AsyncCapabilityHandler handler,
                          bool lists = false) {
        registry.add(Capability{.name = std::move(name),
                                .title = std::move(title),
                                .description = std::move(description),
                                .input_schema = std::move(input),
                                .output_schema = std::move(output),
                                .kind = CapabilityKind::Read,
                                .object = "customer",
                                .action = "read",
                                .resolve_object = customer_object,
                                .authorizes_candidates = lists,
                                .async_handler = std::move(handler)});
    };
    const json deleted{{"type", "object"},
                       {"properties",
                        {{"deleted", {{"type", "boolean"}, {"const", true}}},
                         {"entries_removed", {{"type", "integer"}, {"minimum", 0}}},
                         {"repository_id", string_schema()},
                         {"store_id", string_schema()}}},
                       {"required", json::array({"deleted", "entries_removed"})},
                       {"additionalProperties", false}};
    const json on_conflict{{"type", "string"},
                           {"enum", {"rename", "replace", "refuse"}},
                           {"description",
                            "a file of the same name in the folder: rename (default) gives the new "
                            "file the first free name, replace replaces it once the upload "
                            "finishes, refuse answers conflict"}};

    action(
        "create_drive",
        "Create a drive",
        "Create a drive in a workspace. `storage` is repository (a liquid repository with "
        "history: an existing `repository_id` of this workspace, or a new repository named "
        "`repository_name`, which gets its first change at once), files (workspace files in the "
        "workspace Zetta, moved and renamed without copying), virtual (folders of shortcuts) or "
        "store (one of the workspace's Ready bucket or filesystem stores, `store_id`, browsed in "
        "place without copying; deleting the drive never deletes the store; a volume is "
        "refused).",
        workspace_input(
            {{"name", string_schema()},
             {"storage",
              {{"type", "string"}, {"enum", {"repository", "files", "virtual", "store"}}}},
             {"repository_id", string_schema()},
             {"repository_name", string_schema()},
             {"store_id", string_schema()}},
            json::array({"name", "storage"})),
        drive_schema(),
        create_drive);
    read("list_drives",
         "List Drives",
         "The workspace's drives, by name, each with how it stores its files (repository, files or "
         "virtual).",
         workspace_input(json::object(), json::array()),
         object_of({{"drives", {{"type", "array"}, {"items", drive_schema()}}}},
                   json::array({"drives"})),
         list_drives,
         true);
    read("get_drive",
         "Read a Drive",
         "One drive of the workspace: its name, its storage and, for a repository drive, its "
         "repository.",
         workspace_input({{"id", string_schema()}}, json::array({"id"})),
         drive_schema(),
         get_drive);
    action("rename_drive",
           "Rename a drive",
           "Give a drive a new name, unique in its workspace.",
           workspace_input({{"id", string_schema()}, {"name", string_schema()}},
                           json::array({"id", "name"})),
           drive_schema(),
           rename_drive);
    action("delete_drive",
           "Delete a drive",
           "Delete a drive; `confirm_name` must be its name. A workspace-files drive's folders, "
           "files and shortcuts are deleted with it and cannot be restored. A repository drive's "
           "repository and a store drive's store are never deleted: delete them from their own "
           "pages.",
           workspace_input({{"id", string_schema()}, {"confirm_name", string_schema()}},
                           json::array({"id", "confirm_name"})),
           deleted,
           delete_drive);
    read("list_drive_entries",
         "List a Drive Folder",
         "The folders, files and shortcuts in one folder of a workspace-files, virtual or store "
         "drive: `folder_id`, or a slash `path` from the top; neither is the top. In a store "
         "drive an item's id is its path in the store. Each shortcut carries "
         "`status`: ok, broken (with a reason) or unknown when Lake cannot check its kind. "
         "`uploads` lists the folder's unfinished uploads with `age_seconds`: each holds its name "
         "until it is finished, deleted (delete_drive_entry) or expires 24 hours after it began.",
         workspace_input({{"drive_id", string_schema()},
                          {"folder_id", string_schema()},
                          {"path", string_schema()}},
                         json::array({"drive_id"})),
         object_of({{"drive", drive_schema()},
                    {"folder_id", {{"type", json::array({"string", "null"})}}},
                    {"path", string_schema()},
                    {"ancestors",
                     {{"type", "array"},
                      {"items",
                       object_of({{"id", string_schema()}, {"name", string_schema()}},
                                 json::array({"id", "name"}))}}},
                    {"items", {{"type", "array"}, {"items", entry_schema()}}},
                    {"uploads", {{"type", "array"}, {"items", upload_schema()}}}},
                   json::array({"drive", "folder_id", "path", "ancestors", "items", "uploads"})),
         list_drive_entries,
         true);
    read("get_drive_entry",
         "Read a Drive Item",
         "One folder, file or shortcut of a workspace-files, virtual or store drive, with its "
         "path.",
         workspace_input({{"drive_id", string_schema()}, {"id", string_schema()}},
                         json::array({"drive_id", "id"})),
         entry_schema(),
         get_drive_entry);
    action("create_drive_folder_entry",
           "Create a folder in a drive",
           "Create a folder in a workspace-files or virtual drive: `path` may name several levels "
           "(\"Reports/2026\"), each made where missing. A folder already there is answered as "
           "it is.",
           workspace_input({{"drive_id", string_schema()},
                            {"parent_id", string_schema()},
                            {"path", string_schema()}},
                           json::array({"drive_id", "path"})),
           entry_schema(),
           create_drive_folder_entry);
    action("create_drive_shortcut",
           "Create a shortcut",
           "Add a shortcut to a workspace-files or virtual drive. A repository, repository path, "
           "drive item or transcript target must exist now; the others are not checked.",
           workspace_input({{"drive_id", string_schema()},
                            {"parent_id", string_schema()},
                            {"name", string_schema()},
                            {"target", target_schema()}},
                           json::array({"drive_id", "name", "target"})),
           entry_schema(),
           create_drive_shortcut);
    action("move_drive_entry",
           "Move or rename a drive item",
           "Move an item to another folder of its drive (`parent_id`, empty for the top), rename "
           "it (`name`), or both. Only its record changes; a file's bytes are not copied.",
           workspace_input({{"drive_id", string_schema()},
                            {"id", string_schema()},
                            {"parent_id", string_schema()},
                            {"name", string_schema()}},
                           json::array({"drive_id", "id"})),
           entry_schema(),
           move_drive_entry);
    action("delete_drive_entry",
           "Delete a drive item",
           "Delete a file, a shortcut, or a folder with everything in it. Workspace files are not "
           "versioned: a deleted file cannot be restored.",
           workspace_input({{"drive_id", string_schema()}, {"id", string_schema()}},
                           json::array({"drive_id", "id"})),
           deleted,
           delete_drive_entry);
    action("begin_drive_upload",
           "Begin a file upload",
           "Start uploading one file into a workspace-files drive. `path` is relative to "
           "`parent_id` (or the top) and may name folders (\"Trip/Day 1/photo.jpg\"), made where "
           "missing. Send its bytes with append_drive_upload, then finish_drive_upload, each "
           "with `id` set to the answer's `entry.id`; the file is listed only once finished. An "
           "upload not finished within 24 hours expires and frees its name.",
           workspace_input({{"drive_id", string_schema()},
                            {"parent_id", string_schema()},
                            {"path", string_schema()},
                            {"size", {{"type", "integer"}, {"minimum", 0}}},
                            {"content_type", string_schema()},
                            {"on_conflict", on_conflict}},
                           json::array({"drive_id", "path", "size"})),
           object_of({{"entry", entry_schema()},
                      {"block_size", {{"type", "integer"}}},
                      {"max_chunk_bytes", {{"type", "integer"}}}},
                     json::array({"entry", "block_size", "max_chunk_bytes"})),
           begin_drive_upload);
    action("append_drive_upload",
           "Send a chunk of an upload",
           "Send the bytes of an upload from `offset`, at most 8 MiB per chunk. Every chunk but "
           "the last is a whole number of MiB (block_size). Sending a chunk again after a lost "
           "answer changes nothing. The answer's `received` is how many bytes the upload holds "
           "now and `complete` whether that is all of them, whichever chunk was sent.",
           workspace_input({{"drive_id", string_schema()},
                            {"id", string_schema()},
                            {"offset", {{"type", "integer"}, {"minimum", 0}}},
                            {"content",
                             {{"type", "string"},
                              {"contentEncoding", "base64"},
                              {"description", "the chunk's bytes as padded base64"}}}},
                           json::array({"drive_id", "id", "offset", "content"})),
           object_of({{"received", {{"type", "integer"}}},
                      {"complete", {{"type", "boolean"}}},
                      {"size", {{"type", "integer"}}}},
                     json::array({"received", "complete", "size"})),
           append_drive_upload);
    action(
        "finish_drive_upload",
        "Finish an upload",
        "Finish an upload once every byte is sent: Lake reads the file back for its SHA-256 "
        "(refusing it when `sha256` is sent and differs) and lists it. With on_conflict "
        "replace, the file it replaces is deleted now.",
        workspace_input(
            {{"drive_id", string_schema()}, {"id", string_schema()}, {"sha256", string_schema()}},
            json::array({"drive_id", "id"})),
        entry_schema(),
        finish_drive_upload);
    action("upload_drive_entries",
           "Upload small files",
           "Upload small files into a workspace-files or store drive in one call (at most 16 MiB "
           "in all), "
           "each at a relative `path` whose folders are made where missing (or a `name` in the "
           "folder: the same item shape as upload_drive_files).",
           workspace_input(
               {{"drive_id", string_schema()},
                {"parent_id", string_schema()},
                {"on_conflict", on_conflict},
                {"files",
                 {{"type", "array"},
                  {"items",
                   object_of({{"path", string_schema()},
                              {"name", string_schema()},
                              {"content", {{"type", "string"}, {"contentEncoding", "base64"}}},
                              {"content_type", string_schema()}},
                             json::array({"content"}))}}}},
               json::array({"drive_id", "files"})),
           object_of({{"items", {{"type", "array"}, {"items", entry_schema()}}}},
                     json::array({"items"})),
           upload_drive_entries);
    read("read_drive_file",
         "Read a Workspace File",
         "A workspace file's (or a store drive file's) bytes as base64: the whole file, or "
         "`length` bytes from `offset`; at most 16 MiB per call. Browsers download from GET "
         "/drive/files/download.",
         workspace_input({{"drive_id", string_schema()},
                          {"id", string_schema()},
                          {"offset", {{"type", "integer"}, {"minimum", 0}}},
                          {"length", {{"type", "integer"}, {"minimum", 0}}}},
                         json::array({"drive_id", "id"})),
         object_of(
             {{"id", string_schema()},
              {"name", string_schema()},
              {"size", {{"type", "integer"}}},
              {"content_type", string_schema()},
              {"sha256", string_schema()},
              {"offset", {{"type", "integer"}}},
              {"content_base64", {{"type", "string"}, {"contentEncoding", "base64"}}}},
             json::array(
                 {"id", "name", "size", "content_type", "sha256", "offset", "content_base64"})),
         read_drive_file);
    action("load_drive_dataset",
           "Load a drive file as a dataset",
           "Load a CSV or JSONL file of a drive into the workspace as a dataset, as "
           "upload_dataset does with a file sent from a browser. `id` is the file: its entry id "
           "in a workspace-files drive, its path in a repository drive (on main). `format` is csv "
           "or jsonl, from the file's name when not sent; `table_name` and `if_exists` are "
           "upload_dataset's. At most 16 MiB.",
           workspace_input({{"drive_id", string_schema()},
                            {"id", string_schema()},
                            {"table_name", string_schema()},
                            {"format", {{"type", "string"}, {"enum", {"csv", "jsonl"}}}},
                            {"if_exists", {{"type", "string"}, {"enum", {"fail", "rename"}}}}},
                           json::array({"drive_id", "id", "table_name"})),
           {{"type", "object"}},
           load_drive_dataset);
    action("load_drive_model",
           "Bring a drive file as a model",
           "Bring a model file of a drive into the workspace as a standalone model, scored, "
           "served and compared like a trained one (the engine's register_model). `id` is the "
           "file: its entry id in a workspace-files drive, its path in a repository drive (on "
           "main). `name` names a new model; `version_of` (a brought model's id) adds the file as "
           "its next version. The engine reads tree models as ONNX (TreeEnsembleRegressor or "
           "TreeEnsembleClassifier, from scikit-learn, XGBoost or LightGBM), XGBoost's JSON or "
           "UBJSON model, LightGBM's text model and its own JSON weights, converted exactly; "
           "anything else is refused. `format` skips detection; `feature_names` names the "
           "columns, in order (ONNX names none). At most 16 MiB.",
           workspace_input({{"drive_id", string_schema()},
                            {"id", string_schema()},
                            {"name", string_schema()},
                            {"version_of", string_schema()},
                            {"kind", {{"type", "string"}, {"enum", {"classifier", "regressor"}}}},
                            {"metrics",
                             {{"type", "object"}, {"additionalProperties", {{"type", "number"}}}}},
                            {"format",
                             {{"type", "string"},
                              {"enum",
                               {"zetta", "onnx", "xgboost_json", "xgboost_ubjson", "lightgbm"}}}},
                            {"feature_names",
                             {{"type", "array"},
                              {"items", string_schema()},
                              {"minItems", 1}}}},
                           json::array({"drive_id", "id"})),
           {{"type", "object"}},
           load_drive_model);
    read("recent_drive_entries",
         "Recent Files",
         "The workspace files changed most recently across the workspace's workspace-files "
         "drives, newest first.",
         workspace_input({{"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}}},
                         json::array()),
         object_of({{"items",
                     {{"type", "array"},
                      {"items",
                       object_of({{"drive",
                                   object_of({{"id", string_schema()}, {"name", string_schema()}},
                                             json::array({"id", "name"}))},
                                  {"entry", entry_schema()}},
                                 json::array({"drive", "entry"}))}}}},
                   json::array({"items"})),
         recent_drive_entries,
         true);
    const json drive_ref = object_of(
        {{"id", string_schema()},
         {"name", string_schema()},
         {"storage", {{"type", "string"}, {"enum", {"repository", "files", "virtual", "store"}}}},
         {"repository_id", {{"type", json::array({"string", "null"})}}},
         {"store_id", {{"type", json::array({"string", "null"})}}}},
        json::array({"id", "name", "storage", "repository_id", "store_id"}));
    const json nullable_string{{"type", json::array({"string", "null"})}};
    read(
        "search_drives",
        "Search Drives",
        "Find drives, folders, files and shortcuts whose name contains `q` (A to Z in either "
        "case) across the workspace's drives, or one `drive_id`: workspace-files and virtual "
        "drives by their items' names, repository drives by the names of the paths at `main`, "
        "store drives by the names in the store (at most 10,000 items each). "
        "Each match names its drive and folder path; drives first, then by drive and path.",
        workspace_input({{"q", {{"type", "string"}, {"minLength", 1}, {"maxLength", 255}}},
                         {"drive_id", string_schema()},
                         {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}}}},
                        json::array({"q"})),
        object_of(
            {{"items",
              {{"type", "array"},
               {"items",
                object_of(
                    {{"drive", drive_ref},
                     {"kind",
                      {{"type", "string"}, {"enum", {"drive", "folder", "file", "shortcut"}}}},
                     {"id", nullable_string},
                     {"parent_id", nullable_string},
                     {"name", string_schema()},
                     {"path", string_schema()},
                     {"folder", string_schema()},
                     {"size", {{"type", "integer"}, {"minimum", 0}}},
                     {"content_type", string_schema()},
                     {"modified_at", string_schema()},
                     {"target", {{"type", "object"}}},
                     {"status", {{"type", "string"}, {"enum", {"ok", "broken", "unknown"}}}},
                     {"reason", string_schema()}},
                    json::array({"drive", "kind", "id", "parent_id", "name", "path", "folder"}))}}},
             {"truncated", {{"type", "boolean"}}},
             {"skipped",
              {{"type", "array"},
               {"items",
                object_of({{"drive", drive_ref}, {"reason", string_schema()}},
                          json::array({"drive", "reason"}))}}}},
            json::array({"items", "truncated", "skipped"})),
        search_drives,
        true);
}

liquid::runtime::task<control::ZettaControlResult<WorkspaceFile>>
find_workspace_file(ApiContext& context,
                    std::string customer_id,
                    std::string workspace_id,
                    std::string drive_id,
                    std::string id) {
    using Result = control::ZettaControlResult<WorkspaceFile>;
    auto records = workspace_records(context, customer_id, workspace_id);
    if (!records) {
        co_return Result(std::unexpected(records.error()));
    }
    // A store drive's file is read from its store (lake#656).
    auto drive = control::settled(co_await control::get_drive(*records, customer_id, drive_id));
    if (!drive) {
        co_return Result(std::unexpected(drive.error()));
    }
    if (*drive && (*drive)->storage == "store") {
        co_return control::settled(co_await find_store_file(context,
                                                            std::move(customer_id),
                                                            std::move(workspace_id),
                                                            std::move(drive_id),
                                                            std::move(id)));
    }
    auto found =
        control::settled(co_await control::get_drive_entry(*records, customer_id, drive_id, id));
    if (!found) {
        co_return Result(std::unexpected(found.error()));
    }
    if (!*found || (*found)->kind != "file" || (*found)->state != "ready") {
        co_return Result(std::unexpected(
            control::ZettaControlError{"not_found", "no such file in this drive", "/id"}));
    }
    const control::DriveEntry& entry = **found;
    co_return Result(WorkspaceFile{
        entry.id, entry.name, entry.content_type, entry.sha256, entry.object_key, entry.size});
}

liquid::runtime::task<control::ZettaControlResult<WorkspaceFile>>
find_store_file(ApiContext& context,
                std::string customer_id,
                std::string workspace_id,
                std::string drive_id,
                std::string id) {
    using Result = control::ZettaControlResult<WorkspaceFile>;
    auto records = workspace_records(context, customer_id, workspace_id);
    if (!records) {
        co_return Result(std::unexpected(records.error()));
    }
    auto drive = control::settled(co_await control::get_drive(*records, customer_id, drive_id));
    if (!drive) {
        co_return Result(std::unexpected(drive.error()));
    }
    if (!*drive || (*drive)->storage != "store") {
        co_return Result(std::unexpected(
            control::ZettaControlError{"not_found", "no such store drive", "/drive_id"}));
    }
    StoreOpened store = store_of(context, **drive, {{"workspace_id", workspace_id}});
    if (store.failure) {
        co_return Result(std::unexpected(
            control::ZettaControlError{store.failure->error.code, store.failure->error.message}));
    }
    auto item = control::settled(co_await store.contents->stat(id));
    if (!item) {
        co_return Result(std::unexpected(item.error()));
    }
    if (!*item || (*item)->folder) {
        co_return Result(std::unexpected(
            control::ZettaControlError{"not_found", "no such file in this drive", "/id"}));
    }
    WorkspaceFile out{id,
                      control::store_name(id),
                      (*item)->content_type.empty()
                          ? control::drive_media_type(control::store_name(id))
                          : (*item)->content_type,
                      "",
                      id,
                      (*item)->size};
    out.store_drive_id = drive_id;
    out.etag = (*item)->etag;
    co_return Result(std::move(out));
}

// The plaintext of a workspace file window, for GET /drive/files/download (http_server.cpp).
liquid::runtime::task<control::ZettaControlResult<std::string>>
read_workspace_file(ApiContext& context,
                    std::string customer_id,
                    std::string workspace_id,
                    std::string object_key,
                    std::uint64_t size,
                    std::uint64_t offset,
                    std::uint64_t length,
                    std::string store_drive_id) {
    if (!store_drive_id.empty()) {
        // A store drive's file: `object_key` is its path in the store.
        auto records = workspace_records(context, customer_id, workspace_id);
        if (!records) {
            co_return control::ZettaControlResult<std::string>(std::unexpected(records.error()));
        }
        auto drive =
            control::settled(co_await control::get_drive(*records, customer_id, store_drive_id));
        if (!drive || !*drive) {
            co_return control::ZettaControlResult<std::string>(std::unexpected(
                drive ? control::ZettaControlError{"not_found", "no such store drive"}
                      : drive.error()));
        }
        StoreOpened store = store_of(context, **drive, {{"workspace_id", workspace_id}});
        if (store.failure) {
            co_return control::ZettaControlResult<std::string>(
                std::unexpected(control::ZettaControlError{store.failure->error.code,
                                                           store.failure->error.message}));
        }
        (void)size;
        co_return control::settled(
            co_await store.contents->read(std::move(object_key), offset, length));
    }
    Files files = file_store(context, customer_id, workspace_id);
    if (files.failure) {
        co_return control::ZettaControlResult<std::string>(std::unexpected(
            control::ZettaControlError{files.failure->error.code, files.failure->error.message}));
    }
    co_return control::settled(
        co_await files.store->read(std::move(object_key), size, offset, length));
}

} // namespace lake::api
