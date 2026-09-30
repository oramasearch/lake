#include <cstdlib>
#include <fstream>
#include <thread>

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include "lake/api/capabilities.hpp"
#include "lake/control/agent_library.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/postgres_store.hpp"
#include "postgres_fixture.hpp"
#include "stream_fixture.hpp"

namespace {
using nlohmann::json;
using namespace std::chrono_literals;

class AgentLeases : public testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        if (GetParam()) {
            const char* configured = lake::test::postgres_uri();
            if (!configured) {
                GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
            }
            uri = configured;
            store = lake::control::make_postgres_store(uri, std::getenv("LAKE_INSTANCE_KEK"));
        } else {
            store = std::make_shared<lake::control::InMemoryStore>();
        }
        const auto user =
            store->create_user({"taap:1:ed25519:lease-owner", "lease@example.test", "Owner"});
        ASSERT_TRUE(user);
        const auto customer = store->create_customer({"individual", user->id, "", "lease-owner"});
        ASSERT_TRUE(customer);
        const auto workspace = store->create_workspace({customer->id, "leases", "dev", ""});
        ASSERT_TRUE(workspace);
        customer_id = customer->id;
        workspace_id = workspace->id;
        const auto task = store->create_agent_task({.customer_id = customer_id,
                                                    .agent = "agent-project",
                                                    .kind = "solution:lease-project",
                                                    .workspace_id = workspace_id,
                                                    .body = {{"lake_token", "{{credential}}"}},
                                                    .scopes = {"server_info"},
                                                    .requested_by_user_id = user->id});
        ASSERT_TRUE(task);
        task_id = task->id;
        const auto service = store->get_service(workspace->zetta_service_id);
        ASSERT_TRUE(service);
        if (auto* memory = dynamic_cast<lake::control::InMemoryStore*>(store.get())) {
            ASSERT_TRUE(
                memory->set_service_status(service->id, "Ready", "", "http://lease-workspace"));
        } else {
            pqxx::connection connection(uri);
            pqxx::work tx(connection);
            tx.exec_params("INSERT INTO service_status (service_id, phase, endpoint) "
                           "VALUES ($1::uuid, 'Ready', 'http://lease-workspace')",
                           service->id);
            tx.commit();
        }
        directory = lake::test::fixture_path("lease-services-" + lake::control::new_uuid_v4());
        std::filesystem::create_directories(directory);
        std::ofstream(directory / (service->slug + ".json"))
            << json{{"kind", "zetta"}, {"token", "workspace-token"}}.dump();
        lake::control::WorkspaceConfig config{directory.string(), 0};
        config.agent_lease_seconds = {{"agent-project", 60}};
        workspaces = std::make_shared<lake::control::WorkspaceService>(config, store, caller);
        bus = std::make_unique<lake::control::AgentTaskBus>(
            store, dispatch, std::vector<std::string>{"agent-project"}, 0s);
        ASSERT_TRUE(lake::control::settled(liquid::runtime::block_on(bus->start())));
        lake::api::register_core_capabilities(registry, "test");
    }

    lake::api::ApiResult call(const std::string& name, json input) {
        lake::api::ApiContext context;
        context.store = store.get();
        context.workspaces = workspaces.get();
        context.agent_bus = bus.get();
        context.authorized = true;
        context.acting_identity = "agent-service:agent-project";
        context.credential_kind = "agent_service";
        context.agent_service = "agent-project";
        if (const char* key = std::getenv("LAKE_INSTANCE_KEK")) {
            context.instance_kek = key;
        }
        input["task_id"] = task_id;
        input["agent"] = "agent-project";
        auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(completed);
        return *completed;
    }

    void age(int seconds) {
        if (auto* memory = dynamic_cast<lake::control::InMemoryStore*>(store.get())) {
            memory->age_agent_task(task_id, std::chrono::seconds(seconds));
        } else {
            pqxx::connection connection(uri);
            pqxx::work tx(connection);
            tx.exec_params(
                "UPDATE agent_task_status SET lease_until = lease_until - make_interval(secs => "
                "$2), "
                "started_at = started_at - make_interval(secs => $2), "
                "updated_at = updated_at - make_interval(secs => $2) WHERE task_id = $1::uuid",
                task_id,
                seconds);
            tx.exec_params(
                "UPDATE agent_tasks SET created_at = created_at - make_interval(secs => $2), "
                "published_at = published_at - make_interval(secs => $2) WHERE id = $1::uuid",
                task_id,
                seconds);
            tx.commit();
        }
    }

    void pump() {
        const auto result =
            lake::control::settled(liquid::runtime::block_on(bus->pump(0ms, workspaces.get())));
        ASSERT_TRUE(result) << result.error().message;
    }

    json events() {
        lake::api::ApiContext context;
        context.store = store.get();
        context.workspaces = workspaces.get();
        context.agent_bus = bus.get();
        context.authorized = true;
        context.acting_identity = "admin";
        if (const char* key = std::getenv("LAKE_INSTANCE_KEK")) {
            context.instance_kek = key;
        }
        const json input{
            {"customer_id", customer_id}, {"workspace_id", workspace_id}, {"task_id", task_id}};
        const auto completed =
            liquid::runtime::block_on(registry.call_async("receive_agent_events", input, context));
        EXPECT_TRUE(completed);
        if (!completed) {
            return json::array();
        }
        EXPECT_TRUE(completed->ok) << completed->error.message;
        return completed->ok ? completed->value.at("events") : json::array();
    }

    std::string uri, customer_id, workspace_id, task_id;
    std::filesystem::path directory;
    std::shared_ptr<lake::control::Store> store;
    std::shared_ptr<lake::test::StreamCaller> caller = std::make_shared<lake::test::StreamCaller>();
    std::shared_ptr<lake::control::MemoryAgentBus> dispatch =
        std::make_shared<lake::control::MemoryAgentBus>();
    std::shared_ptr<lake::control::WorkspaceService> workspaces;
    std::unique_ptr<lake::control::AgentTaskBus> bus;
    lake::api::Registry registry;
};

