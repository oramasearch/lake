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

// Who each TAAP identity is, for the `people` member of a read that names signers (lake#1326):
// `{<taap_id>: {kind, name, handle}}` for every identity an account acts for, resolved as
// Store::account_for_identity resolves a linked own identity. An unknown identity is absent, and a
// page shows it as an unknown signer with a short key. Never an email. A failed lookup answers an
// empty object: the record is still served, with its keys.
nlohmann::json signer_people(ApiContext& context, const std::vector<std::string>& taap_ids);

// The schema of that member.
nlohmann::json signer_people_schema();

// The identities a forge record names: a review's opener, reviewers and approver, a comment's
// author (a TAAP identifier, or liquid's actor object with `taap_id`).
void forge_record_signers(const nlohmann::json& value, std::vector<std::string>& out);

} // namespace lake::api
