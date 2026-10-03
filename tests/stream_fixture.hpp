#pragma once

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>

#include "lake/control/agent_bus.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "record_fixture.hpp"
#include "temporary_directory.hpp"

namespace lake::test {

class StreamCaller final : public control::WorkspaceCaller {
public:
    using json = nlohmann::json;
    struct Topic {
        json settings = {{"exposed", true}, {"max_events", 1000}, {"retention_ms", nullptr}};
        json events = json::array();
        std::uint64_t next = 1;
        std::uint64_t position = 1;
    };
    std::map<std::string, Topic> topics;
    std::map<std::string, std::map<std::string, Topic>> routed_topics;
    // Lake's workspace records (record_fixture.hpp), one store per workspace endpoint.
    std::map<std::string, RecordTables> records;
    std::string first_url;
    std::vector<std::pair<std::string, json>> calls;
    std::string last_url, last_token;
    std::mutex mutex;

    liquid::runtime::task<control::ZettaControlResult<json>>
    call(std::string url, std::string token, std::string capability, bool read, json input) override {
        std::lock_guard lock(mutex);
        last_url = std::move(url);
        if (first_url.empty()) {
            first_url = last_url;
        }
        auto& topics = last_url == first_url ? this->topics : routed_topics[last_url];
        last_token = std::move(token);
        calls.emplace_back(capability, input);
        static_cast<void>(read);
        const auto fail = [](std::string code) -> control::ZettaControlResult<json> {
            return std::unexpected(control::ZettaControlError{code, code});
        };
        if (RecordTables::handles(capability, input)) {
            co_return records[last_url].run(capability, input);
        }
        if (capability == "list_topics") {
            json rows = json::array();
            for (const auto& [name, topic] : topics) {
                rows.push_back({{"topic", "topic:" + name}, {"subscribers", 1},
                    {"held_events", topic.events.size()}, {"next_sequence", topic.next}});
            }
            co_return json{{"topics", rows}};
        }
        if (!input.contains("topic")) {
            co_return json::object();
        }
        std::string name = input.at("topic");
        if (name.starts_with("topic:")) {
            name.erase(0, 6);
        }
        if (capability == "create_topic") {
            if (topics.contains(name)) {
                co_return fail("conflict");
            }
            Topic topic;
            topic.settings.update(input.value("settings", json::object()));
            topics[name] = topic;
            co_return json{{"topic", "topic:" + name}, {"exposed", true}, {"settings", topic.settings}};
        }
        if (!topics.contains(name)) {
            co_return fail("not_found");
        }
        auto& topic = topics.at(name);
        const auto earliest = topic.events.empty() ? topic.next : topic.events.front()["sequence"].get<std::uint64_t>();
        if (capability == "get_topic") {
            co_return json{{"topic", "topic:" + name}, {"settings", topic.settings},
                {"subscriber_count", 1}, {"held_events", topic.events.size()},
                {"earliest_sequence", earliest}, {"next_sequence", topic.next},
                {"subscriptions", {{{"id", 1}, {"subscriber", "agent:consumer"},
                    {"positions", {topic.position}}, {"next_sequence", topic.position},
                    {"lag", topic.events.size()}, {"in_flight", 0}, {"redeliveries", 0},
                    {"connected", true}, {"group", ""}}}}};
        }
        if (capability == "publish_event") {
            const auto sequence = topic.next++;
            const std::string payload = input.at("payload");
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            topic.events.push_back({{"sequence", sequence}, {"id", "event-" + std::to_string(sequence)},
                {"from", "service:lake"}, {"published_at", now}, {"payload", payload},
                {"correlation_id", input.value("correlation_id", "")},
                {"size", payload.size()}, {"encoding", "json"}, {"headers", json::array()},
                {"revision", 1}, {"edited_at", nullptr}, {"edited_by", ""}});
            co_return json{{"event_id", "event-" + std::to_string(sequence)},
                           {"accepted", true}, {"clamped", json::array()}};
        }
        if (capability == "browse_topic" || capability == "receive_events") {
            json events = json::array();
            const bool backward = input.value("direction", "forward") == "backward";
            const auto from = input.value("from_sequence", backward ? topic.next : std::uint64_t{1});
            for (const auto& event : topic.events) {
                if (backward ? event.at("sequence") <= from : event.at("sequence") >= from) {
                    events.push_back(event);
                }
            }
            if (backward) {
                std::reverse(events.begin(), events.end());
            }
            while (events.size() > input.value(capability == "receive_events" ? "max" : "limit", std::size_t{100})) {
                events.erase(events.end() - 1);
            }
            if (capability == "receive_events") {
                topic.position = events.empty() ? topic.next : events.back().at("sequence").get<std::uint64_t>() + 1;
                co_return json{{"topic", "topic:" + name}, {"events", events},
                               {"earliest_sequence", earliest}, {"next_sequence", topic.position}};
            }
            co_return json{{"topic", "topic:" + name}, {"events", events},
                           {"earliest_sequence", earliest}, {"next_sequence", topic.next}};
        }
        if (capability == "update_event") {
            for (auto& event : topic.events) {
                if (event["sequence"] != input["sequence"]) {
                    continue;
                }
                if (event["revision"] != input["expected_revision"]) {
                    co_return fail("conflict");
                }
                event["payload"] = input["payload"];
                event["revision"] = event["revision"].get<int>() + 1;
                event["size"] = input["payload"].get<std::string>().size();
                event["edited_at"] = 1800000000000LL;
                event["edited_by"] = "service:lake";
                co_return json{{"topic", "topic:" + name}, {"sequence", input["sequence"]},
                    {"revision", event["revision"]}, {"edited_at", event["edited_at"]},
                    {"edited_by", event["edited_by"]}};
            }
            co_return fail("not_found");
        }
        if (capability == "delete_events") {
            const auto before = topic.events.size();
            auto& events = topic.events;
            for (auto it = events.begin(); it != events.end();) {
                const bool selected = input.contains("before_sequence")
                    ? it->at("sequence") < input.at("before_sequence")
                    : std::ranges::find(input.at("sequences"), it->at("sequence")) != input.at("sequences").end();
                if (selected) {
                    it = events.erase(it);
                } else {
                    ++it;
                }
            }
            co_return json{{"topic", "topic:" + name}, {"removed", before - topic.events.size()}};
        }
        if (capability == "update_topic") {
            topic.settings.merge_patch(input.at("settings"));
            const auto before = topic.events.size();
            const auto limit = topic.settings.value("max_events", std::size_t{1000});
            while (topic.events.size() > limit) {
                topic.events.erase(topic.events.begin());
            }
            co_return json{{"topic", "topic:" + name}, {"settings", topic.settings},
                           {"trimmed", before - topic.events.size()}};
        }
        if (capability == "drop_topic") {
            topics.erase(name);
            co_return json{{"topic", "topic:" + name}, {"exposed", true}};
        }
        co_return fail("unknown_capability");
    }
};

struct StreamWorkspace {
    std::string id;
    std::filesystem::path directory;
    std::shared_ptr<StreamCaller> caller = std::make_shared<StreamCaller>();
    std::shared_ptr<control::WorkspaceService> service;

