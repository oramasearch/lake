// SPDX-License-Identifier: MIT

// lake#728: secure-networking desired state is one permission-checked capability registry rendered
// as REST, OpenAPI and MCP. Live tunnel sessions and activity explicitly await their workspace
// record provider; their reads authorize before returning an explicit pending marker.

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/mcp.hpp"
#include "lake/api/rest.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/network_records.hpp"
#include "lake/control/pki.hpp"
#include <liquid/runtime.hpp>

namespace {

using nlohmann::json;

class NetworkCapabilities : public testing::Test {
protected:
    void SetUp() override {
        lake::api::register_core_capabilities(registry, "test");
        const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
        const auto made_owner = store.create_user({"taap:1:ed25519:network-owner-" + suffix,
                                                   "network-owner-" + suffix + "@example.test",
                                                   "Network Owner"});
        ASSERT_TRUE(made_owner) << made_owner.error().message;
        owner_id = made_owner->id;
        owner_taap = made_owner->taap_id;
        const auto made_reader = store.create_user({"taap:1:ed25519:network-reader-" + suffix,
                                                    "network-reader-" + suffix + "@example.test",
                                                    "Network Reader"});
        ASSERT_TRUE(made_reader) << made_reader.error().message;
        reader_id = made_reader->id;
        reader_taap = made_reader->taap_id;
        const auto made_outsider =
            store.create_user({"taap:1:ed25519:network-outsider-" + suffix,
                               "network-outsider-" + suffix + "@example.test",
                               "Network Outsider"});
        ASSERT_TRUE(made_outsider) << made_outsider.error().message;
        outsider_id = made_outsider->id;
        outsider_taap = made_outsider->taap_id;

        const auto customer =
            store.create_customer({"individual", owner_id, "", "network-capabilities-" + suffix});
        ASSERT_TRUE(customer) << customer.error().message;
        customer_id = customer->id;
        const auto object = store.find_policy_object("customer", customer_id);
        ASSERT_TRUE(object) << object.error().message;
        ASSERT_TRUE(store.create_grant(
            {owner_taap, object->id, "admin", "owner-attestation", "admin", "owner-grant"}));
        ASSERT_TRUE(store.create_grant(
            {reader_taap, object->id, "read", "reader-attestation", "admin", "reader-grant"}));
        const auto workspace = store.create_workspace({customer_id, "Network", "dev", owner_taap});
        ASSERT_TRUE(workspace) << workspace.error().message;
        workspace_id = workspace->id;
        target_id = workspace->zetta_service_id;
        const auto app = store.create_web_app({.customer_id = customer_id,
                                               .workspace_id = workspace_id,
                                               .name = "network-client",
                                               .mode = "image",
                                               .image = "client:v1",
                                               .port = 3000,
                                               .host = "network-client.test"});
        ASSERT_TRUE(app) << app.error().message;
        source_id = app->id;

        lake::control::NetworkPeerSpec peer{
            .name = "remote",
            .remote = "gateway.test:7443",
            .exports = json::array(
                {{{"target_kind", "service"}, {"target_id", target_id}, {"port", 4800}}}),
            .imports = json::array({{{"name", "reader"}, {"port", 4800}}})};
        const auto created_peer = store.create_network_peer(
            {customer_id, workspace_id},
            peer,
            {owner_id, owner_taap, "network-peer-setup", {.instance_kek = std::string(64, '7')}});
        ASSERT_TRUE(created_peer) << created_peer.error().message;
        peer_id = created_peer->id;

        context.store = &store;
        context.authorized = true;
        context.pki = &pki;
        context.instance_kek = std::string(64, '7');
    }

    json scope() const {
        return {{"customer_id", customer_id}, {"workspace_id", workspace_id}};
    }

    json connection_input() const {
        json input = scope();
        input.update({{"source_kind", "app"},
                      {"source_id", source_id},
                      {"target_kind", "service"},
                      {"target_id", target_id},
                      {"ports", json::array({4800})},
                      {"credential_scope", {{"access", "read"}, {"schema", "analytics"}}}});
        return input;
    }

    json egress_input() const {
        json input = scope();
        input.update({{"dns_name", "api.example.test"}, {"ports", json::array({443})}});
        return input;
    }

