#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace lake::api {

struct SyncStoreError {
    std::string code;
    std::string message;
    std::string context;
    std::string rule;
};

template <class T> using SyncStoreResult = std::expected<T, SyncStoreError>;

struct BranchRefUpdate {
    std::string name;
    std::optional<std::string> old_head;
    std::string new_head;
    bool fast_forward = false;
};

enum class SyncRefVerdictKind { Accepted, Refused };

struct SyncRefVerdict {
    std::string name;
    SyncRefVerdictKind verdict = SyncRefVerdictKind::Accepted;
    std::string reason;
};

enum class SyncPushItemKind { Change, Reference };

struct SyncPushItem {
    SyncPushItemKind kind = SyncPushItemKind::Change;
    std::string change_id;
};

using SyncPushAuthorizer = std::function<SyncStoreResult<std::vector<SyncRefVerdict>>(
    const SyncPushItem&, std::span<const BranchRefUpdate>)>;

struct RepositoryContentRef {
    std::string name;
    std::string change;
};

struct RepositoryTreeEntry {
    std::string path;
    std::string kind;
    std::string blob;
    std::uint32_t mode = 0;
    std::string node_id;
    std::uint64_t size = 0;
    std::uint64_t stored_size = 0;
    std::string last_change;
    std::string last_message;
    std::string last_author;
    std::string last_changed_at;
};

struct RepositoryFile {
    std::string path;
    std::string blob;
    std::string contents;
    std::uint64_t size = 0;
    std::vector<std::byte> raw_contents;
};

struct RepositoryChange {
    std::string id;
    std::vector<std::string> parents;
    std::string author;
    std::string message;
    std::string occurred_at;
};

struct RepositoryBlameLine {
    std::uint64_t line = 0;
    std::string change;
    std::string author;
    std::string occurred_at;
};

