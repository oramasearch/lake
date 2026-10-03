#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/apps.hpp"
#include "lake/control/workspace_records.hpp"

namespace lake::control {

// An application's users (lake#353) and organizations (lake#354), kept in the workspace Zetta of
// the application's workspace (`auth_applications.workspace_id`, docs/architecture/
// workspace-records.md). Lake's PostgreSQL keeps neither; the retired auth_app_users and
// auth_app_organizations tables are read only by the operator's one-shot copy.
//
// Both are mutable records found by the application (`application:<id>`), so one list_records call
// reads an application's users and another its organizations. A user's key is the blind index of
// `application:<id>\nuser:<id>`, because a lake user who authorizes several applications of one
// workspace is a user of each; an organization's key is its id.
//
// What must be unique within an application is a claim record in the same collection, keyed by
// the blind index of the unique value and written with `if_absent`: a user's email
// (`application:<id>\nemail:<lowercase email>`, for users of the application's own pool), and an
// organization's TAAP identity (`…\ntaap:<id>`) and name (`…\nname:<name>`). A claim names its
// holder and is found by `application:<id>\nclaims`, so it never appears in a listing. Two callers
// cannot both hold one claim; the second is `conflict`.

struct ApplicationUserCodec {
    using Value = ApplicationUser;
    static constexpr RecordCollection collection{"auth_app_users", 1};

    static std::string lookup(const ApplicationUser& user);
    static nlohmann::json encode(const ApplicationUser& user);
    static std::optional<ApplicationUser> decode(const StoredRecord& record);
};

struct ApplicationOrganizationCodec {
    using Value = ApplicationOrganization;
    static constexpr RecordCollection collection{"auth_app_organizations", 1};

    static std::string lookup(const ApplicationOrganization& organization);
    static nlohmann::json encode(const ApplicationOrganization& organization);
    static std::optional<ApplicationOrganization> decode(const StoredRecord& record);
};

// The lookup of an application's users or organizations, and of its claims.
std::string application_directory_lookup(const std::string& application_id);
std::string application_claims_lookup(const std::string& application_id);

// The id of a retired auth_app_users row: md5(application id, newline, user id) as a UUID, the
// same value migration 0055 records in workspace_assignments.
std::string application_user_row_id(const std::string& application_id, const std::string& user_id);

// The record key of a user of an application in `records`' workspace.
std::string application_user_key(const WorkspaceRecords& records,
                                 const std::string& application_id,
                                 const std::string& user_id);

// The kinds of claim, and the key of each claim in `records`' workspace. An email is compared in
// lowercase, as the pool's sign-in compares it; a name and a TAAP identity exactly.
inline constexpr std::string_view email_claim = "email";
inline constexpr std::string_view taap_claim = "taap";
inline constexpr std::string_view name_claim = "name";
std::string application_claim_key(const WorkspaceRecords& records,
                                  RecordCollection collection,
                                  const std::string& application_id,
                                  std::string_view kind,
                                  const std::string& value);

// One claim as read: who holds it, since when, and the engine revision to remove it at.
struct ApplicationClaim {
    std::string holder;
    std::int64_t at_us = 0;
    std::int64_t revision = 0;
};

// The claim at `key`; nullopt when nobody holds it.
liquid::runtime::task<ZettaControlResult<std::optional<ApplicationClaim>>>
read_application_claim(WorkspaceRecords& records, RecordCollection collection, std::string key);

// Writes the claim for `holder`: only if absent, or, given `over_revision`, only over the claim
// read at that revision (a stale claim being taken over). `conflict` when another write won.
liquid::runtime::task<ZettaControlResult<ApplicationClaim>>
write_application_claim(WorkspaceRecords& records,
                        RecordCollection collection,
                        std::string application_id,
                        std::string key,
                        std::string holder,
                        std::optional<std::int64_t> over_revision = std::nullopt);

// Removes the claim at `key` if `holder` holds it; nothing when it is absent or held by another.
liquid::runtime::task<ZettaControlResult<void>> release_application_claim(
    WorkspaceRecords& records, RecordCollection collection, std::string key, std::string holder);

// One user of an application; nullopt when this workspace holds none.
liquid::runtime::task<ZettaControlResult<std::optional<ApplicationUser>>>
get_application_user(WorkspaceRecords& records, std::string application_id, std::string user_id);

// An application's users, newest first: at most `limit`, those whose email or display name
// contains `query` (case-insensitive) when it is not empty.
liquid::runtime::task<ZettaControlResult<std::vector<ApplicationUser>>> list_application_users(
    WorkspaceRecords& records, std::string application_id, std::string query, std::size_t limit);

// Writes a new user record only if the application has no record of that user: `conflict` when
// it has. `user.at_us` 0 is now; a copy keeps the row's instant.
liquid::runtime::task<ZettaControlResult<ApplicationUser>>
create_application_user(WorkspaceRecords& records, ApplicationUser user);

// Sets a user's status with a write conditional on the revision it read, rereading on a conflict.
// `not_found` when this workspace holds no such user of the application.
liquid::runtime::task<ZettaControlResult<ApplicationUser>> set_application_user_status(
    WorkspaceRecords& records, std::string application_id, std::string user_id, std::string status);

// Removes a user's record at the revision it was read at, then the email claim it holds.
liquid::runtime::task<ZettaControlResult<void>> remove_application_user(WorkspaceRecords& records,
                                                                        ApplicationUser user);

// One organization of an application; nullopt when this workspace holds none.
liquid::runtime::task<ZettaControlResult<std::optional<ApplicationOrganization>>>
get_application_organization(WorkspaceRecords& records,
                             std::string application_id,
                             std::string organization_id);

// An application's organizations, newest first, at most `limit`.
liquid::runtime::task<ZettaControlResult<std::vector<ApplicationOrganization>>>
list_application_organizations(WorkspaceRecords& records,
                               std::string application_id,
                               std::size_t limit);

// Writes an organization record only if absent, keeping `organization.id` and `at_us` when set.
// The caller holds its TAAP and name claims first.
liquid::runtime::task<ZettaControlResult<ApplicationOrganization>>
put_new_application_organization(WorkspaceRecords& records, ApplicationOrganization organization);

// Writes `next` over the organization read at `next.revision`; `conflict` when it changed since.
liquid::runtime::task<ZettaControlResult<ApplicationOrganization>>
replace_application_organization(WorkspaceRecords& records, ApplicationOrganization next);

// Removes an organization at the revision it was read at, then the claims it holds.
liquid::runtime::task<ZettaControlResult<void>>
remove_application_organization(WorkspaceRecords& records, ApplicationOrganization organization);

} // namespace lake::control
