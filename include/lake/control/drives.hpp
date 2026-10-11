#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/workspace_records.hpp"

namespace lake::control {

// Drives (docs/api/drives.md, lake#561): the places a workspace keeps files, each one record in
// the sealed collection `lake_drives` of the workspace Zetta, found by its space. A drive stores
// its files one of three ways:
//
//   repository  a liquid repository of the workspace, browsed and written with the drive_*
//               capabilities; every write is a change with history. The drive names it; deleting
//               the drive never deletes the repository.
//   files       workspace files: folders, files and shortcuts as `lake_drive_entries` records,
//               each file's bytes one opaque object in the workspace Zetta's `lake_files` bucket
//               (drive_files.hpp), so a move or rename rewrites one record and never the bytes.
//   virtual     folders of shortcuts only: links to things that live elsewhere (a repository or
//               a path in one, a workspace file, a dataset, a transcript, a project, an agent, a
//               model or a web address).
struct Drive {
    std::string id;
    std::string customer_id;
    std::string name;
    std::string storage;       // repository, files or virtual
    std::string repository_id; // storage repository only
    std::string created_by;
    std::string created_at;
    std::int64_t at_us = 0;
    std::int64_t lake_revision = 1;
};

struct DriveCodec {
    using Value = Drive;
    static constexpr RecordCollection collection{"drives", 1};

    static std::string lookup(const Drive& drive);
    static nlohmann::json encode(const Drive& drive);
    static std::optional<Drive> decode(const StoredRecord& record);
};

using Drives = RecordSet<DriveCodec>;

std::string drives_lookup(const std::string& customer_id);
bool known_drive_storage(std::string_view storage);
// At most this many bytes in a drive's or an entry's name.
inline constexpr std::size_t max_drive_name_bytes = 255;
// A name a drive or an entry may have: 1 to 255 bytes, no "/" or "\", no control character, not
// "." or "..", and not the repository drives' empty-folder marker `.lake-folder`.
bool valid_drive_name(std::string_view name);
std::optional<std::string> drive_problem(const Drive& drive);

// One folder, file or shortcut of a workspace-files or virtual drive.
struct DriveEntry {
    std::string id;
    std::string customer_id;
    std::string drive_id;
    std::string parent_id; // the folder holding it; empty: the drive's top
    std::string kind;      // folder, file or shortcut
    std::string name;
    // A file's bytes: its length, its media type, its SHA-256 (hex, set when its upload
    // finished), its object in `lake_files`, and whether its upload finished (ready) or not yet
    // (uploading). An uploading file is never listed.
    std::uint64_t size = 0;
    std::string content_type;
    std::string sha256;
    std::string object_key;
    std::string state = "ready";
    // An uploading file that replaces the ready file `replaces` of the same folder and name when
    // its upload finishes; empty otherwise.
    std::string replaces;
    // A shortcut's target: {"kind": ..., ...} (drive_shortcut_problem).
    nlohmann::json target = nullptr;
    std::string created_by;
    std::string modified_by;
    std::int64_t modified_us = 0; // when its name, place or bytes last changed
    std::string created_at;
    std::int64_t at_us = 0;
    std::int64_t lake_revision = 1;
};

struct DriveEntryCodec {
    using Value = DriveEntry;
    static constexpr RecordCollection collection{"drive_entries", 1};