    json tunnel_input(const std::string& user_id) const {
        json input = scope();
        input.update({{"user_id", user_id},
                      {"target_kind", "service"},
                      {"target_id", target_id},
                      {"port", 4800},
                      {"expires_at",
                       lake::control::iso8601_at(std::chrono::system_clock::now() +
                                                 std::chrono::hours(1))}});
        return input;
    }

    lake::api::ApiResult call(const std::string& name,
                              const json& input,
                              const std::string& identity = {},
                              const std::string& user_id = {}) {
        context.acting_identity = identity.empty() ? owner_taap : identity;
        context.acting_user_id = user_id.empty() ? owner_id : user_id;
        context.operator_credential = context.acting_identity == "admin";
        context.request_id = "network-capability-" + std::to_string(++request_number);
        context.audit_customer_id.clear();
        context.audit_object.clear();
        context.audit_details = json::object();
        return liquid::runtime::block_on(registry.call_async(name, input, context)).value();
    }

    lake::control::InMemoryStore store;
    lake::control::pki::Service pki;
    lake::api::Registry registry;
    lake::api::ApiContext context;
    int request_number = 0;
    std::string owner_id;
    std::string owner_taap;
    std::string reader_id;
    std::string reader_taap;
    std::string outsider_id;
    std::string outsider_taap;
    std::string customer_id;
    std::string workspace_id;
    std::string source_id;
    std::string target_id;
    std::string peer_id;
};

TEST_F(NetworkCapabilities, DesiredRulesCanBeDeclaredListedEndedAndReadEffectively) {
    const auto connection = call("create_network_connection", connection_input());
    ASSERT_TRUE(connection.ok) << connection.error.message;
    EXPECT_EQ(connection.value.at("credential_scope").at("access"), "read");
    EXPECT_EQ(connection.value.at("desired"), "present");
    const std::string connection_id = connection.value.at("id");
    ASSERT_TRUE(store.set_network_connection_status(connection_id,
                                                    {.phase = "Ready",
                                                     .message = "applied",
                                                     .observed_generation = 1,
                                                     .credential_secret = "network-credential"}));

    const auto connections = call("list_network_connections", scope());
    ASSERT_TRUE(connections.ok) << connections.error.message;
    ASSERT_EQ(connections.value.at("connections").size(), 1U);
    EXPECT_EQ(connections.value.at("connections")[0].at("status").at("phase"), "Ready");
    EXPECT_EQ(connections.value.at("connections")[0].at("status").at("credential_secret"),
              "network-credential");

    json rotate = scope();
    rotate["connection_id"] = connection_id;
    const auto rotated = call("rotate_network_connection_credential", rotate);
    ASSERT_TRUE(rotated.ok) << rotated.error.message;
    EXPECT_EQ(rotated.value.at("generation"), 2);
    EXPECT_EQ(rotated.value.at("credential_scope"), connection.value.at("credential_scope"));
    EXPECT_EQ(rotated.value.at("status").at("observed_generation"), 1);
    EXPECT_EQ(rotated.value.at("status").at("credential_secret"), "network-credential");
    ASSERT_TRUE(store.set_network_connection_status(connection_id,
                                                    {.phase = "Ready",
                                                     .message = "rotated",
                                                     .observed_generation = 2,
                                                     .credential_secret = "network-credential"}));

    const auto egress = call("create_network_egress_rule", egress_input());
    ASSERT_TRUE(egress.ok) << egress.error.message;
    EXPECT_EQ(egress.value.at("dns_name"), "api.example.test");
    const auto egress_rules = call("list_network_egress_rules", scope());
    ASSERT_TRUE(egress_rules.ok) << egress_rules.error.message;
    ASSERT_EQ(egress_rules.value.at("egress_rules").size(), 1U);

    const auto tunnel = call("grant_network_tunnel", tunnel_input(owner_id));
    ASSERT_TRUE(tunnel.ok) << tunnel.error.message;
    EXPECT_TRUE(tunnel.value.at("active"));
    const std::string tunnel_id = tunnel.value.at("id");
    ASSERT_TRUE(store.set_network_tunnel_grant_status(tunnel_id,
                                                      {.phase = "Ready",
                                                       .message = "relay ready",
                                                       .observed_generation = 1,
                                                       .endpoint = "relay.test:7443"}));
    const auto grants = call("list_network_tunnel_grants", scope());
    ASSERT_TRUE(grants.ok) << grants.error.message;
    ASSERT_EQ(grants.value.at("tunnel_grants").size(), 1U);
    EXPECT_EQ(grants.value.at("tunnel_grants")[0].at("status").at("endpoint"), "relay.test:7443");

    const auto peers = call("list_network_peers", scope());
    ASSERT_TRUE(peers.ok) << peers.error.message;
    ASSERT_EQ(peers.value.at("peers").size(), 1U);
    EXPECT_EQ(peers.value.at("peers")[0].at("name"), "remote");

    const auto effective = call("network_rules", scope());
    ASSERT_TRUE(effective.ok) << effective.error.message;
    EXPECT_EQ(effective.value.at("connections").size(), 1U);
    EXPECT_EQ(effective.value.at("connections")[0].at("status").at("observed_generation"), 2);
    EXPECT_EQ(effective.value.at("connections")[0].at("status").at("credential_secret"),
              "network-credential");
    EXPECT_EQ(effective.value.at("egress_rules").size(), 1U);
    EXPECT_EQ(effective.value.at("tunnel_grants").size(), 1U);
    EXPECT_EQ(effective.value.at("peers").size(), 1U);

    json remove = scope();
    remove["connection_id"] = connection_id;
    const auto removed = call("remove_network_connection", remove);
    ASSERT_TRUE(removed.ok) << removed.error.message;
    EXPECT_EQ(removed.value.at("desired"), "absent");
    EXPECT_EQ(removed.value.at("generation"), 3);
    const auto withdrawn_rotation = call("rotate_network_connection_credential", rotate);
    EXPECT_FALSE(withdrawn_rotation.ok);
    EXPECT_EQ(withdrawn_rotation.error.code, "conflict");
    EXPECT_EQ(withdrawn_rotation.error.path, "/connection_id");
    json revoke = scope();
    revoke["tunnel_grant_id"] = tunnel_id;
    const auto revoked = call("revoke_network_tunnel", revoke);
    ASSERT_TRUE(revoked.ok) << revoked.error.message;
    EXPECT_FALSE(revoked.value.at("active"));
    ASSERT_TRUE(store.withdraw_network_peer(
        {customer_id, workspace_id},
        peer_id,
        {owner_id, owner_taap, "withdraw-peer", {.instance_kek = context.instance_kek}}));

    const auto after = call("network_rules", scope());
    ASSERT_TRUE(after.ok) << after.error.message;
    EXPECT_TRUE(after.value.at("connections").empty());
    EXPECT_EQ(after.value.at("egress_rules").size(), 1U);
    EXPECT_TRUE(after.value.at("tunnel_grants").empty());
    EXPECT_TRUE(after.value.at("peers").empty());
    EXPECT_EQ(call("list_network_connections", scope()).value.at("connections").size(), 1U);
    EXPECT_EQ(call("list_network_tunnel_grants", scope()).value.at("tunnel_grants").size(), 1U);
    EXPECT_EQ(call("list_network_peers", scope()).value.at("peers").size(), 1U);
}

TEST_F(NetworkCapabilities, OnlyScopedCredentialsCanBeRotated) {
    json input = connection_input();
    input["credential_scope"] = nullptr;
    const auto connection = call("create_network_connection", input);
    ASSERT_TRUE(connection.ok) << connection.error.message;
    json rotate = scope();
    rotate["connection_id"] = connection.value.at("id");
    const auto refused = call("rotate_network_connection_credential", rotate);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "conflict");
    EXPECT_EQ(refused.error.path, "/connection_id");
    EXPECT_EQ(call("list_network_connections", scope()).value.at("connections")[0].at("generation"),
              1);
}

