#include "lake/control/agent_task_records.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

namespace lake::control {
namespace {

template <class T> ZettaControlResult<T> failed(const ZettaControlError& error) {
    return ZettaControlResult<T>(std::unexpected(error));
}

std::int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool finished(const std::string& phase) {
    return phase == "Succeeded" || phase == "Failed";
}

ZettaControlResult<AgentTaskContent> decoded(const StoredRecord& record) {
    auto content = AgentTaskContentCodec::decode(record);
    if (!content) {
        return failed<AgentTaskContent>(
            ZettaControlError{"store_error", "the stored agent task did not decode"});
    }
    return std::move(*content);
}

// Reads the task's record and writes `change`'s next value of it conditionally on the revision
// read, rereading on a conflict. `change` answers nullopt to leave the record as it is (or
// absent); an absent record is written `if_absent`.
using ContentChange =
    std::function<std::optional<AgentTaskContent>(const std::optional<AgentTaskContent>&)>;

liquid::runtime::task<ZettaControlResult<AgentTaskContent>>
change_content(WorkspaceRecords& records, std::string task_id, std::string customer_id,
               ContentChange change, int attempts) {
    ZettaControlError last{"conflict", "the agent task kept changing"};
    for (int attempt = 0; attempt < std::max(attempts, 1); ++attempt) {
        auto found = settled(co_await get_agent_task_content(records, task_id, customer_id));
        if (!found) {
            co_return failed<AgentTaskContent>(found.error());
        }
        std::optional<AgentTaskContent> next = change(*found);
        if (!next) {
            if (*found) {
                co_return ZettaControlResult<AgentTaskContent>(std::move(**found));
            }
            co_return failed<AgentTaskContent>(
                ZettaControlError{"not_found", "the agent task has no record in its workspace"});
        }
        next->task_id = task_id;
        next->customer_id = customer_id;
        RecordCondition condition;
        std::int64_t lake_revision = 1;
        std::int64_t at_us = next->at_us > 0 ? next->at_us : now_us();
        if (*found) {
            condition.if_revision = (*found)->revision;
            lake_revision = (*found)->lake_revision + 1;
            at_us = (*found)->at_us;
        } else {
            condition.if_absent = true;
        }
        auto written = settled(co_await records.put(
            AgentTaskContentCodec::collection, task_id, AgentTaskContentCodec::lookup(*next),
            AgentTaskContentCodec::encode(*next), condition, lake_revision, at_us));
        if (written) {
            co_return decoded(*written);
        }
        if (written.error().code != "conflict") {
            co_return failed<AgentTaskContent>(written.error());
        }
        last = written.error(); // another writer won: reread and decide again
    }
    co_return failed<AgentTaskContent>(last);
}

} // namespace

std::string agent_task_content_lookup(const std::string& customer_id) {
    return "customer:" + customer_id;
}

std::string AgentTaskContentCodec::lookup(const AgentTaskContent& content) {
    return agent_task_content_lookup(content.customer_id);
}

nlohmann::json AgentTaskContentCodec::encode(const AgentTaskContent& content) {
    return {{"task_id", content.task_id},
            {"customer_id", content.customer_id},
            {"agent", content.agent},
            {"request", content.request},
            {"message", content.message},
            {"result", content.result},
            {"phase", content.phase}};
}

std::optional<AgentTaskContent> AgentTaskContentCodec::decode(const StoredRecord& record) {
    // Version 1 is the only body schema so far; a later version decodes its predecessors here.
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    const nlohmann::json& body = record.body;
    AgentTaskContent content;
    content.task_id = body.value("task_id", std::string());
    content.customer_id = body.value("customer_id", std::string());
    content.agent = body.value("agent", std::string());
    content.request = body.value("request", nlohmann::json());
    content.message = body.value("message", std::string());
    content.result = body.value("result", nlohmann::json());
    content.phase = body.value("phase", std::string());
    // Keyed by its task: a body under another key is not this task's.
    if (content.task_id.empty() || content.task_id != record.id || content.customer_id.empty() ||
        (!content.request.is_null() && !content.request.is_object()) ||
        (!content.phase.empty() && !finished(content.phase))) {
        return std::nullopt;
    }
    content.at_us = record.at_us;
    content.revision = record.revision;
    content.lake_revision = record.lake_revision;
    return content;
}

liquid::runtime::task<ZettaControlResult<std::optional<AgentTaskContent>>>
get_agent_task_content(WorkspaceRecords& records, std::string task_id, std::string customer_id) {
    using Found = std::optional<AgentTaskContent>;
    auto stored = settled(co_await records.get(AgentTaskContentCodec::collection, task_id));
    // Every failure is reported, a workspace that is gone included: absence is only what the
    // workspace says is absent.
    if (!stored) {
        co_return failed<Found>(stored.error());
    }
    if (!*stored) {
        co_return ZettaControlResult<Found>(Found{});
    }
    auto content = AgentTaskContentCodec::decode(**stored);
    if (!content || content->task_id != task_id ||
        (!customer_id.empty() && content->customer_id != customer_id)) {
        co_return ZettaControlResult<Found>(Found{});
    }
    co_return ZettaControlResult<Found>(std::move(content));
}

liquid::runtime::task<ZettaControlResult<AgentTaskContent>>
create_agent_task_content(WorkspaceRecords& records, AgentTaskContent content) {
    const std::int64_t at_us = content.at_us > 0 ? content.at_us : now_us();
    auto stored = settled(co_await records.put(
        AgentTaskContentCodec::collection, content.task_id, AgentTaskContentCodec::lookup(content),
        AgentTaskContentCodec::encode(content), RecordCondition{.if_absent = true}, 1, at_us));
    if (!stored) {
        co_return failed<AgentTaskContent>(stored.error());
    }
    co_return decoded(*stored);
}

liquid::runtime::task<ZettaControlResult<AgentTaskContent>>
record_agent_task_content(WorkspaceRecords& records, AgentTaskContent event, int attempts) {
    const std::string task_id = event.task_id;
    const std::string customer_id = event.customer_id;
    co_return co_await change_content(
        records, task_id, customer_id,
        [event = std::move(event)](const std::optional<AgentTaskContent>& current)
            -> std::optional<AgentTaskContent> {
            if (current && finished(current->phase)) {
                return std::nullopt; // the first accepted answer stays
            }
            AgentTaskContent next = current ? *current : AgentTaskContent{};
            if (!current) {
                next.agent = event.agent;
            }
            next.message = event.message;
            if (finished(event.phase)) {
                next.phase = event.phase;
                next.result = event.result;
            }
            return next;
        },
        attempts);
}

} // namespace lake::control
