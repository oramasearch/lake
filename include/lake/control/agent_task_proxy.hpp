#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "lake/control/store.hpp"

namespace lake::control {

// Task credentials by reference (lake#871, docs/api/agent-bus.md#task-credentials-by-reference).
//
// A library agent that claims a task by reference never receives the task's credential. It calls
// Lake with its own bus credential as the bearer and names the task in this header; Lake checks
// that the task is running on that agent (and in the bus credential's workspace, when it is bound
// to one) and acts as the task's credential for that one request. A compromised agent process
// therefore holds no credential it can take elsewhere, and can act only for the tasks it is
// serving at that moment, only through Lake.
inline constexpr std::string_view agent_task_header = "x-lake-agent-task";

// What a file in LAKE_AGENT_BUS_DIR names. `agent-project` is the shared credential of that agent,
// for every workspace. `agent-project.<workspace id>` is a credential bound to one workspace, for
// an agent process that serves only that workspace (an enterprise or isolated install): it
// receives, claims, reports and acts only for that workspace's tasks, and always claims by
// reference.
struct AgentBusKey {
    std::string agent;
    std::string workspace_id;

    bool bound() const {
        return !workspace_id.empty();
    }
};

// Splits a LAKE_AGENT_BUS_DIR file name at its first '.'. Agent keys hold no '.'.
AgentBusKey agent_bus_key(std::string_view name);

// LAKE_AGENT_BUS_DIR, re-read whenever it changes, so the operator adds or removes a workspace's
// bound credential without restarting lake. Every lookup compares the directory's signature (its
// modification time, and the target of Kubernetes' `..data` link, which a Secret update swaps)
// with the last one read, and reads the files again when it differs: a file added is accepted, and
// a file removed stops working, on the next request after the change reaches the directory. A
// file edited in place without either changing is not noticed; Kubernetes never writes that way.
class AgentBusCredentials {
public:
    using Tokens = std::map<std::string, std::string>;

    explicit AgentBusCredentials(std::string directory);

    // The credentials as the directory holds them now (file name to token).
    std::shared_ptr<const Tokens> current();

private:
    std::string signature() const;

    std::string directory_;
    std::mutex mutex_;
    std::string signature_;
    bool read_ = false;
    std::shared_ptr<const Tokens> tokens_;
};

// The principal of the task `task_id` for one request of the agent `caller`: the same principal
// the task's own credential resolves to, including the stored claim digest that every call's lease
// renewal checks. Refused (`not_found`, "session is not active", as for a wrong credential) unless
// the task is present, claimed, Running and on that agent, and in the caller's workspace when the
// caller's credential is bound to one. `unauthorized` when the person who asked can no longer act.
Result<Principal> agent_task_principal(Store& store,
                                       const std::string& task_id,
                                       const AgentBusKey& caller);

} // namespace lake::control