struct RepositoryConflictByteRange {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

struct RepositoryConflictNodeRange {
    std::string first;
    std::string last;
};

struct RepositoryConflictRegion {
    std::string path;
    std::optional<RepositoryConflictByteRange> bytes;
    std::optional<RepositoryConflictNodeRange> nodes;
};

struct RepositoryConflict {
    std::string id;
    std::string base;
    std::string left;
    std::string right;
    RepositoryConflictRegion region;
    std::string kind;
    std::string explanation;
    std::optional<std::string> proposal;
    std::vector<std::string> reasons;
    std::optional<std::string> node;
    std::vector<std::string> version_blobs;
};

struct RepositoryConflictResolution {
    std::string conflict;
    std::string value;
    std::string change;
};

struct RepositoryLineHunk {
    std::uint64_t old_line = 0;
    std::uint64_t old_count = 0;
    std::uint64_t new_line = 0;
    std::uint64_t new_count = 0;
    std::vector<std::string> removed_lines;
    std::vector<std::string> added_lines;
};

struct RepositoryDiffEntry {
    std::string path;
    std::string old_path;
    std::string kind;
    std::vector<std::string> changes;
    std::optional<std::uint32_t> old_mode;
    std::optional<std::uint32_t> new_mode;
    std::string old_blob;
    std::string new_blob;
    std::optional<std::vector<RepositoryLineHunk>> line_hunks;
    std::vector<std::string> old_conflicts;
    std::vector<std::string> new_conflicts;
};

struct RepositoryArtifactFilter {
    std::string action;
    std::string output;
    std::string content;
    std::string cursor;
    std::uint64_t limit = 100;
};

struct RepositoryArtifactOutput {
    std::string path;
    std::uint32_t mode = 0;
    std::string blob;
};

struct RepositoryArtifactTiming {
    std::uint64_t queued_ms = 0;
    std::uint64_t sandbox_ms = 0;
    std::uint64_t exec_ms = 0;
    std::uint64_t hash_ms = 0;
    std::uint64_t store_ms = 0;
};

struct RepositoryArtifactAttestation {
    std::string id;
    std::string profile;
    std::string subject;
    std::string issuer;
    bool verified = false;
    std::string reason;
};

struct RepositoryArtifactTrace {
    std::string id;
    std::uint64_t wall_ms = 0;
    std::uint64_t actions = 0;
    std::uint64_t dependencies = 0;
    std::uint64_t cache_hits = 0;
    std::uint64_t cache_misses = 0;
};

struct RepositoryArtifactMember {
    std::string artifact;
    std::string version;
    std::string path;
    std::string digest;
    bool indexed = false;
    std::string reason;
};

struct RepositoryArtifact {
    std::string id;
    std::string action;
    int exit_code = 0;
    std::vector<RepositoryArtifactOutput> outputs;
    RepositoryArtifactTiming timing;
    std::string cache_status;
    bool divergent = false;
    std::string verification;
    std::string worker;
    std::string observed_at;
    std::string started_at;
    std::string finished_at;
    std::vector<RepositoryArtifactAttestation> attestations;
    std::optional<RepositoryArtifactTrace> trace;
    std::vector<RepositoryArtifactMember> members;
};

struct RepositoryArtifactPage {
    std::vector<RepositoryArtifact> items;
    std::string next_cursor;
    bool done = false;
};

enum class RepositoryWriteKind { File, LfsFile, Folder, Moved, DeletedFile, DeletedFolder };

struct RepositoryWriteOperation {
    RepositoryWriteKind kind = RepositoryWriteKind::File;
    std::string path;
    std::string source_path;
    std::vector<std::byte> contents;
    std::string oid;
    std::uint64_t size = 0;
};

struct RepositoryRevisionStorage {
    std::string revision;
    std::uint64_t logical_bytes = 0;
    std::uint64_t unique_bytes = 0;
    std::uint64_t deduplicated_bytes = 0;
    double dedup_ratio = 0.0;
};

struct RepositoryStorageSummary {
    std::uint64_t logical_bytes = 0;
    std::uint64_t unique_bytes = 0;
    std::uint64_t deduplicated_bytes = 0;
    double dedup_ratio = 0.0;
    std::vector<RepositoryRevisionStorage> revisions;
};

struct RepositoryModelProvenance {
    std::string model_revision;
    std::string dataset_repository_id;
    std::string dataset_revision;
    std::string code_revision;
    std::string model_attestation;
    std::string run_attestation;
    std::vector<std::string> parameters;
    nlohmann::json environment = nlohmann::json::object();
    std::string environment_digest;
    std::string runner;
    std::string builder;
    std::string invocation_id;
    bool verified = false;
    std::string reason;
};

struct RepositoryReplicationReport {
    std::string cursor;
    std::uint64_t metadata_items = 0;
    std::uint64_t received_objects = 0;
    std::uint64_t received_bytes = 0;
    std::size_t references = 0;
};

// The portable portion of a lake export. `history` is liquid's signed sync
// data plus every referenced object. `artifacts` is the public #166
// enumeration used to check the reconstructed store. `git` is present only
// when the caller requested a Git projection.
struct RepositoryPortableData {
    nlohmann::json history = nlohmann::json::object();
    nlohmann::json artifacts = nlohmann::json::array();
    nlohmann::json git = nlohmann::json::object();
};

struct RepositoryGitImport {
    std::uint64_t commits = 0;
    std::uint64_t blobs = 0;
    nlohmann::json commit_changes = nlohmann::json::object();
};

// One repository's copy into its workspace's store (lake#380, liquid#328 repository::transfer):
// what the transfer examined and wrote, and, in verify mode, each way the workspace store differs.
// `absent` is a repository the space's store never held (nothing to copy).
struct WorkspaceStoreTransfer {
    std::string repository_id;
    bool absent = false;
    bool complete = false;
    std::uint64_t objects = 0;
    std::uint64_t objects_present = 0;
    std::uint64_t objects_written = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t state_written = 0;
    std::uint64_t leases_written = 0;
    std::vector<std::string> missing;
    std::vector<std::pair<std::string, std::string>> differences;
};

// The removal of a space's old store from lake's volume once every workspace of the space is served
// from its own store (remove_space_store): each directory removed (or, on a dry run, that would be),
// with the bytes of the regular files under it. `exists` is false for a directory already gone.
struct SpaceStoreDirectory {
    std::string path;
    bool exists = false;
    std::uint64_t bytes = 0;
    std::uint64_t files = 0;
};
struct SpaceStoreRemoval {
    std::vector<SpaceStoreDirectory> directories;
    std::uint64_t bytes = 0;
    bool removed = false;
};

// A tenant image's blob as an object of its workspace's liquid store (lake#383 phase B).
struct WorkspaceBlob {
    std::string object; // the liquid identity, `blake3:<hex>`
    std::uint64_t size = 0;
};

// What move_workspace_store `provision` did: `created` the store, or found it already there
// (`objects` it holds).
struct WorkspaceStoreProvision {
    bool created = false;
    std::uint64_t objects = 0;
};

// HTTP-free access to the liquid store selected by a repository's customer.
// The libliquid implementation remains inside the HTTP adapter translation
// unit.
class SyncStore {
public:
    virtual ~SyncStore() = default;

