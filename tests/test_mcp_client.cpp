// SPDX-License-Identifier: MIT

// Lake as an MCP client (lake#652, docs/api/mcp-connections.md#the-client): the pure
// protocol pieces, then the client against a local test MCP server over the server's own outbound
// transport, honest and hostile.

#include <chrono>
#include <deque>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "lake/api/mcp_client.hpp"
#include "mcp_test_server.hpp"

namespace {

using json = nlohmann::json;
using lake::api::McpClient;
using lake::api::McpClientOptions;
using lake::test::McpTestServer;

std::string nested_arrays(std::size_t depth) {
    return std::string(depth, '[') + "0" + std::string(depth, ']');
}

TEST(McpClient, RemoteJsonRejectsNestingBeforeBuildingAnUnboundedDocument) {
    using lake::api::mcp_parse_json;
    EXPECT_FALSE(mcp_parse_json(nested_arrays(64)).is_discarded());
    EXPECT_TRUE(mcp_parse_json(nested_arrays(65)).is_discarded());
    EXPECT_TRUE(mcp_parse_json(nested_arrays(100'000)).is_discarded());
    std::string objects = "0";
    for (int i = 0; i < 65; ++i) {
        objects = "{\"x\":" + objects + "}";
    }
    EXPECT_TRUE(mcp_parse_json(objects).is_discarded());
    EXPECT_TRUE(
        mcp_parse_json("{\"result\":{},\"ignored\":" + nested_arrays(64) + "}").is_discarded())
        << "a valid prefix must not survive rejection";
    EXPECT_TRUE(
        mcp_parse_json("{\"ignored\":" + nested_arrays(64) + ",\"ignored\":null,\"result\":{}}")
            .is_discarded())
        << "duplicate keys cannot hide excessive nesting";
    EXPECT_TRUE(mcp_parse_json("[}").is_discarded());
    EXPECT_EQ(mcp_parse_json(R"({"text":"[ { \" escaped brackets } ]"})").at("text"),
              "[ { \" escaped brackets } ]");
    EXPECT_EQ(mcp_parse_json("{\"ok\":true}").at("ok"), true);
}

class DepthTestStream : public lake::api::McpEventStream {
public:
    explicit DepthTestStream(std::shared_ptr<std::deque<std::string>> events)
        : events_(std::move(events)) {
        head_.status = 200;
        head_.headers["content-type"] = "text/event-stream";
    }
    const lake::api::McpHttpResponse& head() const override {
        return head_;
    }
    Read read(std::chrono::milliseconds) override {
        if (events_->empty()) {
            return {{}, true, {}};
        }
        std::string bytes = std::move(events_->front());
        events_->pop_front();
        return {std::move(bytes), false, {}};
    }

private:
    lake::api::McpHttpResponse head_;
    std::shared_ptr<std::deque<std::string>> events_;
};

// Exercise both response parsers, the early-answer callback, and both notification readers
// without asking a test server's own JSON serializer to process the hostile document.
class DepthTestHttp : public lake::api::McpHttp {
public:
    std::string transport = "json";
    std::string result = "{}";
    std::string before_answer;
    std::string after_answer;
    bool batch = false;
    bool early_answer = false;
    auto stream_events() {
        return events_;
    }
    lake::api::McpHttpResponse exchange(const lake::api::McpHttpRequest& request,
                                        const lake::api::McpEnough& enough) override {
        lake::api::McpHttpResponse response;
        response.status = 200;
        response.headers["content-type"] = "application/json";
        const json message = json::parse(request.body);
        if (!message.contains("id")) {
            response.status = 202;
            return response;
        }
        const std::string value = message.at("method") == "initialize"
                                      ? R"({"protocolVersion":"2025-06-18","capabilities":{}})"
                                      : result;
        std::string body =
            "{\"jsonrpc\":\"2.0\",\"id\":" + message.at("id").dump() + ",\"result\":" + value + "}";
        if (batch) {
            body = "[" + body + "]";
        }
        if (transport == "legacy") {
            events_->push_back(before_answer + "data: " + body + "\n\n" + after_answer);
            response.status = 202;
        } else if (transport == "sse") {
            response.headers["content-type"] = "text/event-stream";
            response.body = before_answer + "data: " + body + "\n\n";
            early_answer = enough && enough(response.body);
        } else {
            response.body = std::move(body);
        }
        return response;
    }
    std::unique_ptr<lake::api::McpEventStream> open(const lake::api::McpHttpRequest&) override {
        if (transport == "legacy" && !opened_) {
            events_->push_back("event: endpoint\ndata: /messages\n\n");
        }
        opened_ = true;
        return std::make_unique<DepthTestStream>(events_);
    }

private:
    bool opened_ = false;
    std::shared_ptr<std::deque<std::string>> events_ = std::make_shared<std::deque<std::string>>();
};

TEST(McpClient, EveryTransportRejectsDeepResultsAndTheNextRequestStillWorks) {
    for (const std::string transport : {"json", "sse", "legacy"}) {
        for (const bool batch : {false, true}) {
            SCOPED_TRACE(transport + (batch ? " batch" : " single"));
            auto http = std::make_shared<DepthTestHttp>();
            http->transport = transport;
            http->batch = batch;
            McpClientOptions options;
            options.url = "https://mcp.example/mcp";
            options.transport = transport == "legacy" ? "sse" : "streamable_http";
            McpClient client(http, options);
            ASSERT_TRUE(client.initialize());
            // Envelope + result + optional batch consume two or three container levels.
            http->result = "{\"value\":" + nested_arrays(batch ? 61 : 62) + "}";
            EXPECT_TRUE(client.call_tool("echo", json::object()));
            if (transport == "sse") {
                EXPECT_TRUE(http->early_answer);
            }
            http->result = "{\"value\":" + nested_arrays(128) + "}";
            EXPECT_FALSE(client.call_tool("echo", json::object()));
            if (transport == "sse") {
                EXPECT_FALSE(http->early_answer);
            }
            http->result = "{\"ok\":true}";
            const auto next = client.call_tool("echo", json::object());
            ASSERT_TRUE(next);
            EXPECT_EQ(next->at("ok"), true);
        }
    }
}

TEST(McpClient, DeepNotificationsAreDroppedOnResponsesAndBothListeningStreams) {
    const std::string deep =
        "data: {\"method\":\"notifications/message\",\"params\":{\"x\":" + nested_arrays(128) +
        "}}\n\n";
    const std::string normal =
        "data: {\"method\":\"notifications/message\",\"params\":{\"ok\":true}}\n\n";
    for (const std::string transport : {"sse", "legacy"}) {
        SCOPED_TRACE(transport);
        auto http = std::make_shared<DepthTestHttp>();
        http->transport = transport;
        McpClientOptions options;
        options.url = "https://mcp.example/mcp";
        options.transport = transport == "legacy" ? "sse" : "streamable_http";
        McpClient client(http, options);
        ASSERT_TRUE(client.initialize());
        http->before_answer = deep + normal;
        if (transport == "legacy") {
            http->after_answer = deep + normal;
        }
        ASSERT_TRUE(client.call_tool("echo", json::object()));
        auto notifications = client.take_notifications();
        ASSERT_EQ(notifications.size(), 1U);
        EXPECT_EQ(notifications[0].params.at("ok"), true);
        if (transport != "legacy") {
            http->stream_events()->push_back(deep + normal);
        }
        const auto listened = client.listen(std::chrono::milliseconds(10));
        ASSERT_TRUE(listened);
        ASSERT_EQ(listened->size(), 1U);
        EXPECT_EQ(listened->at(0).params.at("ok"), true);
    }
}

TEST(McpClient, ToolSchemasAndOldRecordsAreCheckedBeforeCopiesAndSerialization) {
    const json boundary =
        json::parse("{\"name\":\"echo\",\"inputSchema\":{\"x\":" + nested_arrays(62) + "}}");
    EXPECT_TRUE(lake::api::mcp_tool_from(boundary, {}));
    for (const std::string key : {"inputSchema", "outputSchema", "annotations"}) {
        json tool{{"name", "echo"}, {"inputSchema", {{"type", "object"}}}};
        tool[key] = json::parse("{\"x\":" + nested_arrays(128) + "}");
        EXPECT_FALSE(lake::api::mcp_tool_from(tool, {})) << key;
    }
    const json stored =
        json::parse("{\"name\":\"echo\",\"input_schema\":{\"x\":" + nested_arrays(128) + "}}");
    EXPECT_FALSE(lake::api::mcp_tool_from_json(stored));
    const json stored_boundary =
        json::parse("{\"name\":\"echo\",\"input_schema\":{\"x\":" + nested_arrays(62) + "}}");
    EXPECT_TRUE(lake::api::mcp_tool_from_json(stored_boundary));
}

TEST(McpClient, EventStreamsParseAsTheWhatwgRulesSay) {
    lake::api::SseParser parser;
    auto events = parser.feed(": a comment\r\nevent: endpoint\r\ndata: /messages\r\n\r\nda");
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].event, "endpoint");
    EXPECT_EQ(events[0].data, "/messages");
    events = parser.feed("ta: {\"a\":\ndata: 1}\n\nid: 7\rdata:x\r\r");
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].event, "message");
    EXPECT_EQ(events[0].data, "{\"a\":\n1}") << "data lines join with a line feed";
    EXPECT_EQ(events[1].id, "7");
    EXPECT_EQ(events[1].data, "x");
    EXPECT_TRUE(parser.feed("event: nothing\n\n").empty()) << "an event without data is dropped";
}

TEST(McpClient, ServerUrlsAreHttpsWithoutUserInfoOrFragments) {
    const auto good = lake::api::parse_mcp_url("https://MCP.Example.com/v1/mcp?x=1", false);
    ASSERT_TRUE(good) << good.error();
    EXPECT_EQ(good->host, "mcp.example.com");
    EXPECT_EQ(good->port, "443");
    EXPECT_EQ(good->path, "/v1/mcp?x=1");
    EXPECT_EQ(good->origin(), "https://mcp.example.com");
    EXPECT_EQ(lake::api::parse_mcp_url("https://[2606:4700::1]:8443", false)->origin(),
              "https://[2606:4700::1]:8443");
    for (const std::string bad : {"http://mcp.example.com/mcp",
                                  "ftp://x",
                                  "https://user@x.com/",
                                  "https://x.com/#frag",
                                  "https://x.com:99999/",
                                  "https:// x.com",
                                  "https://x.com:8a/",
                                  "https://"}) {
        EXPECT_FALSE(lake::api::parse_mcp_url(bad, false)) << bad;
    }
    EXPECT_FALSE(lake::api::parse_mcp_url("http://127.0.0.1:9/mcp", false));
    EXPECT_TRUE(lake::api::parse_mcp_url("http://127.0.0.1:9/mcp", true)) << "tests only";
    EXPECT_FALSE(lake::api::parse_mcp_url("http://10.0.0.1/mcp", true))
        << "plain http is loopback only";

    const auto base = *lake::api::parse_mcp_url("https://mcp.example.com/a/sse", false);
    EXPECT_EQ(*lake::api::resolve_mcp_url(base, "/messages?s=1"),
              "https://mcp.example.com/messages?s=1");
    EXPECT_EQ(*lake::api::resolve_mcp_url(base, "messages"), "https://mcp.example.com/a/messages");
    EXPECT_EQ(*lake::api::resolve_mcp_url(base, "https://mcp.example.com/m"),
              "https://mcp.example.com/m");
    for (const std::string elsewhere : {"https://evil.example/m",
                                        "//evil.example/m",
                                        "http://mcp.example.com/m",
                                        "javascript://x"}) {
        EXPECT_FALSE(lake::api::resolve_mcp_url(base, elsewhere)) << elsewhere;
    }
}

TEST(McpClient, OnlyPublicAddressesAreReachable) {
    using bytes = std::vector<std::uint8_t>;
    for (const bytes& refused : {bytes{127, 0, 0, 1},
                                 bytes{10, 1, 2, 3},
                                 bytes{172, 16, 0, 1},
                                 bytes{172, 31, 255, 255},
                                 bytes{192, 168, 1, 1},
                                 bytes{169, 254, 169, 254},
                                 bytes{100, 64, 0, 1},
                                 bytes{0, 0, 0, 0},
                                 bytes{224, 0, 0, 1},
                                 bytes{255, 255, 255, 255}}) {
        EXPECT_FALSE(lake::api::mcp_public_address(refused))
            << int(refused[0]) << "." << int(refused[1]);
    }
    EXPECT_TRUE(lake::api::mcp_public_address({8, 8, 8, 8}));
    EXPECT_TRUE(lake::api::mcp_public_address({172, 32, 0, 1}));
    bytes loopback6(16, 0);
    loopback6[15] = 1;
    EXPECT_FALSE(lake::api::mcp_public_address(loopback6));
    bytes mapped(16, 0);
    mapped[10] = mapped[11] = 0xFF;
    mapped[12] = 10;
    EXPECT_FALSE(lake::api::mcp_public_address(mapped)) << "IPv4-mapped 10.0.0.0";
    mapped[12] = 8;
    mapped[13] = 8;
    mapped[14] = 8;
    mapped[15] = 8;
    EXPECT_TRUE(lake::api::mcp_public_address(mapped));
    bytes nat64{0x00, 0x64, 0xFF, 0x9B, 0, 0, 0, 0, 0, 0, 0, 0, 169, 254, 169, 254};
    EXPECT_FALSE(lake::api::mcp_public_address(nat64)) << "the metadata address through NAT64";
    // LSR-265 (lake#1008): every transition and site-local form that hides a private address.
    bytes compatible(16, 0);
    compatible[12] = 10;
    EXPECT_FALSE(lake::api::mcp_public_address(compatible)) << "IPv4-compatible ::10.0.0.0";
    bytes local_nat64{0x00, 0x64, 0xFF, 0x9B, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 8, 8, 8, 8};
    EXPECT_FALSE(lake::api::mcp_public_address(local_nat64)) << "local-use NAT64 64:ff9b:1::/48";
    bytes six_to_four{0x20, 0x02, 10, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    EXPECT_FALSE(lake::api::mcp_public_address(six_to_four)) << "6to4 of 10.0.0.1";
    six_to_four[2] = 8;
    six_to_four[3] = 8;
    six_to_four[4] = 8;
    six_to_four[5] = 8;
    EXPECT_TRUE(lake::api::mcp_public_address(six_to_four)) << "6to4 of 8.8.8.8";
    bytes teredo{0x20, 0x01, 0x00, 0x00, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 1};
    EXPECT_FALSE(lake::api::mcp_public_address(teredo)) << "Teredo";
    bytes site_local(16, 0);
    site_local[0] = 0xFE;
    site_local[1] = 0xC0;
    EXPECT_FALSE(lake::api::mcp_public_address(site_local)) << "site-local fec0::/10";
    bytes global{0x26, 0x06, 0x47, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    EXPECT_TRUE(lake::api::mcp_public_address(global)) << "2606:4700::1";
    bytes unique(16, 0);
    unique[0] = 0xFD;
    EXPECT_FALSE(lake::api::mcp_public_address(unique));
    bytes link(16, 0);
    link[0] = 0xFE;
    link[1] = 0x80;
    EXPECT_FALSE(lake::api::mcp_public_address(link));
    bytes cloudflare(16, 0);
    cloudflare[0] = 0x26;
    cloudflare[1] = 0x06;
    cloudflare[2] = 0x47;
    EXPECT_TRUE(lake::api::mcp_public_address(cloudflare));
}

TEST(McpClient, AToolIsKeptOnlyWithinBounds) {
    lake::api::McpLimits limits;
    limits.max_description_bytes = 10;
    limits.max_schema_bytes = 200;
    const auto kept =
        lake::api::mcp_tool_from({{"name", "search.v2"},
                                  {"description", "Searches the whole archive \xC3\xA9tc"},
                                  {"inputSchema", {{"properties", {{"q", {{"type", "string"}}}}}}},
                                  {"annotations", {{"readOnlyHint", true}, {"title", "Search"}}}},
                                 limits);
    ASSERT_TRUE(kept) << kept.error();
    EXPECT_EQ(kept->description, "Searches t");
    EXPECT_EQ(kept->title, "Search");
    EXPECT_EQ(kept->input_schema.at("type"), "object");
    EXPECT_EQ(kept->read_only_hint, true);
    EXPECT_FALSE(kept->destructive_hint);
    for (const json& bad :
         {json{{"name", "has space"}, {"inputSchema", {{"type", "object"}}}},
          json{{"name", std::string(129, 'a')}, {"inputSchema", {{"type", "object"}}}},
          json{{"name", "x"}, {"inputSchema", {{"type", "string"}}}},
          json{{"name", "x"}},
          json{{"name", "x"},
               {"inputSchema", {{"type", "object"}, {"description", std::string(300, 'a')}}}},
          json("not an object")}) {
        EXPECT_FALSE(lake::api::mcp_tool_from(bad, limits)) << bad.dump().substr(0, 80);
    }
    const auto round = lake::api::mcp_tool_from_json(lake::api::mcp_tool_json(*kept));
    ASSERT_TRUE(round);
    EXPECT_EQ(round->name, "search.v2");
    EXPECT_EQ(round->read_only_hint, true);
}

McpClientOptions options_for(const McpTestServer& server) {
    McpClientOptions options;
    options.url = server.url();
    options.allow_private_addresses = true;
    options.limits.timeout = std::chrono::milliseconds(3000);
    options.client_version = "test";
    return options;
}

TEST(McpClient, DiscoversToolsResourcesAndPromptsOverStreamableHttp) {
    McpTestServer server;
    McpClient client(lake::api::make_mcp_http(true), options_for(server));
    const auto found = client.discover();
    ASSERT_TRUE(found) << lake::api::mcp_error_text(found.error());
    EXPECT_EQ(found->transport, "streamable_http");
    EXPECT_EQ(found->server.name, "test-server");
    EXPECT_EQ(found->server.protocol_version, "2025-06-18");
    EXPECT_TRUE(found->server.resources_subscribe);
    ASSERT_EQ(found->tools.size(), 4U) << "both pages of tools/list";
    EXPECT_EQ(found->tools[0].name, "echo");
    EXPECT_EQ(found->tools[0].read_only_hint, true);
    EXPECT_EQ(found->tools[1].read_only_hint, false);
    EXPECT_FALSE(found->tools[2].read_only_hint) << "inject proposes nothing";
    ASSERT_EQ(found->resources.size(), 1U);
    EXPECT_EQ(found->resources[0].uri, "memo://inbox");
    ASSERT_EQ(found->prompts.size(), 1U);

    const auto called = client.call_tool("echo", {{"text", "hello"}});
    ASSERT_TRUE(called) << lake::api::mcp_error_text(called.error());
    EXPECT_EQ(called->at("structuredContent").at("echo").at("text"), "hello");
    client.close();

    // initialize, then the session on every later request with the negotiated version.
    const auto seen = server.seen();
    ASSERT_GE(seen.size(), 6U);
    EXPECT_EQ(seen[0].rpc.at("method"), "initialize");
    EXPECT_EQ(seen[0].rpc.at("params").at("capabilities"), json::object())
        << "Lake declares no sampling, roots or elicitation";
    EXPECT_EQ(seen[0].headers.count("mcp-session-id"), 0U);
    EXPECT_EQ(seen[1].rpc.at("method"), "notifications/initialized");
    for (std::size_t i = 1; i < seen.size(); ++i) {
        EXPECT_TRUE(seen[i].headers.at("mcp-session-id").starts_with("session-")) << i;
        EXPECT_EQ(seen[i].headers.at("mcp-protocol-version"), "2025-06-18") << i;
        EXPECT_EQ(seen[i].headers.at("user-agent"), "Lake-MCP-Client/test");
    }
    EXPECT_EQ(seen.back().method, "DELETE") << "close ends the session";
    EXPECT_GT(client.traffic().sent_bytes, 0U);
    EXPECT_GT(client.traffic().received_bytes, 0U);
}

TEST(McpClient, AnEventStreamAnswerIsReadUntilItsAnswerAndNoLonger) {
    McpTestServer server;
    server.answer = "sse_keep_open";
    server.notify_before_answer = true;
    server.wrong_id_first = true;
    server.server_request_first = true;
    auto options = options_for(server);
    options.limits.timeout = std::chrono::milliseconds(5000);
    McpClient client(lake::api::make_mcp_http(true), options);
    const auto started = std::chrono::steady_clock::now();
    const auto called = client.call_tool("echo", {{"text", "mine"}});
    const auto took = std::chrono::steady_clock::now() - started;
    ASSERT_TRUE(called) << lake::api::mcp_error_text(called.error());
    EXPECT_EQ(called->at("structuredContent").at("echo").at("text"), "mine")
        << "the answer to another id is not this call's";
    EXPECT_LT(took, std::chrono::milliseconds(2500))
        << "a stream left open after the answer does not hold the call to its deadline";
    const auto notes = client.take_notifications();
    ASSERT_EQ(notes.size(), 1U);
    EXPECT_EQ(notes[0].method, "notifications/message");
    for (const auto& request : server.seen()) {
        EXPECT_NE(request.rpc.value("id", json()), json(9999))
            << "a server request is never answered";
    }
}

TEST(McpClient, AHostileServerIsBoundedByDeadlineSizeAndShape) {
    {
        McpTestServer server;
        server.slow_ms = 1500;
        auto options = options_for(server);
        options.limits.timeout = std::chrono::milliseconds(400);
        McpClient client(lake::api::make_mcp_http(true), options);
        ASSERT_TRUE(client.initialize());
        const auto started = std::chrono::steady_clock::now();
        const auto called = client.call_tool("echo", {{"text", "x"}});
        ASSERT_FALSE(called);
        EXPECT_EQ(called.error().kind, "timeout");
        EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(1400));
    }
    {
        McpTestServer server;
        server.huge_bytes = 300'000;
        auto options = options_for(server);
        options.limits.max_response_bytes = 64 * 1024;
        McpClient client(lake::api::make_mcp_http(true), options);
        const auto called = client.call_tool("echo", {{"text", "x"}});
        ASSERT_FALSE(called);
        EXPECT_EQ(called.error().kind, "too_large");
    }
    {
        McpTestServer server;
        server.huge_bytes = 300'000;
        server.answer = "sse";
        auto options = options_for(server);
        options.limits.max_response_bytes = 64 * 1024;
        McpClient client(lake::api::make_mcp_http(true), options);
        const auto called = client.call_tool("echo", {{"text", "x"}});
        ASSERT_FALSE(called);
        EXPECT_EQ(called.error().kind, "too_large") << "on an event stream too";
    }
    {
        McpTestServer server;
        server.garbage = true;
        McpClient client(lake::api::make_mcp_http(true), options_for(server));
        const auto called = client.call_tool("echo", {{"text", "x"}});
        ASSERT_FALSE(called);
        EXPECT_EQ(called.error().kind, "protocol");
    }
    {
        McpTestServer server;
        server.endless_events = true;
        auto options = options_for(server);
        options.limits.max_events = 50;
        McpClient client(lake::api::make_mcp_http(true), options);
        const auto started = std::chrono::steady_clock::now();
        const auto called = client.call_tool("echo", {{"text", "x"}});
        ASSERT_FALSE(called);
        EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(2500))
            << "events that never answer stop at the event bound, not the deadline";
    }
    {
        McpTestServer server;
        server.protocol_version = "1999-01-01";
        McpClient client(lake::api::make_mcp_http(true), options_for(server));
        const auto started = client.initialize();
        ASSERT_FALSE(started);
        EXPECT_EQ(started.error().kind, "unsupported_version");
    }
    {
        McpTestServer server;
        server.tools = json::array(
            {{{"name", "ok"}, {"inputSchema", {{"type", "object"}}}},
             {{"name", "bad name"}, {"inputSchema", {{"type", "object"}}}},
             {{"name", "ok"}, {"inputSchema", {{"type", "object"}}}},
             {{"name", "big"},
              {"inputSchema", {{"type", "object"}, {"description", std::string(40'000, 'x')}}}}});
        McpClient client(lake::api::make_mcp_http(true), options_for(server));
        const auto found = client.discover();
        ASSERT_TRUE(found);
        ASSERT_EQ(found->tools.size(), 1U);
        EXPECT_EQ(found->skipped.size(), 3U) << found->skipped.size();
    }
}

TEST(McpClient, SignInIsAskedForWithTheResourceMetadata) {
    McpTestServer server;
    server.required_bearer = "right";
    McpClient refused(lake::api::make_mcp_http(true), options_for(server));
    const auto started = refused.initialize();
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error().kind, "auth_required");
    EXPECT_EQ(started.error().resource_metadata,
              server.base() + "/.well-known/oauth-protected-resource/mcp");
    auto options = options_for(server);
    options.credential.bearer = "right";
    McpClient accepted(lake::api::make_mcp_http(true), options);
    EXPECT_TRUE(accepted.initialize());
}

TEST(McpClient, AnExpiredSessionIsStartedAgainOnce) {
    McpTestServer server;
    McpClient client(lake::api::make_mcp_http(true), options_for(server));
    ASSERT_TRUE(client.initialize());
    const std::string first = client.session_id();
    server.expire_session_once = true;
    const auto called = client.call_tool("echo", {{"text", "again"}});
    ASSERT_TRUE(called) << lake::api::mcp_error_text(called.error());
    EXPECT_NE(client.session_id(), first);
    EXPECT_EQ(server.calls("initialize").size(), 2U);
}

TEST(McpClient, TheOlderHttpSseTransportIsUsedWhenTheServerHasNoOther) {
    McpTestServer server;
    server.transport = "sse";
    McpClient client(lake::api::make_mcp_http(true), options_for(server));
    const auto found = client.discover();
    ASSERT_TRUE(found) << lake::api::mcp_error_text(found.error());
    EXPECT_EQ(found->transport, "sse");
    EXPECT_EQ(found->tools.size(), 4U);
    const auto called = client.call_tool("echo", {{"text", "old"}});
    ASSERT_TRUE(called) << lake::api::mcp_error_text(called.error());
    EXPECT_EQ(called->at("structuredContent").at("echo").at("text"), "old");
    client.close();
}

TEST(McpClient, NotificationsArriveOnTheGetStream) {
    McpTestServer server;
    McpClient client(lake::api::make_mcp_http(true), options_for(server));
    ASSERT_TRUE(client.initialize());
    ASSERT_TRUE(client.subscribe_resource("memo://inbox"));
    EXPECT_EQ(server.subscribed().count("memo://inbox"), 1U);
    std::vector<lake::api::McpNotification> got;
    for (int i = 0; i < 20 && got.size() < 3; ++i) {
        auto heard = client.listen(std::chrono::milliseconds(500));
        if (!heard) {
            break;
        }
        got.insert(got.end(), heard->begin(), heard->end());
    }
    ASSERT_EQ(got.size(), 3U);
    EXPECT_EQ(got[0].method, "notifications/resources/updated");
    EXPECT_EQ(got[2].params.at("n"), 2);

    McpTestServer quiet;
    quiet.no_get_stream = true;
    McpClient other(lake::api::make_mcp_http(true), options_for(quiet));
    const auto heard = other.listen(std::chrono::milliseconds(200));
    ASSERT_FALSE(heard);
    EXPECT_EQ(heard.error().kind, "not_supported");
}

TEST(McpClient, ThePublicTransportRefusesPrivateAddresses) {
    McpTestServer server;
    McpClientOptions options;
    options.url =
        "https://127.0.0.1:" + server.base().substr(server.base().rfind(':') + 1) + "/mcp";
    options.limits.timeout = std::chrono::milliseconds(2000);
    McpClient client(lake::api::make_mcp_http(false), options);
    const auto started = client.initialize();
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error().kind, "unreachable");
    EXPECT_NE(started.error().message.find("not a public address"), std::string::npos)
        << started.error().message;
    EXPECT_TRUE(server.seen().empty()) << "nothing was sent";
}

// lake#659: a stdio server's App is reached at its cluster Service name over plain http, and only
// there: the flag never opens loopback, a private literal or another host.
TEST(McpClient, AWorkspaceAppIsReachedOnlyAtAClusterServiceName) {
    using lake::api::mcp_cluster_service_host;
    using lake::api::parse_mcp_url;
    EXPECT_TRUE(mcp_cluster_service_host("app-0a1b2c3d4e5f.dev-ws-1.svc.cluster.local"));
    for (const std::string host : {"svc.cluster.local",
                                   "x.svc.cluster.local",
                                   "a.b.c.svc.cluster.local",
                                   "App.ns.svc.cluster.local",
                                   "-a.ns.svc.cluster.local",
                                   "a.ns.svc.cluster.local.evil.example",
                                   "10.0.0.1"}) {
        EXPECT_FALSE(mcp_cluster_service_host(host)) << host;
    }
    const std::string app = "http://app-0a1b2c3d4e5f.dev-ws-1.svc.cluster.local/mcp";
    EXPECT_FALSE(parse_mcp_url(app, false)) << "never without the flag";
    EXPECT_FALSE(parse_mcp_url(app, true)) << "the test allowance is loopback only";
    const auto parsed = parse_mcp_url(app, false, true);
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_EQ(parsed->port, "80");
    EXPECT_FALSE(parse_mcp_url("http://10.0.0.1/mcp", false, true));
    EXPECT_FALSE(parse_mcp_url("http://127.0.0.1:9/mcp", false, true));
    EXPECT_FALSE(parse_mcp_url("http://mcp.example.com/mcp", false, true));

    // A client marked for a workspace App refuses any other host before sending anything.
    McpTestServer server;
    McpClientOptions options = options_for(server);
    options.allow_private_addresses = false;
    options.cluster_service = true;
    McpClient client(lake::api::make_mcp_http(false), options);
    const auto started = client.initialize();
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error().kind, "unreachable");
    EXPECT_TRUE(server.seen().empty());
}

// lake#659: Lake's own client through lake-mcp-runner (oramasearch/lake-mcp-runner) serving a real
// stdio server. Run the runner locally first, e.g. with the MCP reference server:
//   MCP_RUNNER_TOKEN=<32+ chars> MCP_STDIO_COMMAND='["npx","--yes",
//     "@modelcontextprotocol/server-everything@2026.8.31","stdio"]' go run ./cmd/lake-mcp-runner
// then LAKE_MCP_RUNNER_URL=http://127.0.0.1:8080/mcp LAKE_MCP_RUNNER_TOKEN=<the token>.
TEST(McpClient, DrivesAStdioServerThroughTheRunner) {
    const char* url = std::getenv("LAKE_MCP_RUNNER_URL");
    const char* token = std::getenv("LAKE_MCP_RUNNER_TOKEN");
    if (url == nullptr || token == nullptr) {
        GTEST_SKIP()
            << "set LAKE_MCP_RUNNER_URL and LAKE_MCP_RUNNER_TOKEN to a running lake-mcp-runner";
    }
    McpClientOptions options;
    options.url = url;
    options.allow_private_addresses = true;
    options.credential.bearer = token;
    options.limits.timeout = std::chrono::milliseconds(30000);
    options.client_version = "runner-conformance";
    McpClient client(lake::api::make_mcp_http(true), options);
    const auto found = client.discover();
    ASSERT_TRUE(found) << lake::api::mcp_error_text(found.error());
    EXPECT_EQ(found->transport, "streamable_http");
    EXPECT_FALSE(found->server.name.empty());
    const auto echo =
        std::ranges::find(found->tools, std::string("echo"), &lake::api::McpTool::name);
    ASSERT_NE(echo, found->tools.end()) << "the reference server's echo tool";
    const auto called = client.call_tool("echo", {{"message", "through the runner"}});
    ASSERT_TRUE(called) << lake::api::mcp_error_text(called.error());
    EXPECT_NE(called->dump().find("through the runner"), std::string::npos) << called->dump();
    EXPECT_FALSE(found->resources.empty());
    const auto read = client.read_resource(found->resources[0].uri);
    ASSERT_TRUE(read) << lake::api::mcp_error_text(read.error());
    client.close();

    McpClientOptions wrong = options;
    wrong.credential.bearer = "not-the-token";
    McpClient refused(lake::api::make_mcp_http(true), wrong);
    const auto denied = refused.initialize();
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().kind, "auth_required") << lake::api::mcp_error_text(denied.error());
}

} // namespace
