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

// Agent traffic over Zetta pub/sub (docs/api/agent-bus.md, lake#269). Lake's own Zetta (the Lake's
// spec.zetta, LAKE_DATA_PLANE_URL) carries it:
//
//   topic:agent-tasks.<agent>  lake → the agent: {task_id, customer_id, agent, kind}
//   topic:agent-events         the agent → lake: {task_id, agent, type, message, result}
//
// Every event's correlation id is the task id. No task body and no credential is ever on the bus:
// an agent claims a task through lake, which hands it the body with a fresh `lat.` credential.
// Agents never hold the bus's token; they reach it through lake's capabilities.
namespace lake::control {

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
    virtual liquid::runtime::task<ZettaControlResult<void>> create_topic(std::string topic) = 0;
    // Returns the event id.
    virtual liquid::runtime::task<ZettaControlResult<std::string>>
    publish(std::string topic, nlohmann::json payload, std::string correlation_id) = 0;
    // Events from `from` on, oldest first, waiting up to `wait` when there are none yet.
    virtual liquid::runtime::task<ZettaControlResult<BusBatch>>
    receive(std::string topic, std::uint64_t from, int max, std::chrono::milliseconds wait) = 0;
};

// The bus on a Zetta node, over its REST capabilities.
class ZettaAgentBus final : public AgentBus {
public:
    ZettaAgentBus(std::string url, std::string token);
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

private:
    std::string url_, token_;
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

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<BusEvent>> topics_;
    std::map<std::string, std::size_t> call_counts_;
    std::uint64_t next_id_ = 1;
};

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
    // The agent-events sequence this replica's loop reads next: every event published after
    // now comes at or after it, so a task claimed now has its events from here on (lake#275).
    std::uint64_t cursor() const {
        return cursor_.load();
    }
    // Creates agent-events and one agent-tasks.<agent> topic per library agent.
    liquid::runtime::task<ZettaControlResult<void>> start();
    // Publishes a task to its agent's topic and records when.
    liquid::runtime::task<ZettaControlResult<void>> publish_task(AgentTask task);
    // One pass: applies every agent event that arrived since the last pass (waiting up to `wait`
    // for one), then re-publishes unclaimed tasks and fails overdue ones. Returns the events
    // applied.
    liquid::runtime::task<ZettaControlResult<std::size_t>> pump(std::chrono::milliseconds wait);

    // One agent event applied to the store: progress keeps the task running with its message,
    // result and failed finish it. Refused for a task that agent did not claim or that finished.
    static Result<AgentTask> apply(Store& store, const nlohmann::json& event);

private:
    std::shared_ptr<Store> store_;
    std::shared_ptr<AgentBus> bus_;
    std::vector<std::string> agents_;
    std::atomic<std::uint64_t> cursor_{1};
    std::chrono::seconds sweep_every_;
    std::chrono::steady_clock::time_point last_sweep_{};
};

} // namespace lake::control
