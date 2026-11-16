// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "lake/control/store.hpp"
#include "lake/control/workspace_records.hpp"

namespace lake::control {

// Drive share links (docs/api/sharing.md, lake#350): one mutable record per link in the workspace
// Zetta of the workspace that holds it. The key is the link id; the lookup is the repository and
// folder object, so the sharing page lists a folder's links with one read. The record holds the
// token digest, role, expiry, creator and revocation; lake's PostgreSQL keeps only the synthetic
// `share-link:<id>` grant (policy) and, for links made before the move, a content-free route from
// the token digest to the workspace (`drive_share_link_routes`, migration 0048).
struct DriveShareLinkCodec {
    using Value = DriveShareLink;
    static constexpr RecordCollection collection{"drive_share_links", 1};

    static std::string lookup(const DriveShareLink& link);
    static nlohmann::json encode(const DriveShareLink& link);
    static std::optional<DriveShareLink> decode(const StoredRecord& record);
};

// The lookup of a folder's links.
std::string drive_share_link_lookup(const std::string& repository_id,
                                    const std::string& object_id);

// Refuses a link without a repository, folder object, token digest or expiry, or with a role other
// than read/write.
std::optional<std::string> drive_share_link_problem(const DriveShareLink& link);

// A link's bearer token names where its record lives: `<workspace id>.<link id>.<secret>`. A
// recipient who holds only the token is routed to the workspace without any central lookup; the
// secret is checked against the digest sealed in the record. Tokens made before lake#350 have no
// dots and are routed through `drive_share_link_routes`.
struct DriveShareToken {
    std::string workspace_id;
    std::string link_id;
};
std::string drive_share_token(const std::string& workspace_id, const std::string& link_id,
                              const std::string& secret);
// nullopt for a token that does not have the routed shape (a legacy token, or garbage).
std::optional<DriveShareToken> parse_drive_share_token(std::string_view token);

using DriveShareLinks = RecordSet<DriveShareLinkCodec>;

} // namespace lake::control
