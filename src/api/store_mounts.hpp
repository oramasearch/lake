// SPDX-License-Identifier: MIT

#pragma once

// The `stores` a service or an app mounts (lake#666, docs/control-plane/storage.md#mounts): the
// input create_service, set_service_stores, create_app and set_app_stores take, how lake checks
// it before the store writes it, and how the reads answer it.

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/api/registry.hpp"
#include "lake/control/store.hpp"

namespace lake::api {

// The JSON schema of `stores`: [{store_id, mount_path?, read_only?}].
nlohmann::json store_mounts_schema();

// Checks `stores` for a workload in `workspace_id` of `customer_id`. `service_kind` is the
// service's catalog kind, or empty for an app. Each store must be a present store of that
// workspace, named once; a volume needs an absolute `mount_path`; a bucket in the customer's own
// account takes none; a store Lake runs (zetta, lake) is not mounted; and only an app or a service
// kind whose pods the Orama operator builds may mount anything. Whether another workload already
// mounts a volume is the store's to check, in the transaction that writes the mounts.
std::expected<std::vector<control::StoreMountRequest>, ApiResult>
checked_store_mounts(const nlohmann::json& stores,
                     const std::string& customer_id,
                     const std::string& workspace_id,
                     std::string_view service_kind,
                     bool app,
                     control::Store& store);

// A workload's `stores`: [{store_id, name, type, provider, mount_path, read_only}], from every
// mount of its workspace (or account).
nlohmann::json workload_stores_json(const std::vector<control::StoreMount>& mounts,
                                    std::string_view object_kind,
                                    std::string_view object_id);

} // namespace lake::api
