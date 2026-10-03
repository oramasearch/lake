#include "agent_task_records_api.hpp"

#include <utility>

#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

template <class T> control::ZettaControlResult<T> failed(control::ZettaControlError error) {
    return control::ZettaControlResult<T>(std::unexpected(std::move(error)));
}

control::ZettaControlResult<control::WorkspaceRecords> task_records(const ApiContext& context,
                                                                    const control::AgentTask& task) {
    if (task.workspace_id.empty()) {
        // A task from before 0038 that names no workspace has nowhere to keep its content.
        return std::unexpected(control::ZettaControlError{
            "workspace_not_ready", "the agent task names no workspace", "/workspace_id"});
    }
    return workspace_records(context, task.customer_id, task.workspace_id);
}

} // namespace

AgentTaskTask<bool> load_agent_task_content(const ApiContext& context, control::AgentTask& task) {
    auto records = task_records(context, task);
    if (!records) {
        co_return failed<bool>(records.error());
    }
    auto found = control::settled(
        co_await control::get_agent_task_content(*records, task.id, task.customer_id));
    if (!found) {
        co_return failed<bool>(found.error());
    }
    if (!*found) {
        co_return control::ZettaControlResult<bool>(false);
    }
    const control::AgentTaskContent& content = **found;
    task.body = content.request.is_object() ? content.request : nlohmann::json::object();
    task.result = content.result;
    if (task.message.empty()) {
        task.message = content.message;
    }
    co_return control::ZettaControlResult<bool>(true);
}

AgentTaskTask<control::AgentTask> agent_task_with_content(const ApiContext& context,
                                                          std::string task_id) {
    if (context.store == nullptr) {
        co_return failed<control::AgentTask>(
            {"store_unavailable", "no control-plane store is configured"});
    }
    auto task = context.store->get_agent_task(task_id);
    if (!task) {
        co_return failed<control::AgentTask>({task.error().code, task.error().message});
    }
    auto loaded = control::settled(co_await load_agent_task_content(context, *task));
    if (!loaded) {
        co_return failed<control::AgentTask>(loaded.error());
    }
    co_return control::ZettaControlResult<control::AgentTask>(std::move(*task));
}

AgentTaskTask<void> write_agent_task_request(const ApiContext& context,
                                             const control::AgentTask& task) {
    auto records = task_records(context, task);
    if (!records) {
        co_return failed<void>(records.error());
    }
    auto written = control::settled(co_await control::create_agent_task_content(
        *records, control::AgentTaskContent{.task_id = task.id,
                                            .customer_id = task.customer_id,
                                            .agent = task.agent,
                                            .request = task.body.is_object()
                                                           ? task.body
                                                           : nlohmann::json::object()}));
    if (!written) {
        co_return failed<void>(written.error());
    }
    co_return control::ZettaControlResult<void>();
}

AgentTaskTask<void> write_agent_task_event(const ApiContext& context,
                                           const control::AgentTask& task, std::string phase,
                                           std::string message, nlohmann::json result) {
    auto records = task_records(context, task);
    if (!records) {
        co_return failed<void>(records.error());
    }
    auto written = control::settled(co_await control::record_agent_task_content(
        *records, control::AgentTaskContent{.task_id = task.id,
                                            .customer_id = task.customer_id,
                                            .agent = task.agent,
                                            .message = std::move(message),
                                            .result = std::move(result),
                                            .phase = phase == "Running" ? std::string()
                                                                        : std::move(phase)}));
    if (!written) {
        co_return failed<void>(written.error());
    }
    co_return control::ZettaControlResult<void>();
}

} // namespace lake::api
