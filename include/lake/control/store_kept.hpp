// SPDX-License-Identifier: MIT

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "lake/control/drive_files.hpp"
#include "lake/control/store_contents.hpp"
#include "lake/control/workspace_records.hpp"

namespace lake::control {

// A store Lake keeps in the workspace Zetta (provider `zetta`, lake#664,
// docs/api/storage.md#browsing-a-store): its tree is sealed `lake_store_items` records in the
// workspace Zetta (one per file and folder, found by the store and the folder it is in), and each
// file's bytes one object under an opaque key in the store's own bucket `lake_st_<12 hex>`,
// sealed in blocks exactly as workspace files are in `lake_files` (drive_files.hpp). The bucket's
// keys and the records' ids say nothing about a path; a move rewrites records, never bytes.
inline constexpr RecordCollection store_items_collection{"store_items", 1};

// The workspace Zetta bucket of the store with this slug (`st-<12 hex>`): `lake_st_<12 hex>`.
std::string kept_store_bucket(std::string_view slug);

// The store's contents, over the workspace's records and a DriveFileStore on the store's bucket.
std::shared_ptr<StoreContents>
zetta_store_contents(WorkspaceRecords records, DriveFileStore files, std::string store_id);

} // namespace lake::control