    virtual SyncStoreResult<std::vector<std::string>>
    find_missing(const std::string& customer_id, const std::vector<std::string>& digests) = 0;
    virtual SyncStoreResult<void> write(const std::string& customer_id,
                                        const std::string& digest,
                                        std::vector<std::byte> contents,
                                        std::uint64_t storage_quota_bytes = 0) = 0;
    virtual SyncStoreResult<std::vector<std::byte>> read(const std::string& customer_id,
                                                         const std::string& digest) = 0;
    virtual SyncStoreResult<std::uint64_t> unique_stored_bytes(const std::string& customer_id) = 0;

    virtual SyncStoreResult<std::vector<RepositoryContentRef>>
    repository_refs(const std::string& customer_id, const std::string& repository_id) = 0;
    virtual SyncStoreResult<std::vector<RepositoryTreeEntry>>
    repository_tree(const std::string& customer_id,
                    const std::string& repository_id,
                    const std::string& ref) = 0;
    virtual SyncStoreResult<RepositoryFile> repository_file(const std::string& customer_id,
                                                            const std::string& repository_id,
                                                            const std::string& ref,
                                                            const std::string& path) = 0;
    virtual SyncStoreResult<RepositoryFile> repository_blob(const std::string&,
                                                            const std::string&,
                                                            const std::string&,
                                                            const std::string&) {
        return std::unexpected(
            SyncStoreError{"unsupported", "repository blob reads are unavailable"});
    }
    virtual SyncStoreResult<std::vector<RepositoryChange>>
    repository_log(const std::string& customer_id, const std::string& repository_id) = 0;
    virtual SyncStoreResult<std::vector<RepositoryDiffEntry>>
    repository_diff(const std::string& customer_id,
                    const std::string& repository_id,
                    const std::string& change_id) = 0;
    virtual SyncStoreResult<RepositoryArtifactPage>
    repository_artifacts(const std::string& customer_id,
                         const std::string& repository_id,
                         const RepositoryArtifactFilter& filter) = 0;
    virtual SyncStoreResult<RepositoryArtifact>
    repository_artifact(const std::string& customer_id,
                        const std::string& repository_id,
                        const std::string& result_id) = 0;
    virtual SyncStoreResult<std::vector<RepositoryConflict>>
    repository_conflicts(const std::string& customer_id, const std::string& repository_id) = 0;
    virtual SyncStoreResult<RepositoryConflictResolution>
    resolve_repository_conflict(const std::string& customer_id,
                                const std::string& repository_id,
                                const std::string& conflict_id,
                                const std::string& resolution,
                                std::vector<std::byte> blob) = 0;
    virtual SyncStoreResult<RepositoryStorageSummary> repository_storage(const std::string&,
                                                                         const std::string&) {
        return std::unexpected(SyncStoreError{"unsupported", "repository storage is unavailable"});
    }
    virtual SyncStoreResult<std::string>
    commit_repository_files(const std::string&,
                            const std::string&,
                            const std::string&,
                            const std::string&,
                            std::vector<RepositoryWriteOperation>) {
        return std::unexpected(SyncStoreError{"unsupported", "repository writes are unavailable"});
    }
    virtual SyncStoreResult<std::uint64_t> stage_repository_file(const std::string&,
                                                                 const std::string&,
                                                                 const std::string&,
                                                                 std::vector<std::byte>,
                                                                 std::uint64_t = 0) {
        return std::unexpected(SyncStoreError{"unsupported", "repository uploads are unavailable"});
    }
    virtual SyncStoreResult<RepositoryModelProvenance>
    attach_model_provenance(const std::string&,
                            const std::string&,
                            const std::string&,
                            const std::string&,
                            const std::string&,
                            std::vector<std::byte>,
                            std::vector<std::byte>) {
        return std::unexpected(SyncStoreError{"unsupported", "model provenance is unavailable"});
    }
    virtual SyncStoreResult<std::optional<RepositoryModelProvenance>>
    model_provenance(const std::string&, const std::string&, const std::string&) {
        return std::optional<RepositoryModelProvenance>{};
    }
    virtual SyncStoreResult<RepositoryChange>
    restore_repository_file(const std::string& customer_id,
                            const std::string& repository_id,
                            const std::string& change_id,
                            const std::string& path) = 0;

