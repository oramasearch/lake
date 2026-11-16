// SPDX-License-Identifier: MIT

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include "lake/api/capabilities.hpp"
#include "lake/control/agent_library.hpp"
#include "lake/control/agent_task_proxy.hpp"
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
        // The request is a record in the task's workspace, as agent_turn writes it (lake#374).
        lake::test::seed_agent_task_request(workspaces, *task, {{"lake_token", "{{credential}}"}},
                                            lake::test::test_instance_kek());
    }

    lake::api::ApiResult call(const std::string& name, json input,
                              const std::string& bound_workspace = "") {
        lake::api::ApiContext context;
        context.agent_service_workspace_id = bound_workspace;
        context.store = store.get();
        context.workspaces = workspaces.get();
        context.agent_bus = bus.get();
        context.authorized = true;
        context.acting_identity = "agent-service:agent-project";
        context.credential_kind = "agent_service";
        context.agent_service = "agent-project";
        context.instance_kek = lake::test::test_instance_kek();
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
        context.instance_kek = lake::test::test_instance_kek();
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
    context.instance_kek = lake::test::test_instance_kek();
    ASSERT_TRUE(registry.call("server_info", json::object(), context).ok);
    age(11);
    pump();
    EXPECT_EQ(store->get_agent_task(task_id)->phase, "Running");
    // Even past the old running deadline, activity keeps the lease alive.
    for (int i = 0; i < 7; ++i) {
        age(50);
        ASSERT_TRUE(store->record_agent_task_event(task_id, "agent-project", "Running"));
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
    // The workspace keeps the winner's answer and nothing of the loser's (lake#375).
    const auto content = lake::test::agent_task_content(workspaces, *task,
                                                        lake::test::test_instance_kek());
    ASSERT_TRUE(content);
    EXPECT_EQ(content->phase, task->phase);
    const json winner = first.ok ? json{{"winner", "a"}} : json();
    EXPECT_EQ(content->result, winner);
    EXPECT_EQ(content->message, first.ok ? "" : "b");
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
        // The expiry scan has its partial index after the migration is replayed. Asserting the
        // index itself, not a query plan: which plan Postgres picks depends on table statistics,
        // so a plan check failed intermittently and then consistently as the schema grew.
        const auto index = tx.exec("SELECT indexdef FROM pg_indexes WHERE tablename = "
                                   "'agent_task_status' AND indexname = "
                                   "'agent_task_status_lease_idx'");
        ASSERT_EQ(index.size(), 1u);
        const std::string definition = index[0][0].as<std::string>();
        EXPECT_NE(definition.find("(lease_until)"), std::string::npos) << definition;
        EXPECT_NE(definition.find("WHERE (phase = 'Running'::text)"), std::string::npos)
            << definition;
        tx.commit();
    }
    const auto after = store->get_agent_task(task_id);
    EXPECT_EQ(after->lease_until, prior->lease_until);
    EXPECT_EQ(after->attempts, 2);
}

TEST_P(AgentLeases, RetryNoticeIsTheTasksMessage) {
    ASSERT_TRUE(call("claim_agent_task", json::object()).ok);
    age(61);
    pump();
    const auto stream = events();
    ASSERT_EQ(stream.size(), 1U);
    const std::string message = stream[0]["message"];
    const auto task = store->get_agent_task(task_id);
    ASSERT_TRUE(task);
    EXPECT_EQ(task->message, message);
}

// lake#871: a library agent holds a task credential only while it serves that task. Once the task
// has its answer, the credential it was handed is refused, and another agent's bus credential
// cannot claim or report on the task.
TEST_P(AgentLeases, ATaskCredentialEndsWithItsTaskAndAnotherAgentCannotNameIt) {
    const auto claimed = call("claim_agent_task", json::object());
    ASSERT_TRUE(claimed.ok) << claimed.error.message;
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
    context.instance_kek = lake::test::test_instance_kek();
    ASSERT_TRUE(registry.call("server_info", json::object(), context).ok);

    lake::api::ApiContext stranger;
    stranger.store = store.get();
    stranger.workspaces = workspaces.get();
    stranger.agent_bus = bus.get();
    stranger.authorized = true;
    stranger.acting_identity = "agent-service:agent-experiments";
    stranger.credential_kind = "agent_service";
    stranger.agent_service = "agent-experiments";
    stranger.instance_kek = lake::test::test_instance_kek();
    const json named{{"task_id", task_id}, {"agent", "agent-experiments"}};
    const auto taken =
        liquid::runtime::block_on(registry.call_async("claim_agent_task", named, stranger));
    ASSERT_TRUE(taken);
    EXPECT_FALSE(taken->ok) << "another agent's bus credential claims no task of this agent";
    const json usage{
        {"task_id", task_id}, {"model_name", "gpt-5"}, {"input_tokens", 1}, {"output_tokens", 1}};
    const auto reported =
        liquid::runtime::block_on(registry.call_async("record_model_usage", usage, stranger));
    ASSERT_TRUE(reported);
    EXPECT_EQ(reported->error.code, "conflict");

    ASSERT_TRUE(call("publish_agent_event", {{"type", "result"}, {"result", {{"done", true}}}}).ok);
    EXPECT_FALSE(registry.call("server_info", json::object(), context).ok)
        << "the finished task's credential acts no more";
}

