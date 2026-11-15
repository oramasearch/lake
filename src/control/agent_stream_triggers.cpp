// SPDX-License-Identifier: MIT

#include "lake/control/agent_stream_triggers.hpp"

#include <algorithm>
#include <utility>

namespace lake::control {
namespace {

using json = nlohmann::json;

std::string text(const json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}

} // namespace

std::string agent_stream_triggers_lookup(const std::string& customer_id) {
    return "customer:" + customer_id;
}

std::string AgentStreamTriggerCodec::lookup(const AgentStreamTrigger& trigger) {
    return agent_stream_triggers_lookup(trigger.customer_id);
}

json AgentStreamTriggerCodec::encode(const AgentStreamTrigger& trigger) {
    return {{"customer_id", trigger.customer_id},
            {"agent_id", trigger.agent_id},
            {"agent_name", trigger.agent_name},
            {"topic", trigger.topic},
            {"cursor", trigger.cursor},
            {"set_by", trigger.set_by},
            {"set_by_identity", trigger.set_by_identity}};
}

std::optional<AgentStreamTrigger> AgentStreamTriggerCodec::decode(const StoredRecord& record) {
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    const json& body = record.body;
    AgentStreamTrigger trigger;
    trigger.id = record.id;
    trigger.customer_id = text(body, "customer_id");
    trigger.agent_id = text(body, "agent_id");
    trigger.agent_name = text(body, "agent_name");
    trigger.topic = text(body, "topic");
    if (const auto found = body.find("cursor");
        found != body.end() && found->is_number_unsigned()) {
        trigger.cursor = found->get<std::uint64_t>();
    }
    trigger.set_by = text(body, "set_by");
    trigger.set_by_identity = text(body, "set_by_identity");
    trigger.created_at = record_instant(record.at_us);
    trigger.at_us = record.at_us;
    if (trigger.customer_id.empty() || trigger.agent_id.empty() || trigger.topic.empty()) {
        return std::nullopt;
    }
    return trigger;
}

liquid::runtime::task<ZettaControlResult<std::vector<AgentStreamTrigger>>>
list_agent_stream_triggers(WorkspaceRecords& records,
                           std::string customer_id,
                           std::string agent_id) {
    RecordSet<AgentStreamTriggerCodec> set(records);
    auto listed = settled(co_await set.list(agent_stream_triggers_lookup(customer_id)));
    if (!listed) {
        co_return ZettaControlResult<std::vector<AgentStreamTrigger>>(
            std::unexpected(listed.error()));
    }
    std::erase_if(*listed, [&](const AgentStreamTrigger& trigger) {
        return trigger.customer_id != customer_id ||
               (!agent_id.empty() && trigger.agent_id != agent_id);
    });
    co_return ZettaControlResult<std::vector<AgentStreamTrigger>>(std::move(*listed));
}

liquid::runtime::task<ZettaControlResult<AgentStreamTrigger>>
record_agent_stream_trigger(WorkspaceRecords& records, AgentStreamTrigger trigger) {
    if (trigger.customer_id.empty() || trigger.agent_id.empty() || trigger.topic.empty()) {
        co_return ZettaControlResult<AgentStreamTrigger>(std::unexpected(
            ZettaControlError{"invalid_input", "a stream trigger names an agent and a stream"}));
    }
    RecordSet<AgentStreamTriggerCodec> set(records);
    co_return settled(co_await set.append(std::move(trigger)));
}

liquid::runtime::task<ZettaControlResult<void>> advance_agent_stream_trigger(
    WorkspaceRecords& records, std::string customer_id, std::string id, std::uint64_t cursor) {
    auto updated = settled(co_await records.update(
        AgentStreamTriggerCodec::collection,
        id,
        agent_stream_triggers_lookup(customer_id),
        [&](const std::optional<StoredRecord>& current) -> std::optional<json> {
            if (!current) {
                return std::nullopt; // removed meanwhile: stays absent
            }
            auto trigger = AgentStreamTriggerCodec::decode(*current);
            if (!trigger || trigger->customer_id != customer_id) {
                return std::optional<json>(std::in_place,
                                           current->body); // not this account's: unchanged
            }
            trigger->cursor = std::max(trigger->cursor, cursor);
            return std::optional<json>(std::in_place, AgentStreamTriggerCodec::encode(*trigger));
        }));
    if (!updated) {
        co_return ZettaControlResult<void>(std::unexpected(updated.error()));
    }
    co_return ZettaControlResult<void>();
}

liquid::runtime::task<ZettaControlResult<void>>
remove_agent_stream_trigger(WorkspaceRecords& records, std::string customer_id, std::string id) {
    auto got = settled(co_await records.get(AgentStreamTriggerCodec::collection, id));
    if (!got) {
        co_return ZettaControlResult<void>(std::unexpected(got.error()));
    }
    if (!*got) {
        co_return ZettaControlResult<void>();
    }
    const auto trigger = AgentStreamTriggerCodec::decode(**got);
    if (!trigger || trigger->customer_id != customer_id) {
        co_return ZettaControlResult<void>(std::unexpected(
            ZettaControlError{"not_found", "no such stream trigger in this workspace"}));
    }
    auto removed =
        settled(co_await records.remove(AgentStreamTriggerCodec::collection, id, (*got)->revision));
    if (!removed && removed.error().code != "not_found") {
        co_return ZettaControlResult<void>(std::unexpected(removed.error()));
    }
    co_return ZettaControlResult<void>();
}

} // namespace lake::control
