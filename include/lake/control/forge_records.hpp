// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "lake/control/store.hpp"
#include "lake/control/workspace_records.hpp"

namespace lake::control {

// Forge records (docs/api/forge.md, lake#341): a repository's issues, issue comments, reviews,
// review comments, releases, webhooks, webhook deliveries, branch protection rules, exports and
// GitHub import reports. Each is one mutable record in the workspace Zetta of the repository's
// workspace (`repositories.workspace_id`, lake#315), keyed by its id and found by its repository
// and kind, so a page that lists one kind pays one `list_records`. Lake's PostgreSQL keeps none of
// them; the retired `forge_records` table is read only by the operator's one-shot copy.
struct ForgeRecordCodec {
    using Value = ForgeRecord;
    static constexpr RecordCollection collection{"forge_records", 1};

    static std::string lookup(const ForgeRecord& record);
    static nlohmann::json encode(const ForgeRecord& record);
    static std::optional<ForgeRecord> decode(const StoredRecord& record);
};

// The kinds a forge record can have: the retired table's `forge_records_kind_check`.
inline constexpr std::array<std::string_view, 10> forge_record_kinds = {
    "issue",   "issue_comment",    "review",            "review_comment", "release",
    "webhook", "webhook_delivery", "branch_protection", "export",         "github_import"};
bool is_forge_record_kind(std::string_view kind);

// The lookup of one repository's records of one kind.
std::string forge_record_lookup(const std::string& repository_id, std::string_view kind);

using ForgeRecords = RecordSet<ForgeRecordCodec>;

} // namespace lake::control
