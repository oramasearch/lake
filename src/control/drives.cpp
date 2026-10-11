#include "lake/control/drives.hpp"

#include "lake/control/store.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <map>
#include <utility>

namespace lake::control {
namespace {

template <class T> ZettaControlResult<T> refused(std::string code, std::string message) {
    return std::unexpected(ZettaControlError{std::move(code), std::move(message)});
}

constexpr int write_attempts = 5;

std::string text(const nlohmann::json& body, const char* key) {
    const auto found = body.find(key);
    return found != body.end() && found->is_string() ? found->get<std::string>() : std::string();
}

std::int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool known_entry_kind(std::string_view kind) {
    return kind == "folder" || kind == "file" || kind == "shortcut";
}

bool required_strings(const nlohmann::json& target, std::initializer_list<const char*> keys) {
    return std::ranges::all_of(keys, [&](const char* key) {
        const auto found = target.find(key);
        return found != target.end() && found->is_string() && !found->get<std::string>().empty() &&
               found->get<std::string>().size() <= 2048;
    });
}

// Reads a drive of the space, or why not (`not_found` for another space's or a missing one).
struct ReadDrive {
    Drive drive;
    std::int64_t revision = 0;
};

liquid::runtime::task<ZettaControlResult<ReadDrive>>
read_drive(WorkspaceRecords& records, const std::string& customer_id, const std::string& id) {
    auto got = settled(co_await records.get(DriveCodec::collection, id));
    if (!got && got.error().code != "invalid_input") {
        co_return ZettaControlResult<ReadDrive>(std::unexpected(got.error()));
    }
    std::optional<Drive> drive;
    std::int64_t revision = 0;
    if (got && *got) {
        drive = DriveCodec::decode(**got);
        revision = (*got)->revision;
    }
    if (!drive || drive->customer_id != customer_id) {
        co_return refused<ReadDrive>("not_found", "no such drive in this workspace");
    }
    co_return ZettaControlResult<ReadDrive>(ReadDrive{std::move(*drive), revision});
}

struct ReadEntry {
    DriveEntry entry;
    std::int64_t revision = 0;
};

liquid::runtime::task<ZettaControlResult<ReadEntry>> read_entry(WorkspaceRecords& records,
                                                                const std::string& customer_id,
                                                                const std::string& drive_id,
                                                                const std::string& id) {
    auto got = settled(co_await records.get(DriveEntryCodec::collection, id));
    if (!got && got.error().code != "invalid_input") {
        co_return ZettaControlResult<ReadEntry>(std::unexpected(got.error()));
    }
    std::optional<DriveEntry> entry;
    std::int64_t revision = 0;
    if (got && *got) {
        entry = DriveEntryCodec::decode(**got);
        revision = (*got)->revision;
    }
    if (!entry || entry->customer_id != customer_id || entry->drive_id != drive_id) {
        co_return refused<ReadEntry>("not_found", "no such item in this drive");
    }
    co_return ZettaControlResult<ReadEntry>(ReadEntry{std::move(*entry), revision});
}

} // namespace

std::string drives_lookup(const std::string& customer_id) {
    return "customer:" + customer_id;
}

std::string drive_entries_lookup(const std::string& drive_id) {
    return "drive:" + drive_id;
}

bool known_drive_storage(std::string_view storage) {
    return storage == "repository" || storage == "files" || storage == "virtual" ||
           storage == "store";
}

bool valid_drive_name(std::string_view name) {
    if (name.empty() || name.size() > max_drive_name_bytes || name == "." || name == ".." ||
        name == ".lake-folder") {
        return false;
    }
    return std::ranges::none_of(name, [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return c == '/' || c == '\\' || byte < 0x20 || byte == 0x7f;
    });
}

std::optional<std::string> drive_problem(const Drive& drive) {
    if (drive.customer_id.empty() || !valid_drive_name(drive.name) ||
        !known_drive_storage(drive.storage) ||
        (drive.storage == "repository") == drive.repository_id.empty() ||
        (drive.storage == "store") == drive.store_id.empty()) {
        return "drive is invalid";
    }
    return std::nullopt;
}

std::string DriveCodec::lookup(const Drive& drive) {
    return drives_lookup(drive.customer_id);
}

nlohmann::json DriveCodec::encode(const Drive& drive) {
    nlohmann::json body{{"customer_id", drive.customer_id},
                        {"name", drive.name},
                        {"storage", drive.storage},
                        {"created_by", drive.created_by}};
    if (!drive.repository_id.empty()) {
        body["repository_id"] = drive.repository_id;
    }
    if (!drive.store_id.empty()) {
        body["store_id"] = drive.store_id;
    }
    return body;
}

std::optional<Drive> DriveCodec::decode(const StoredRecord& record) {
    // Version 1 is the only body schema so far; a later version decodes its predecessors here.
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    Drive drive;
    drive.id = record.id;
    drive.customer_id = text(record.body, "customer_id");
    drive.name = text(record.body, "name");
    drive.storage = text(record.body, "storage");
    drive.repository_id = text(record.body, "repository_id");
    drive.store_id = text(record.body, "store_id");
    drive.created_by = text(record.body, "created_by");
    drive.created_at = record_instant(record.at_us);
    drive.at_us = record.at_us;
    drive.lake_revision = record.lake_revision;
    if (drive_problem(drive)) {
        return std::nullopt;
    }
    return drive;
}

std::optional<std::string> drive_shortcut_problem(const nlohmann::json& target) {
    if (!target.is_object()) {
        return "a shortcut's target is an object with its kind";
    }
    const std::string kind = text(target, "kind");
    bool complete = false;
    if (kind == "repository") {
        complete = required_strings(target, {"repository_id"});
    } else if (kind == "repository_path") {
        complete = required_strings(target, {"repository_id", "path"});
    } else if (kind == "drive_entry") {
        complete = required_strings(target, {"drive_id", "entry_id"});
    } else if (kind == "dataset" || kind == "transcript" || kind == "agent" || kind == "model") {
        complete = required_strings(target, {"id"});
    } else if (kind == "project") {
        complete = required_strings(target, {"name"});
    } else if (kind == "url") {
        const std::string url = text(target, "url");
        complete =
            required_strings(target, {"url"}) &&
            (url.starts_with("https://") || url.starts_with("http://")) &&
            std::ranges::none_of(url, [](char c) {
                return static_cast<unsigned char>(c) <= 0x20 || c == '"' || c == '<' || c == '>';
            });
    } else {
        return "a shortcut points at a repository, a repository path, a drive item, a dataset, a "
               "transcript, a project, an agent, a model or a web address";
    }
    if (!complete) {
        return "the shortcut's target is missing a field its kind needs";
    }
    return std::nullopt;
}

std::optional<std::string> drive_entry_problem(const DriveEntry& entry) {
    if (entry.customer_id.empty() || entry.drive_id.empty() || !known_entry_kind(entry.kind) ||
        !valid_drive_name(entry.name)) {
        return "drive item is invalid";
    }
    if (entry.kind == "file" &&
        (entry.object_key.empty() || (entry.state != "uploading" && entry.state != "ready"))) {
        return "drive file is invalid";
    }
    if (entry.kind == "shortcut") {
        return drive_shortcut_problem(entry.target);
    }
    return std::nullopt;
}

std::string DriveEntryCodec::lookup(const DriveEntry& entry) {
    return drive_entries_lookup(entry.drive_id);
}

nlohmann::json DriveEntryCodec::encode(const DriveEntry& entry) {
    nlohmann::json body{{"customer_id", entry.customer_id},
                        {"drive_id", entry.drive_id},
                        {"parent_id", entry.parent_id},
                        {"kind", entry.kind},
                        {"name", entry.name},
                        {"created_by", entry.created_by},
                        {"modified_by", entry.modified_by},
                        {"modified_us", entry.modified_us}};
    if (entry.kind == "file") {
        body["size"] = entry.size;
        body["content_type"] = entry.content_type;
        body["sha256"] = entry.sha256;
        body["object_key"] = entry.object_key;
        body["state"] = entry.state;
        if (!entry.replaces.empty()) {
            body["replaces"] = entry.replaces;
        }
    }
    if (entry.kind == "shortcut") {
        body["target"] = entry.target;
    }
    return body;
}

std::optional<DriveEntry> DriveEntryCodec::decode(const StoredRecord& record) {
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    const nlohmann::json& body = record.body;
    DriveEntry entry;
    entry.id = record.id;
    entry.customer_id = text(body, "customer_id");
    entry.drive_id = text(body, "drive_id");
    entry.parent_id = text(body, "parent_id");
    entry.kind = text(body, "kind");
    entry.name = text(body, "name");
    if (body.contains("size") && body.at("size").is_number_unsigned()) {
        entry.size = body.at("size").get<std::uint64_t>();
    }
    entry.content_type = text(body, "content_type");
    entry.sha256 = text(body, "sha256");
    entry.object_key = text(body, "object_key");
    entry.state = entry.kind == "file" ? text(body, "state") : std::string("ready");
    entry.replaces = text(body, "replaces");
    entry.target = body.value("target", nlohmann::json(nullptr));
    entry.created_by = text(body, "created_by");
    entry.modified_by = text(body, "modified_by");
    entry.modified_us = body.value("modified_us", std::int64_t{0});
    if (entry.modified_us == 0) {
        entry.modified_us = record.at_us;
    }
    entry.created_at = record_instant(record.at_us);
    entry.at_us = record.at_us;
    entry.lake_revision = record.lake_revision;
    if (drive_entry_problem(entry)) {
        return std::nullopt;
    }
    return entry;
}

liquid::runtime::task<ZettaControlResult<std::vector<Drive>>> list_drives(WorkspaceRecords& records,
                                                                          std::string customer_id) {
    Drives set(records);
    auto listed = settled(co_await set.list(drives_lookup(customer_id)));
    if (!listed) {
        co_return ZettaControlResult<std::vector<Drive>>(std::unexpected(listed.error()));
    }
    std::erase_if(*listed, [&](const Drive& drive) { return drive.customer_id != customer_id; });
    std::ranges::sort(*listed, [](const Drive& a, const Drive& b) {
        return a.name != b.name ? a.name < b.name : a.id < b.id;
    });
    co_return ZettaControlResult<std::vector<Drive>>(std::move(*listed));
}

liquid::runtime::task<ZettaControlResult<std::optional<Drive>>>
get_drive(WorkspaceRecords& records, std::string customer_id, std::string id) {
    using Found = std::optional<Drive>;
    auto read = settled(co_await read_drive(records, customer_id, id));
    if (!read) {
        if (read.error().code == "not_found") {
            co_return ZettaControlResult<Found>(Found{});
        }
        co_return ZettaControlResult<Found>(std::unexpected(read.error()));
    }
    co_return ZettaControlResult<Found>(std::move(read->drive));
}

liquid::runtime::task<ZettaControlResult<Drive>> record_drive(WorkspaceRecords& records,
                                                              Drive drive) {
    if (auto problem = drive_problem(drive)) {
        co_return refused<Drive>("invalid_input", *problem);
    }
    Drives set(records);
    co_return settled(co_await set.append(std::move(drive)));
}

liquid::runtime::task<ZettaControlResult<Drive>>
rename_drive(WorkspaceRecords& records, std::string customer_id, std::string id, std::string name) {
    ZettaControlError last{"conflict", "the drive kept changing"};
    for (int attempt = 0; attempt < write_attempts; ++attempt) {
        auto read = settled(co_await read_drive(records, customer_id, id));
        if (!read) {
            co_return ZettaControlResult<Drive>(std::unexpected(read.error()));
        }
        Drive next = read->drive;
        next.name = name;
        if (auto problem = drive_problem(next)) {
            co_return refused<Drive>("invalid_input", *problem);
        }
        auto written = settled(co_await records.put(DriveCodec::collection,
                                                    id,
                                                    DriveCodec::lookup(next),
                                                    DriveCodec::encode(next),
                                                    RecordCondition{.if_revision = read->revision},
                                                    next.lake_revision + 1,
                                                    next.at_us));
        if (written) {
            auto decoded = DriveCodec::decode(*written);
            if (!decoded) {
                co_return refused<Drive>("store_error", "the stored record did not decode");
            }
            co_return ZettaControlResult<Drive>(std::move(*decoded));
        }
        if (written.error().code != "conflict") {
            co_return ZettaControlResult<Drive>(std::unexpected(written.error()));
        }
        last = written.error();
    }
    co_return ZettaControlResult<Drive>(std::unexpected(last));
}

liquid::runtime::task<ZettaControlResult<void>>
remove_drive(WorkspaceRecords& records, std::string customer_id, std::string id) {
    ZettaControlError last{"conflict", "the drive kept changing"};
    for (int attempt = 0; attempt < write_attempts; ++attempt) {
        auto read = settled(co_await read_drive(records, customer_id, id));
        if (!read) {
            co_return ZettaControlResult<void>(std::unexpected(read.error()));
        }
        auto removed = settled(co_await records.remove(DriveCodec::collection, id, read->revision));
        if (removed || removed.error().code == "not_found") {
            co_return ZettaControlResult<void>();
        }
        if (removed.error().code != "conflict") {
            co_return ZettaControlResult<void>(std::unexpected(removed.error()));
        }
        last = removed.error();
    }
    co_return ZettaControlResult<void>(std::unexpected(last));
}

liquid::runtime::task<ZettaControlResult<std::vector<DriveEntry>>>
list_drive_entries(WorkspaceRecords& records, std::string customer_id, std::string drive_id) {
    DriveEntries set(records);
    auto listed = settled(co_await set.list(drive_entries_lookup(drive_id)));
    if (!listed) {
        co_return ZettaControlResult<std::vector<DriveEntry>>(std::unexpected(listed.error()));
    }
    std::erase_if(*listed, [&](const DriveEntry& entry) {
        return entry.customer_id != customer_id || entry.drive_id != drive_id;
    });
    co_return ZettaControlResult<std::vector<DriveEntry>>(std::move(*listed));
}

liquid::runtime::task<ZettaControlResult<std::optional<DriveEntry>>> get_drive_entry(
    WorkspaceRecords& records, std::string customer_id, std::string drive_id, std::string id) {
    using Found = std::optional<DriveEntry>;
    auto read = settled(co_await read_entry(records, customer_id, drive_id, id));
    if (!read) {
        if (read.error().code == "not_found") {
            co_return ZettaControlResult<Found>(Found{});
        }
        co_return ZettaControlResult<Found>(std::unexpected(read.error()));
    }
    co_return ZettaControlResult<Found>(std::move(read->entry));
}

liquid::runtime::task<ZettaControlResult<DriveEntry>> record_drive_entry(WorkspaceRecords& records,
                                                                         DriveEntry entry) {
    if (entry.modified_us == 0) {
        entry.modified_us = now_us();
    }
    if (entry.modified_by.empty()) {
        entry.modified_by = entry.created_by;
    }
    if (auto problem = drive_entry_problem(entry)) {
        co_return refused<DriveEntry>("invalid_input", *problem);
    }
    RecordWrite write{entry.id, 0};
    DriveEntries set(records);
    co_return settled(co_await set.append(std::move(entry), std::move(write)));
}

liquid::runtime::task<ZettaControlResult<DriveEntry>> change_drive_entry(WorkspaceRecords& records,
                                                                         std::string customer_id,
                                                                         std::string drive_id,
                                                                         std::string id,
                                                                         DriveEntryChange change) {
    ZettaControlError last{"conflict", "the item kept changing"};
    for (int attempt = 0; attempt < write_attempts; ++attempt) {
        auto read = settled(co_await read_entry(records, customer_id, drive_id, id));
        if (!read) {
            co_return ZettaControlResult<DriveEntry>(std::unexpected(read.error()));
        }
        DriveEntry next = read->entry;
        if (auto refusal = change(next)) {
            co_return ZettaControlResult<DriveEntry>(std::unexpected(std::move(*refusal)));
        }
        // Its space, drive and kind never change, so the write keeps its lookup.
        next.customer_id = read->entry.customer_id;
        next.drive_id = read->entry.drive_id;
        next.kind = read->entry.kind;
        if (auto problem = drive_entry_problem(next)) {
            co_return refused<DriveEntry>("invalid_input", *problem);
        }
        auto written = settled(co_await records.put(DriveEntryCodec::collection,
                                                    id,
                                                    DriveEntryCodec::lookup(next),
                                                    DriveEntryCodec::encode(next),
                                                    RecordCondition{.if_revision = read->revision},
                                                    next.lake_revision + 1,
                                                    next.at_us));
        if (written) {
            auto decoded = DriveEntryCodec::decode(*written);
            if (!decoded) {
                co_return refused<DriveEntry>("store_error", "the stored record did not decode");
            }
            co_return ZettaControlResult<DriveEntry>(std::move(*decoded));
        }
        if (written.error().code != "conflict") {
            co_return ZettaControlResult<DriveEntry>(std::unexpected(written.error()));
        }
        last = written.error();
    }
    co_return ZettaControlResult<DriveEntry>(std::unexpected(last));
}

liquid::runtime::task<ZettaControlResult<void>> remove_drive_entry(WorkspaceRecords& records,
                                                                   std::string customer_id,
                                                                   std::string drive_id,
                                                                   std::string id) {
    ZettaControlError last{"conflict", "the item kept changing"};
    for (int attempt = 0; attempt < write_attempts; ++attempt) {
        auto read = settled(co_await read_entry(records, customer_id, drive_id, id));
        if (!read) {
            if (read.error().code == "not_found") {
                co_return ZettaControlResult<void>();
            }
            co_return ZettaControlResult<void>(std::unexpected(read.error()));
        }
        auto removed =
            settled(co_await records.remove(DriveEntryCodec::collection, id, read->revision));
        if (removed || removed.error().code == "not_found") {
            co_return ZettaControlResult<void>();
        }
        if (removed.error().code != "conflict") {
            co_return ZettaControlResult<void>(std::unexpected(removed.error()));
        }
        last = removed.error();
    }
    co_return ZettaControlResult<void>(std::unexpected(last));
}

DriveTree::DriveTree(std::vector<DriveEntry> entries) : entries_(std::move(entries)) {}

void DriveTree::add(DriveEntry entry) {
    entries_.push_back(std::move(entry));
}

void DriveTree::remove(std::string_view id) {
    std::erase_if(entries_, [&](const DriveEntry& entry) { return entry.id == id; });
}

std::vector<const DriveEntry*> DriveTree::uploads(std::string_view parent_id) const {
    std::vector<const DriveEntry*> out;
    for (const DriveEntry& entry : entries_) {
        if (entry.parent_id == parent_id && entry.kind == "file" && entry.state == "uploading") {
            out.push_back(&entry);
        }
    }
    return out;
}

std::string drive_media_type(std::string_view name) {
    static const std::map<std::string, std::string, std::less<>> types{
        {"txt", "text/plain"},
        {"md", "text/markdown"},
        {"csv", "text/csv"},
        {"tsv", "text/tab-separated-values"},
        {"json", "application/json"},
        {"jsonl", "application/x-ndjson"},
        {"ndjson", "application/x-ndjson"},
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

bool drive_upload_expired(const DriveEntry& entry, std::int64_t now_us) {
    return entry.kind == "file" && entry.state == "uploading" &&
           now_us - entry.modified_us >= drive_upload_expiry_us;
}

const DriveEntry* DriveTree::find(std::string_view id) const {
    if (id.empty()) {
        return nullptr;
    }
    const auto found = std::ranges::find(entries_, id, &DriveEntry::id);
    return found == entries_.end() ? nullptr : &*found;
}

std::vector<const DriveEntry*> DriveTree::children(std::string_view parent_id) const {
    std::vector<const DriveEntry*> out;
    for (const DriveEntry& entry : entries_) {
        if (entry.parent_id == parent_id && entry.state == "ready") {
            out.push_back(&entry);
        }
    }
    return out;
}

const DriveEntry* DriveTree::child(std::string_view parent_id, std::string_view name) const {
    for (const DriveEntry& entry : entries_) {
        if (entry.parent_id == parent_id && entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

std::vector<const DriveEntry*> DriveTree::ancestors(std::string_view id) const {
    std::vector<const DriveEntry*> chain;
    const DriveEntry* current = find(id);
    while (current != nullptr) {
        if (chain.size() > entries_.size()) {
            return {}; // a cycle: never written, but never followed forever
        }
        chain.push_back(current);
        if (current->parent_id.empty()) {
            std::ranges::reverse(chain);
            return chain;
        }
        current = find(current->parent_id);
    }
    return {};
}

std::string DriveTree::path(std::string_view id) const {
    std::string out;
    for (const DriveEntry* entry : ancestors(id)) {
        if (!out.empty()) {
            out += '/';
        }
        out += entry->name;
    }
    return out;
}

bool DriveTree::within(std::string_view id, std::string_view folder_id) const {
    for (const DriveEntry* entry : ancestors(id)) {
        if (entry->id == folder_id) {
            return true;
        }
    }
    return false;
}

std::vector<const DriveEntry*> DriveTree::subtree(std::string_view id) const {
    std::vector<const DriveEntry*> out;
    const DriveEntry* root = find(id);
    if (root == nullptr) {
        return out;
    }
    // Breadth first from the root, then reversed: every child before its parent.
    out.push_back(root);
    for (std::size_t index = 0; index < out.size(); ++index) {
        for (const DriveEntry& entry : entries_) {
            if (entry.parent_id == out[index]->id && std::ranges::find(out, &entry) == out.end()) {
                out.push_back(&entry);
            }
        }
    }
    std::ranges::reverse(out);
    return out;
}

const DriveEntry* DriveTree::resolve(std::string_view parent_id, std::string_view path) const {
    std::string parent(parent_id);
    const DriveEntry* found = find(parent_id);
    std::size_t offset = 0;
    while (offset < path.size()) {
        const std::size_t slash = path.find('/', offset);
        const std::string_view part = path.substr(
            offset, slash == std::string_view::npos ? std::string_view::npos : slash - offset);
        if (!part.empty()) {
            found = child(parent, part);
            if (found == nullptr) {
                return nullptr;
            }
            parent = found->id;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        offset = slash + 1;
    }
    return found;
}

std::string
drive_free_name(const DriveTree& tree, std::string_view parent_id, std::string_view name) {
    if (tree.child(parent_id, name) == nullptr) {
        return std::string(name);
    }
    const std::size_t dot = name.rfind('.');
    const bool extension = dot != std::string_view::npos && dot > 0;
    const std::string_view stem = extension ? name.substr(0, dot) : name;
    const std::string_view suffix = extension ? name.substr(dot) : std::string_view();
    for (int number = 2;; ++number) {
        std::string candidate = std::format("{} ({}){}", stem, number, suffix);
        if (candidate.size() > max_drive_name_bytes) {
            candidate = std::format("{} ({})", lake::control::new_uuid_v4(), number);
        }
        if (tree.child(parent_id, candidate) == nullptr) {
            return candidate;
        }
    }
}

} // namespace lake::control