    virtual SyncStoreResult<RepositoryPortableData> export_portable(
        const std::string& customer_id, const std::string& repository_id, bool include_git) = 0;
    virtual SyncStoreResult<void> import_portable(const std::string& customer_id,
                                                  const std::string& repository_id,
                                                  const RepositoryPortableData& data) = 0;
    virtual SyncStoreResult<RepositoryGitImport>
    import_git(const std::string& customer_id,
               const std::string& repository_id,
               const std::string& source_directory) = 0;

    virtual SyncStoreResult<std::vector<std::pair<std::string, std::string>>>
    refs(const std::string& customer_id, const std::string& repository_id) = 0;
    virtual SyncStoreResult<std::vector<std::byte>>
    find_missing(const std::string& customer_id,
                 const std::string& repository_id,
                 std::span<const std::byte> request) = 0;
    virtual SyncStoreResult<std::vector<std::byte>> push(const std::string& customer_id,
                                                         const std::string& repository_id,
                                                         std::span<const std::byte> request,
                                                         SyncPushAuthorizer authorize,
                                                         std::uint64_t storage_quota_bytes = 0) = 0;
    virtual SyncStoreResult<std::vector<std::byte>> pull(const std::string& customer_id,
                                                         const std::string& repository_id,
                                                         std::span<const std::byte> request) = 0;
    virtual SyncStoreResult<std::uint64_t> upload_chunks(const std::string& customer_id,
                                                         const std::string& repository_id,
                                                         std::span<const std::byte> request,
                                                         std::uint64_t storage_quota_bytes = 0) = 0;
    virtual SyncStoreResult<std::vector<std::byte>> read_chunk(const std::string& customer_id,
                                                               const std::string& repository_id,
                                                               const std::string& digest,
                                                               std::uint64_t offset,
                                                               std::uint64_t length) = 0;
    // lake#380: a workspace's repositories move from the space's store on lake's volume into the
    // workspace's own store in its workspace Zetta. Copy (or, with verify, compare) one repository.
    virtual SyncStoreResult<WorkspaceStoreTransfer>
    transfer_to_workspace_store(const std::string&, const std::string&, const std::string&, bool) {
        return std::unexpected(SyncStoreError{"unsupported", "workspace stores are unavailable"});
    }
    // Holds the workspace's and its space's repository requests, then calls `list` for the
    // workspace's repositories, copies what changed since the last copy, verifies every
    // repository, creates in the workspace store each one the space store never held, and only
    // when all are complete calls `record` and serves the workspace's repositories from its store
    // from then on.
    virtual SyncStoreResult<std::vector<WorkspaceStoreTransfer>>
    switch_workspace_store(const std::string&,
                           const std::string&,
                           const std::function<SyncStoreResult<std::vector<std::string>>()>&,
                           const std::function<SyncStoreResult<void>()>&) {
        return std::unexpected(SyncStoreError{"unsupported", "workspace stores are unavailable"});
    }
    // Runs `create_row` (which creates the repository's control-store row) while no switch of the
    // workspace can run, then, when the workspace is served from its own store, creates the
    // repository there. A repository in a workspace store is never created implicitly.
    virtual SyncStoreResult<void>
    create_repository_content(const std::string&,
                              const std::string&,
                              const std::string&,
                              const std::function<SyncStoreResult<void>()>& create_row) {
        return create_row();
    }
    // Whether the repository's workspace store serves it but does not hold it (a creation whose
    // store step failed). False for a repository in the space's store.
    virtual SyncStoreResult<bool> repository_missing_from_workspace_store(const std::string&,
                                                                          const std::string&) {
        return false;
    }
    // Removes the space's store on lake's volume (<root>/<space>/store) and its repository
    // directories (<root>/<space>/repositories). Holds the space's lane, refusing new space-store
    // requests and waiting for admitted ones, calls `check` under that hold (the caller's
    // precondition: every workspace switched), closes the store, then deletes the two
    // directories. A dry run takes no hold, closes nothing and reports what would be removed.
    virtual SyncStoreResult<SpaceStoreRemoval>
    remove_space_store(const std::string&, bool, const std::function<SyncStoreResult<void>()>&) {
        return std::unexpected(SyncStoreError{"unsupported", "space stores are unavailable"});
    }
    // Closes the workspace's store and releases its bucket lease (workspace deletion, drain).
    virtual SyncStoreResult<void> close_workspace_store(const std::string&) {
        return {};
    }
    // The repair of a workspace switched before a switch created the store of a workspace with no
    // repository (move_workspace_store `provision`): creates its bucket and data key only when the
    // bucket is absent or empty. A bucket whose store already opens is left as it is (`created`
    // false); one that holds objects but does not open is `conflict`, naming how many, so a wiped
    // store of a workspace that held data is never recreated empty. The caller checks that the
    // workspace is switched and never held a repository or an image blob.
    virtual SyncStoreResult<WorkspaceStoreProvision> provision_workspace_store(const std::string&,
                                                                               const std::string&) {
        return std::unexpected(SyncStoreError{"unsupported", "workspace stores are unavailable"});
    }
    // lake#383 phase B (decision 54 S5): a tenant image's blob in its workspace's liquid store.
    // Streams the file at `source` into the store of a switched workspace and answers the object's
    // liquid identity and size; the space's storage quota is checked first (`busy`, rule
    // `storage_quota`). `admitted`: a request entering the workspace's lane (refused at once when
    // the lane is full or moving); otherwise lake's own work (the copy), which waits for it.
    virtual SyncStoreResult<WorkspaceBlob> put_workspace_blob(const std::string&,
                                                              const std::string&,
                                                              const std::filesystem::path&,
                                                              bool) {
        return std::unexpected(SyncStoreError{"unsupported", "workspace stores are unavailable"});
    }
    // Reads up to `into.size()` bytes of the object at `offset`, under the workspace's lane for
    // this one read; answers how many it read. A read of a blob the store does not hold is
    // `not_found`. `admitted` as for put_workspace_blob: a pull is admitted once, for its first
    // read, and its later reads wait for the lane rather than cut the layer off part way.
    virtual SyncStoreResult<std::uint64_t> read_workspace_blob(const std::string&,
                                                               const std::string&,
                                                               const std::string&,
                                                               std::uint64_t,
                                                               std::span<std::byte>,
                                                               bool) {
        return std::unexpected(SyncStoreError{"unsupported", "workspace stores are unavailable"});
    }
    virtual SyncStoreResult<RepositoryReplicationReport> replicate_from(const std::string&,
                                                                        const std::string&,
                                                                        const std::string&,
                                                                        const std::string&,
                                                                        const std::string&) {
        return std::unexpected(
            SyncStoreError{"unsupported", "repository replication is unavailable"});
    }
};

} // namespace lake::api
