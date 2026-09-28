#include "lake/control/agent_bus.hpp"

#include <algorithm>

#include "lake/control/zetta_workspace.hpp"

namespace lake::control {
namespace {

template <class T> ZettaControlResult<T> bus_failure(std::string code, std::string message) {
    return std::unexpected(ZettaControlError{std::move(code), std::move(message)});
}

BusBatch batch_from(const nlohmann::json& value, std::uint64_t from) {
    BusBatch batch;
    batch.next_sequence = value.value("next_sequence", from);
    for (const nlohmann::json& row : value.value("events", nlohmann::json::array())) {
        BusEvent event;
        event.sequence = row.value("sequence", std::uint64_t{0});
        event.id = row.value("id", std::string());
        event.correlation_id = row.value("correlation_id", std::string());
        event.payload = nlohmann::json::parse(row.value("payload", std::string()), nullptr, false);
        if (event.payload.is_discarded()) {
            event.payload = nullptr;
        }
        batch.events.push_back(std::move(event));
    }
    return batch;
}

} // namespace

std::string agent_tasks_topic(std::string_view agent) {
    return "agent-tasks." + std::string(agent);
}

ZettaAgentBus::ZettaAgentBus(std::string url, std::string token)
    : url_(std::move(url)), token_(std::move(token)) {}

liquid::runtime::task<ZettaControlResult<nlohmann::json>>
AgentBus::call(std::string, nlohmann::json) {
    co_return bus_failure<nlohmann::json>("not_supported", "stream inspection is unavailable");
}

liquid::runtime::task<ZettaControlResult<nlohmann::json>>
ZettaAgentBus::call(std::string capability, nlohmann::json input) {
    ZettaWorkspaceCaller caller;
    co_return settled(co_await caller.call(url_, token_, std::move(capability), false,
                                          std::move(input)));
}

liquid::runtime::task<ZettaControlResult<void>> ZettaAgentBus::create_topic(std::string topic) {
    ZettaWorkspaceCaller caller;
    auto created = settled(co_await caller.call(
        url_,
        token_,
        "create_topic",
        false,
        {{"topic", topic}, {"exposed", true}, {"guarantee", "at_least_once"}}));
    if (!created && created.error().message.find("already exists") == std::string::npos) {
        co_return bus_failure<void>(created.error().code, created.error().message);
    }
    co_return ZettaControlResult<void>{};
}

liquid::runtime::task<ZettaControlResult<std::string>>
ZettaAgentBus::publish(std::string topic, nlohmann::json payload, std::string correlation_id) {
    ZettaWorkspaceCaller caller;
    auto published = settled(co_await caller.call(url_,
                                          token_,
                                          "publish_event",
                                          false,
                                          {{"topic", topic},
                                           {"payload", payload.dump()},
                                           {"encoding", "json"},
                                           {"mime_type", "application/json"},
                                           {"correlation_id", correlation_id}}));
    if (!published) {
        co_return bus_failure<std::string>(published.error().code, published.error().message);
    }
    co_return published->value("event_id", std::string());
}

liquid::runtime::task<ZettaControlResult<BusBatch>> ZettaAgentBus::receive(
    std::string topic, std::uint64_t from, int max, std::chrono::milliseconds wait) {
    ZettaWorkspaceCaller caller;
    // A read's HTTP timeout is 15 s; the long poll stays well inside it.
    const auto waited = std::min<long long>(wait.count(), 10'000);
    auto received = settled(co_await caller.call(url_,
                                         token_,
                                         "receive_events",
                                         true,
                                         {{"topic", topic},
                                          {"from_sequence", std::max<std::uint64_t>(from, 1)},
                                          {"max", std::clamp(max, 1, 1000)},
                                          {"wait_ms", waited}}));
    if (!received) {
        co_return bus_failure<BusBatch>(received.error().code, received.error().message);
    }
    co_return batch_from(*received, from);
}

liquid::runtime::task<ZettaControlResult<nlohmann::json>>
MemoryAgentBus::call(std::string capability, nlohmann::json input) {
    using nlohmann::json;
    std::lock_guard lock(mutex_);
    ++call_counts_[capability];
    if (capability == "list_topics") {
        json topics = json::array();
        for (const auto& [name, events] : topics_) {
            topics.push_back({{"topic", "topic:" + name}, {"subscribers", 0},
                              {"held_events", events.size()}, {"next_sequence", events.size() + 1}});
        }
        co_return json{{"topics", std::move(topics)}};
    }
    std::string topic = input.at("topic");
    if (topic.starts_with("topic:")) {
        topic.erase(0, 6);
    }
    const auto found = topics_.find(topic);
    if (found == topics_.end()) {
        co_return bus_failure<json>("not_found", "topic does not exist");
    }
    const auto& held = found->second;
    const auto next = held.size() + 1;
    if (capability == "get_topic") {
        co_return json{{"topic", "topic:" + topic}, {"settings", {{"exposed", true}}},
                       {"subscriber_count", 0}, {"subscriptions", json::array()},
                       {"held_events", held.size()}, {"earliest_sequence", held.empty() ? next : 1},
                       {"next_sequence", next}};
    }
    if (capability != "browse_topic") {
        co_return bus_failure<json>("not_supported", "the memory bus exposes stream reads only");
    }
    const bool backward = input.value("direction", "forward") == "backward";
    const auto from = input.value("from_sequence", backward ? next : std::size_t{1});
    json events = json::array();
    for (const auto& event : held) {
        if ((backward && event.sequence > from) || (!backward && event.sequence < from)) {
            continue;
        }
        const auto payload = event.payload.dump();
        events.push_back({{"sequence", event.sequence}, {"id", event.id}, {"from", "service:lake"},
                          {"published_at", 0}, {"payload", payload}, {"size", payload.size()},
                          {"revision", 1}, {"encoding", "json"}, {"headers", json::array()},
                          {"edited_at", nullptr}, {"edited_by", ""}});
    }
    if (backward) {
        std::reverse(events.begin(), events.end());
    }
    const auto limit = std::clamp(input.value("limit", 100), 1, 1000);
    while (events.size() > static_cast<std::size_t>(limit)) {
        events.erase(events.end() - 1);
    }
    co_return json{{"topic", "topic:" + topic}, {"events", std::move(events)},
                   {"earliest_sequence", held.empty() ? next : 1}, {"next_sequence", next}};
}

liquid::runtime::task<ZettaControlResult<void>> MemoryAgentBus::create_topic(std::string topic) {
    std::lock_guard lock(mutex_);
    topics_.try_emplace(topic);
    co_return ZettaControlResult<void>{};
}

liquid::runtime::task<ZettaControlResult<std::string>>
MemoryAgentBus::publish(std::string topic, nlohmann::json payload, std::string correlation_id) {
    std::string id;
    {
        std::lock_guard lock(mutex_);
        const auto found = topics_.find(topic);
        if (found == topics_.end()) {
            co_return bus_failure<std::string>(
                "not_found", "topic:" + topic + " does not exist or is not exposed");
        }
        id = "event-" + std::to_string(next_id_++);
        found->second.push_back(BusEvent{found->second.size() + 1,
                                         id,
                                         std::move(correlation_id),
                                         std::move(payload)});
    }
    co_return id;
}

liquid::runtime::task<ZettaControlResult<BusBatch>> MemoryAgentBus::receive(
    std::string topic, std::uint64_t from, int max, std::chrono::milliseconds wait) {
    std::lock_guard lock(mutex_);
    if (!topics_.contains(topic)) {
        co_return bus_failure<BusBatch>("not_found",
                                        "topic:" + topic + " does not exist or is not exposed");
    }
    // Never waits: a long poll would hold the caller's thread, which in lake is the I/O thread.
    static_cast<void>(wait);
    from = std::max<std::uint64_t>(from, 1);
    BusBatch batch;
    batch.next_sequence = from;
    const std::vector<BusEvent>& held = topics_[topic];
    for (std::size_t i = from - 1; i < held.size() && batch.events.size() < std::size_t(max);
         ++i) {
        batch.events.push_back(held[i]);
        batch.next_sequence = held[i].sequence + 1;
    }
    co_return batch;
}

std::vector<BusEvent> MemoryAgentBus::events(const std::string& topic) const {
    std::lock_guard lock(mutex_);
    const auto found = topics_.find(topic);
    return found == topics_.end() ? std::vector<BusEvent>{} : found->second;
}

std::size_t MemoryAgentBus::call_count(std::string_view capability) const {
    std::lock_guard lock(mutex_);
    const auto found = call_counts_.find(std::string(capability));
    return found == call_counts_.end() ? 0 : found->second;
}

AgentTaskBus::AgentTaskBus(std::shared_ptr<Store> store,
                           std::shared_ptr<AgentBus> bus,
                           std::vector<std::string> agents,
                           std::chrono::seconds sweep_every)
    : store_(std::move(store)), bus_(std::move(bus)), agents_(std::move(agents)),
      sweep_every_(sweep_every) {}

liquid::runtime::task<ZettaControlResult<void>> AgentTaskBus::start() {
    if (auto created = settled(co_await bus_->create_topic(std::string(agent_events_topic))); !created) {
        co_return created;
    }
    for (const std::string& agent : agents_) {
        if (auto created = settled(co_await bus_->create_topic(agent_tasks_topic(agent))); !created) {
            co_return created;
        }
    }
    co_return ZettaControlResult<void>{};
}

liquid::runtime::task<ZettaControlResult<void>> AgentTaskBus::publish_task(AgentTask task) {
    auto published = settled(co_await bus_->publish(agent_tasks_topic(task.agent),
                                            {{"task_id", task.id},
                                             {"customer_id", task.customer_id},
                                             {"agent", task.agent},
                                             {"kind", task.kind}},
                                            task.id));
    if (!published) {
        co_return bus_failure<void>(published.error().code, published.error().message);
    }
    if (auto marked = store_->mark_agent_task_published(task.id); !marked) {
        co_return bus_failure<void>(marked.error().code, marked.error().message);
    }
    co_return ZettaControlResult<void>{};
}

Result<AgentTask> AgentTaskBus::apply(Store& store, const nlohmann::json& event) {
    if (!event.is_object()) {
        return std::unexpected(StoreError{"invalid_input", "an agent event is a JSON object"});
    }
    const std::string task_id = event.value("task_id", std::string());
    const std::string agent = event.value("agent", std::string());
    const std::string type = event.value("type", std::string());
    std::string phase;
    if (type == "progress") {
        phase = "Running";
    } else if (type == "result") {
        phase = "Succeeded";
    } else if (type == "failed") {
        phase = "Failed";
    } else {
        return std::unexpected(
            StoreError{"invalid_input", "an agent event's type is progress, result or failed"});
    }
    return store.record_agent_task_event(task_id,
                                         agent,
                                         phase,
                                         event.value("message", std::string()),
                                         type == "result" ? event.value("result", nlohmann::json())
                                                          : nlohmann::json());
}

liquid::runtime::task<ZettaControlResult<std::size_t>>
AgentTaskBus::pump(std::chrono::milliseconds wait) {
    std::size_t applied = 0;
    auto batch = settled(co_await bus_->receive(
        std::string(agent_events_topic), cursor_.load(), 100, wait));
    if (!batch) {
        co_return bus_failure<std::size_t>(batch.error().code, batch.error().message);
    }
    for (const BusEvent& event : batch->events) {
        // A refused event (a task that finished, or another agent's) changes nothing.
        if (apply(*store_, event.payload)) {
            ++applied;
        }
    }
    cursor_.store(std::max(cursor_.load(), batch->next_sequence));

    const auto now = std::chrono::steady_clock::now();
    if (now - last_sweep_ >= sweep_every_) {
        last_sweep_ = now;
        auto stale = store_->stale_agent_tasks(agent_task_republish_after);
        if (stale) {
            for (const AgentTask& task : stale->overdue) {
                static_cast<void>(store_->fail_agent_task(
                    task.id,
                    task.phase.empty()
                        ? "No agent picked this up within " +
                              std::to_string(task.deadline_seconds) + " seconds."
                        : "The agent did not finish within " +
                              std::to_string(task.deadline_seconds) + " seconds."));
            }
            for (const AgentTask& task : stale->unclaimed) {
                static_cast<void>(co_await publish_task(task));
            }
        }
    }
    co_return applied;
}

} // namespace lake::control
