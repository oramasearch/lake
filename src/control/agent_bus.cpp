#include "lake/control/agent_bus.hpp"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

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
    const bool read = capability == "receive_events";
    co_return settled(co_await caller.call(url_, token_, std::move(capability), read,
                                          std::move(input)));
}

WorkspaceAgentBus::WorkspaceAgentBus(WorkspaceService* workspaces, std::string customer_id,
                                   std::string workspace_id)
    : workspaces_(workspaces), customer_id_(std::move(customer_id)),
      workspace_id_(std::move(workspace_id)) {}

liquid::runtime::task<ZettaControlResult<nlohmann::json>>
WorkspaceAgentBus::call(std::string capability, nlohmann::json input) {
    if (!workspaces_) {
        co_return bus_failure<nlohmann::json>("workspace_not_ready", "a workspace Zetta is required");
    }
    const bool read = capability == "receive_events";
    co_return settled(co_await workspaces_->call(customer_id_, workspace_id_,
                                                std::move(capability), read, std::move(input)));
}

liquid::runtime::task<ZettaControlResult<void>> AgentBus::create_topic(std::string topic) {
    const bool dispatch = topic.starts_with("agent-tasks.");
    nlohmann::json retention{{"retention_ms", dispatch ? 3'600'000 : 86'400'000},
                             {"max_events", dispatch || topic == "agent-events" ? 10'000 : 5'000}};
    auto settings = retention;
    settings["exposed"] = true;
    settings["delivery"] = {{"guarantee", "at_least_once"}};
    auto created = settled(co_await call("create_topic", {{"topic", topic}, {"settings", settings}}));
    if (!created && created.error().code != "conflict" &&
        created.error().message.find("already exists") == std::string::npos) {
        co_return bus_failure<void>(created.error().code, created.error().message);
    }
    if (!created) {
        auto updated = settled(co_await call("update_topic", {{"topic", topic}, {"settings", retention}}));
        if (!updated) {
            co_return bus_failure<void>(updated.error().code, updated.error().message);
        }
    }
    co_return ZettaControlResult<void>{};
}

