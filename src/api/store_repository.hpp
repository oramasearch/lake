#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <string>

#include "lake/api/sync_store.hpp"
#include "lake/control/store_contents.hpp"

namespace lake::api {

// A `lake` store's contents (lake#664, docs/api/storage.md#browsing-a-store): a liquid repository
// of the workspace, browsed and written on its `main` branch through the repository store. Every
// write, folder, move and removal is one liquid change, so the store keeps its history. The
// repository store blocks, so each call runs on lake's blocking pool.
std::shared_ptr<control::StoreContents>
repository_store_contents(SyncStore* repositories, std::string customer_id,
                          std::string repository_id, std::string branch = "main");

// The three repository-store calls the contents use, on one repository's branch (tests inject
// a fake of liquid's semantics). Each blocks.
struct RepositoryBranch {
    std::function<SyncStoreResult<std::vector<RepositoryTreeEntry>>()> tree;
    std::function<SyncStoreResult<RepositoryFile>(const std::string& path)> file;
    std::function<SyncStoreResult<std::string>(const std::string& message,
                                               std::vector<RepositoryWriteOperation> operations)>
        commit;
};
std::shared_ptr<control::StoreContents> repository_store_contents(RepositoryBranch branch);

} // namespace lake::api
