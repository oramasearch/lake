// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/workspace_records.hpp"

namespace lake::control {

// A workspace's own settings (lake#1230, docs/api/workspaces.md#settings). They are workspace
// data, so they live in the workspace's Zetta, one sealed record in `lake_workspace_settings`
// under the key `workspace`, found by its account; nothing of them is in lake's Postgres.
//
// `agent_model` is the model every agent turn of the workspace calls through Lake's model gateway
// (docs/api/model-gateway.md#the-workspaces-model). Empty: not chosen, so the gateway's default.
struct WorkspaceSettings {
    std::string customer_id;
    std::string agent_model;
    std::string set_by;          // the user id of whoever changed them last
    std::string set_by_identity; // their identity, for the record
    std::string updated_at;      // "YYYY-MM-DDTHH:MM:SSZ"; empty when never set
    std::int64_t at_us = 0;      // when the record was first written
};

struct WorkspaceSettingsCodec {
    using Value = WorkspaceSettings;
    static constexpr RecordCollection collection{"workspace_settings", 1};

    static std::string lookup(const WorkspaceSettings& settings);
    static nlohmann::json encode(const WorkspaceSettings& settings);
    static std::optional<WorkspaceSettings> decode(const StoredRecord& record);
};

// The one record's key.
inline constexpr std::string_view workspace_settings_key = "workspace";

// The workspace's settings for this account: nullopt when nothing was ever set (or the record
// belongs to another account, which never happens to a workspace's own record).
liquid::runtime::task<ZettaControlResult<std::optional<WorkspaceSettings>>>
get_workspace_settings(WorkspaceRecords& records, std::string customer_id);

// Writes the settings, rereading on a conflict; `settings.customer_id` names the account.
liquid::runtime::task<ZettaControlResult<WorkspaceSettings>>
put_workspace_settings(WorkspaceRecords& records, WorkspaceSettings settings);

} // namespace lake::control
