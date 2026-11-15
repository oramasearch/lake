#include "lake/control/agent_task_proxy.hpp"

namespace lake::control {

AgentBusKey agent_bus_key(std::string_view name) {
    const std::size_t dot = name.find('.');
    if (dot == std::string_view::npos) {
        return {std::string(name), ""};
    }
    return {std::string(name.substr(0, dot)), std::string(name.substr(dot + 1))};
}

Result<Principal> agent_task_principal(Store& store,
                                       const std::string& task_id,
                                       const AgentBusKey& caller) {
    const auto inactive = [] {
        return std::unexpected(StoreError{"not_found", "session is not active"});
    };
    if (task_id.empty() || caller.agent.empty()) {
        return inactive();
    }
    const auto task = store.get_agent_task(task_id);
    if (!task || task->agent != caller.agent || task->desired != "present" ||
        task->phase != "Running" || task->credential_sha256.empty()) {
        return inactive();
    }
    if (caller.bound() && task->workspace_id != caller.workspace_id) {
        return inactive();
    }
    const auto user = store.get_user(task->requested_by_user_id);
    if (!user || user->taap_id.empty() || !user->may_act()) {
        return std::unexpected(StoreError{"unauthorized", "task identity is unavailable"});
    }
    return Principal{user->id,
                     user->taap_id,
                     "task:" + task->id,
                     "",
                     "lake",
                     "agent_task",
                     task->scopes,
                     std::nullopt,
                     task->id,
                     "",
                     "",
                     "",
                     task->credential_sha256};
}

} // namespace lake::control