// lake#871: claimed by reference, the task's credential never leaves Lake. The agent names the task
// next to its own bus credential and Lake acts as the task for that request, only while the task
// runs on that agent.
TEST_P(AgentLeases, ByReferenceClaimHandsTheAgentNoCredential) {
    const auto claimed = call("claim_agent_task", {{"credentials", "by_reference"}});
    ASSERT_TRUE(claimed.ok) << claimed.error.message;
    EXPECT_EQ(claimed.value["credentials"], "by_reference");
    EXPECT_EQ(claimed.value["body"]["lake_token"], "");
    EXPECT_EQ(claimed.value.dump().find("lat."), std::string::npos);

    const lake::control::AgentBusKey shared{"agent-project", ""};
    const auto principal = lake::control::agent_task_principal(*store, task_id, shared);
    ASSERT_TRUE(principal) << principal.error().message;
    EXPECT_EQ(principal->kind, "agent_task");
    EXPECT_EQ(principal->session_id, "task:" + task_id);
    EXPECT_EQ(principal->scopes, std::vector<std::string>{"server_info"});
    // Another agent's bus credential, or a task that does not exist, acts for nothing.
    EXPECT_FALSE(lake::control::agent_task_principal(
        *store, task_id, lake::control::AgentBusKey{"agent-creator", ""}));
    EXPECT_FALSE(
        lake::control::agent_task_principal(*store, lake::control::new_uuid_v4(), shared));

    // The call renews the lease exactly as the task's own credential would.
    lake::api::ApiContext context;
    context.store = store.get();
    context.authorized = true;
    context.credential_kind = principal->kind;
    context.credential_session_id = principal->session_id;
    context.credential_token_hash = principal->workload_token_hash;
    context.acting_identity = principal->taap_id;
    context.instance_kek = lake::test::test_instance_kek();
    age(50);
    const auto aged = store->get_agent_task(task_id)->lease_until;
    ASSERT_TRUE(registry.call("server_info", json::object(), context).ok);
    EXPECT_GT(store->get_agent_task(task_id)->lease_until, aged);

    // Once the lease ends and the task is reclaimed, the reference opens nothing.
    age(61);
    pump();
    EXPECT_FALSE(lake::control::agent_task_principal(*store, task_id, shared));
    EXPECT_EQ(registry.call("server_info", json::object(), context).error.code, "conflict");
}

TEST_P(AgentLeases, AFinishedTaskOpensNothingByReference) {
    ASSERT_TRUE(call("claim_agent_task", {{"credentials", "by_reference"}}).ok);
    ASSERT_TRUE(call("publish_agent_event", {{"type", "result"}, {"result", {{"ok", true}}}}).ok);
    EXPECT_FALSE(lake::control::agent_task_principal(
        *store, task_id, lake::control::AgentBusKey{"agent-project", ""}));
}

