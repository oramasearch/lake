#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace lake::api {

struct SyncStoreError {
    std::string code;
    std::string message;
};

template <class T> using SyncStoreResult = std::expected<T, SyncStoreError>;

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
    virtual SyncStoreResult<std::vector<RepositoryConflict>>
    repository_conflicts(const std::string& customer_id, const std::string& repository_id) = 0;
};

} // namespace lake::api
