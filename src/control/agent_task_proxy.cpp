#include "lake/control/agent_task_proxy.hpp"

#include <filesystem>
#include <format>
#include <system_error>

#include "lake/control/zetta_workspace.hpp"

namespace lake::control {

AgentBusCredentials::AgentBusCredentials(std::string directory)
    : directory_(std::move(directory)), tokens_(std::make_shared<const Tokens>()) {}

std::string AgentBusCredentials::signature() const {
    if (directory_.empty()) {
        return {};
    }
    std::error_code error;
    const auto modified = std::filesystem::last_write_time(directory_, error);
    if (error) {
        return "absent";
    }
    std::string signature =
        std::format("{}", modified.time_since_epoch().count());
    const auto data = std::filesystem::read_symlink(std::filesystem::path(directory_) / "..data",
                                                    error);
    if (!error) {
        signature += "\n" + data.string();
    }
    return signature;
}

std::shared_ptr<const AgentBusCredentials::Tokens> AgentBusCredentials::current() {
    const std::string now = signature();
    std::lock_guard lock(mutex_);
    if (!read_ || now != signature_) {
        tokens_ = std::make_shared<const Tokens>(read_agent_bus_tokens(directory_));
        signature_ = now;
        read_ = true;
    }
    return tokens_;
}

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