    StreamWorkspace(const std::shared_ptr<control::InMemoryStore>& store, const std::string& customer) {
        directory = fixture_path("stream-credentials-" + control::new_uuid_v4());
        std::filesystem::create_directories(directory);
        service = std::make_shared<control::WorkspaceService>(
            control::WorkspaceConfig{directory.string(), 0}, store, caller);
        const auto state = liquid::runtime::block_on(service->request(customer));
        if (!state || !*state) {
            throw std::runtime_error("could not request test workspace");
        }
        const auto& workspace = **state;
        id = workspace.workspace_id;
        const auto ready = store->set_service_status(workspace.service_id, "Ready", "", "http://workspace.test");
        if (!ready) {
            throw std::runtime_error(ready.error().message);
        }
        std::ofstream(directory / (workspace.slug + ".json")) <<
            nlohmann::json{{"kind", "zetta"}, {"token", "test-workspace-service-token"}}.dump();
    }

    std::string add(const std::shared_ptr<control::InMemoryStore>& store,
                    const std::string& customer, const std::string& name) {
        const auto row = store->create_workspace({customer, name, "prod", ""});
        if (!row) {
            throw std::runtime_error(row.error().message);
        }
        const auto workspace = store->get_service(row->zetta_service_id);
        const auto ready = store->set_service_status(workspace->id, "Ready", "", "http://" + name + ".test");
        if (!ready) {
            throw std::runtime_error(ready.error().message);
        }
        std::ofstream(directory / (workspace->slug + ".json")) <<
            nlohmann::json{{"kind", "zetta"}, {"token", "token-" + name}}.dump();
        return row->id;
    }
};
} // namespace lake::test
