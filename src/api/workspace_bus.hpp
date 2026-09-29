#pragma once

#include <string>
#include <utility>

#include "lake/api/capabilities.hpp"
#include "lake/control/agent_bus.hpp"

namespace lake::api {

class WorkspaceApiBus final : public control::AgentBus {
public:
    WorkspaceApiBus(const ApiContext& context, std::string customer_id, std::string workspace_id)
        : context_(context), customer_id_(std::move(customer_id)) {
        context_.workspace_id = std::move(workspace_id);
    }

    liquid::runtime::task<control::ZettaControlResult<nlohmann::json>>
    call(std::string capability, nlohmann::json input) override {
        if (!context_.workspaces) {
            co_return control::ZettaControlResult<nlohmann::json>(std::unexpected(
                control::ZettaControlError{
                    "workspace_not_ready", "a workspace Zetta is required"}));
        }
        const bool read = capability == "receive_events";
        co_return control::settled(co_await call_workspace(
            context_, customer_id_, std::move(capability), read, std::move(input)));
    }

private:
    ApiContext context_;
    std::string customer_id_;
};

} // namespace lake::api
