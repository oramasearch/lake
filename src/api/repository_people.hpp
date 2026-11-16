// SPDX-License-Identifier: MIT

#pragma once

#include <expected>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/control/store.hpp"

namespace lake::api {

// One of a repository's people: the person who owns its space, or, for an organization's space,
// the organization itself and every person who is a member of it (or of one of its teams). A
// Drive folder is shared with them (drive_share_people) and an issue is assigned to them
// (issue_assignees).
struct RepositoryPerson {
    std::string taap_id;
    std::string kind; // user or organization
    std::string display_name;
    std::string email;
    std::string handle;
};

nlohmann::json repository_person_json(const RepositoryPerson& person);

// The repository's people, each with their handle, in membership order with no duplicates.
std::expected<std::vector<RepositoryPerson>, ApiError>
repository_people(ApiContext& context, const control::Repository& repository);

} // namespace lake::api