TEST_F(NetworkCapabilities, AccountPermissionsApplyBeforeEveryReadOrWrite) {
    const auto connection = call("create_network_connection", connection_input());
    ASSERT_TRUE(connection.ok) << connection.error.message;
    const auto tunnel = call("grant_network_tunnel", tunnel_input(owner_id));
    ASSERT_TRUE(tunnel.ok) << tunnel.error.message;

    const std::vector<std::string> reads = {"list_network_connections",
                                            "list_network_egress_rules",
                                            "list_network_tunnel_grants",
                                            "list_network_tunnel_sessions",
                                            "list_network_peers",
                                            "network_rules",
                                            "network_activity"};
    for (const std::string& name : reads) {
        const auto reader = call(name, scope(), reader_taap, reader_id);
        EXPECT_NE(reader.error.code, "unauthorized") << name;
        const auto outsider = call(name, scope(), outsider_taap, outsider_id);
        EXPECT_FALSE(outsider.ok) << name;
        EXPECT_EQ(outsider.error.code, "unauthorized") << name;
    }

    json remove = scope();
    remove["connection_id"] = connection.value.at("id");
    json revoke = scope();
    revoke["tunnel_grant_id"] = tunnel.value.at("id");
    const std::vector<std::pair<std::string, json>> writes = {
        {"create_network_connection", connection_input()},
        {"rotate_network_connection_credential", remove},
        {"remove_network_connection", remove},
        {"create_network_egress_rule", egress_input()},
        {"grant_network_tunnel", tunnel_input(owner_id)},
        {"revoke_network_tunnel", revoke}};
    for (const auto& [name, input] : writes) {
        const auto reader = call(name, input, reader_taap, reader_id);
        EXPECT_FALSE(reader.ok) << name;
        EXPECT_EQ(reader.error.code, "unauthorized") << name;
    }

    const auto sessions = call("list_network_tunnel_sessions", scope());
    ASSERT_TRUE(sessions.ok) << sessions.error.message;
    EXPECT_TRUE(sessions.value.at("sessions").empty());
    EXPECT_FALSE(sessions.value.at("available"));
    EXPECT_EQ(sessions.value.at("pending"), "zetta#1281");
    const auto activity = call("network_activity", scope());
    ASSERT_TRUE(activity.ok) << activity.error.message;
    EXPECT_TRUE(activity.value.at("events").empty());
    EXPECT_FALSE(activity.value.at("available"));
    EXPECT_EQ(activity.value.at("pending"), "zetta#1281");

    const auto foreign_recipient = call("grant_network_tunnel", tunnel_input(outsider_id));
    EXPECT_FALSE(foreign_recipient.ok);
    EXPECT_EQ(foreign_recipient.error.code, "invalid_input");
    EXPECT_EQ(foreign_recipient.error.path, "/user_id");

    const auto other_customer =
        store.create_customer({"individual", outsider_id, "", "network-foreign-account"});
    ASSERT_TRUE(other_customer) << other_customer.error().message;
    json mismatched = connection_input();
    mismatched["customer_id"] = other_customer->id;
    EXPECT_EQ(call("create_network_connection", mismatched, "admin", "").error.path,
              "/customer_id");
}