// The Lake half of one library-agent process per workspace (lake#871): a bus credential bound to a
// workspace sees, claims, reports and acts only for that workspace's tasks, and always by
// reference.
TEST_P(AgentLeases, AWorkspaceBoundBusCredentialReachesOnlyItsWorkspace) {
    const std::string other = lake::control::new_uuid_v4();
    EXPECT_EQ(call("claim_agent_task", json::object(), other).error.code, "forbidden");
    EXPECT_EQ(call("publish_agent_event", {{"type", "progress"}, {"message", "x"}}, other)
                  .error.code,
              "conflict");

    const auto claimed = call("claim_agent_task", json::object(), workspace_id);
    ASSERT_TRUE(claimed.ok) << claimed.error.message;
    EXPECT_EQ(claimed.value["credentials"], "by_reference");
    EXPECT_EQ(claimed.value["body"]["lake_token"], "");
    EXPECT_EQ(call("publish_agent_event", {{"type", "progress"}, {"message", "x"}}, other)
                  .error.code,
              "forbidden");
    EXPECT_TRUE(
        call("publish_agent_event", {{"type", "progress"}, {"message", "x"}}, workspace_id).ok);
    EXPECT_FALSE(lake::control::agent_task_principal(
        *store, task_id, lake::control::AgentBusKey{"agent-project", other}));
    EXPECT_TRUE(lake::control::agent_task_principal(
        *store, task_id, lake::control::AgentBusKey{"agent-project", workspace_id}));
}

TEST(AgentBusKeys, AFileNameNamesTheAgentAndItsBoundWorkspace) {
    const auto shared = lake::control::agent_bus_key("agent-project");
    EXPECT_EQ(shared.agent, "agent-project");
    EXPECT_FALSE(shared.bound());
    const auto bound =
        lake::control::agent_bus_key("agent-project.0b6c3c1e-8d7a-4f5e-9a8b-1c2d3e4f5a6b");
    EXPECT_EQ(bound.agent, "agent-project");
    EXPECT_EQ(bound.workspace_id, "0b6c3c1e-8d7a-4f5e-9a8b-1c2d3e4f5a6b");
    EXPECT_TRUE(bound.bound());
}

// The operator adds and removes a workspace's bound credential without restarting lake: each
// lookup re-reads LAKE_AGENT_BUS_DIR when it changed, and a removed file stops working at once.
TEST(AgentBusKeys, TheDirectoryIsReReadWhenItChanges) {
    const auto dir = lake::test::fixture_path("agent-bus-" + lake::control::new_uuid_v4());
    std::filesystem::create_directories(dir);
    // Moves the directory's time on, as adding or removing an entry does, so the test does not
    // depend on the file system's time resolution.
    const auto touch = [&] {
        std::filesystem::last_write_time(dir,
                                         std::filesystem::last_write_time(dir) + 1s);
    };
    std::ofstream(dir / "agent-project") << "shared-token";
    lake::control::AgentBusCredentials credentials(dir.string());
    auto tokens = credentials.current();
    ASSERT_EQ(tokens->size(), 1U);
    EXPECT_EQ(tokens->at("agent-project"), "shared-token");
    EXPECT_EQ(credentials.current(), tokens) << "unchanged: not read again";

    const std::string bound = "agent-project.0b6c3c1e-8d7a-4f5e-9a8b-1c2d3e4f5a6b";
    std::ofstream(dir / bound) << "bound-token";
    touch();
    tokens = credentials.current();
    ASSERT_EQ(tokens->size(), 2U);
    EXPECT_EQ(tokens->at(bound), "bound-token");

    std::filesystem::remove(dir / bound);
    touch();
    tokens = credentials.current();
    EXPECT_EQ(tokens->size(), 1U);
    EXPECT_FALSE(tokens->contains(bound)) << "a removed credential stops working at once";

    // A Kubernetes Secret update swaps the `..data` link; that alone is a change.
    std::filesystem::remove(dir / "agent-project");
    for (const char* version : {"..2026_11_14_a", "..2026_11_14_b"}) {
        std::filesystem::create_directories(dir / version);
        std::ofstream(dir / version / "agent-project") << std::string("token-") + version;
    }
    std::filesystem::create_directory_symlink("..2026_11_14_a", dir / "..data");
    std::filesystem::create_symlink("..data/agent-project", dir / "agent-project");
    touch();
    EXPECT_EQ(credentials.current()->at("agent-project"), "token-..2026_11_14_a");
    const auto mtime = std::filesystem::last_write_time(dir);
    std::filesystem::remove(dir / "..data");
    std::filesystem::create_directory_symlink("..2026_11_14_b", dir / "..data");
    std::filesystem::last_write_time(dir, mtime);
    EXPECT_EQ(credentials.current()->at("agent-project"), "token-..2026_11_14_b");

    EXPECT_TRUE(lake::control::AgentBusCredentials("").current()->empty());
    std::filesystem::remove_all(dir);
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
