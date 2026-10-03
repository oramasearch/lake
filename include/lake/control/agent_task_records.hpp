#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/workspace_records.hpp"

namespace lake::control {

// What was written for one background agent turn (lake#374, lake#375): the request the agent
// receives when it claims the task, the agent's latest message, and its answer. It lives in the
// workspace Zetta of the task's workspace (docs/architecture/workspace-records.md), one sealed
// record per task in lake_agent_tasks, keyed by the task id and found by its space
// (`customer:<id>`). Lake's PostgreSQL keeps only the content-free dispatch rows (agent_tasks and
// agent_task_status: ids, space, workspace, agent, kind, scopes, phase, claim, lease, attempts,
// the asking task and lake's own notices), which the bus, the leases and the claim use without
// reading a workspace.
//
// `request` is the body handed to the agent at its claim, with every "{{credential}}" still in
// place: the credential is never stored. `phase` is empty while the task runs and Succeeded or
// Failed once the agent finished it: a finished record is never changed again, so the first
// terminal write PostgreSQL accepted is the one the workspace keeps. A record written for an
// event of a task whose request was not copied yet holds a null `request`, which the one-shot
// copy fills in.
struct AgentTaskContent {
    std::string task_id, customer_id, agent;
    nlohmann::json request = nullptr;
    std::string message;
    nlohmann::json result = nullptr;
    std::string phase;
    std::int64_t at_us = 0;          // the task's creation instant in microseconds
    std::int64_t revision = 0;       // the engine revision (pass it as if_revision)
    std::int64_t lake_revision = 1;  // 1 as created, one more for each event recorded
};

struct AgentTaskContentCodec {
    using Value = AgentTaskContent;
    static constexpr RecordCollection collection{"agent_tasks", 1};

    static std::string lookup(const AgentTaskContent& content);
    static nlohmann::json encode(const AgentTaskContent& content);
    static std::optional<AgentTaskContent> decode(const StoredRecord& record);
};

// The lookup of a space's agent task records.
std::string agent_task_content_lookup(const std::string& customer_id);

// A task's record; nullopt when there is none here (or it belongs to another task or space, or
// does not open in this workspace). An empty `customer_id` accepts any space: only the one-shot
// copy reads that way, and its verify compares the space.
liquid::runtime::task<ZettaControlResult<std::optional<AgentTaskContent>>>
get_agent_task_content(WorkspaceRecords& records, std::string task_id, std::string customer_id);

// Writes a new task's record (or a copied one) only where none exists: `conflict` otherwise.
// `content.at_us` 0 is now.
liquid::runtime::task<ZettaControlResult<AgentTaskContent>>
create_agent_task_content(WorkspaceRecords& records, AgentTaskContent content);

// Records what an event PostgreSQL accepted said: the message, and for a finished task (phase
// Succeeded or Failed) its answer and phase. A write conditional on the revision read, reread on
// a conflict. A finished record is left as it is: the answer it holds was accepted first. A task
// without a record yet (one created before the move, not copied) gets one with a null request.
liquid::runtime::task<ZettaControlResult<AgentTaskContent>>
record_agent_task_content(WorkspaceRecords& records, AgentTaskContent event, int attempts = 5);

// Gives a record without a request the request of its task (the one-shot copy, for a task whose
// events were recorded before its request was copied). A request already there is kept.
liquid::runtime::task<ZettaControlResult<AgentTaskContent>>
fill_agent_task_request(WorkspaceRecords& records, std::string task_id, std::string customer_id,
                        nlohmann::json request, int attempts = 5);

} // namespace lake::control
