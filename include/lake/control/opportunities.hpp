// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/store.hpp"
#include "lake/control/workspace_records.hpp"

namespace lake::control {

// Design → Opportunities (docs/api/discover.md, lake#376): what Auto-Suggest Agent recommended and what
// the person decided, a mutable collection on WorkspaceRecords. An Opportunity lives in the
// workspace Zetta of the workspace its scan ran in, found by its space, so another workspace of
// the same space never sees it. Its key is derived from its scan and rank, so a scan is saved once
// however many readers settle it, and a status change is a conditional write on the revision read.
struct OpportunityCodec {
    using Value = Opportunity;
    static constexpr RecordCollection collection{"opportunities", 1};

    static std::string lookup(const Opportunity& opportunity);
    static nlohmann::json encode(const Opportunity& opportunity);
    static std::optional<Opportunity> decode(const StoredRecord& record);
};

using Opportunities = RecordSet<OpportunityCodec>;

// The lookup key of a space's Opportunities.
std::string opportunities_lookup(const std::string& customer_id);

// The record key of a scan's Opportunity at `rank`: a UUID (version 8) derived from both.
std::string opportunity_id(const std::string& task_id, int rank);

// Refuses an Opportunity without a space, a scan or an intent, or with an unknown status.
std::optional<std::string> opportunity_problem(const Opportunity& opportunity);

// Newest scan first, each scan in its rank order.
void sort_opportunities(std::vector<Opportunity>& opportunities);

// Saves a finished scan's Opportunities, each keyed by opportunity_id(task, rank) and written only
// if absent: a second settle of the same scan, on any replica, writes nothing. Answers how many it
// wrote.
liquid::runtime::task<ZettaControlResult<std::size_t>>
record_opportunities(WorkspaceRecords& records, std::vector<Opportunity> rows);

// Every Opportunity of the space in this workspace, sorted by sort_opportunities.
liquid::runtime::task<ZettaControlResult<std::vector<Opportunity>>>
list_opportunities(WorkspaceRecords& records, std::string customer_id);

// One Opportunity of the space by id; nullopt when this workspace holds none.
liquid::runtime::task<ZettaControlResult<std::optional<Opportunity>>>
get_opportunity(WorkspaceRecords& records, std::string customer_id, std::string id);

// Sets an Opportunity's status and project with a write conditional on the revision it read,
// rereading on a conflict. `not_found` when this workspace holds no such Opportunity of the space.
liquid::runtime::task<ZettaControlResult<Opportunity>>
set_opportunity_status(WorkspaceRecords& records, std::string customer_id, std::string id,
                       std::string status, std::string project);

} // namespace lake::control
