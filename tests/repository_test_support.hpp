// SPDX-License-Identifier: MIT

#pragma once

#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "lake/control/store.hpp"

namespace lake::test {

// The workspace a test repository is created in (lake#315: every repository belongs to one
// workspace of its space): the space's first live workspace, or a new one named "repositories"
// when the space has none. Tests that are about workspaces create their own instead.
inline std::string repository_workspace(control::Store& store, const std::string& customer_id) {
    if (const auto rows = store.list_workspaces(customer_id)) {
        for (const control::Workspace& workspace : *rows) {
            if (workspace.desired != "deleted") {
                return workspace.id;
            }
        }
    }
    const auto created = store.create_workspace({customer_id, "repositories", "dev", ""});
    if (!created) {
        throw std::runtime_error("create_workspace: " + created.error().message);
    }
    return created->id;
}
inline std::string repository_workspace(control::Store& store, const nlohmann::json& customer_id) {
    return repository_workspace(store, customer_id.get<std::string>());
}

} // namespace lake::test
