#pragma once

// An in-memory repository store for the methodology tests (lake#655): each repository is a list of
// changes, each change the whole tree as {path: contents}; `main` names the newest. Only what a
// methodology's code needs (refs, tree, file, commit) works; the rest of the SyncStore interface
// answers `unsupported`.

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "lake/api/sync_store.hpp"

namespace lake::test {

struct MethodologyRepositories final : lake::api::SyncStore {
    using SyncStoreError = lake::api::SyncStoreError;
    template <class T> using Result = lake::api::SyncStoreResult<T>;

    struct Change {
        std::string id;
        std::map<std::string, std::string> files;
    };
    std::map<std::string, std::vector<Change>> repositories;
    std::vector<std::string> messages;
    mutable std::mutex mutex;

    static SyncStoreError unsupported() {
        return SyncStoreError{"unsupported", "not in the methodology test store"};
    }

    const Change* change(const std::string& repository, const std::string& ref) const {
        const auto found = repositories.find(repository);
        if (found == repositories.end() || found->second.empty()) {
            return nullptr;
        }
        if (ref == "main" || ref.empty()) {
            return &found->second.back();
        }
        for (const Change& candidate : found->second) {
            if (candidate.id == ref) {
                return &candidate;
            }
        }
        return nullptr;
    }

    Result<std::vector<std::string>> find_missing(const std::string&,
                                                  const std::vector<std::string>&) override {
        return std::unexpected(unsupported());
    }
    Result<void>
    write(const std::string&, const std::string&, std::vector<std::byte>, std::uint64_t) override {
        return std::unexpected(unsupported());
    }
    Result<std::vector<std::byte>> read(const std::string&, const std::string&) override {
        return std::unexpected(unsupported());
    }
    Result<std::uint64_t> unique_stored_bytes(const std::string&) override {
        return 0;
    }
    Result<std::vector<lake::api::RepositoryContentRef>>
    repository_refs(const std::string&, const std::string& repository) override {
        std::lock_guard lock(mutex);
        std::vector<lake::api::RepositoryContentRef> refs;
        if (const Change* tip = change(repository, "main")) {
            refs.push_back({"main", tip->id});
        }
        return refs;
    }
    Result<std::vector<lake::api::RepositoryTreeEntry>> repository_tree(
        const std::string&, const std::string& repository, const std::string& ref) override {
        std::lock_guard lock(mutex);
        const Change* at = change(repository, ref);
        if (at == nullptr) {
            return std::unexpected(SyncStoreError{"not_found", "no such revision"});
        }
        std::vector<lake::api::RepositoryTreeEntry> entries;
        for (const auto& [path, contents] : at->files) {
            lake::api::RepositoryTreeEntry entry;
            entry.path = path;
            entry.kind = "file";
            entry.size = contents.size();
            entries.push_back(std::move(entry));
        }
        return entries;
    }
    Result<lake::api::RepositoryFile> repository_file(const std::string&,
                                                      const std::string& repository,
                                                      const std::string& ref,
                                                      const std::string& path) override {
        std::lock_guard lock(mutex);
        const Change* at = change(repository, ref);
        if (at == nullptr || !at->files.contains(path)) {
            return std::unexpected(SyncStoreError{"not_found", "no such file"});
        }
        lake::api::RepositoryFile file;
        file.path = path;
        file.contents = at->files.at(path);
        file.size = file.contents.size();
        return file;
    }
    Result<std::vector<lake::api::RepositoryChange>> repository_log(const std::string&,
                                                                    const std::string&) override {
        return std::vector<lake::api::RepositoryChange>{};
    }
    Result<std::vector<lake::api::RepositoryDiffEntry>>
    repository_diff(const std::string&, const std::string&, const std::string&) override {
        return std::unexpected(unsupported());
    }
    Result<lake::api::RepositoryArtifactPage>
    repository_artifacts(const std::string&,
                         const std::string&,
                         const lake::api::RepositoryArtifactFilter&) override {
        return std::unexpected(unsupported());
    }
    Result<lake::api::RepositoryArtifact>
    repository_artifact(const std::string&, const std::string&, const std::string&) override {
        return std::unexpected(unsupported());
    }
    Result<std::vector<lake::api::RepositoryConflict>>
    repository_conflicts(const std::string&, const std::string&) override {
        return std::vector<lake::api::RepositoryConflict>{};
    }
    Result<lake::api::RepositoryConflictResolution>
    resolve_repository_conflict(const std::string&,
                                const std::string&,
                                const std::string&,
                                const std::string&,
                                std::vector<std::byte>) override {
        return std::unexpected(unsupported());
    }
    Result<std::string>
    commit_repository_files(const std::string&,
                            const std::string& repository,
                            const std::string& branch,
                            const std::string& message,
                            std::vector<lake::api::RepositoryWriteOperation> operations) override {
        std::lock_guard lock(mutex);
        if (branch != "main") {
            return std::unexpected(SyncStoreError{"invalid_input", "only main in this store"});
        }
        auto& changes = repositories[repository];
        Change next{"change-" + std::to_string(changes.size() + 1) + "-" + repository.substr(0, 8),
                    changes.empty() ? std::map<std::string, std::string>{} : changes.back().files};
        for (const lake::api::RepositoryWriteOperation& operation : operations) {
            if (operation.kind == lake::api::RepositoryWriteKind::DeletedFile) {
                next.files.erase(operation.path);
            } else {
                next.files[operation.path] =
                    std::string(reinterpret_cast<const char*>(operation.contents.data()),
                                operation.contents.size());
            }
        }
        changes.push_back(next);
        messages.push_back(message);
        return next.id;
    }
    Result<lake::api::RepositoryChange> restore_repository_file(const std::string&,
                                                                const std::string&,
                                                                const std::string&,
                                                                const std::string&) override {
        return std::unexpected(unsupported());
    }
    Result<lake::api::RepositoryPortableData>
    export_portable(const std::string&, const std::string&, bool) override {
        return std::unexpected(unsupported());
    }
    Result<void> import_portable(const std::string&,
                                 const std::string&,
                                 const lake::api::RepositoryPortableData&) override {
        return std::unexpected(unsupported());
    }
    Result<lake::api::RepositoryGitImport>
    import_git(const std::string&, const std::string&, const std::string&) override {
        return std::unexpected(unsupported());
    }
    Result<std::vector<std::pair<std::string, std::string>>> refs(const std::string&,
                                                                  const std::string&) override {
        return std::vector<std::pair<std::string, std::string>>{};
    }
    Result<std::vector<std::byte>>
    find_missing(const std::string&, const std::string&, std::span<const std::byte>) override {
        return std::unexpected(unsupported());
    }
    Result<std::vector<std::byte>> push(const std::string&,
                                        const std::string&,
                                        std::span<const std::byte>,
                                        lake::api::SyncPushAuthorizer,
                                        std::uint64_t) override {
        return std::unexpected(unsupported());
    }
    Result<std::vector<std::byte>>
    pull(const std::string&, const std::string&, std::span<const std::byte>) override {
        return std::unexpected(unsupported());
    }
    Result<std::uint64_t> upload_chunks(const std::string&,
                                        const std::string&,
                                        std::span<const std::byte>,
                                        std::uint64_t) override {
        return std::unexpected(unsupported());
    }
    Result<std::vector<std::byte>> read_chunk(const std::string&,
                                              const std::string&,
                                              const std::string&,
                                              std::uint64_t,
                                              std::uint64_t) override {
        return std::unexpected(unsupported());
    }
};

} // namespace lake::test