liquid::runtime::task<ZettaControlResult<std::string>>
AgentBus::publish(std::string topic, nlohmann::json payload, std::string correlation_id) {
    auto published = settled(co_await call(
                                          "publish_event",
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

liquid::runtime::task<ZettaControlResult<BusBatch>> AgentBus::receive(
    std::string topic, std::uint64_t from, int max, std::chrono::milliseconds wait) {
    const auto waited = std::clamp<long long>(wait.count(), 0, 30'000);
    auto received = settled(co_await call(
                                         "receive_events",
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
                              {"held_events", events.size()}, {"next_sequence", sequences_[name]}});
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
    if (capability == "drop_topic") {
        topics_.erase(found);
        sequences_.erase(topic);
        co_return json{{"topic", "topic:" + topic}, {"exposed", true}};
    }
    const auto& held = found->second;
    const auto next = sequences_[topic];
    const auto earliest = held.empty() ? next : held.front().sequence;
    if (capability == "get_topic") {
        co_return json{{"topic", "topic:" + topic}, {"settings", {{"exposed", true}}},
                       {"subscriber_count", 0}, {"subscriptions", json::array()},
                       {"held_events", held.size()}, {"earliest_sequence", earliest},
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
                   {"earliest_sequence", earliest}, {"next_sequence", next}};
}

liquid::runtime::task<ZettaControlResult<void>> MemoryAgentBus::create_topic(std::string topic) {
    std::lock_guard lock(mutex_);
    topics_.try_emplace(topic);
    sequences_.try_emplace(topic, 1);
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
        found->second.push_back(BusEvent{sequences_[topic]++,
                                         id,
                                         std::move(correlation_id),
                                         std::move(payload), now_});
        trim(topic);
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
    for (const auto& event : held) {
        if (event.sequence >= from && batch.events.size() < static_cast<std::size_t>(max)) {
            batch.events.push_back(event);
            batch.next_sequence = event.sequence + 1;
        }
    }
    if (batch.events.empty()) {
        batch.next_sequence = std::max(from, sequences_[topic]);
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

void MemoryAgentBus::trim(const std::string& topic) {
    auto& events = topics_.at(topic);
    const bool dispatch = topic.starts_with("agent-tasks.");
    const auto age = std::chrono::hours(dispatch ? 1 : 24);
    const std::size_t count = dispatch || topic == "agent-events" ? 10'000 : 5'000;
    std::erase_if(events, [&](const BusEvent& event) {
        return now_ - event.published_at >= age;
    });
    if (events.size() > count) {
        events.erase(events.begin(), events.begin() + (events.size() - count));
    }
}

void MemoryAgentBus::advance_time(std::chrono::milliseconds elapsed) {
    std::lock_guard lock(mutex_);
    now_ += elapsed;
    for (const auto& [topic, events] : topics_) {
        trim(topic);
    }
}

bool build_topic_expired(const Build& build, std::chrono::system_clock::time_point now) {
    if (build.phase != "Succeeded" && build.phase != "Failed" && build.phase != "Cancelled") {
        return false;
    }
    std::istringstream input(build.updated_at);
    std::tm timestamp{};
    input >> std::get_time(&timestamp, "%Y-%m-%dT%H:%M:%S");
    const auto terminal = std::chrono::system_clock::from_time_t(timegm(&timestamp));
    return !input.fail() && now - terminal >= std::chrono::hours(24);
}

liquid::runtime::task<ZettaControlResult<std::size_t>>
reap_workspace_topics(Store& store, WorkspaceService& workspaces,
                      std::chrono::system_clock::time_point now) {
    const auto customers = store.list_customers(std::numeric_limits<int>::max());
    if (!customers) {
        co_return bus_failure<std::size_t>(customers.error().code, customers.error().message);
    }
    std::size_t reaped = 0;
    for (const auto& customer : *customers) {
        const auto services = store.list_services(customer.id);
        if (!services) {
            co_return bus_failure<std::size_t>(services.error().code, services.error().message);
        }
        for (const auto& service : *services) {
            if (service.kind != "zetta" || service.desired != "present" || service.phase != "Ready") {
                continue;
            }
            WorkspaceAgentBus bus(&workspaces, customer.id, service.id);
            const auto topics = settled(co_await bus.call("list_topics", nlohmann::json::object()));
            if (!topics) {
                continue;
            }
            for (const auto& row : topics->at("topics")) {
                std::string topic = row.at("topic");
                if (topic.starts_with("topic:")) {
                    topic.erase(0, 6);
                }
                bool remove = false;
                if (topic.starts_with("app-events.")) {
                    const auto app = store.get_web_app(topic.substr(11));
                    remove = !app ? app.error().code == "not_found"
                        : app->customer_id == customer.id && app->workspace_id == service.id &&
                          (app->desired == "deleted" || app->phase == "Deleted");
                } else if (topic.starts_with("build-events.")) {
                    const auto build = store.get_build(topic.substr(13));
                    remove = !build && build.error().code == "not_found";
                    if (build && build->customer_id == customer.id && build->workspace_id == service.id) {
                        remove = build_topic_expired(*build, now);
                    }
                }
                if (remove) {
                    const auto dropped = settled(co_await bus.call("drop_topic", {{"topic", topic}}));
                    if (dropped) {
                        ++reaped;
                    }
                }
            }
        }
    }
    co_return reaped;
}

AgentTaskBus::AgentTaskBus(std::shared_ptr<Store> store,
                           std::shared_ptr<AgentBus> bus,
                           std::vector<std::string> agents,
                           std::chrono::seconds sweep_every)
    : store_(std::move(store)), bus_(std::move(bus)), agents_(std::move(agents)),
      sweep_every_(sweep_every) {}

liquid::runtime::task<ZettaControlResult<void>> AgentTaskBus::start() {
    for (const std::string& agent : agents_) {
        if (auto created = settled(co_await bus_->create_topic(agent_tasks_topic(agent))); !created) {
            co_return created;
        }
    }
    co_return settled(co_await reap());
}

liquid::runtime::task<ZettaControlResult<void>> AgentTaskBus::reap() {
    auto listed = settled(co_await bus_->call("list_topics", nlohmann::json::object()));
    if (!listed) {
        co_return bus_failure<void>(listed.error().code, listed.error().message);
    }
    for (const auto& row : listed->at("topics")) {
        std::string topic = row.at("topic");
        if (topic.starts_with("topic:")) {
            topic.erase(0, 6);
        }
        const bool wanted = std::ranges::any_of(agents_, [&](const std::string& agent) {
            return topic == agent_tasks_topic(agent);
        });
        if (wanted) {
            continue;
        }
        std::clog << nlohmann::json{{"event", "bus_topic_reap"}, {"dry_run", true},
                                    {"topic", topic}}.dump() << '\n';
        auto dropped = settled(co_await bus_->call("drop_topic", {{"topic", topic}}));
        if (dropped) {
            ++reaped_topics_;
        }
        if (!dropped) {
            co_return bus_failure<void>(dropped.error().code, dropped.error().message);
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
    static_cast<void>(wait);

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
