#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/store.hpp"
#include "lake/control/zetta_control.hpp"

// Agent traffic over Zetta pub/sub (docs/api/agent-bus.md, lake#311).
//
//   Lake bus: agent-tasks.<agent> carries {task_id, customer_id, agent, kind}.
//   Task workspace: agent-events carries progress and results, published through Lake.
//
// Every event's correlation id is the task id. No task body and no credential is ever on the bus:
// an agent claims a task through lake, which hands it the body with a fresh `lat.` credential.
// Agents never hold the bus's token; they reach it through lake's capabilities.
namespace lake::control {

class WorkspaceService;

inline constexpr std::string_view agent_events_topic = "agent-events";

// A co_await'ed task's own failure (the coroutine machinery's) folded into the call's result.
template <class T>
ZettaControlResult<T> settled(std::expected<ZettaControlResult<T>, liquid::error> completed) {
    if (!completed) {
        return std::unexpected(ZettaControlError{"store_unavailable", completed.error().message});
    }
    return std::move(*completed);
}
std::string agent_tasks_topic(std::string_view agent);

struct BusEvent {
    std::uint64_t sequence = 0;
    std::string id, correlation_id;
    nlohmann::json payload; // the event's JSON payload; null when it was not JSON
    std::chrono::milliseconds published_at{0};
};
struct BusBatch {
    std::vector<BusEvent> events;
    std::uint64_t next_sequence = 1;
};

// The pub/sub calls lake makes: Zetta's create_topic, publish_event and receive_events.
class AgentBus {
public:
    virtual ~AgentBus() = default;
    // Non-consuming stream inspection through the same service credential.
    virtual liquid::runtime::task<ZettaControlResult<nlohmann::json>>
    call(std::string capability, nlohmann::json input);
    // Creates an exposed topic; one that already exists is fine.
    virtual liquid::runtime::task<ZettaControlResult<void>> create_topic(std::string topic);
    // Returns the event id.
    virtual liquid::runtime::task<ZettaControlResult<std::string>>
    publish(std::string topic, nlohmann::json payload, std::string correlation_id);
    // Events from `from` on, oldest first, waiting up to `wait` when there are none yet.
    virtual liquid::runtime::task<ZettaControlResult<BusBatch>>
    receive(std::string topic, std::uint64_t from, int max, std::chrono::milliseconds wait);
};

// The bus on a Zetta node, over its REST capabilities.
class ZettaAgentBus final : public AgentBus {
public:
    ZettaAgentBus(std::string url, std::string token);
    liquid::runtime::task<ZettaControlResult<nlohmann::json>>
    call(std::string capability, nlohmann::json input) override;
private:
    std::string url_, token_;
};

// Event traffic always uses the object's persisted workspace identity.
class WorkspaceAgentBus final : public AgentBus {
public:
    WorkspaceAgentBus(WorkspaceService* workspaces, std::string customer_id,
                      std::string workspace_id);
    liquid::runtime::task<ZettaControlResult<nlohmann::json>>
    call(std::string capability, nlohmann::json input) override;

private:
    WorkspaceService* workspaces_;
    std::string customer_id_, workspace_id_;
};

// The same contract in memory, for tests and a lake run with --memory-store. It never waits.
class MemoryAgentBus final : public AgentBus {
public:
    liquid::runtime::task<ZettaControlResult<nlohmann::json>>
    call(std::string capability, nlohmann::json input) override;
    liquid::runtime::task<ZettaControlResult<void>> create_topic(std::string topic) override;
    liquid::runtime::task<ZettaControlResult<std::string>>
    publish(std::string topic, nlohmann::json payload, std::string correlation_id) override;
    liquid::runtime::task<ZettaControlResult<BusBatch>>
    receive(std::string topic,
            std::uint64_t from,
            int max,
            std::chrono::milliseconds wait) override;
    // Everything published to a topic, for assertions.
    std::vector<BusEvent> events(const std::string& topic) const;
    std::size_t call_count(std::string_view capability) const;
    void advance_time(std::chrono::milliseconds elapsed);

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<BusEvent>> topics_;
    std::map<std::string, std::size_t> call_counts_;
    std::map<std::string, std::uint64_t> sequences_;
    std::chrono::milliseconds now_{0};
    void trim(const std::string& topic);
    std::uint64_t next_id_ = 1;
};

liquid::runtime::task<ZettaControlResult<std::size_t>>
reap_workspace_topics(Store& store, WorkspaceService& workspaces,
                      std::chrono::system_clock::time_point now = std::chrono::system_clock::now());
bool build_topic_expired(const Build& build,
                         std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

// How long lake waits before it re-publishes a task nobody claimed (the bus's restart survival is
// best effort, so a lost event must not lose the task).
inline constexpr std::chrono::seconds agent_task_republish_after{60};

// Lake's side of the bus: publishes tasks, applies what agents publish to agent_task_status, and
// recovers what the bus lost. Every lake replica runs one; applying an event twice changes nothing.
class AgentTaskBus {
public:
    // `sweep_every`: how often a pass also looks for unclaimed and overdue tasks.
    AgentTaskBus(std::shared_ptr<Store> store,
                 std::shared_ptr<AgentBus> bus,
                 std::vector<std::string> agents,
                 std::chrono::seconds sweep_every = std::chrono::seconds(15));

    AgentBus& bus() {
        return *bus_;
    }
    const std::vector<std::string>& agents() const {
        return agents_;
    }
    // Creates dispatch topics and removes obsolete exposed bus topics with a dry-run log first.
    liquid::runtime::task<ZettaControlResult<void>> start();
    liquid::runtime::task<ZettaControlResult<void>> reap();
    std::size_t reaped_topics() const {
        return reaped_topics_.load();
    }
    // Publishes a task to its agent's topic and records when.
    liquid::runtime::task<ZettaControlResult<void>> publish_task(AgentTask task);
    // Re-publishes unclaimed tasks and fails overdue ones. Event publication applies status
    // directly; this loop never reads a shared event topic. Returns zero events applied.
    liquid::runtime::task<ZettaControlResult<std::size_t>> pump(std::chrono::milliseconds wait);

    // One agent event applied to the store: progress keeps the task running with its message,
    // result and failed finish it. Refused for a task that agent did not claim or that finished.
    static Result<AgentTask> apply(Store& store, const nlohmann::json& event);

private:
    std::shared_ptr<Store> store_;
    std::shared_ptr<AgentBus> bus_;
    std::vector<std::string> agents_;
    std::atomic<std::size_t> reaped_topics_{0};
    std::chrono::seconds sweep_every_;
    std::chrono::steady_clock::time_point last_sweep_{};
};

} // namespace lake::control
