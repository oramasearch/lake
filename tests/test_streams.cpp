#include <chrono>
#include <cstdlib>
#include <format>
#include <iostream>

#include <gtest/gtest.h>

#include "api/workspace_records_api.hpp"
#include "audit_test_support.hpp"
#include "http1/client.hpp"
#include "lake/api/agent_docs.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/server.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/control/stream_owners.hpp"
#include "one_thread_io.hpp"
#include "postgres_fixture.hpp"
#include "repository_test_support.hpp"
#include "stream_fixture.hpp"

namespace {
using nlohmann::json;

struct Streams : testing::Test {
    std::shared_ptr<lake::control::InMemoryStore> store = std::make_shared<lake::control::InMemoryStore>();
    lake::api::Registry registry;
    std::string customer;
    std::string other;
    std::unique_ptr<lake::test::StreamWorkspace> workspace;
    std::shared_ptr<lake::control::MemoryAgentBus> raw_bus = std::make_shared<lake::control::MemoryAgentBus>();
    std::shared_ptr<lake::control::AgentTaskBus> bus;
    lake::api::ApiContext context;

    void SetUp() override {
        const auto user = store->create_user({"taap:stream-owner", "", "Streams"});
        ASSERT_TRUE(user);
        const auto space = store->create_customer({"individual", user->id, "", "streams"});
        ASSERT_TRUE(space);
        customer = space->id;
        const auto other_user = store->create_user({"taap:stream-other", "", "Other"});
        ASSERT_TRUE(other_user);
        other = store->create_customer({"individual", other_user->id, "", "other"})->id;
        workspace = std::make_unique<lake::test::StreamWorkspace>(store, customer);
        bus = std::make_shared<lake::control::AgentTaskBus>(store, raw_bus, std::vector<std::string>{});
        lake::api::register_core_capabilities(registry, "test");
        context.store = store.get();
        context.workspaces = workspace->service.get();
        context.agent_bus = bus.get();
        context.authorized = true;
        context.operator_credential = true;
        context.acting_identity = "admin";
        context.instance_kek = "73747265616d2d6f776e6572732d776f726b73706163652d7265636f72647331";
    }
    json input(std::string topic = "orders", std::string zetta = "workspace") {
        return {{"customer_id", customer}, {"workspace_id", workspace->id}, {"zetta", zetta}, {"topic", topic}};
    }
    lake::api::ApiResult call(const std::string& name, json args) {
        if (!args.contains("workspace_id")) {
            args["workspace_id"] = workspace->id;
        }
        context.request_id = lake::control::new_uuid_v4();
        const auto result = liquid::runtime::block_on(registry.call_async(name, args, context));
        EXPECT_TRUE(result);
        return *result;
    }
};

TEST_F(Streams, NonConsumingPagingRevisionsRetentionAndAudit) {
    ASSERT_TRUE(call("create_stream", input()).ok);
    auto publish = input();
    publish["payload"] = R"({"message":"first"})";
    ASSERT_TRUE(call("publish_stream_message", publish).ok);
    ASSERT_TRUE(call("publish_stream_message", publish).ok);
    auto query = input();
    query["limit"] = 1;
    const auto newest = call("browse_stream", query);
    ASSERT_TRUE(newest.ok) << newest.error.message;
    EXPECT_EQ(newest.value["events"][0]["sequence"], 2);
    EXPECT_EQ(newest.value["next_page_sequence"], 1);
    EXPECT_EQ(workspace->caller->topics["orders"].position, 1);
    query["direction"] = "forward";
    query["from_sequence"] = 3;
    query["wait_ms"] = 30000;
    ASSERT_TRUE(call("browse_stream", query).ok);
    EXPECT_EQ(workspace->caller->calls.back().first, "browse_topic");
    EXPECT_EQ(workspace->caller->calls.back().second["wait_ms"], 30000);
    EXPECT_EQ(workspace->caller->last_token, "test-workspace-service-token");
    auto edit = publish;
    edit["sequence"] = 1;
    edit["expected_revision"] = 1;
    edit["payload"] = R"({"message":"edited"})";
    ASSERT_TRUE(call("update_stream_message", edit).ok);
    EXPECT_FALSE(workspace->caller->calls.back().second.value("redeliver", false));
    const auto stale = call("update_stream_message", edit);
    EXPECT_FALSE(stale.ok);
    EXPECT_EQ(stale.error.code, "conflict");
    auto update = input();
    update["settings"] = {{"max_events", 1}};
    update["dry_run"] = true;
    const auto preview = call("update_stream", update);
    ASSERT_TRUE(preview.ok) << preview.error.message;
    EXPECT_EQ(preview.value["trimmed"], 1);
    EXPECT_EQ(workspace->caller->topics["orders"].events.size(), 2);
    update["dry_run"] = false;
    ASSERT_TRUE(call("update_stream", update).ok);
    const auto events = lake::test::audit_events(*store, {.request_id = context.request_id});
    ASSERT_TRUE(events);
    ASSERT_EQ(events->size(), 1);
    EXPECT_EQ(events->at(0).draft.action, "update_stream");
    EXPECT_EQ(workspace->caller->topics["orders"].position, 1);
    for (const auto& [name, args] : workspace->caller->calls) {
        EXPECT_NE(name, "receive_events");
    }
}

TEST_F(Streams, WorkspaceIdentityIsRequiredAndTopicRetentionIsExplicit) {
    auto args = input();
    args.erase("workspace_id");
    const auto missing = liquid::runtime::block_on(registry.call_async("create_stream", args, context));
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->error.code, "invalid_input");
    EXPECT_EQ(missing->error.path, "/workspace_id");
    args["workspace_id"] = lake::control::new_uuid_v4();
    EXPECT_EQ(call("create_stream", args).error.code, "invalid_input");
    args["workspace_id"] = 123;
    EXPECT_EQ(call("create_stream", args).error.code, "invalid_input");
    lake::control::WorkspaceAgentBus events(workspace->service.get(), customer, workspace->id);
    ASSERT_TRUE(liquid::runtime::block_on(events.create_topic("agent-events")).value());
    ASSERT_TRUE(liquid::runtime::block_on(events.create_topic("app-events.one")).value());
    EXPECT_EQ(workspace->caller->topics.at("agent-events").settings.at("retention_ms"), 86'400'000);
    EXPECT_EQ(workspace->caller->topics.at("agent-events").settings.at("max_events"), 10'000);
    EXPECT_EQ(workspace->caller->topics.at("app-events.one").settings.at("max_events"), 5'000);
}

TEST_F(Streams, SyntheticWeekKeepsDispatchBoundedAndCutoverRemovesOldTopics) {
    for (const std::string topic : {"agent-events", "app-events.old", "build-events.old", "agent-turn.old"}) {
        ASSERT_TRUE(liquid::runtime::block_on(raw_bus->create_topic(topic)).value());
    }
    lake::control::AgentTaskBus dispatch(store, raw_bus, {"agent-project"});
    ASSERT_TRUE(liquid::runtime::block_on(dispatch.start()).value());
    const auto topics = liquid::runtime::block_on(raw_bus->call("list_topics", json::object()));
    ASSERT_TRUE(topics && *topics);
    ASSERT_EQ((**topics).at("topics").size(), 1);
    const std::string topic = "agent-tasks.agent-project";
    for (int hour = 0; hour < 7 * 24; ++hour) {
        for (int turn = 0; turn < 101; ++turn) {
            ASSERT_TRUE(liquid::runtime::block_on(raw_bus->publish(topic,
                {{"task_id", std::to_string(turn)}, {"customer_id", customer},
                 {"agent", "agent-project"}, {"kind", "test"}}, std::to_string(turn))).value());
        }
        EXPECT_LE(raw_bus->events(topic).size(), 10'000);
        raw_bus->advance_time(std::chrono::hours(1));
        EXPECT_TRUE(raw_bus->events(topic).empty());
    }
    for (int turn = 0; turn < 10'100; ++turn) {
        ASSERT_TRUE(liquid::runtime::block_on(raw_bus->publish(topic, {{"task_id", turn}}, "turn")).value());
    }
    EXPECT_EQ(raw_bus->events(topic).size(), 10'000);
    const auto tail = liquid::runtime::block_on(raw_bus->receive(topic, 1, 1000, std::chrono::milliseconds(0)));
    ASSERT_TRUE(tail && *tail);
    EXPECT_GT((**tail).events.front().sequence, 10'100);
}

TEST_F(Streams, TaskCredentialCannotSelectAnotherWorkspaceInTheSameSpace) {
    const auto second = workspace->add(store, customer, "second");
    const auto task = store->create_agent_task({
        .customer_id = customer, .agent = "agent-project", .kind = "test",
        .workspace_id = workspace->id, .scopes = {"list_streams"}});
    ASSERT_TRUE(task);
    context.credential_token_hash = lake::control::identity_crypto::sha256_hex("test-claim");
    ASSERT_TRUE(store->claim_agent_task(task->id, "agent-project", context.credential_token_hash));
    context.session_credential = true;
    context.operator_credential = false;
    context.credential_kind = "agent_task";
    context.credential_session_id = "task:" + task->id;
    context.scopes = {"list_streams"};
    EXPECT_TRUE(call("list_streams", {{"customer_id", customer}}).ok);
    const auto refused = call("list_streams", {{"customer_id", customer}, {"workspace_id", second}});
    EXPECT_EQ(refused.error.code, "unauthorized");
    EXPECT_NE(refused.error.message.find("workspace"), std::string::npos);
    // The audit event names the agent the task credential belongs to (lake#445).
    const auto events = store->list_audit_events({.limit = 1, .newest_first = true});
    ASSERT_TRUE(events && events->size() == 1U);
    EXPECT_EQ(events->front().draft.action, "list_streams");
    EXPECT_EQ(events->front().draft.details.value("acting_agent", ""), "agent-project");
}

// LSR-009 (lake#918): a launch's credential, like a task's, acts only in the workspace it was made
// for, whichever workspace a call names.
TEST_F(Streams, LaunchCredentialCannotSelectAnotherWorkspaceInTheSameSpace) {
    const auto second = workspace->add(store, customer, "second");
    lake::control::NewAgentLaunch row;
    row.customer_id = customer;
    row.agent_id = "a-1";
    row.agent_name = "Watcher";
    row.mode = "job";
    row.trigger = "manual";
    row.scopes = {"list_streams"};
    row.workspace_id = workspace->id;
    const auto launch = store->create_agent_launch(row);
    ASSERT_TRUE(launch) << launch.error().message;
    context.session_credential = true;
    context.operator_credential = false;
    context.credential_kind = "agent_launch";
    context.credential_session_id = "launch:" + launch->id;
    context.scopes = {"list_streams"};
    const auto refused = call("list_streams", {{"customer_id", customer}, {"workspace_id", second}});
    EXPECT_EQ(refused.error.code, "unauthorized");
    EXPECT_NE(refused.error.message.find("another workspace"), std::string::npos);
    // A schedule's credential naming a launch that is not its own matches no workspace.
    context.credential_kind = "agent_schedule";
    context.credential_session_id = "schedule:missing";
    EXPECT_EQ(call("list_streams", {{"customer_id", customer}, {"workspace_id", workspace->id}})
                  .error.code,
              "unauthorized");
}

// LSR-009 (lake#918): a launch's credential reaches only its own workspace through the object its
// input names, whose workspace the handler derives (here a repository), and its `workspace_act`
// runs no action its credential does not name. A public repository stays readable.
TEST_F(Streams, LaunchCredentialIsBoundThroughObjectsAndRunsNoUnnamedAction) {
    const auto second = workspace->add(store, customer, "second");
    const auto own = store->create_repository({customer, "own", "", "private", workspace->id});
    const auto foreign = store->create_repository({customer, "foreign", "", "private", second});
    const auto open = store->create_repository({customer, "open", "", "public", second});
    ASSERT_TRUE(own && foreign && open);
    lake::control::NewAgentLaunch row;
    row.customer_id = customer;
    row.agent_id = "a-1";
    row.agent_name = "Watcher";
    row.mode = "job";
    row.trigger = "manual";
    row.scopes = {"repository_tree", "workspace_act"};
    row.created_by_taap_id = "taap:stream-owner";
    row.workspace_id = workspace->id;
    const auto launch = store->create_agent_launch(row);
    ASSERT_TRUE(launch) << launch.error().message;
    context.session_credential = true;
    context.operator_credential = false;
    context.credential_kind = "agent_launch";
    context.credential_session_id = "launch:" + launch->id;
    context.scopes = row.scopes;
    const auto tree = [&](const std::string& id) {
        context.request_id = lake::control::new_uuid_v4();
        const auto result = liquid::runtime::block_on(
            registry.call_async("repository_tree", {{"id", id}, {"ref", "main"}}, context));
        EXPECT_TRUE(result);
        return *result;
    };
    const auto refused = tree(foreign->id);
    EXPECT_EQ(refused.error.code, "unauthorized");
    EXPECT_NE(refused.error.message.find("another workspace"), std::string::npos)
        << refused.error.message;
    EXPECT_EQ(tree(own->id).error.message.find("another workspace"), std::string::npos);
    EXPECT_EQ(tree(open->id).error.message.find("another workspace"), std::string::npos)
        << "a public repository is readable by anyone";

    // workspace_act alone runs no action: the run's code proposes, a person confirms.
    const auto acted = call("workspace_act",
                            {{"customer_id", customer},
                             {"capability", "delete_project"},
                             {"input", {{"project", "p-1"}}}});
    EXPECT_EQ(acted.error.code, "unauthorized");
    EXPECT_NE(acted.error.message.find("does not grant the workspace action delete_project"),
              std::string::npos)
        << acted.error.message;
}

TEST_F(Streams, DeletedAppsAndExpiredBuildsAreReapedWithoutTouchingLiveTopics) {
    const auto app = store->create_web_app({.customer_id = customer, .workspace_id = workspace->id,
        .name = "deleted-app", .mode = "image", .image = "app:v1", .host = "deleted.test"});
    ASSERT_TRUE(app);
    lake::control::WebAppPatch patch;
    patch.desired = "deleted";
    ASSERT_TRUE(store->update_web_app(app->id, patch));
    const auto repository = store->create_repository({customer, "source", "", "private",
                                                      lake::test::repository_workspace(
                                                          *store, customer)});
    ASSERT_TRUE(repository);
    const auto build = store->create_build({.customer_id = customer, .workspace_id = workspace->id,
        .repository = repository->id, .revision = "main", .target = "app:v1"});
    ASSERT_TRUE(build);
    ASSERT_TRUE(store->set_build_status(build->id, {.phase = "Succeeded"}));
    lake::control::WorkspaceAgentBus events(workspace->service.get(), customer, workspace->id);
    for (const auto& topic : {"app-events." + app->id, "build-events." + build->id,
                              "build-events." + lake::control::new_uuid_v4(), std::string("agent-events")}) {
        ASSERT_TRUE(liquid::runtime::block_on(events.create_topic(topic)).value());
    }
    const auto first = liquid::runtime::block_on(lake::control::reap_workspace_topics(*store, *workspace->service));
    ASSERT_TRUE(first && *first);
    EXPECT_EQ(**first, 2);
    EXPECT_TRUE(workspace->caller->topics.contains("build-events." + build->id));
    const auto later = liquid::runtime::block_on(lake::control::reap_workspace_topics(*store, *workspace->service,
        std::chrono::system_clock::now() + std::chrono::hours(25)));
    ASSERT_TRUE(later && *later);
    EXPECT_EQ(**later, 1);
    EXPECT_EQ(workspace->caller->topics.size(), 1);
    EXPECT_TRUE(workspace->caller->topics.contains("agent-events"));
}

TEST_F(Streams, WorkspaceIsolationAndDispatchOnlyBus) {
    const auto second = workspace->add(store, customer, "second");
    const auto foreign = workspace->add(store, other, "foreign");
    lake::control::WorkspaceAgentBus first_bus(workspace->service.get(), customer, workspace->id);
    lake::control::WorkspaceAgentBus second_bus(workspace->service.get(), customer, second);
    lake::control::WorkspaceAgentBus foreign_bus(workspace->service.get(), other, foreign);
    for (auto* target : {&first_bus, &second_bus, &foreign_bus}) {
        ASSERT_TRUE(liquid::runtime::block_on(target->create_topic("agent-events")).value());
    }
    ASSERT_TRUE(liquid::runtime::block_on(first_bus.publish("agent-events", {{"message", "first"}}, "first")).value());
    ASSERT_TRUE(liquid::runtime::block_on(second_bus.publish("agent-events", {{"message", "second"}}, "second")).value());
    ASSERT_TRUE(liquid::runtime::block_on(foreign_bus.publish("agent-events", {{"message", "foreign"}}, "foreign")).value());
    const auto task = store->create_agent_task({.customer_id = customer, .agent = "agent-project",
        .kind = "test", .workspace_id = workspace->id, .scopes = {"list_streams"}});
    ASSERT_TRUE(task);
    for (const std::string topic : {"agent-events", "app-events.old", "agent-tasks.agent-project"}) {
        ASSERT_TRUE(liquid::runtime::block_on(raw_bus->create_topic(topic)).value());
    }
    ASSERT_TRUE(liquid::runtime::block_on(raw_bus->publish("agent-tasks.agent-project",
        {{"task_id", task->id}, {"customer_id", customer}, {"agent", "agent-project"}}, task->id)).value());
    ASSERT_TRUE(liquid::runtime::block_on(raw_bus->publish("agent-tasks.agent-project",
        {{"task_id", "foreign"}, {"customer_id", other}, {"agent", "agent-project"}}, "foreign")).value());
    const auto listing = call("list_streams", {{"customer_id", customer}});
    ASSERT_TRUE(listing.ok) << listing.error.message;
    ASSERT_EQ(listing.value.at("groups")[1].at("topics").size(), 1);
    EXPECT_EQ(listing.value.at("groups")[1].at("topics")[0].at("topic"), "agent-tasks.agent-project");
    const auto first = call("browse_stream", input("agent-events"));
    ASSERT_TRUE(first.ok) << first.error.message;
    ASSERT_EQ(first.value.at("events").size(), 1);
    EXPECT_EQ(json::parse(first.value.at("events")[0].at("payload").get<std::string>()).at("message"), "first");
    auto selected = input("agent-events");
    selected["workspace_id"] = second;
    const auto next = call("browse_stream", selected);
    ASSERT_TRUE(next.ok);
    EXPECT_EQ(json::parse(next.value.at("events")[0].at("payload").get<std::string>()).at("message"), "second");
    selected["workspace_id"] = foreign;
    EXPECT_EQ(call("browse_stream", selected).error.code, "invalid_input");
    EXPECT_FALSE(call("browse_stream", input("agent-events", "bus")).ok);
    EXPECT_FALSE(call("browse_stream", input("app-events.old", "bus")).ok);
    const auto dispatch = call("browse_stream", input("agent-tasks.agent-project", "bus"));
    ASSERT_TRUE(dispatch.ok);
    ASSERT_EQ(dispatch.value.at("events").size(), 1);
    EXPECT_EQ(dispatch.value.dump().find("foreign"), std::string::npos);
    for (const std::string name : {"agent-events", "app-events.one", "build-events.one",
                                  "run-metrics.one", "study-events.one"}) {
        EXPECT_FALSE(call("create_stream", input(name)).ok);
    }
    EXPECT_FALSE(lake::api::workspace_action_allowed("drop_topic"));
    EXPECT_FALSE(lake::api::workspace_action_allowed("publish_event"));
}

TEST_F(Streams, ThousandTopicListIsPagedAndUsesOneCallPerZetta) {
    for (int index = 0; index < 1000; ++index) {
        workspace->caller->topics.emplace(std::format("topic-{:04}", index),
                                          lake::test::StreamCaller::Topic{});
    }
    ASSERT_TRUE(liquid::runtime::block_on(raw_bus->create_topic("agent-events")).value());
    ASSERT_TRUE(
        liquid::runtime::block_on(raw_bus->create_topic("agent-tasks.agent-project")).value());
    ASSERT_TRUE(store->create_agent_task({.customer_id = customer, .agent = "agent-project",
        .kind = "test", .workspace_id = workspace->id, .scopes = {"list_streams"}}));
    const auto workspace_calls = workspace->caller->calls.size();
    const auto bus_lists = raw_bus->call_count("list_topics");
    const auto bus_gets = raw_bus->call_count("get_topic");
    const auto bus_browses = raw_bus->call_count("browse_topic");

    const auto started = std::chrono::steady_clock::now();
    const auto first = call("list_streams", {{"customer_id", customer}});
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    ASSERT_TRUE(first.ok) << first.error.message;
    EXPECT_LT(elapsed, std::chrono::milliseconds(500));
    EXPECT_EQ(first.value.at("total"), 1001);
    EXPECT_EQ(first.value.at("next_cursor"), "50");
    EXPECT_TRUE(first.value.at("previous_cursor").is_null());
    std::size_t shown = 0;
    for (const auto& group : first.value.at("groups")) {
        shown += group.at("topics").size();
    }
    EXPECT_EQ(shown, 50);
    EXPECT_EQ(first.value.dump().find("topic:"), std::string::npos);
    ASSERT_EQ(workspace->caller->calls.size(), workspace_calls + 1);
    EXPECT_EQ(workspace->caller->calls.back().first, "list_topics");
    EXPECT_EQ(raw_bus->call_count("list_topics"), bus_lists + 1);
    EXPECT_EQ(raw_bus->call_count("get_topic"), bus_gets);
    EXPECT_EQ(raw_bus->call_count("browse_topic"), bus_browses);

    const auto second = call("list_streams", {{"customer_id", customer}, {"cursor", "50"}});
    ASSERT_TRUE(second.ok);
    EXPECT_EQ(second.value.at("previous_cursor"), "0");
    EXPECT_EQ(second.value.at("next_cursor"), "100");
    EXPECT_EQ(raw_bus->call_count("list_topics"), bus_lists + 2);
    const auto filtered = call("list_streams",
                               {{"customer_id", customer}, {"prefix", "topic-0999"}});
    ASSERT_TRUE(filtered.ok);
    EXPECT_EQ(filtered.value.at("total"), 1);
    EXPECT_EQ(filtered.value.at("groups")[0].at("topics")[0].at("topic"), "topic-0999");
    ASSERT_EQ(workspace->caller->calls.size(), workspace_calls + 3);
    for (std::size_t index = workspace_calls; index < workspace->caller->calls.size(); ++index) {
        EXPECT_EQ(workspace->caller->calls[index].first, "list_topics");
    }
    EXPECT_EQ(raw_bus->call_count("list_topics"), bus_lists + 3);
    EXPECT_EQ(raw_bus->call_count("get_topic"), bus_gets);
    EXPECT_EQ(raw_bus->call_count("browse_topic"), bus_browses);
}

TEST_F(Streams, ReadsWritesDeletesAndCreatorOwnershipUseSpacePolicy) {
    ASSERT_TRUE(call("create_stream", input()).ok);
    const auto object = store->find_policy_object("customer", customer);
    ASSERT_TRUE(object);
    ASSERT_TRUE(store->create_grant({"reader", object->id, "read", "test-attestation", "admin", ""}));
    ASSERT_TRUE(store->create_grant({"writer", object->id, "write", "test-attestation", "admin", ""}));
    ASSERT_TRUE(store->create_grant({"manager", object->id, "admin", "test-attestation", "admin", ""}));
    context.operator_credential = false;
    context.acting_identity = "reader";
    EXPECT_TRUE(call("list_streams", {{"customer_id", customer}}).ok);
    EXPECT_TRUE(call("get_stream", input()).ok);
    EXPECT_FALSE(call("create_stream", input("new")).ok);
    context.acting_identity = "writer";
    EXPECT_TRUE(call("publish_stream_message", {{"customer_id", customer}, {"zetta", "workspace"},
        {"topic", "orders"}, {"payload", "{}"}}).ok);
    auto drop = input();
    drop["confirm_name"] = "orders";
    EXPECT_FALSE(call("delete_stream", drop).ok);
    context.acting_identity = "manager";
    EXPECT_FALSE(call("delete_stream", drop).ok) << "space admins cannot drop another creator's topic";
    context.acting_identity = "outsider";
    const auto calls_before = workspace->caller->calls.size();
    EXPECT_FALSE(call("list_streams", {{"customer_id", customer}}).ok);
    EXPECT_EQ(workspace->caller->calls.size(), calls_before);
    context.acting_identity = "admin";
    context.operator_credential = true;
    EXPECT_TRUE(call("delete_stream", drop).ok);
}
// LSR-119 (lake#986): a workspace's agent-events carry each turn's answer, which is the person's
// who asked. A member reads the events of their own turns only, browsing or selecting a task.
TEST_F(Streams, AgentEventsShowAMemberOnlyTheirOwnTurns) {
    lake::control::WorkspaceAgentBus events(workspace->service.get(), customer, workspace->id);
    ASSERT_TRUE(liquid::runtime::block_on(events.create_topic("agent-events")).value());
    lake::control::AgentTask mine_row;
    mine_row.customer_id = customer;
    mine_row.agent = "agent-ask-orama";
    mine_row.kind = "test";
    mine_row.workspace_id = workspace->id;
    mine_row.requested_by_user_id = "user-ada";
    auto theirs_row = mine_row;
    theirs_row.requested_by_user_id = "user-grace";
    const auto mine = store->create_agent_task(mine_row);
    const auto theirs = store->create_agent_task(theirs_row);
    ASSERT_TRUE(mine && theirs);
    ASSERT_TRUE(liquid::runtime::block_on(events.publish(
        "agent-events", {{"task_id", mine->id}, {"reply", "ada's answer"}}, mine->id)).value());
    ASSERT_TRUE(liquid::runtime::block_on(events.publish(
        "agent-events", {{"task_id", theirs->id}, {"reply", "grace's answer"}}, theirs->id)).value());

    const auto object = store->find_policy_object("customer", customer);
    ASSERT_TRUE(object);
    ASSERT_TRUE(store->create_grant({"reader", object->id, "read", "test-attestation", "admin", ""}));
    context.operator_credential = false;
    context.acting_identity = "reader";
    context.acting_user_id = "user-ada";
    const auto browsed = call("browse_stream", input("agent-events"));
    ASSERT_TRUE(browsed.ok) << browsed.error.message;
    EXPECT_NE(browsed.value.dump().find("ada's answer"), std::string::npos);
    EXPECT_EQ(browsed.value.dump().find("grace's answer"), std::string::npos);
    auto selected = input("agent-events");
    selected["task"] = theirs->id;
    EXPECT_EQ(call("browse_stream", selected).error.code, "not_found");

    context.acting_identity = "admin";
    context.acting_user_id.clear();
    context.operator_credential = true;
    EXPECT_NE(call("browse_stream", input("agent-events")).value.dump().find("grace's answer"),
              std::string::npos);
}

TEST_F(Streams, AgentDocumentationIncludesEveryStreamToolAndConfirmationGuidance) {
    const auto groups = lake::api::agent_docs::tool_groups(registry);
    const auto group = std::ranges::find(groups, "Streams", &lake::api::agent_docs::Group::title);
    ASSERT_NE(group, groups.end());
    EXPECT_EQ(group->tools.size(), 9);
    EXPECT_NE(group->summary.find("person's confirmation"), std::string::npos);
    EXPECT_TRUE(registry.find("list_streams")->authorizes_candidates);
    for (const auto& tool : group->tools) {
        EXPECT_EQ(tool.writes, registry.find(tool.name)->action != "read");
    }
    const auto docs = lake::api::agent_docs::llms_full_txt(registry, "http://127.0.0.1");
    EXPECT_NE(docs.find("browse_stream"), std::string::npos);
    EXPECT_NE(docs.find("Application Agent"), std::string::npos);
}

TEST_F(Streams, ValidationPurgeConfirmationAndExistingTopicsFailClosed) {
    ASSERT_TRUE(call("create_stream", input()).ok);
    EXPECT_FALSE(call("create_stream", input()).ok);
    EXPECT_TRUE(call("get_stream", input()).value.at("can_manage"));
    auto malformed = input();
    malformed["settings"] = nullptr;
    EXPECT_EQ(call("create_stream", malformed).error.code, "invalid_input");
    malformed["settings"] = {{"max_events", -1}};
    EXPECT_EQ(call("update_stream", malformed).error.code, "invalid_input");
    malformed["settings"] = {{"store", "queue"}};
    EXPECT_EQ(call("update_stream", malformed).error.code, "invalid_input");
    auto query = input();
    query["wait_ms"] = 30001;
    EXPECT_EQ(call("browse_stream", query).error.code, "invalid_input");
    auto publish = input();
    publish["payload"] = "{";
    EXPECT_EQ(call("publish_stream_message", publish).error.code, "invalid_input");
    publish["payload"] = "{}";
    ASSERT_TRUE(call("publish_stream_message", publish).ok);
    ASSERT_TRUE(call("publish_stream_message", publish).ok);
    auto removal = input();
    removal["before_sequence"] = 10;
    removal["dry_run"] = true;
    EXPECT_EQ(call("delete_stream_messages", removal).value.at("removed"), 2);
    ASSERT_TRUE(call("publish_stream_message", publish).ok);
    removal["dry_run"] = false;
    removal["confirm_count"] = 2;
    EXPECT_EQ(call("delete_stream_messages", removal).error.code, "conflict");
    removal["confirm_count"] = 3;
    removal["sequences"] = {1};
    EXPECT_EQ(call("delete_stream_messages", removal).error.code, "invalid_input");
    removal.erase("sequences");
    EXPECT_EQ(call("delete_stream_messages", removal).value.at("removed"), 3);
    EXPECT_TRUE(call("browse_stream", input()).value.at("events").empty());
    const auto unknown = liquid::runtime::block_on(workspace->caller->call("http://workspace.test", "", "create_topic",
        false, {{"topic", "external"}, {"settings", json::object()}}));
    ASSERT_TRUE(unknown && *unknown);
    EXPECT_FALSE(call("create_stream", input("external")).ok);
    EXPECT_FALSE(call("get_stream", input("external")).value.at("can_manage"));
}

TEST_F(Streams, ExistingOwnershipCannotBeReassignedAfterAnExternalDrop) {
    ASSERT_TRUE(call("create_stream", input()).ok);
    workspace->caller->topics.erase("orders");
    const auto object = store->find_policy_object("customer", customer);
    ASSERT_TRUE(object);
    ASSERT_TRUE(store->create_grant({"another-creator", object->id, "write",
        "test-attestation", "admin", ""}));
    context.acting_identity = "another-creator";
    context.operator_credential = false;
    const auto calls_before = workspace->caller->calls.size();
    EXPECT_EQ(call("create_stream", input()).error.code, "conflict");
    // Only the ownership record was read (lake#379); the topic was never recreated upstream.
    ASSERT_EQ(workspace->caller->calls.size(), calls_before + 1);
    EXPECT_EQ(workspace->caller->calls.back().first, "get_record");
    EXPECT_FALSE(workspace->caller->topics.contains("orders"));
}

TEST_F(Streams, DispatchPagingCanContinueAcrossEmptyFilteredPages) {
    ASSERT_TRUE(liquid::runtime::block_on(raw_bus->create_topic("agent-tasks.agent-project")).value());
    const auto task = store->create_agent_task({.customer_id = customer, .agent = "agent-project",
        .kind = "test", .workspace_id = workspace->id, .scopes = {"list_streams"}});
    ASSERT_TRUE(task);
    for (int index = 0; index < 35; ++index) {
        const auto id = index == 34 ? task->id : "another-space-task";
        ASSERT_TRUE(liquid::runtime::block_on(raw_bus->publish("agent-tasks.agent-project",
            {{"task_id", id}, {"customer_id", index == 34 ? customer : other}, {"index", index}}, id)).value());
    }
    auto query = input("agent-tasks.agent-project", "bus");
    query["task"] = task->id;
    query["direction"] = "forward";
    query["limit"] = 1;
    auto page = call("browse_stream", query);
    ASSERT_TRUE(page.ok);
    EXPECT_TRUE(page.value.at("events").empty());
    ASSERT_EQ(page.value.at("next_page_sequence"), 33);
    query["from_sequence"] = page.value.at("next_page_sequence");
    page = call("browse_stream", query);
    ASSERT_TRUE(page.ok);
    ASSERT_EQ(page.value.at("events").size(), 1);
    EXPECT_EQ(page.value.at("events")[0].at("sequence"), 35);
    const auto detail = call("get_stream", input("agent-tasks.agent-project", "bus"));
    ASSERT_TRUE(detail.ok);
    EXPECT_FALSE(detail.value.at("counts_available"));
    EXPECT_EQ(detail.value.at("held_events"), 0);
    EXPECT_TRUE(detail.value.at("subscriptions").empty());
}

TEST_F(Streams, RestAndMcpOperateEveryStreamCapabilityAlike) {
    lake::api::ApiServer server({.port = 0, .store = store,
        .workspaces = workspace->service, .agent_bus = bus, .admin_token = "stream-operator",
        .instance_kek = context.instance_kek});
    ASSERT_TRUE(server.start()) << server.reason();
    const auto request = [&](const std::string& method, const std::string& path, const json& body = nullptr) {
        const auto data = body.is_null() ? std::string() : body.dump();
        const auto* bytes = reinterpret_cast<const std::byte*>(data.data());
        const auto response = lake::test::http_request({
            .url = "http://127.0.0.1:" + std::to_string(server.port()), .method = method, .suffix = path,
            .headers = {{"Authorization", "Bearer stream-operator"}, {"Content-Type", "application/json"}},
            .body = std::vector<std::byte>(bytes, bytes + data.size())});
        EXPECT_TRUE(response);
        if (!response) {
            return std::string();
        }
        EXPECT_EQ(response->status, 200U);
        return std::string(reinterpret_cast<const char*>(response->body.data()), response->body.size());
    };
    const auto openapi = json::parse(request("GET", "/v1/openapi.json"));
    const auto tools = json::parse(request("POST", "/mcp", {{"jsonrpc", "2.0"}, {"id", 1},
        {"method", "tools/list"}})).at("result").at("tools");
    const std::vector<std::string> names = {"list_streams", "get_stream", "browse_stream",
        "create_stream", "update_stream", "delete_stream", "publish_stream_message",
        "update_stream_message", "delete_stream_messages"};
    for (const auto& name : names) {
        const auto tool = std::ranges::find_if(tools, [&](const auto& item) {
            return item["name"] == name;
        });
        ASSERT_NE(tool, tools.end());
        const auto operation = openapi["paths"]["/v1/" + name]["post"];
        ASSERT_FALSE(operation.is_null()) << name;
        EXPECT_EQ(tool->at("inputSchema"), operation["requestBody"]["content"]["application/json"]["schema"]);
        EXPECT_EQ(tool->at("outputSchema"), operation["responses"]["200"]["content"]["application/json"]["schema"]);
        EXPECT_EQ(tool->at("annotations")["readOnlyHint"], registry.find(name)->kind == lake::api::CapabilityKind::Read);
    }
    for (const bool mcp : {false, true}) {
        const auto invoke = [&](const std::string& name, const json& args) {
            const auto response = mcp ? json::parse(request("POST", "/mcp", {{"jsonrpc", "2.0"},
                {"id", 1}, {"method", "tools/call"}, {"params", {{"name", name}, {"arguments", args}}}}))
                    .at("result").at("structuredContent")
                : json::parse(request("POST", "/v1/" + name, args));
            EXPECT_TRUE(response.at("ok").get<bool>()) << name << ": " << response.dump();
            return response.value("value", json::object());
        };
        auto args = input(mcp ? "mcp-orders" : "rest-orders");
        invoke("create_stream", args);
        invoke("list_streams", {{"customer_id", customer}, {"workspace_id", workspace->id}});
        invoke("get_stream", args);
        auto publish = args;
        publish["payload"] = R"({"text":"hello <script>"})";
        invoke("publish_stream_message", publish);
        auto live = args;
        live["wait_ms"] = 1000;
        live["limit"] = 10;
        invoke("browse_stream", live);
        auto edit = publish;
        edit["sequence"] = 1;
        edit["expected_revision"] = 1;
        edit["redeliver"] = true;
        invoke("update_stream_message", edit);
        auto update = args;
        update["settings"] = {{"retention_ms", 60000}};
        invoke("update_stream", update);
        if (!mcp && std::getenv("LAKE_STREAM_BROWSER")) {
            std::cout << "Streams browser fixture: http://127.0.0.1:" << server.port()
                      << "/train/stream?customer_id=" << customer << "&zetta=workspace&topic=rest-orders\n"
                      << "Operator token: stream-operator\nPress Enter to finish.\n" << std::flush;
            std::string line;
            std::getline(std::cin, line);
        }
        auto deletion = args;
        deletion["sequences"] = {1};
        deletion["dry_run"] = true;
        const auto preview = invoke("delete_stream_messages", deletion);
        EXPECT_EQ(preview["removed"], 1);
        deletion["dry_run"] = false;
        deletion["confirm_count"] = 1;
        invoke("delete_stream_messages", deletion);
        args["confirm_name"] = args["topic"];
        invoke("delete_stream", args);
    }
}

// lake#379: a topic's creator is a sealed record in the workspace Zetta that holds the topic, never
// a control-store row, so it follows that workspace incarnation.
TEST_F(Streams, CreatorOwnershipLivesInTheTopicsWorkspaceZetta) {
    const std::string home_url = "http://workspace.test";
    const auto lab = workspace->add(store, customer, "lab");
    const auto theirs = workspace->add(store, other, "theirs");
    const auto at = [&](const std::string& customer_id, const std::string& workspace_id,
                        std::string topic = "orders") {
        return json{{"customer_id", customer_id}, {"workspace_id", workspace_id},
                    {"zetta", "workspace"}, {"topic", std::move(topic)}};
    };
    ASSERT_TRUE(call("create_stream", input()).ok);
    auto& home_rows = workspace->caller->records[home_url].rows("stream_owners");
    ASSERT_EQ(home_rows.size(), 1U);
    EXPECT_EQ(home_rows.begin()->first.find("orders"), std::string::npos)
        << "the key is the blind index of the topic, not its name";
    EXPECT_EQ(home_rows.begin()->second.dump().find("admin"), std::string::npos);

    // The same name in another workspace of the space, or in another space, is another topic with
    // its own creator, recorded only at that workspace's endpoint.
    ASSERT_TRUE(call("create_stream", at(customer, lab)).ok);
    ASSERT_TRUE(call("create_stream", at(other, theirs)).ok);
    EXPECT_EQ(workspace->caller->records["http://lab.test"].rows("stream_owners").size(), 1U);
    EXPECT_EQ(workspace->caller->records["http://theirs.test"].rows("stream_owners").size(), 1U);
    EXPECT_EQ(home_rows.size(), 1U);

    // Only the creator manages it; another writer of the space reads can_manage false.
    const auto object = store->find_policy_object("customer", customer);
    ASSERT_TRUE(object);
    ASSERT_TRUE(store->create_grant({"writer", object->id, "write", "test-attestation", "admin", ""}));
    context.operator_credential = false;
    context.acting_identity = "writer";
    EXPECT_FALSE(call("get_stream", input()).value.at("can_manage"));
    auto drop = input();
    drop["confirm_name"] = "orders";
    EXPECT_EQ(call("delete_stream", drop).error.code, "unauthorized");
    EXPECT_EQ(home_rows.size(), 1U) << "a refused drop keeps the creator";
    context.operator_credential = true;
    context.acting_identity = "admin";
    EXPECT_TRUE(call("get_stream", input()).value.at("can_manage"));

    // Ownership is immutable: a second claim of a recorded name conflicts in the engine.
    auto records = lake::api::workspace_records(context, customer, workspace->id);
    ASSERT_TRUE(records) << records.error().message;
    const auto second = liquid::runtime::block_on(lake::control::claim_stream_owner(
        *records, {customer, workspace->id, "orders", "taap:someone-else"}));
    ASSERT_TRUE(second);
    ASSERT_FALSE(*second);
    EXPECT_EQ(second->error().code, "conflict");
    EXPECT_EQ(*liquid::runtime::block_on(lake::control::stream_creator(*records, customer, "orders"))
                   .value(), "admin");

    // Missing and unknown workspace ids name the field and reach no Zetta.
    const auto calls_before = workspace->caller->calls.size();
    auto missing = input("fresh");
    missing.erase("workspace_id");
    const auto refused = liquid::runtime::block_on(registry.call_async("create_stream", missing, context));
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->error.code, "invalid_input");
    EXPECT_EQ(refused->error.path, "/workspace_id");
    for (const std::string bad : {std::string("not-a-workspace"), lake::control::new_uuid_v4()}) {
        const auto unknown = call("create_stream", at(customer, bad, "fresh"));
        EXPECT_EQ(unknown.error.code, "invalid_input") << bad;
        EXPECT_EQ(unknown.error.path, "/workspace_id") << bad;
    }
    const auto foreign = call("create_stream", at(customer, theirs, "fresh"));
    EXPECT_EQ(foreign.error.code, "invalid_input") << "another space's workspace";
    EXPECT_EQ(workspace->caller->calls.size(), calls_before);

    // A workspace that is not ready refuses; nothing falls back to another store.
    const auto lab_row = store->get_workspace(lab);
    ASSERT_TRUE(store->set_service_status(lab_row->zetta_service_id, "Provisioning", "", "http://lab.test"));
    EXPECT_FALSE(call("create_stream", at(customer, lab, "fresh")).ok);
    EXPECT_EQ(workspace->caller->records["http://lab.test"].rows("stream_owners").size(), 1U);

    // Deleting a workspace takes its topics and their creators with it: the id is refused, and the
    // other workspaces keep theirs.
    ASSERT_TRUE(store->update_workspace(lab, {.desired = "deleted"}));
    const auto deleted = call("get_stream", at(customer, lab));
    EXPECT_EQ(deleted.error.code, "invalid_input");
    EXPECT_EQ(deleted.error.path, "/workspace_id");
    EXPECT_TRUE(call("get_stream", input()).value.at("can_manage"));

    // Dropping the topic releases the name in its workspace only.
    ASSERT_TRUE(call("delete_stream", drop).ok);
    EXPECT_TRUE(home_rows.empty());
    EXPECT_EQ(workspace->caller->records["http://theirs.test"].rows("stream_owners").size(), 1U);
    ASSERT_TRUE(call("create_stream", input()).ok) << "a released name can be created again";
}
} // namespace