    static std::string lookup(const DriveEntry& entry);
    static nlohmann::json encode(const DriveEntry& entry);
    static std::optional<DriveEntry> decode(const StoredRecord& record);
};

using DriveEntries = RecordSet<DriveEntryCodec>;

std::string drive_entries_lookup(const std::string& drive_id);
std::optional<std::string> drive_entry_problem(const DriveEntry& entry);
// What a shortcut may point at, and the fields each kind needs (all strings):
//   repository       repository_id
//   repository_path  repository_id, path (ref optional; main when absent)
//   drive_entry      drive_id, entry_id
//   dataset          id          transcript  id          project  name
//   agent            id          model       id          url      url (http or https)
std::optional<std::string> drive_shortcut_problem(const nlohmann::json& target);

// The drives of a space in this workspace, by name.
liquid::runtime::task<ZettaControlResult<std::vector<Drive>>> list_drives(WorkspaceRecords& records,
                                                                          std::string customer_id);
// One drive of the space; nullopt when this workspace holds none.
liquid::runtime::task<ZettaControlResult<std::optional<Drive>>>
get_drive(WorkspaceRecords& records, std::string customer_id, std::string id);
// Writes a new drive under a new id; answers it as stored.
liquid::runtime::task<ZettaControlResult<Drive>> record_drive(WorkspaceRecords& records,
                                                              Drive drive);
// Renames a drive at the revision read, rereading on a conflict. `not_found` for none.
liquid::runtime::task<ZettaControlResult<Drive>>
rename_drive(WorkspaceRecords& records, std::string customer_id, std::string id, std::string name);
// Deletes the drive's record at the revision read. Its entries are the caller's to remove first.
liquid::runtime::task<ZettaControlResult<void>>
remove_drive(WorkspaceRecords& records, std::string customer_id, std::string id);

// Every entry of a drive, uploading ones included.
liquid::runtime::task<ZettaControlResult<std::vector<DriveEntry>>>
list_drive_entries(WorkspaceRecords& records, std::string customer_id, std::string drive_id);
// One entry of the drive; nullopt when there is none.
liquid::runtime::task<ZettaControlResult<std::optional<DriveEntry>>> get_drive_entry(
    WorkspaceRecords& records, std::string customer_id, std::string drive_id, std::string id);
// Writes a new entry under a new id (`id` set: under that id, if absent).
liquid::runtime::task<ZettaControlResult<DriveEntry>> record_drive_entry(WorkspaceRecords& records,
                                                                         DriveEntry entry);
// Changes an entry with a write conditional on the revision read, rereading and calling `change`
// again on a conflict. `change` answers a refusal (code and message) or nothing to write it.
// `not_found` when the drive holds no such entry.
using DriveEntryChange = std::function<std::optional<ZettaControlError>(DriveEntry&)>;
liquid::runtime::task<ZettaControlResult<DriveEntry>> change_drive_entry(WorkspaceRecords& records,
                                                                         std::string customer_id,
                                                                         std::string drive_id,
                                                                         std::string id,
                                                                         DriveEntryChange change);
// Deletes an entry's record at the revision read. A missing one is not an error.
liquid::runtime::task<ZettaControlResult<void>> remove_drive_entry(WorkspaceRecords& records,
                                                                   std::string customer_id,
                                                                   std::string drive_id,
                                                                   std::string id);

// The media type a file name's extension suggests (text/csv for "a.csv"), or
// application/octet-stream; for workspace files uploaded without one and repository files, which
// keep none (lake#587).
std::string drive_media_type(std::string_view name);

// An upload not finished within this time of its start expires (lake#588): it no longer counts as
// holding its name, the next upload of that name in its folder removes it (object, then record),
// and it takes no more chunks. Before it expires, list_drive_entries lists it under `uploads`.
inline constexpr std::int64_t drive_upload_expiry_us = 24LL * 60 * 60 * 1'000'000;
// Whether `entry` is an unfinished upload started (modified_us) at least drive_upload_expiry_us
// before `now_us`.
bool drive_upload_expired(const DriveEntry& entry, std::int64_t now_us);

// The folder structure over a drive's entries, read once (pure).
class DriveTree {
public:
    explicit DriveTree(std::vector<DriveEntry> entries);

    const std::vector<DriveEntry>& entries() const {
        return entries_;
    }
    // Adds an entry just written, so later lookups see it.
    void add(DriveEntry entry);
    // Forgets an entry just removed.
    void remove(std::string_view id);
    // The unfinished uploads in a folder ("" for the top).
    std::vector<const DriveEntry*> uploads(std::string_view parent_id) const;
    const DriveEntry* find(std::string_view id) const;
    // The listed (not uploading) children of a folder ("" for the top).
    std::vector<const DriveEntry*> children(std::string_view parent_id) const;
    // A child of the folder with this name, uploading ones included: names are unique within a
    // folder, an unfinished upload's too.
    const DriveEntry* child(std::string_view parent_id, std::string_view name) const;
    // "a/b/c" for the entry, from the top; empty for an unknown id.
    std::string path(std::string_view id) const;
    // The folders from the top down to `id` itself (inclusive), or empty when a parent is
    // missing.
    std::vector<const DriveEntry*> ancestors(std::string_view id) const;
    // Whether `id` is `folder_id` or inside it.
    bool within(std::string_view id, std::string_view folder_id) const;
    // Every entry inside the folder, deepest first, the folder itself last.
    std::vector<const DriveEntry*> subtree(std::string_view id) const;
    // The entry a slash path names from `parent_id`, or nullptr.
    const DriveEntry* resolve(std::string_view parent_id, std::string_view path) const;

private:
    std::vector<DriveEntry> entries_;
};

// "name (2).ext", "name (3).ext", … : the first name not taken in the folder.
std::string
drive_free_name(const DriveTree& tree, std::string_view parent_id, std::string_view name);

} // namespace lake::control
