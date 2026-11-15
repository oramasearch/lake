// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/workspace_records.hpp"

namespace lake::control {

// An agent's stream trigger (lake#677, docs/api/agent-launches.md#stream-triggers): one workspace
// stream (a topic of the workspace Zetta) whose new messages each start a run of the agent, with
// the message as the run's input. It is workspace data, so it lives in the workspace Zetta of the
// agent, in the sealed collection `lake_agent_stream_triggers`, found by its account; nothing of it
// is in lake's Postgres. The run acts as the person who set the trigger (`set_by`), as a schedule's
// runs act as whoever set the schedule.
struct AgentStreamTrigger {
    std::string id; // the record's key
    std::string customer_id;
    std::string agent_id;
    std::string agent_name;
    std::string topic; // a topic of the workspace Zetta
    // The last sequence that started a run (or that was already on the stream when the trigger was
    // set): the next run starts from the message after it.
    std::uint64_t cursor = 0;
    std::string set_by;          // the user id the runs act as
    std::string set_by_identity; // their identity, for the record
    std::string created_at;      // "YYYY-MM-DDTHH:MM:SSZ"
    std::int64_t at_us = 0;
};

struct AgentStreamTriggerCodec {
    using Value = AgentStreamTrigger;
    static constexpr RecordCollection collection{"agent_stream_triggers", 1};

    static std::string lookup(const AgentStreamTrigger& trigger);
    static nlohmann::json encode(const AgentStreamTrigger& trigger);
    static std::optional<AgentStreamTrigger> decode(const StoredRecord& record);
};

// The lookup of an account's stream triggers in one workspace.
std::string agent_stream_triggers_lookup(const std::string& customer_id);

// Every stream trigger of the account in this workspace, oldest first; with `agent_id`, only that
// agent's.
liquid::runtime::task<ZettaControlResult<std::vector<AgentStreamTrigger>>>
list_agent_stream_triggers(WorkspaceRecords& records,
                           std::string customer_id,
                           std::string agent_id = {});

// Appends one trigger under a new id.
liquid::runtime::task<ZettaControlResult<AgentStreamTrigger>>
record_agent_stream_trigger(WorkspaceRecords& records, AgentStreamTrigger trigger);

// Moves a trigger's cursor forward to `cursor` (never back), rereading on a conflict. Nothing
// happens to a trigger that was removed meanwhile.
liquid::runtime::task<ZettaControlResult<void>> advance_agent_stream_trigger(
    WorkspaceRecords& records, std::string customer_id, std::string id, std::uint64_t cursor);

// Removes a trigger; one that is already gone is fine.
liquid::runtime::task<ZettaControlResult<void>>
remove_agent_stream_trigger(WorkspaceRecords& records, std::string customer_id, std::string id);

} // namespace lake::control
