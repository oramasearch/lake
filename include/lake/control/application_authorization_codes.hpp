// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/apps.hpp"
#include "lake/control/workspace_records.hpp"

namespace lake::control {

// An application's one-use OIDC authorization codes (lake#363), kept in the workspace Zetta of the
// application's workspace (docs/architecture/workspace-records.md, shape "one-use codes"). A code
// is found by its application (`application:<id>`), and its key is the blind index of the
// application and the code's SHA-256 (`application_authorization_code_key`), so neither the code
// nor its hash is visible in the workspace.
//
// - Issue writes the record with `if_absent`.
// - Redeem reads it and deletes it at the revision read (`remove(id, if_revision)`): of two
//   exchanges racing for one code, only one deletes it; the other gets `conflict` or `not_found`
//   and is refused. A code is therefore redeemed at most once.
// - Expiry is the collection's retention (five minutes after the last write, enforced by the
//   engine), and redeem also refuses a code whose own `expires_us` has passed, which holds for a
//   copied code written later than it was issued.

struct ApplicationAuthorizationCodeCodec {
    using Value = ApplicationAuthorizationCode;
    // How long a code may be redeemed after it is issued.
    static constexpr std::int64_t lifetime_ms = 5LL * 60 * 1000;
    static constexpr RecordCollection collection{"auth_app_authorization_codes", 1, lifetime_ms};

    static std::string lookup(const ApplicationAuthorizationCode& code);
    static nlohmann::json encode(const ApplicationAuthorizationCode& code);
    static std::optional<ApplicationAuthorizationCode> decode(const StoredRecord& record);
};

// The record key of a code in this workspace: the blind index of its application and its hash.
std::string application_authorization_code_key(const WorkspaceRecords& records,
                                               const std::string& application_id,
                                               const std::string& code_hash);

// Writes a code only where its key is absent: `conflict` otherwise. `code.at_us` 0 is now, and
// `code.expires_us` 0 is `at_us` plus the lifetime.
liquid::runtime::task<ZettaControlResult<ApplicationAuthorizationCode>>
issue_application_authorization_code(WorkspaceRecords& records, ApplicationAuthorizationCode code);

// Redeems a code once: reads it, refuses it when it is absent, of another application or expired,
// and deletes it at the revision read. Every refusal of the code itself is `unauthorized`; a
// failure to reach the workspace is reported as it is. `now_us` 0 is now.
liquid::runtime::task<ZettaControlResult<ApplicationAuthorizationCode>>
redeem_application_authorization_code(WorkspaceRecords& records, std::string application_id,
                                      std::string code_hash, std::int64_t now_us = 0);

// The application's codes still in the workspace, oldest first (for the one-shot copy).
liquid::runtime::task<ZettaControlResult<std::vector<ApplicationAuthorizationCode>>>
list_application_authorization_codes(WorkspaceRecords& records, std::string application_id);

} // namespace lake::control