TEST_P(AgentLeases, ReclaimsThreeAttemptsAndPublishesEveryNotice) {
    std::string prior_credential;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        const auto claimed = call("claim_agent_task", json::object());
        ASSERT_TRUE(claimed.ok) << claimed.error.message;
        EXPECT_EQ(claimed.value["attempts"], attempt);
        EXPECT_EQ(claimed.value["lease_seconds"], 60);
        EXPECT_FALSE(claimed.value["lease_until"].get<std::string>().empty());
        const std::string credential = claimed.value["body"]["lake_token"];
        EXPECT_NE(credential, prior_credential);
        if (!prior_credential.empty()) {
            EXPECT_FALSE(store->context_for(prior_credential));
        }
        prior_credential = credential;
        EXPECT_TRUE(store->context_for(credential));
        EXPECT_EQ(call("claim_agent_task", json::object()).error.code, "conflict");
        age(61);
        auto stale = store->stale_agent_tasks(60s);
        ASSERT_TRUE(stale);
        ASSERT_EQ(stale->reclaimed.size(), 1U);
        EXPECT_EQ(stale->reclaimed[0].attempts, std::min(attempt + 1, 3));
        for (const auto* type : {"progress", "result", "failed"}) {
            EXPECT_EQ(call("publish_agent_event", {{"type", type}}).error.code, "conflict");
        }
        EXPECT_FALSE(store->context_for(credential));
        // A pass with no workspace cannot lose the notice or increment the attempt twice.
        EXPECT_FALSE(lake::control::settled(liquid::runtime::block_on(bus->pump(0ms))));
        EXPECT_TRUE(store->get_agent_task(task_id)->retry_notice_pending);
        pump();
        pump();
        const auto stream = events();
        ASSERT_EQ(stream.size(), attempt);
        const auto& notice = stream.back();
        EXPECT_EQ(notice["attempts"], std::min(attempt + 1, 3));
        EXPECT_EQ(notice["reclaimed"], true);
        EXPECT_EQ(notice["type"], attempt < 3 ? "progress" : "failed");
        EXPECT_EQ(notice["message"],
                  attempt < 3 ? "the agent stopped responding; retrying (attempt " +
                                    std::to_string(attempt + 1) + " of 3)"
                              : "the agent stopped responding");
        EXPECT_EQ(dispatch->events("agent-tasks.agent-project").size(), std::min(attempt, 2));
    }
    const auto failed = store->get_agent_task(task_id);
    ASSERT_TRUE(failed);
    EXPECT_EQ(failed->phase, "Failed");
    EXPECT_EQ(failed->message, "the agent stopped responding");
    EXPECT_FALSE(failed->finished_at.empty());
    EXPECT_TRUE(failed->lease_until.empty());
    EXPECT_EQ(call("claim_agent_task", json::object()).error.code, "conflict");
}