class TestNetworkRecords final : public lake::control::NetworkRecords {
public:
    liquid::runtime::task<lake::control::ZettaControlResult<lake::control::NetworkRecordPage>>
    sessions(lake::control::NetworkScope scope, std::size_t limit) override {
        ++session_calls;
        last_scope = scope;
        last_limit = limit;
        co_return page;
    }

    liquid::runtime::task<lake::control::ZettaControlResult<lake::control::NetworkRecordPage>>
    activity(lake::control::NetworkScope scope, std::size_t limit) override {
        ++activity_calls;
        last_scope = scope;
        last_limit = limit;
        co_return page;
    }

    int session_calls = 0;
    int activity_calls = 0;
    lake::control::NetworkScope last_scope;
    std::size_t last_limit = 0;
    lake::control::NetworkRecordPage page{
        .records = json::array({json::object()}), .available = true, .pending = ""};
};

TEST_F(NetworkCapabilities, NetworkRecordProviderIsScopedBoundedAndPermissionChecked) {
    TestNetworkRecords provider;
    context.network_records = &provider;
    auto request = scope();
    request["limit"] = 2;
    EXPECT_FALSE(call("list_network_tunnel_sessions", request, outsider_taap, outsider_id).ok);
    EXPECT_FALSE(call("network_activity", request, outsider_taap, outsider_id).ok);
    EXPECT_EQ(provider.session_calls, 0);
    EXPECT_EQ(provider.activity_calls, 0);
    for (const auto& name : {"list_network_tunnel_sessions", "network_activity"}) {
        const auto result = call(name, request, reader_taap, reader_id);
        ASSERT_TRUE(result.ok) << result.error.message;
        EXPECT_TRUE(result.value.at("available"));
        EXPECT_EQ(result.value.at("pending"), "");
        EXPECT_EQ(provider.last_scope.customer_id, customer_id);
        EXPECT_EQ(provider.last_scope.workspace_id, workspace_id);
        EXPECT_EQ(provider.last_limit, 2U);
    }
    provider.page.records = json::array({json::object(), json::object(), json::object()});
    EXPECT_EQ(call("list_network_tunnel_sessions", request).error.code, "store_error");
    provider.page.records = json::object();
    EXPECT_EQ(call("network_activity", request).error.code, "store_error");
    provider.page.records = json::array({1});
    EXPECT_EQ(call("network_activity", request).error.code, "store_error");
    provider.page.records = json::array();
    provider.page.available = false;
    EXPECT_EQ(call("network_activity", request).error.code, "store_error");
}

