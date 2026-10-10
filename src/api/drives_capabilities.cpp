// Drives (docs/api/drives.md, lake#561): a workspace's drives, each a sealed `lake_drives` record
// in its workspace Zetta that keeps its files in a repository, as workspace files, or as shortcuts
// only (virtual). Workspace files are `lake_drive_entries` records and, for each file, one sealed
// object in the workspace Zetta's `lake_files` bucket (drive_files.hpp). A repository drive is
// browsed and written with the drive_* capabilities of drive_capabilities.cpp.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <openssl/evp.h>

#include "lake/api/capabilities.hpp"
#include "lake/control/drive_files.hpp"
#include "lake/control/drives.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/store.hpp"
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
    static const std::map<std::string, std::string, std::less<>> types{
        {"txt", "text/plain"},
        {"md", "text/markdown"},
        {"csv", "text/csv"},
        {"tsv", "text/tab-separated-values"},
        {"json", "application/json"},
        {"pdf", "application/pdf"},
        {"png", "image/png"},
        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},
        {"webp", "image/webp"},
        {"svg", "image/svg+xml"},
        {"mp3", "audio/mpeg"},
        {"m4a", "audio/mp4"},
        {"wav", "audio/wav"},
        {"mp4", "video/mp4"},
        {"webm", "video/webm"},
        {"zip", "application/zip"},
        {"html", "text/html"},
        {"xml", "application/xml"},
        {"parquet", "application/vnd.apache.parquet"},
        {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"}};
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos) {
        return "application/octet-stream";
    }
    std::string extension(name.substr(dot + 1));
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const auto found = types.find(extension);
    return found == types.end() ? std::string("application/octet-stream") : found->second;
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
        {"created_by", drive.created_by},
        {"created_at", drive.created_at}};
    return out;
}

json drive_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"name", {{"type", "string"}}},
              {"storage", {{"type", "string"}, {"enum", {"repository", "files", "virtual"}}}},
              {"repository_id", {{"type", json::array({"string", "null"})}}},
              {"created_by", {{"type", "string"}}},
              {"created_at", {{"type", "string"}}}}},
            {"required",
             json::array({"id", "name", "storage", "repository_id", "created_by", "created_at"})},
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
                                   "this lake keeps no workspace files: it has no workspace Zetta "
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
    if (opened.drive.storage != "repository") {
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
    context.audit_details["folder_id"] = folder_id;
    co_return ApiResult::success(
        {{"drive", drive_json(opened.drive)},
         {"folder_id", folder_id.empty() ? json(nullptr) : json(folder_id)},
         {"path", base},
         {"ancestors", std::move(ancestors)},
         {"items", std::move(items)}});
}

