#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <vector>

namespace lake::api {

struct SyncStoreError {
    std::string code;
    std::string message;
};

template <class T> using SyncStoreResult = std::expected<T, SyncStoreError>;

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
};

} // namespace lake::api
