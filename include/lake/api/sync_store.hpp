#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
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

enum class SyncPushItemKind { Change, Reference };

struct SyncPushItem {
    SyncPushItemKind kind = SyncPushItemKind::Change;
    std::string change_id;
};

using SyncPushAuthorizer =
    std::function<SyncStoreResult<void>(const SyncPushItem&, std::span<const BranchRefUpdate>)>;

struct RepositoryContentRef {
    std::string name;
    std::string change;
};

struct RepositoryTreeEntry {
    std::string path;
    std::string kind;
    std::string blob;
    std::uint32_t mode = 0;
};

struct RepositoryFile {
    std::string path;
    std::string blob;
    std::string contents;
    std::uint64_t size = 0;
};

struct RepositoryChange {
    std::string id;
    std::vector<std::string> parents;
    std::string author;
    std::string message;
    std::string occurred_at;
};

struct RepositoryConflict {
    std::string path;
    std::vector<std::string> blobs;
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
};

struct RepositoryArtifactPage {
    std::vector<RepositoryArtifact> items;
    std::string next_cursor;
    bool done = false;
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
                                        std::vector<std::byte> contents) = 0;
    virtual SyncStoreResult<std::vector<std::byte>> read(const std::string& customer_id,
                                                         const std::string& digest) = 0;

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
                                                         SyncPushAuthorizer authorize) = 0;
    virtual SyncStoreResult<std::vector<std::byte>> pull(const std::string& customer_id,
                                                         const std::string& repository_id,
                                                         std::span<const std::byte> request) = 0;
    virtual SyncStoreResult<void> upload_chunks(const std::string& customer_id,
                                                const std::string& repository_id,
                                                std::span<const std::byte> request) = 0;
    virtual SyncStoreResult<std::vector<std::byte>> read_chunk(const std::string& customer_id,
                                                               const std::string& repository_id,
                                                               const std::string& digest,
                                                               std::uint64_t offset,
                                                               std::uint64_t length) = 0;
};

} // namespace lake::api