liquid::runtime::task<ApiResult> get_drive_entry(const json& input, ApiContext& context) {
    Opened opened = awaited(co_await open_drive(input, context, true));
    if (opened.failure) {
        co_return *opened.failure;
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
    co_return ApiResult::success(
        {{"received", offset + content->size()}, {"complete", last}, {"size", entry.size}});
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
    std::uint64_t total = 0;
    for (std::size_t index = 0; index < list.size(); ++index) {
        const std::string field = "/files/" + std::to_string(index);
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
                                                  parent_id,
                                                  list.at(index).at("path"),
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
    co_return ApiResult::success({{"items", std::move(items)}});
}

liquid::runtime::task<ApiResult> read_drive_file(const json& input, ApiContext& context) {
    Opened opened = awaited(co_await open_drive(input, context, true));
    if (opened.failure) {
        co_return *opened.failure;
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
                         {"repository_id", string_schema()}}},
                       {"required", json::array({"deleted", "entries_removed"})},
                       {"additionalProperties", false}};
    const json on_conflict{{"type", "string"},
                           {"enum", {"rename", "replace", "refuse"}},
                           {"description",
                            "a file of the same name in the folder: rename (default) gives the new "
                            "file the first free name, replace replaces it once the upload "
                            "finishes, refuse answers conflict"}};

    action("create_drive",
           "Create a drive",
           "Create a drive in a workspace. `storage` is repository (a liquid repository with "
           "history: an existing `repository_id` of this workspace, or a new repository named "
           "`repository_name`, which gets its first change at once), files (workspace files in the "
           "workspace Zetta, moved and renamed without copying) or virtual (folders of shortcuts).",
           workspace_input(
               {{"name", string_schema()},
                {"storage", {{"type", "string"}, {"enum", {"repository", "files", "virtual"}}}},
                {"repository_id", string_schema()},
                {"repository_name", string_schema()}},
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
           "repository is never deleted: delete it from the repository's own page.",
           workspace_input({{"id", string_schema()}, {"confirm_name", string_schema()}},
                           json::array({"id", "confirm_name"})),
           deleted,
           delete_drive);
    read("list_drive_entries",
         "List a Drive Folder",
         "The folders, files and shortcuts in one folder of a workspace-files or virtual drive: "
         "`folder_id`, or a slash `path` from the top; neither is the top. Each shortcut carries "
         "`status`: ok, broken (with a reason) or unknown when lake cannot check its kind.",
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
                    {"items", {{"type", "array"}, {"items", entry_schema()}}}},
                   json::array({"drive", "folder_id", "path", "ancestors", "items"})),
         list_drive_entries,
         true);
    read("get_drive_entry",
         "Read a Drive Item",
         "One folder, file or shortcut of a workspace-files or virtual drive, with its path.",
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
           "missing. Send its bytes with append_drive_upload, then finish_drive_upload; the file "
           "is listed only once finished.",
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
           "answer changes nothing.",
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
        "Finish an upload once every byte is sent: lake reads the file back for its SHA-256 "
        "(refusing it when `sha256` is sent and differs) and lists it. With on_conflict "
        "replace, the file it replaces is deleted now.",
        workspace_input(
            {{"drive_id", string_schema()}, {"id", string_schema()}, {"sha256", string_schema()}},
            json::array({"drive_id", "id"})),
        entry_schema(),
        finish_drive_upload);
    action("upload_drive_entries",
           "Upload small files",
           "Upload small files into a workspace-files drive in one call (at most 16 MiB in all), "
           "each at a relative `path` whose folders are made where missing.",
           workspace_input(
               {{"drive_id", string_schema()},
                {"parent_id", string_schema()},
                {"on_conflict", on_conflict},
                {"files",
                 {{"type", "array"},
                  {"items",
                   object_of({{"path", string_schema()},
                              {"content", {{"type", "string"}, {"contentEncoding", "base64"}}},
                              {"content_type", string_schema()}},
                             json::array({"path", "content"}))}}}},
               json::array({"drive_id", "files"})),
           object_of({{"items", {{"type", "array"}, {"items", entry_schema()}}}},
                     json::array({"items"})),
           upload_drive_entries);
    read("read_drive_file",
         "Read a Workspace File",
         "A workspace file's bytes as base64: the whole file, or `length` bytes from `offset`; at "
         "most 16 MiB per call. Browsers download from GET /drive/files/download.",
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

// The plaintext of a workspace file window, for GET /drive/files/download (http_server.cpp).
liquid::runtime::task<control::ZettaControlResult<std::string>>
read_workspace_file(ApiContext& context,
                    std::string customer_id,
                    std::string workspace_id,
                    std::string object_key,
                    std::uint64_t size,
                    std::uint64_t offset,
                    std::uint64_t length) {
    Files files = file_store(context, customer_id, workspace_id);
    if (files.failure) {
        co_return control::ZettaControlResult<std::string>(std::unexpected(
            control::ZettaControlError{files.failure->error.code, files.failure->error.message}));
    }
    co_return control::settled(
        co_await files.store->read(std::move(object_key), size, offset, length));
}

} // namespace lake::api
