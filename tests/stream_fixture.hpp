// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>

#include <pqxx/pqxx>

#include "lake/control/agent_bus.hpp"
#include "lake/control/application_credential_records.hpp"
#include "lake/control/agent_task_records.hpp"
#include "lake/control/application_settings_records.hpp"
#include "lake/control/apps.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/store.hpp"
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
    // The workspace endpoint of each call, in order (one per entry of `calls`).
    std::vector<std::string> call_urls;
    // Fixed answers for workspace capabilities the fake does not model (upload_dataset, ...).
    std::map<std::string, json> answers;
    // Answers that depend on the input (get_solution by id, …); nullopt falls through to the rest.
    std::function<std::optional<control::ZettaControlResult<json>>(const std::string&, const json&)>
        responder;
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
        call_urls.push_back(last_url);
        static_cast<void>(read);
        const auto fail = [](std::string code) -> control::ZettaControlResult<json> {
            return std::unexpected(control::ZettaControlError{code, code});
        };
        if (RecordTables::handles(capability, input)) {
            co_return records[last_url].run(capability, input);
        }
        if (responder) {
            if (auto answered = responder(capability, input)) {
                co_return std::move(*answered);
            }
        }
        if (const auto found = answers.find(capability); found != answers.end()) {
            co_return found->second;
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

    // Zetta-Dataset-Scope (lake#1055): each scoped call's capability and header value. With
    // `enforces_dataset_scope`, server_info under a scope is refused as zetta#1515 refuses it;
    // without it the call is answered like any other, as a Zetta that does not know the header.
    bool enforces_dataset_scope = false;
    std::vector<std::pair<std::string, std::string>> scoped;

    liquid::runtime::task<control::ZettaControlResult<json>>
    scoped_call(std::string url, std::string token, std::string capability, bool read, json input,
                std::string scope) override {
        bool refuse = false;
        {
            std::lock_guard lock(mutex);
            scoped.emplace_back(capability, scope);
            refuse = enforces_dataset_scope && capability == "server_info";
        }
        if (refuse) {
            co_return control::ZettaControlResult<json>(std::unexpected(control::ZettaControlError{
                "unauthorized",
                "'server_info' cannot run under a dataset scope",
                "",
                {{"dataset_scope", scope}, {"datasets_read", nullptr}}}));
        }
        co_return control::settled(co_await call(
            std::move(url), std::move(token), std::move(capability), read, std::move(input)));
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
// A new workspace of `customer`, Ready at http://<name>.test with its credential in `directory`
// (the WorkspaceService's service directory), over the in-memory store or, when `database_uri` is
// set, PostgreSQL, where the operator's status row is written directly.
inline std::string ready_workspace(control::Store& store, const std::filesystem::path& directory,
                                   const std::string& customer, const std::string& name,
                                   const std::string& database_uri = {}) {
    const auto row = store.create_workspace({customer, name, "prod", ""});
    if (!row) {
        throw std::runtime_error(row.error().message);
    }
    const auto service = store.get_service(row->zetta_service_id);
    if (!service) {
        throw std::runtime_error(service.error().message);
    }
    const std::string endpoint = "http://" + name + ".test";
    if (database_uri.empty()) {
        const auto ready = dynamic_cast<control::InMemoryStore&>(store).set_service_status(
            service->id, "Ready", "", endpoint);
        if (!ready) {
            throw std::runtime_error(ready.error().message);
        }
    } else {
        pqxx::connection connection(database_uri);
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO service_status (service_id, phase, endpoint) VALUES ($1::uuid, "
                       "'Ready', $2) ON CONFLICT (service_id) DO UPDATE SET phase = 'Ready', "
                       "endpoint = EXCLUDED.endpoint",
                       service->id, endpoint);
        tx.commit();
    }
    std::filesystem::create_directories(directory);
    std::ofstream(directory / (service->slug + ".json"))
        << nlohmann::json{{"kind", "zetta"}, {"token", "token-" + name}}.dump();
    return row->id;
}

// Writes a first signing key for an application created directly in `store`, as
// register_application does (lake#352): the seed enveloped by the store, the key ring in the
// application's workspace Zetta, reached through `workspaces`.
inline void seed_application_signing_key(control::Store& store,
                                         const std::shared_ptr<control::WorkspaceService>& workspaces,
                                         const std::string& customer,
                                         const std::string& workspace,
                                         const std::string& application_id,
                                         std::string_view instance_kek) {
    auto key = control::apps::generate_signing_key(application_id, 1);
    if (!key) {
        throw std::runtime_error(key.error());
    }
    auto envelope =
        store.encrypt_application_credential("application.jwt." + application_id, key->seed);
    if (!envelope) {
        throw std::runtime_error(envelope.error().message);
    }
    key->encrypted_seed = *envelope;
    key->seed.clear();
    auto records = control::WorkspaceRecords::open(
        [workspaces, customer, workspace](std::string capability, bool read, nlohmann::json input) {
            return workspaces->call(customer, workspace, std::move(capability), read,
                                    std::move(input));
        },
        workspace, instance_kek);
    if (!records) {
        throw std::runtime_error(records.error().message);
    }
    auto written = control::settled(liquid::runtime::block_on(control::create_application_signing_keys(
        *records, {.application_id = application_id, .keys = {*key}})));
    if (!written) {
        throw std::runtime_error(written.error().message);
    }
}

// Writes the settings of an application created directly in `store`, as register_application does
// (lake#351): one record in the application's workspace Zetta, reached through `workspaces`.
inline void seed_application_settings(const std::shared_ptr<control::WorkspaceService>& workspaces,
                                      const std::string& workspace,
                                      control::ApplicationSettings settings,
                                      std::string_view instance_kek) {
    const std::string customer = settings.customer_id;
    auto records = control::WorkspaceRecords::open(
        [workspaces, customer, workspace](std::string capability, bool read, nlohmann::json input) {
            return workspaces->call(customer, workspace, std::move(capability), read,
                                    std::move(input));
        },
        workspace, instance_kek);
    if (!records) {
        throw std::runtime_error(records.error().message);
    }
    auto written = control::settled(liquid::runtime::block_on(
        control::create_application_settings(*records, std::move(settings))));
    if (!written) {
        throw std::runtime_error(written.error().message);
    }
}

// The instance KEK of tests that run without LAKE_INSTANCE_KEK: workspace records need one.
inline std::string test_instance_kek() {
    if (const char* configured = std::getenv("LAKE_INSTANCE_KEK"); configured && *configured) {
        return configured;
    }
    return "6167656e742d7461736b2d7265636f7264732d746573742d6b656b2d30303031";
}

// The records of `workspace`, reached through `workspaces` as lake reaches them.
inline control::WorkspaceRecords test_workspace_records(
    const std::shared_ptr<control::WorkspaceService>& workspaces, const std::string& customer,
    const std::string& workspace, std::string_view instance_kek) {
    auto records = control::WorkspaceRecords::open(
        [workspaces, customer, workspace](std::string capability, bool read, nlohmann::json input) {
            return workspaces->call(customer, workspace, std::move(capability), read,
                                    std::move(input));
        },
        workspace, instance_kek);
    if (!records) {
        throw std::runtime_error(records.error().message);
    }
    return std::move(*records);
}

// Writes the request of a task created directly in `store`, as agent_turn does (lake#374): one
// record in the task's workspace Zetta.
inline void seed_agent_task_request(const std::shared_ptr<control::WorkspaceService>& workspaces,
                                    const control::AgentTask& task, nlohmann::json request,
                                    std::string_view instance_kek) {
    auto records =
        test_workspace_records(workspaces, task.customer_id, task.workspace_id, instance_kek);
    auto written = control::settled(liquid::runtime::block_on(control::create_agent_task_content(
        records, {.task_id = task.id,
                  .customer_id = task.customer_id,
                  .agent = task.agent,
                  .request = std::move(request)})));
    if (!written) {
        throw std::runtime_error(written.error().message);
    }
}

// Records what the task's agent said, as publish_agent_event does once the store accepted the
// event (lake#375): its message, and for Succeeded or Failed its answer.
inline void seed_agent_task_answer(const std::shared_ptr<control::WorkspaceService>& workspaces,
                                   const control::AgentTask& task, std::string phase,
                                   std::string message, nlohmann::json result,
                                   std::string_view instance_kek) {
    auto records =
        test_workspace_records(workspaces, task.customer_id, task.workspace_id, instance_kek);
    auto written = control::settled(liquid::runtime::block_on(control::record_agent_task_content(
        records, {.task_id = task.id,
                  .customer_id = task.customer_id,
                  .agent = task.agent,
                  .message = std::move(message),
                  .result = std::move(result),
                  .phase = phase == "Running" ? std::string() : std::move(phase)})));
    if (!written) {
        throw std::runtime_error(written.error().message);
    }
}

// A task's record in its workspace, or nullopt.
inline std::optional<control::AgentTaskContent>
agent_task_content(const std::shared_ptr<control::WorkspaceService>& workspaces,
                   const control::AgentTask& task, std::string_view instance_kek) {
    auto records =
        test_workspace_records(workspaces, task.customer_id, task.workspace_id, instance_kek);
    auto found = control::settled(liquid::runtime::block_on(
        control::get_agent_task_content(records, task.id, task.customer_id)));
    if (!found) {
        throw std::runtime_error(found.error().message);
    }
    return *found;
}

} // namespace lake::test