TEST_F(NetworkCapabilities, OpenApiAndMcpRenderAndCallEveryNetworkCapability) {
    const std::vector<std::string> names = {"create_network_connection",
                                            "list_network_connections",
                                            "rotate_network_connection_credential",
                                            "remove_network_connection",
                                            "create_network_egress_rule",
                                            "list_network_egress_rules",
                                            "grant_network_tunnel",
                                            "issue_network_tunnel_certificate",
                                            "list_network_tunnel_grants",
                                            "revoke_network_tunnel",
                                            "list_network_tunnel_sessions",
                                            "list_network_peers",
                                            "network_rules",
                                            "network_activity"};
    const json openapi = lake::api::openapi_document(registry, "test");
    const auto signer = registry.find("issue_network_tunnel_certificate");
    ASSERT_NE(signer, nullptr);
    EXPECT_TRUE(lake::api::capability_writes(*signer));
    EXPECT_FALSE(lake::api::capability_idempotent(*signer));
    EXPECT_FALSE(lake::api::capability_destroys(*signer));
    context.acting_identity = owner_taap;
    context.acting_user_id = owner_id;
    context.request_id = "network-mcp-list";
    const auto listed = lake::api::mcp_dispatch(
        registry, {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}, context, "test");
    ASSERT_TRUE(listed);
    const json tools = json::parse(*listed).at("result").at("tools");
    for (const std::string& name : names) {
        const auto capability = registry.find(name);
        ASSERT_NE(capability, nullptr) << name;
        const std::string method =
            capability->kind == lake::api::CapabilityKind::Read ? "get" : "post";
        ASSERT_TRUE(openapi.at("paths").contains("/v1/" + name)) << name;
        EXPECT_TRUE(openapi.at("paths").at("/v1/" + name).contains(method)) << name;
        const auto tool = std::ranges::find_if(
            tools, [&](const json& value) { return value.at("name") == name; });
        ASSERT_NE(tool, tools.end()) << name;
        EXPECT_EQ(tool->at("inputSchema"), capability->input_schema) << name;
        EXPECT_EQ(tool->at("annotations").at("readOnlyHint"),
                  !lake::api::capability_writes(*capability))
            << name;
    }

    context.request_id = "network-mcp-call";
    const auto called = lake::api::mcp_dispatch(
        registry,
        {{"jsonrpc", "2.0"},
         {"id", 2},
         {"method", "tools/call"},
         {"params", {{"name", "list_network_peers"}, {"arguments", scope()}}}},
        context,
        "test");
    ASSERT_TRUE(called);
    const json reply = json::parse(*called).at("result");
    EXPECT_FALSE(reply.at("isError"));
    EXPECT_EQ(reply.at("structuredContent").at("value").at("peers").size(), 1U);
}

} // namespace
