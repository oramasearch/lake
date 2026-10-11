#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/workspace_records.hpp"

namespace lake::control {

// The revisions of an agent's definition (lake#653, docs/api/train-agents.md#definition-revisions):
// what a person designs in Design → Agents (its purpose, prompt, skills, model, pipelines, work
// area and trigger). The workspace engine keeps one record per agent and overwrites it on every
// update_agent, so lake keeps each definition it wrote: one sealed, append-only record per
// create_agent or update_agent in the collection `lake_agent_revisions` of the agent's workspace
// Zetta, found by the account and the agent's id. A person reads them as the agent's history and
// restores one by calling update_agent with its definition, which writes a new revision.
//
// Revisions are not the engine's trained versions (`agent_training_status`'s `versions`, the
// checkpoints training produced), which Train → Agents promotes and rolls back.
struct AgentRevision {
    std::string id; // the record's key
    std::string customer_id;
    std::string agent_id;      // the engine's agent id
    std::string change;        // "create" or "update"
    nlohmann::json definition; // the definition fields the engine answered (agent_definition_of)
    std::string created_by;    // the user id (or the operator's identity) that wrote it
    std::string created_at;    // "YYYY-MM-DDTHH:MM:SSZ"
};

struct AgentRevisionCodec {
    using Value = AgentRevision;
    static constexpr RecordCollection collection{"agent_revisions", 1};

    static std::string lookup(const AgentRevision& revision);
    static nlohmann::json encode(const AgentRevision& revision);
    static std::optional<AgentRevision> decode(const StoredRecord& record);
};

// The lookup of one agent's revisions in an account.
std::string agent_revisions_lookup(const std::string& customer_id, const std::string& agent_id);

// The fields of an engine agent record that a person designs, in the shape create_agent and
// update_agent take: everything but its id, its status, its training (set_training_schedule), its
// trained versions and its timestamps.
nlohmann::json agent_definition_of(const nlohmann::json& agent);

// Appends one revision of an agent.
liquid::runtime::task<ZettaControlResult<AgentRevision>>
record_agent_revision(WorkspaceRecords& records, AgentRevision revision);

// An agent's revisions in the account, oldest first, at most the newest `newest` (0: all).
liquid::runtime::task<ZettaControlResult<std::vector<AgentRevision>>>
list_agent_revisions(WorkspaceRecords& records, std::string customer_id, std::string agent_id,
                     std::size_t newest = 0);

} // namespace lake::control