TEST_P(AgentLeases, DelayedRetryAcknowledgementCannotClearTheFinalFailureNotice) {
    for (int attempt = 1; attempt <= 2; ++attempt) {
        ASSERT_TRUE(call("claim_agent_task", json::object()).ok);
        age(61);
        pump();
    }
    const auto retry = store->get_agent_task(task_id);
    ASSERT_TRUE(retry);
    ASSERT_EQ(retry->attempts, 3);
    ASSERT_TRUE(retry->phase.empty());
    ASSERT_TRUE(call("claim_agent_task", json::object()).ok);
    age(61);
    const auto stale = store->stale_agent_tasks(60s);
    ASSERT_TRUE(stale);
    ASSERT_EQ(stale->reclaimed.size(), 1U);
    ASSERT_TRUE(store->mark_agent_task_notice_published(task_id, retry->attempts, retry->phase));
    EXPECT_TRUE(store->get_agent_task(task_id)->retry_notice_pending);
    pump();
    const auto stream = events();
    ASSERT_EQ(stream.size(), 3U);
    EXPECT_EQ(stream.back()["type"], "failed");
    EXPECT_EQ(stream.back()["message"], "the agent stopped responding");
    EXPECT_FALSE(store->get_agent_task(task_id)->retry_notice_pending);
}

TEST_P(AgentLeases, EventsAndTaskCapabilityCallsExtendTheLease) {
    const auto claimed = call("claim_agent_task", json::object());
    ASSERT_TRUE(claimed.ok);
    age(50);
    const auto aged = store->get_agent_task(task_id)->lease_until;
    ASSERT_TRUE(
        call("publish_agent_event", {{"type", "progress"}, {"message", "still working"}}).ok);
    EXPECT_GT(store->get_agent_task(task_id)->lease_until, aged);
    age(11);
    pump();
    EXPECT_EQ(store->get_agent_task(task_id)->attempts, 1);
    age(40);
    const std::string credential = claimed.value["body"]["lake_token"];
    const auto principal = store->context_for(credential);
    ASSERT_TRUE(principal);
    lake::api::ApiContext context;
    context.store = store.get();
    context.authorized = true;
    context.credential_kind = principal->kind;
    context.credential_session_id = principal->session_id;
    context.credential_token_hash = principal->workload_token_hash;
    context.acting_identity = principal->taap_id;
    if (const char* key = std::getenv("LAKE_INSTANCE_KEK")) {
        context.instance_kek = key;
    }
    ASSERT_TRUE(registry.call("server_info", json::object(), context).ok);
    age(11);
    pump();
    EXPECT_EQ(store->get_agent_task(task_id)->phase, "Running");
    // Even past the old running deadline, activity keeps the lease alive.
    for (int i = 0; i < 7; ++i) {
        age(50);
        ASSERT_TRUE(store->record_agent_task_event(
            task_id, "agent-project", "Running", "working", nullptr));
        pump();
    }
    EXPECT_EQ(store->get_agent_task(task_id)->attempts, 1);
    age(61);
    pump();
    EXPECT_EQ(registry.call("server_info", json::object(), context).error.code, "conflict");
    EXPECT_EQ(store->get_agent_task(task_id)->attempts, 2);
}

TEST_P(AgentLeases, ReclaimedTaskUsesFirstTerminalWriteAndRejectsLaterEvents) {
    ASSERT_TRUE(call("claim_agent_task", json::object()).ok);
    age(61);
    pump();
    ASSERT_TRUE(call("claim_agent_task", json::object()).ok);
    for (const char* message : {"old claimant progress", "new claimant progress"}) {
        ASSERT_TRUE(call("publish_agent_event", {{"type", "progress"}, {"message", message}}).ok);
    }
    lake::api::ApiResult first, second;
    std::thread a([&] {
        first = call("publish_agent_event", {{"type", "result"}, {"result", {{"winner", "a"}}}});
    });
    std::thread b([&] {
        const json event{{"type", "failed"}, {"message", "b"}};
        second = call("publish_agent_event", event);
    });
    a.join();
    b.join();
    EXPECT_NE(first.ok, second.ok);
    EXPECT_EQ((first.ok ? second : first).error.code, "conflict");
    const auto task = store->get_agent_task(task_id);
    ASSERT_TRUE(task);
    EXPECT_EQ(task->phase, first.ok ? "Succeeded" : "Failed");
    const auto stream = events();
    ASSERT_EQ(stream.size(), 4U) << "only the winning terminal event is published";
    EXPECT_EQ(stream.back()["type"], first.ok ? "result" : "failed");
    EXPECT_EQ(call("publish_agent_event", {{"type", "progress"}}).error.code, "conflict");
    EXPECT_EQ(call("publish_agent_event", {{"type", "result"}}).error.code, "conflict");
}

