#include "lake/control/agent_revisions.hpp"

#include <array>
#include <string_view>
#include <utility>

namespace lake::control {

std::string agent_revisions_lookup(const std::string& customer_id, const std::string& agent_id) {
    return "customer:" + customer_id + "/agent:" + agent_id;
}

std::string AgentRevisionCodec::lookup(const AgentRevision& revision) {
    return agent_revisions_lookup(revision.customer_id, revision.agent_id);
}

nlohmann::json AgentRevisionCodec::encode(const AgentRevision& revision) {
    return {{"customer_id", revision.customer_id},
            {"agent_id", revision.agent_id},
            {"change", revision.change},
            {"definition", revision.definition},
            {"created_by", revision.created_by}};
}

std::optional<AgentRevision> AgentRevisionCodec::decode(const StoredRecord& record) {
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    AgentRevision revision;
    revision.id = record.id;
    revision.customer_id = record.body.value("customer_id", std::string());
    revision.agent_id = record.body.value("agent_id", std::string());
    revision.change = record.body.value("change", std::string());
    revision.definition = record.body.value("definition", nlohmann::json::object());
    revision.created_by = record.body.value("created_by", std::string());
    revision.created_at = record_instant(record.at_us);
    if (revision.customer_id.empty() || revision.agent_id.empty() ||
        !revision.definition.is_object()) {
        return std::nullopt;
    }
    return revision;
}

nlohmann::json agent_definition_of(const nlohmann::json& agent) {
    // The fields create_agent and update_agent take that a person designs (Zetta's
    // create_agent schema), less `status`, which the engine moves as the agent works.
    static constexpr std::array<std::string_view, 21> designed = {
        "name",         "description", "purpose",  "kind",        "runtime",
        "work_area",    "manifesto",   "goal",     "goals",       "skills",
        "trigger",      "model",       "model_name", "model_version", "system_prompt",
        "tools",        "pipelines",   "definition", "endpoint",  "chat_capable",
        "is_default"};
    nlohmann::json definition = nlohmann::json::object();
    if (!agent.is_object()) {
        return definition;
    }
    for (std::string_view field : designed) {
        const std::string key(field);
        if (const auto found = agent.find(key); found != agent.end() && !found->is_null()) {
            definition[key] = *found;
        }
    }
    return definition;
}

liquid::runtime::task<ZettaControlResult<AgentRevision>>
record_agent_revision(WorkspaceRecords& records, AgentRevision revision) {
    if (revision.customer_id.empty() || revision.agent_id.empty()) {
        co_return ZettaControlResult<AgentRevision>(
            std::unexpected(ZettaControlError{"invalid_input", "the revision names no agent"}));
    }
    RecordSet<AgentRevisionCodec> set(records);
    co_return settled(co_await set.append(std::move(revision)));
}

liquid::runtime::task<ZettaControlResult<std::vector<AgentRevision>>>
list_agent_revisions(WorkspaceRecords& records, std::string customer_id, std::string agent_id,
                     std::size_t newest) {
    RecordSet<AgentRevisionCodec> set(records);
    auto listed = settled(co_await set.list(agent_revisions_lookup(customer_id, agent_id), newest));
    if (!listed) {
        co_return ZettaControlResult<std::vector<AgentRevision>>(std::unexpected(listed.error()));
    }
    // A record copied in from another space or agent does not open under this lookup, but say so
    // twice: only this agent's revisions in this space.
    std::erase_if(*listed, [&](const AgentRevision& revision) {
        return revision.customer_id != customer_id || revision.agent_id != agent_id;
    });
    co_return ZettaControlResult<std::vector<AgentRevision>>(std::move(*listed));
}

} // namespace lake::control
