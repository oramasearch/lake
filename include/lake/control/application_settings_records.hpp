// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/apps.hpp"
#include "lake/control/workspace_records.hpp"

namespace lake::control {

// An application's settings (lake#351): its name, allowed origins, redirect URIs and hosted-page
// theme, kept in the workspace Zetta of the application's workspace
// (docs/architecture/workspace-records.md). Lake's PostgreSQL keeps only the content-free routing
// row (auth_applications: id, space, pool, custom sign-in host, workspace), which finds the
// workspace; the retired settings columns are read only by the operator's one-shot copy.
//
// One record per application, keyed by the application id, found by its space
// (`customer:<id>`), so one list_records reads every application of a space that a workspace
// holds. The body names the application and its space, and a record under another key or of
// another space is not this application's. A change (the theme) is a write conditional on the
// revision read, reread on a conflict.

struct ApplicationSettingsCodec {
    using Value = ApplicationSettings;
    static constexpr RecordCollection collection{"auth_applications", 1};

    static std::string lookup(const ApplicationSettings& settings);
    static nlohmann::json encode(const ApplicationSettings& settings);
    static std::optional<ApplicationSettings> decode(const StoredRecord& record);
};

// The lookup of a space's application settings.
std::string application_settings_lookup(const std::string& customer_id);

// An application's settings; nullopt when there are none here (or they belong to another
// application or space, or do not open in this workspace). An empty `customer_id` accepts any
// space: only the one-shot copy reads that way, and its verify compares the space.
liquid::runtime::task<ZettaControlResult<std::optional<ApplicationSettings>>>
get_application_settings(WorkspaceRecords& records, std::string application_id,
                         std::string customer_id);

// Writes a new application's settings (or copied ones) only where none exist: `conflict`
// otherwise. `settings.at_us` 0 is now.
liquid::runtime::task<ZettaControlResult<ApplicationSettings>>
create_application_settings(WorkspaceRecords& records, ApplicationSettings settings);

// Changes an application's settings: reads them, applies `change`, and writes them conditionally on
// the revision read, rereading and applying it again on a conflict, up to `attempts` times.
// `not_found` when the application has no settings here.
using ApplicationSettingsChange = std::function<void(ApplicationSettings&)>;
liquid::runtime::task<ZettaControlResult<ApplicationSettings>>
update_application_settings(WorkspaceRecords& records, std::string application_id,
                            std::string customer_id, ApplicationSettingsChange change,
                            int attempts = 5);

// Every application settings record of a space in this workspace, by application id.
liquid::runtime::task<ZettaControlResult<std::vector<ApplicationSettings>>>
list_application_settings(WorkspaceRecords& records, std::string customer_id);

} // namespace lake::control