TEST_P(AgentLeases, DefaultsBoundsAndUnclaimedDeadline) {
    EXPECT_EQ(store->claim_agent_task(task_id, "agent-project", "hash", 0, 59).error().code,
              "invalid_input");
    EXPECT_EQ(store->claim_agent_task(task_id, "agent-project", "hash", 0, 3601).error().code,
              "invalid_input");
    age(61);
    pump();
    EXPECT_EQ(dispatch->events("agent-tasks.agent-project").size(), 1U);
    const auto claimed = store->claim_agent_task(task_id, "agent-project", "hash");
    ASSERT_TRUE(claimed);
    EXPECT_EQ(claimed->lease_seconds, 600);
    age(301);
    pump();
    EXPECT_EQ(store->get_agent_task(task_id)->phase, "Running");
    EXPECT_EQ(store->get_agent_task(task_id)->attempts, 1);
}

TEST_P(AgentLeases, WaitingDeadlineCannotFailAConcurrentClaim) {
    age(301);
    const auto stale = store->stale_agent_tasks(60s);
    ASSERT_TRUE(stale);
    ASSERT_EQ(stale->overdue.size(), 1U);
    ASSERT_TRUE(call("claim_agent_task", json::object()).ok);
    ASSERT_TRUE(store->fail_agent_task(task_id, "No agent picked this up"));
    EXPECT_EQ(store->get_agent_task(task_id)->phase, "Running");
    age(61);
    pump();
    ASSERT_TRUE(store->get_agent_task(task_id)->phase.empty());
    ASSERT_TRUE(store->fail_agent_task(task_id, "No agent picked this up"));
    EXPECT_TRUE(store->get_agent_task(task_id)->phase.empty()) << "a retry has a fresh deadline";
    age(301);
    pump();
    EXPECT_EQ(store->get_agent_task(task_id)->phase, "Failed");
    EXPECT_EQ(store->get_agent_task(task_id)->message,
              "No agent picked this up within 300 seconds.");
}

TEST_P(AgentLeases, MigrationAdoptsLegacyClaimsAndPreservesExistingLeases) {
    if (!GetParam()) {
        return;
    }
    ASSERT_TRUE(call("claim_agent_task", json::object()).ok);
    std::ifstream file(std::filesystem::path(LAKE_TEST_SOURCE_DIR) / "db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto begin = schema.find("-- 0042_agent_task_leases:");
    ASSERT_NE(begin, std::string::npos);
    pqxx::connection connection(uri);
    {
        pqxx::work tx(connection);
        tx.exec("ALTER TABLE agent_task_status DROP COLUMN lease_until, DROP COLUMN attempts, "
                "DROP COLUMN lease_seconds, DROP COLUMN retry_notice_pending");
        tx.exec("UPDATE agent_task_status SET started_at = NULL");
        tx.exec(schema.substr(begin));
        tx.commit();
    }
    const auto adopted = store->get_agent_task(task_id);
    ASSERT_TRUE(adopted);
    EXPECT_EQ(adopted->attempts, 1);
    EXPECT_EQ(adopted->lease_seconds, 600);
    EXPECT_FALSE(adopted->lease_until.empty());
    age(601);
    pump();
    ASSERT_TRUE(call("claim_agent_task", json::object()).ok);
    const auto prior = store->get_agent_task(task_id);
    {
        pqxx::work tx(connection);
        tx.exec(schema.substr(begin));
        tx.exec("SET LOCAL enable_seqscan = off");
        const auto plan = tx.exec("EXPLAIN SELECT task_id FROM agent_task_status "
                                  "WHERE phase = 'Running' AND lease_until < now()");
        std::string lines;
        for (const auto& row : plan) {
            lines += row[0].as<std::string>();
        }
        EXPECT_NE(lines.find("agent_task_status_lease_idx"), std::string::npos);
        tx.commit();
    }
    const auto after = store->get_agent_task(task_id);
    EXPECT_EQ(after->lease_until, prior->lease_until);
    EXPECT_EQ(after->attempts, 2);
}

TEST(AgentLeaseConfig, ParsesPerAgentBoundsAndRejectsInvalidConfiguration) {
    const auto parsed =
        lake::control::parse_agent_leases("agent-project=60,agent-experiments=3600");
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->at("agent-project"), 60);
    EXPECT_EQ(parsed->at("agent-experiments"), 3600);
    EXPECT_TRUE(lake::control::parse_agent_leases("")->empty());
    for (const auto* invalid : {"agent-project=59",
                                "agent-project=3601",
                                "unknown=600",
                                "agent-project=abc",
                                "agent-project=60.5",
                                "agent-project=",
                                "agent-project=60,",
                                ",agent-project=60",
                                "agent-project=60,agent-project=600"}) {
        EXPECT_FALSE(lake::control::parse_agent_leases(invalid)) << invalid;
    }
}

INSTANTIATE_TEST_SUITE_P(Stores, AgentLeases, testing::Bool(), [](const auto& info) {
    return info.param ? "Postgres" : "Memory";
});
} // namespace
