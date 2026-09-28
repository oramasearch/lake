#include <cstdlib>
#include <iostream>
#include <regex>

#include <gtest/gtest.h>

#include "http1/client.hpp"
#include "lake/api/agent_docs.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/server.hpp"
#include "lake/api/web_ui.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/postgres_store.hpp"
#include "audit_test_support.hpp"
#include "postgres_fixture.hpp"
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
    }
    json input(std::string topic = "orders", std::string zetta = "workspace") {
        return {{"customer_id", customer}, {"zetta", zetta}, {"topic", topic}};
    }
    lake::api::ApiResult call(const std::string& name, json args) {
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

TEST_F(Streams, SpaceIsolationSystemProtectionAndSharedAgentFiltering) {
    const auto app = store->create_web_app({.customer_id = customer, .name = "mine",
        .mode = "image", .image = "example:v1", .host = "mine.test"});
    const auto foreign = store->create_web_app({.customer_id = other, .name = "foreign",
        .mode = "image", .image = "example:v1", .host = "foreign.test"});
    ASSERT_TRUE(app);
    ASSERT_TRUE(foreign);
    const auto own_topic = "app-events." + app->id;
    const auto foreign_topic = "app-events." + foreign->id;
    for (const auto& topic : {own_topic, foreign_topic, std::string("agent-events")}) {
        ASSERT_TRUE(liquid::runtime::block_on(raw_bus->create_topic(topic)).value());
    }
    const auto task = store->create_agent_task({.customer_id = customer, .agent = "agent-project",
        .kind = "test", .scopes = {"list_streams"}});
    const auto foreign_task = store->create_agent_task({.customer_id = other, .agent = "agent-project",
        .kind = "test", .scopes = {"list_streams"}});
    ASSERT_TRUE(task);
    ASSERT_TRUE(foreign_task);
    ASSERT_TRUE(liquid::runtime::block_on(raw_bus->publish("agent-events", {{"task_id", foreign_task->id},
        {"message", "private"}}, foreign_task->id)).value());
    ASSERT_TRUE(liquid::runtime::block_on(raw_bus->publish("agent-events", {{"task_id", task->id},
        {"message", "own progress"}}, task->id)).value());
    const auto listing = call("list_streams", {{"customer_id", customer}});
    ASSERT_TRUE(listing.ok) << listing.error.message;
    EXPECT_NE(listing.value.dump().find(own_topic), std::string::npos);
    EXPECT_EQ(listing.value.dump().find(foreign_topic), std::string::npos);
    EXPECT_FALSE(call("browse_stream", input(foreign_topic, "bus")).ok);
    const auto turn = call("browse_stream", input("agent-turn." + task->id, "bus"));
    ASSERT_TRUE(turn.ok) << turn.error.message;
    ASSERT_EQ(turn.value["events"].size(), 1);
    EXPECT_EQ(turn.value.dump().find("private"), std::string::npos);
    EXPECT_FALSE(call("get_stream", input("agent-turn." + foreign_task->id, "bus")).ok);
    EXPECT_FALSE(call("get_stream", input("agent-events", "bus")).ok);
    for (const std::string name : {"create_stream", "update_stream", "delete_stream",
                                  "publish_stream_message", "update_stream_message", "delete_stream_messages"}) {
        json args = input(own_topic, "bus");
        const auto properties = registry.find(name)->input_schema["properties"];
        if (properties.contains("settings")) {
            args["settings"] = json::object();
        }
        if (properties.contains("confirm_name")) {
            args["confirm_name"] = own_topic;
        }
        if (properties.contains("payload")) {
            args["payload"] = "{}";
        }
        if (properties.contains("sequence")) {
            args["sequence"] = 1;
        }
        if (properties.contains("expected_revision")) {
            args["expected_revision"] = 1;
        }
        if (properties.contains("before_sequence")) {
            args["before_sequence"] = 2;
        }
        EXPECT_FALSE(call(name, args).ok) << name;
        args["zetta"] = "workspace";
        EXPECT_FALSE(call(name, args).ok) << name << " on a reserved workspace topic";
    }
    EXPECT_FALSE(lake::api::workspace_action_allowed("drop_topic"));
    EXPECT_FALSE(lake::api::workspace_action_allowed("publish_event"));
    for (int event = 0; event < 3; ++event) {
        ASSERT_TRUE(liquid::runtime::block_on(raw_bus->publish(own_topic, {{"phase", event}}, app->id)).value());
        ASSERT_TRUE(call("browse_stream", input(own_topic, "bus")).ok);
    }
    const auto consumer = liquid::runtime::block_on(raw_bus->receive(own_topic, 1, 100, std::chrono::milliseconds(0)));
    ASSERT_TRUE(consumer && *consumer);
    EXPECT_EQ((**consumer).events.size(), 3);
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
    EXPECT_NE(docs.find("ApplicationAgent"), std::string::npos);
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
    const auto unknown = liquid::runtime::block_on(workspace->caller->call("", "", "create_topic",
        false, {{"topic", "external"}, {"settings", json::object()}}));
    ASSERT_TRUE(unknown && *unknown);
    EXPECT_FALSE(call("create_stream", input("external")).ok);
    EXPECT_FALSE(call("get_stream", input("external")).value.at("can_manage"));
}

TEST_F(Streams, AgentTurnPagingCanContinueAcrossEmptyFilteredPages) {
    ASSERT_TRUE(liquid::runtime::block_on(raw_bus->create_topic("agent-events")).value());
    const auto task = store->create_agent_task({.customer_id = customer, .agent = "agent-project",
        .kind = "test", .scopes = {"list_streams"}});
    ASSERT_TRUE(task);
    for (int index = 0; index < 35; ++index) {
        const auto id = index == 34 ? task->id : "another-space-task";
        ASSERT_TRUE(liquid::runtime::block_on(raw_bus->publish("agent-events",
            {{"task_id", id}, {"index", index}}, id)).value());
    }
    auto query = input("agent-turn." + task->id, "bus");
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
    const auto detail = call("get_stream", input("agent-turn." + task->id, "bus"));
    ASSERT_TRUE(detail.ok);
    EXPECT_EQ(detail.value.at("held_events"), 1);
    EXPECT_TRUE(detail.value.at("subscriptions").empty());
}

TEST_F(Streams, RestMcpParityAndEveryControlUsesACapability) {
    lake::api::ApiServer server({.port = 0, .store = store,
        .workspaces = workspace->service, .agent_bus = bus, .admin_token = "stream-operator"});
    ASSERT_TRUE(server.start()) << server.reason();
    const auto request = [&](const std::string& method, const std::string& path, const json& body = nullptr) {
        const auto data = body.is_null() ? std::string() : body.dump();
        const auto* bytes = reinterpret_cast<const std::byte*>(data.data());
        const auto response = liquid::runtime::block_on(liquid::http1::request({
            .url = "http://127.0.0.1:" + std::to_string(server.port()), .method = method, .suffix = path,
            .headers = {{"Authorization", "Bearer stream-operator"}, {"Content-Type", "application/json"}},
            .body = std::vector<std::byte>(bytes, bytes + data.size())}));
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
        invoke("list_streams", {{"customer_id", customer}});
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
        const auto list_html = request("GET", "/train/streams?customer_id=" + customer);
        const auto detail_html = request("GET", "/train/stream?customer_id=" + customer +
            "&zetta=workspace&topic=" + args.at("topic").get<std::string>());
        EXPECT_NE(detail_html.find("href=\"/train/streams\" aria-current=\"page\""), std::string::npos);
        EXPECT_NE(list_html.find("href=\"/train/streams\" aria-current=\"page\""), std::string::npos);
        EXPECT_EQ(detail_html.find("test-workspace-service-token"), std::string::npos);
        EXPECT_NE(detail_html.find("&lt;script&gt;"), std::string::npos);
        const std::string html = list_html + detail_html;
        for (const auto& name : names) {
            EXPECT_NE(html.find("data-capability=\"" + name + "\""), std::string::npos) << name;
        }
        const std::regex controls("<(?:form|a|input|button)[^>]*data-capability=\"([^\"]+)\"");
        for (auto match = std::sregex_iterator(html.begin(), html.end(), controls);
             match != std::sregex_iterator(); ++match) {
            EXPECT_NE(registry.find((*match)[1]), nullptr);
        }
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

TEST(StreamsPostgres, OwnershipSurvivesReopenAndIsBoundToSpaceAndWorkspace) {
    const char* uri = lake::test::postgres_uri();
    if (!uri) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is absent";
    }
    auto store = lake::control::make_postgres_store(uri, "stream-test-kek");
    const auto user = store->create_user({"taap:stream-store", "", "Streams"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "streams"});
    ASSERT_TRUE(customer);
    const lake::control::StreamOwner owner{customer->id, "workspace-one", "orders", "taap:creator"};
    ASSERT_TRUE(store->create_stream_owner(owner));
    EXPECT_FALSE(store->create_stream_owner(owner));
    store.reset();
    store = lake::control::make_postgres_store(uri, "stream-test-kek");
    ASSERT_TRUE(store->stream_owner(owner));
    EXPECT_EQ(*store->stream_owner(owner), "taap:creator");
    auto replaced = owner;
    replaced.workspace_id = "workspace-two";
    EXPECT_TRUE(store->stream_owner(replaced)->empty());
    replaced = owner;
    replaced.created_by = "someone-else";
    ASSERT_TRUE(store->delete_stream_owner(replaced));
    EXPECT_EQ(*store->stream_owner(owner), "taap:creator");
    ASSERT_TRUE(store->delete_stream_owner(owner));
    EXPECT_TRUE(store->stream_owner(owner)->empty());
}
} // namespace
