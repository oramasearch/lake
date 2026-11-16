// SPDX-License-Identifier: MIT

#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/telemetry/metrics.hpp"
#include "postgres_fixture.hpp"

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <set>

namespace {
using namespace lake::control;
using nlohmann::json;
constexpr const char* test_kek = "network-control-plane-test-key";

std::string future_time() {
    return iso8601_at(std::chrono::system_clock::now() + std::chrono::hours(1));
}

std::string schema_sql() {
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    return std::string(std::istreambuf_iterator<char>(file), {});
}

class NetworkStore : public testing::TestWithParam<bool> {
protected:
    virtual bool postgres() const {
        return GetParam();
    }

    void SetUp() override {
        if (postgres()) {
            uri = lake::test::postgres_uri();
            if (!uri) {
                GTEST_SKIP() << "LAKE_TEST_DB_URI is absent";
            }
            backing = make_postgres_store(uri, test_kek);
        } else {
            backing = std::make_shared<InMemoryStore>();
        }
        store = lake::telemetry::instrument_store(backing, metrics);
        auto user = store->create_user({"taap:network-owner", "", "Owner"});
        ASSERT_TRUE(user);
        auto editor = store->create_user({"taap:network-editor", "", "Editor"});
        ASSERT_TRUE(editor);
        actor = {user->id, user->taap_id, "network-create"};
        other_actor = {editor->id, editor->taap_id, "network-update"};
        auto customer = store->create_customer({"individual", user->id, "", "network"});
        ASSERT_TRUE(customer);
        auto workspace = store->create_workspace({customer->id, "Network", "dev", user->taap_id});
        ASSERT_TRUE(workspace);
        scope = {customer->id, workspace->id};
        target = workspace->zetta_service_id;
        auto another = store->create_workspace({customer->id, "Other", "dev", user->taap_id});
        ASSERT_TRUE(another);
        other_scope = {customer->id, another->id};
        other_target = another->zetta_service_id;
        auto foreign_customer = store->create_customer({"individual", editor->id, "", "foreign"});
        ASSERT_TRUE(foreign_customer);
        auto foreign =
            store->create_workspace({foreign_customer->id, "Foreign", "dev", editor->taap_id});
        ASSERT_TRUE(foreign);
        foreign_scope = {foreign_customer->id, foreign->id};
        auto app = store->create_web_app({.customer_id = scope.customer_id,
                                          .workspace_id = scope.workspace_id,
                                          .name = "client",
                                          .mode = "image",
                                          .image = "client:v1",
                                          .port = 3000,
                                          .host = "network.test"});
        ASSERT_TRUE(app) << app.error().message;
        source = app->id;
    }

    NetworkConnectionSpec connection_spec() const {
        return {.source_kind = "app",
                .source_id = source,
                .target_kind = "service",
                .target_id = target};
    }

    NetworkEgressRuleSpec egress_spec() const {
        return {.cidr = "203.0.113.0/24", .ports = {443}};
    }

    NetworkTunnelGrantSpec tunnel_spec() const {
        return {.user_id = actor.user_id,
                .target_kind = "service",
                .target_id = target,
                .port = 4800,
                .expires_at = future_time(),
                .client_ca_pem = "client-ca"};
    }

    NetworkPeerSpec peer_spec() const {
        return {.name = "remote",
                .remote = "gateway.test:7443",
                .exports = json::array(
                    {{{"target_kind", "service"}, {"target_id", target}, {"port", 4800}}}),
                .imports = json::array({{{"name", "reader"}, {"port", 4800}}})};
    }

    void observe(const std::string& kind, const std::string& id) {
        NetworkStatus status{.phase = "Ready", .message = "applied", .observed_generation = 1};
        if (kind == "connection") {
            status.credential_secret = "connection-credential";
        } else if (kind == "tunnel_grant") {
            status.endpoint = "relay.test:7443";
            status.relay_ca_pem = "relay-ca";
        }
        if (auto* memory = dynamic_cast<InMemoryStore*>(backing.get())) {
            if (kind == "connection") {
                ASSERT_TRUE(memory->set_network_connection_status(id, status));
            } else if (kind == "egress_rule") {
                ASSERT_TRUE(memory->set_network_egress_rule_status(id, status));
            } else if (kind == "tunnel_grant") {
                ASSERT_TRUE(memory->set_network_tunnel_grant_status(id, status));
            } else {
                ASSERT_TRUE(memory->set_network_peer_status(id, status));
            }
            return;
        }
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        const std::string key = kind == "connection"     ? "connection_id"
                                : kind == "egress_rule"  ? "rule_id"
                                : kind == "tunnel_grant" ? "grant_id"
                                                         : "peer_id";
        tx.exec_params(
            "INSERT INTO network_" + kind + "_status (" + key +
                ", phase, message, observed_generation) VALUES ($1::uuid, 'Ready', 'applied', 1)",
            id);
        if (kind == "connection") {
            tx.exec_params("UPDATE network_connection_status SET credential_secret = $1 WHERE "
                           "connection_id = $2::uuid",
                           status.credential_secret,
                           id);
        } else if (kind == "tunnel_grant") {
            tx.exec_params("UPDATE network_tunnel_grant_status SET endpoint = $1, relay_ca_pem = "
                           "$3 WHERE grant_id = $2::uuid",
                           status.endpoint,
                           id,
                           status.relay_ca_pem);
        }
        tx.commit();
    }

    void verify_audit(const std::string& id, const std::string& kind) {
        const auto events = store->list_audit_events({});
        ASSERT_TRUE(events);
        ASSERT_EQ(events->size(), 3u);
        ASSERT_TRUE(lake::audit::verify(*store, *events));
        EXPECT_EQ(events->at(0).draft.principal, actor.principal);
        EXPECT_EQ(events->at(1).draft.principal, other_actor.principal);
        EXPECT_EQ(events->at(2).draft.principal, other_actor.principal);
        for (std::size_t i = 0; i < events->size(); ++i) {
            const auto& draft = events->at(i).draft;
            EXPECT_EQ(draft.customer_id, scope.customer_id);
            EXPECT_EQ(draft.outcome, "success");
            EXPECT_EQ(draft.object, "network_" + kind + "/" + id);
            EXPECT_EQ(draft.details.at("workspace_id"), scope.workspace_id);
            EXPECT_EQ(draft.details.at("generation"), i + 1);
            EXPECT_EQ(draft.request_id, i == 0 ? actor.request_id : other_actor.request_id);
        }
        EXPECT_EQ(events->at(2).draft.action,
                  (kind == "tunnel_grant" ? "revoke_network_" : "withdraw_network_") + kind);
    }

    const char* uri = nullptr;
    std::shared_ptr<Store> backing;
    std::shared_ptr<Store> store;
    lake::telemetry::Metrics metrics;
    NetworkScope scope, other_scope, foreign_scope;
    NetworkActor actor, other_actor;
    std::string source, target, other_target;
};

TEST_P(NetworkStore, ConnectionsKeepStatusAndAuditEveryMutation) {
    auto spec = connection_spec();
    auto created = store->create_network_connection(scope, spec, actor);
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_FALSE(created->status);
    EXPECT_TRUE(created->spec.ports.empty());
    EXPECT_TRUE(created->spec.credential_scope.is_null());
    EXPECT_EQ(created->created_by, actor.user_id);
    observe("connection", created->id);
    spec.ports = {4800, 4801};
    spec.credential_scope = {{"access", "read"}, {"schema", "analytics"}};
    auto updated = store->update_network_connection(scope, created->id, spec, other_actor);
    ASSERT_TRUE(updated) << updated.error().message;
    EXPECT_EQ(updated->generation, 2);
    EXPECT_EQ(updated->created_by, actor.user_id);
    EXPECT_EQ(updated->spec.credential_scope, spec.credential_scope);
    EXPECT_EQ(updated->spec.ports, spec.ports);
    ASSERT_TRUE(updated->status);
    EXPECT_EQ(updated->status->credential_secret, "connection-credential");
    EXPECT_EQ(updated->status->observed_generation, 1);
    EXPECT_EQ(store->get_network_connection(other_scope, created->id).error().code, "not_found");
    EXPECT_TRUE(store->list_network_connections(other_scope)->empty());
    EXPECT_EQ(store->get_network_connection(foreign_scope, created->id).error().code, "not_found");
    EXPECT_EQ(
        store->withdraw_network_connection(other_scope, created->id, other_actor).error().code,
        "not_found");
    auto removed = store->withdraw_network_connection(scope, created->id, other_actor);
    ASSERT_TRUE(removed);
    EXPECT_EQ(removed->desired, "absent");
    EXPECT_EQ(removed->generation, 3);
    EXPECT_EQ(store->update_network_connection(scope, created->id, spec, actor).error().code,
              "conflict");
    EXPECT_EQ(store->withdraw_network_connection(scope, created->id, actor).error().code,
              "conflict");
    EXPECT_EQ(store->get_network_connection(scope, created->id)->desired, "absent");
    EXPECT_EQ(store->list_network_connections(scope)->size(), 1u);
    verify_audit(created->id, "connection");
}

TEST_P(NetworkStore, EgressReplacesCidrWithDnsAndCanBeWithdrawn) {
    auto spec = egress_spec();
    auto created = store->create_network_egress_rule(scope, spec, actor);
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_FALSE(created->spec.source_kind);
    EXPECT_FALSE(created->spec.source_id);
    observe("egress_rule", created->id);
    spec.source_kind = "app";
    spec.source_id = source;
    spec.cidr.reset();
    spec.dns_name = "api.example.test";
    spec.ports = {443, 8443};
    auto updated = store->update_network_egress_rule(scope, created->id, spec, other_actor);
    ASSERT_TRUE(updated) << updated.error().message;
    EXPECT_EQ(updated->generation, 2);
    EXPECT_FALSE(updated->spec.cidr);
    EXPECT_EQ(updated->spec.dns_name, spec.dns_name);
    ASSERT_TRUE(updated->status);
    EXPECT_EQ(updated->status->phase, "Ready");
    EXPECT_EQ(store->get_network_egress_rule(other_scope, created->id).error().code, "not_found");
    EXPECT_EQ(
        store->update_network_egress_rule(foreign_scope, created->id, spec, actor).error().code,
        "not_found");
    EXPECT_TRUE(store->list_network_egress_rules(other_scope)->empty());
    auto removed = store->withdraw_network_egress_rule(scope, created->id, other_actor);
    ASSERT_TRUE(removed);
    EXPECT_EQ(removed->desired, "absent");
    EXPECT_EQ(store->withdraw_network_egress_rule(scope, created->id, actor).error().code,
              "conflict");
    EXPECT_EQ(store->update_network_egress_rule(scope, created->id, spec, actor).error().code,
              "conflict");
    EXPECT_EQ(store->get_network_egress_rule(scope, created->id)->generation, 3);
    EXPECT_EQ(store->list_network_egress_rules(scope)->size(), 1u);
    verify_audit(created->id, "egress_rule");
}

TEST_P(NetworkStore, GrantsRevokeWithoutAUnifyingDesiredColumn) {
    auto spec = tunnel_spec();
    auto created = store->create_network_tunnel_grant(scope, spec, actor, {});
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_FALSE(created->revoked_at);
    observe("tunnel_grant", created->id);
    spec.port = 443;
    spec.expires_at = iso8601_at(std::chrono::system_clock::now() + std::chrono::hours(2));
    auto updated = store->update_network_tunnel_grant(scope, created->id, spec, other_actor);
    ASSERT_TRUE(updated) << updated.error().message;
    EXPECT_EQ(updated->generation, 2);
    EXPECT_EQ(updated->spec.port, 443);
    EXPECT_EQ(updated->spec.expires_at, spec.expires_at);
    ASSERT_TRUE(updated->status);
    EXPECT_EQ(updated->status->endpoint, "relay.test:7443");
    EXPECT_EQ(updated->status->relay_ca_pem, "relay-ca");
    EXPECT_EQ(updated->spec.client_ca_pem, "client-ca");
    EXPECT_EQ(store->get_network_tunnel_grant(other_scope, created->id).error().code, "not_found");
    EXPECT_EQ(store->revoke_network_tunnel_grant(foreign_scope, created->id, actor).error().code,
              "not_found");
    EXPECT_TRUE(store->list_network_tunnel_grants(other_scope)->empty());
    auto revoked = store->revoke_network_tunnel_grant(scope, created->id, other_actor);
    ASSERT_TRUE(revoked);
    ASSERT_TRUE(revoked->revoked_at);
    EXPECT_EQ(revoked->generation, 3);
    EXPECT_EQ(store->update_network_tunnel_grant(scope, created->id, spec, actor).error().code,
              "conflict");
    EXPECT_EQ(store->revoke_network_tunnel_grant(scope, created->id, actor).error().code,
              "conflict");
    EXPECT_TRUE(store->get_network_tunnel_grant(scope, created->id)->revoked_at);
    EXPECT_EQ(store->list_network_tunnel_grants(scope)->size(), 1u);
    verify_audit(created->id, "tunnel_grant");
}

// lake#983: a new grant past the workspace's or the account's active grant cap is
// refused with quota_exceeded and writes nothing; a revoked grant frees its place, and an account
// elsewhere is unaffected.
TEST_P(NetworkStore, ActiveTunnelGrantsAreCappedPerWorkspaceAndPerAccount) {
    const NetworkTunnelGrantLimits limits{.per_workspace = 2, .per_customer = 3};
    auto first = store->create_network_tunnel_grant(scope, tunnel_spec(), actor, limits);
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(store->create_network_tunnel_grant(scope, tunnel_spec(), actor, limits));
    const auto over_workspace =
        store->create_network_tunnel_grant(scope, tunnel_spec(), actor, limits);
    ASSERT_FALSE(over_workspace);
    EXPECT_EQ(over_workspace.error().code, "quota_exceeded");
    EXPECT_EQ(store->list_network_tunnel_grants(scope)->size(), 2u);

    // The account's other workspace has room for one more before the account's cap.
    auto other_spec = tunnel_spec();
    other_spec.target_id = other_target;
    ASSERT_TRUE(store->create_network_tunnel_grant(other_scope, other_spec, actor, limits));
    const auto over_account =
        store->create_network_tunnel_grant(other_scope, other_spec, actor, limits);
    ASSERT_FALSE(over_account);
    EXPECT_EQ(over_account.error().code, "quota_exceeded");

    // Revoking frees a place; no limit (0) is not refused at all.
    ASSERT_TRUE(store->revoke_network_tunnel_grant(scope, first->id, actor));
    EXPECT_TRUE(store->create_network_tunnel_grant(scope, tunnel_spec(), actor, limits));
    EXPECT_TRUE(store->create_network_tunnel_grant(scope, tunnel_spec(), actor, {}));
}

TEST_P(NetworkStore, PeersRoundTripNamedExportsAndImports) {
    auto spec = peer_spec();
    auto created = store->create_network_peer(scope, spec, actor);
    ASSERT_TRUE(created) << created.error().message;
    observe("peer", created->id);
    spec.name = "other-gateway";
    spec.remote = "second.test:7443";
    spec.exports = json::array();
    spec.imports.push_back({{"name", "writer"}, {"port", 5432}});
    auto updated = store->update_network_peer(scope, created->id, spec, other_actor);
    ASSERT_TRUE(updated) << updated.error().message;
    EXPECT_EQ(updated->spec.imports, spec.imports);
    EXPECT_TRUE(updated->spec.exports.empty());
    EXPECT_EQ(updated->spec.remote, spec.remote);
    EXPECT_EQ(updated->generation, 2);
    ASSERT_TRUE(updated->status);
    EXPECT_EQ(updated->status->message, "applied");
    EXPECT_EQ(store->get_network_peer(foreign_scope, created->id).error().code, "not_found");
    EXPECT_EQ(store->withdraw_network_peer(other_scope, created->id, actor).error().code,
              "not_found");
    EXPECT_TRUE(store->list_network_peers(other_scope)->empty());
    auto removed = store->withdraw_network_peer(scope, created->id, other_actor);
    ASSERT_TRUE(removed);
    EXPECT_EQ(removed->desired, "absent");
    EXPECT_EQ(store->withdraw_network_peer(scope, created->id, actor).error().code, "conflict");
    EXPECT_EQ(store->update_network_peer(scope, created->id, spec, actor).error().code, "conflict");
    EXPECT_EQ(store->get_network_peer(scope, created->id)->generation, 3);
    EXPECT_EQ(store->list_network_peers(scope)->size(), 1u);
    verify_audit(created->id, "peer");
}

TEST_P(NetworkStore, InvalidAndForeignWritesLeaveRowsAndAuditAlone) {
    auto spec = connection_spec();
    spec.target_id = other_target;
    EXPECT_EQ(store->create_network_connection(scope, spec, actor).error().code, "invalid_input");
    spec = connection_spec();
    spec.ports = {0};
    EXPECT_EQ(store->create_network_connection(scope, spec, actor).error().code, "invalid_input");
    spec.ports = {65536};
    EXPECT_EQ(store->create_network_connection(scope, spec, actor).error().code, "invalid_input");
    spec.ports.clear();
    spec.credential_scope = {{"access", "read"}, {"password", "not-a-scope"}};
    EXPECT_EQ(store->create_network_connection(scope, spec, actor).error().code, "invalid_input");
    spec = connection_spec();
    spec.source_kind = "service";
    spec.source_id = target;
    spec.target_kind = "app";
    spec.target_id = source;
    spec.credential_scope = {{"access", "read"}};
    EXPECT_EQ(store->create_network_connection(scope, spec, actor).error().code, "invalid_input");
    EXPECT_EQ(
        store->create_network_connection({"bad", scope.workspace_id}, connection_spec(), actor)
            .error()
            .code,
        "invalid_input");
    EXPECT_EQ(store
                  ->create_network_connection(
                      {foreign_scope.customer_id, scope.workspace_id}, connection_spec(), actor)
                  .error()
                  .code,
              "invalid_input");
    NetworkActor forged{actor.user_id, other_actor.principal, "forged"};
    EXPECT_EQ(store->create_network_connection(scope, connection_spec(), forged).error().code,
              "invalid_input");
    EXPECT_EQ(store->create_network_connection(scope, connection_spec(), {}).error().code,
              "invalid_input");
    auto egress = egress_spec();
    egress.dns_name = "example.test";
    EXPECT_EQ(store->create_network_egress_rule(scope, egress, actor).error().code,
              "invalid_input");
    egress.cidr.reset();
    egress.source_kind = "app";
    EXPECT_EQ(store->create_network_egress_rule(scope, egress, actor).error().code,
              "invalid_input");
    egress = egress_spec();
    egress.cidr = "203.0.113.1/24";
    EXPECT_EQ(store->create_network_egress_rule(scope, egress, actor).error().code,
              "invalid_input");
    auto tunnel = tunnel_spec();
    tunnel.expires_at = "2026-02-30T00:00:00Z";
    EXPECT_EQ(store->create_network_tunnel_grant(scope, tunnel, actor, {}).error().code,
              "invalid_input");
    tunnel.expires_at = "2000-01-01T00:00:00Z";
    EXPECT_EQ(store->create_network_tunnel_grant(scope, tunnel, actor, {}).error().code,
              "invalid_input");
    tunnel = tunnel_spec();
    tunnel.user_id = new_uuid_v4();
    EXPECT_EQ(store->create_network_tunnel_grant(scope, tunnel, actor, {}).error().code,
              "invalid_input");
    auto peer = peer_spec();
    peer.exports[0]["target_id"] = other_target;
    EXPECT_EQ(store->create_network_peer(scope, peer, actor).error().code, "invalid_input");
    peer = peer_spec();
    peer.imports = json::array({{{"port", 443}}});
    EXPECT_EQ(store->create_network_peer(scope, peer, actor).error().code, "invalid_input");
    EXPECT_TRUE(store->list_network_connections(scope)->empty());
    EXPECT_TRUE(store->list_network_egress_rules(scope)->empty());
    EXPECT_TRUE(store->list_network_tunnel_grants(scope)->empty());
    EXPECT_TRUE(store->list_network_peers(scope)->empty());
    EXPECT_TRUE(store->list_audit_events({})->empty());
}

TEST_P(NetworkStore, SourcesIncludeServicesAgentsAndLiveTunnels) {
    auto spec = connection_spec();
    spec.source_kind = "service";
    spec.source_id = target;
    ASSERT_TRUE(store->create_network_connection(scope, spec, actor));
    auto launch = store->create_agent_launch({.customer_id = scope.customer_id,
                                              .agent_id = "workspace-agent",
                                              .agent_name = "Agent",
                                              .mode = "service",
                                              .trigger = "manual",
                                              .scopes = {"workspace_query"},
                                              .created_by_user_id = actor.user_id,
                                              .created_by_taap_id = actor.principal,
                                              .workspace_id = scope.workspace_id});
    ASSERT_TRUE(launch) << launch.error().message;
    spec.source_kind = "agent";
    spec.source_id = launch->id;
    ASSERT_TRUE(store->create_network_connection(scope, spec, actor));
    auto grant = store->create_network_tunnel_grant(scope, tunnel_spec(), actor, {});
    ASSERT_TRUE(grant);
    spec.source_kind = "tunnel";
    spec.source_id = grant->id;
    ASSERT_TRUE(store->create_network_connection(scope, spec, actor));
    ASSERT_TRUE(store->revoke_network_tunnel_grant(scope, grant->id, actor));
    EXPECT_EQ(store->create_network_connection(scope, spec, actor).error().code, "invalid_input");
}

TEST_P(NetworkStore, DeletedWorkspaceRejectsWritesButKeepsHistoryReadable) {
    auto created = store->create_network_connection(scope, connection_spec(), actor);
    ASSERT_TRUE(created);
    ASSERT_TRUE(store->update_workspace(scope.workspace_id, {.desired = "deleted"}));
    EXPECT_EQ(store->create_network_connection(scope, connection_spec(), actor).error().code,
              "invalid_input");
    EXPECT_EQ(
        store->update_network_connection(scope, created->id, connection_spec(), actor).error().code,
        "invalid_input");
    EXPECT_EQ(store->get_network_connection(scope, created->id)->id, created->id);
    EXPECT_EQ(store->list_network_connections(scope)->size(), 1u);
}

INSTANTIATE_TEST_SUITE_P(Control, NetworkStore, testing::Bool(), [](const auto& info) {
    return info.param ? "Postgres" : "Memory";
});

class PostgresNetwork : public NetworkStore {
protected:
    bool postgres() const override {
        return true;
    }
};

TEST_F(PostgresNetwork, TunnelTrustMigrationKeepsExistingGrantsAndIsIdempotent) {
    const auto created = store->create_network_tunnel_grant(scope, tunnel_spec(), actor, {});
    ASSERT_TRUE(created);
    observe("tunnel_grant", created->id);
    pqxx::connection connection(uri);
    pqxx::work tx(connection);
    tx.exec("ALTER TABLE network_tunnel_grants DROP COLUMN client_ca_pem");
    tx.exec("ALTER TABLE network_tunnel_grant_status DROP COLUMN relay_ca_pem");
    tx.exec("DELETE FROM schema_migrations WHERE version = '0085_network_tunnel_ca'");
    const auto sql = schema_sql();
    const auto start = sql.find("-- 0085_network_tunnel_ca");
    ASSERT_NE(start, std::string::npos);
    tx.exec(sql.substr(start));
    tx.exec(sql.substr(start));
    const auto row =
        tx.exec_params("SELECT g.generation, g.client_ca_pem, s.relay_ca_pem, s.endpoint "
                       "FROM network_tunnel_grants g JOIN network_tunnel_grant_status s "
                       "ON s.grant_id=g.id WHERE g.id=$1::uuid",
                       created->id);
    ASSERT_EQ(row.size(), 1U);
    EXPECT_EQ(row[0][0].as<int>(), 1);
    EXPECT_EQ(row[0][1].as<std::string>(), "");
    EXPECT_EQ(row[0][2].as<std::string>(), "");
    EXPECT_EQ(row[0][3].as<std::string>(), "relay.test:7443");
    EXPECT_EQ(
        tx.exec(
              "SELECT count(*) FROM schema_migrations WHERE version='0085_network_tunnel_ca'")[0][0]
            .as<int>(),
        1);
    tx.commit();
}

TEST_F(PostgresNetwork, ColumnsConstraintsAndDefaultsMatchTheOperatorContract) {
    pqxx::connection connection(uri);
    pqxx::work tx(connection);
    const std::vector<std::array<std::string, 4>> expected{
        {"network_connections", "id", "uuid", "NO"},
        {"network_connections", "customer_id", "uuid", "NO"},
        {"network_connections", "workspace_id", "uuid", "NO"},
        {"network_connections", "source_kind", "text", "NO"},
        {"network_connections", "source_id", "uuid", "NO"},
        {"network_connections", "target_kind", "text", "NO"},
        {"network_connections", "target_id", "uuid", "NO"},
        {"network_connections", "ports", "_int4", "NO"},
        {"network_connections", "credential_scope", "jsonb", "YES"},
        {"network_connections", "desired", "text", "NO"},
        {"network_connections", "generation", "int8", "NO"},
        {"network_connections", "created_by", "uuid", "YES"},
        {"network_connections", "created_at", "timestamptz", "NO"},
        {"network_connections", "updated_at", "timestamptz", "NO"},
        {"network_connection_status", "connection_id", "uuid", "NO"},
        {"network_connection_status", "phase", "text", "NO"},
        {"network_connection_status", "message", "text", "NO"},
        {"network_connection_status", "credential_secret", "text", "NO"},
        {"network_connection_status", "observed_generation", "int8", "NO"},
        {"network_connection_status", "updated_at", "timestamptz", "NO"},
        {"network_egress_rules", "id", "uuid", "NO"},
        {"network_egress_rules", "customer_id", "uuid", "NO"},
        {"network_egress_rules", "workspace_id", "uuid", "NO"},
        {"network_egress_rules", "source_kind", "text", "YES"},
        {"network_egress_rules", "source_id", "uuid", "YES"},
        {"network_egress_rules", "cidr", "cidr", "YES"},
        {"network_egress_rules", "dns_name", "text", "YES"},
        {"network_egress_rules", "ports", "_int4", "NO"},
        {"network_egress_rules", "desired", "text", "NO"},
        {"network_egress_rules", "generation", "int8", "NO"},
        {"network_egress_rules", "created_by", "uuid", "YES"},
        {"network_egress_rules", "created_at", "timestamptz", "NO"},
        {"network_egress_rules", "updated_at", "timestamptz", "NO"},
        {"network_egress_rule_status", "rule_id", "uuid", "NO"},
        {"network_egress_rule_status", "phase", "text", "NO"},
        {"network_egress_rule_status", "message", "text", "NO"},
        {"network_egress_rule_status", "observed_generation", "int8", "NO"},
        {"network_egress_rule_status", "updated_at", "timestamptz", "NO"},
        {"network_tunnel_grants", "id", "uuid", "NO"},
        {"network_tunnel_grants", "customer_id", "uuid", "NO"},
        {"network_tunnel_grants", "workspace_id", "uuid", "NO"},
        {"network_tunnel_grants", "user_id", "uuid", "NO"},
        {"network_tunnel_grants", "target_kind", "text", "NO"},
        {"network_tunnel_grants", "target_id", "uuid", "NO"},
        {"network_tunnel_grants", "port", "int4", "NO"},
        {"network_tunnel_grants", "expires_at", "timestamptz", "NO"},
        {"network_tunnel_grants", "revoked_at", "timestamptz", "YES"},
        {"network_tunnel_grants", "generation", "int8", "NO"},
        {"network_tunnel_grants", "created_by", "uuid", "YES"},
        {"network_tunnel_grants", "created_at", "timestamptz", "NO"},
        {"network_tunnel_grants", "updated_at", "timestamptz", "NO"},
        {"network_tunnel_grants", "client_ca_pem", "text", "NO"},
        {"network_tunnel_grant_status", "grant_id", "uuid", "NO"},
        {"network_tunnel_grant_status", "phase", "text", "NO"},
        {"network_tunnel_grant_status", "message", "text", "NO"},
        {"network_tunnel_grant_status", "endpoint", "text", "NO"},
        {"network_tunnel_grant_status", "observed_generation", "int8", "NO"},
        {"network_tunnel_grant_status", "updated_at", "timestamptz", "NO"},
        {"network_tunnel_grant_status", "relay_ca_pem", "text", "NO"},
        {"network_peers", "id", "uuid", "NO"},
        {"network_peers", "customer_id", "uuid", "NO"},
        {"network_peers", "workspace_id", "uuid", "NO"},
        {"network_peers", "name", "text", "NO"},
        {"network_peers", "remote", "text", "NO"},
        {"network_peers", "exports", "jsonb", "NO"},
        {"network_peers", "imports", "jsonb", "NO"},
        {"network_peers", "desired", "text", "NO"},
        {"network_peers", "generation", "int8", "NO"},
        {"network_peers", "created_by", "uuid", "YES"},
        {"network_peers", "created_at", "timestamptz", "NO"},
        {"network_peers", "updated_at", "timestamptz", "NO"},
        {"network_peer_status", "peer_id", "uuid", "NO"},
        {"network_peer_status", "phase", "text", "NO"},
        {"network_peer_status", "message", "text", "NO"},
        {"network_peer_status", "observed_generation", "int8", "NO"},
        {"network_peer_status", "updated_at", "timestamptz", "NO"},
        {"network_guard", "applied_at", "timestamptz", "NO"},
    };
    const auto columns = tx.exec(
        "SELECT table_name, column_name, udt_name, is_nullable FROM information_schema.columns "
        "WHERE table_schema = 'public' AND table_name LIKE 'network_%' "
        "ORDER BY table_name, ordinal_position");
    std::multiset<std::array<std::string, 4>> actual;
    for (const auto& row : columns) {
        actual.insert({row[0].as<std::string>(),
                       row[1].as<std::string>(),
                       row[2].as<std::string>(),
                       row[3].as<std::string>()});
    }
    EXPECT_EQ(actual,
              (std::multiset<std::array<std::string, 4>>(expected.begin(), expected.end())));
    EXPECT_EQ(tx.exec("SELECT count(*) FROM schema_migrations WHERE version = '0084_network'")[0][0]
                  .as<int>(),
              1);
    EXPECT_EQ(
        tx.exec("SELECT count(*) FROM schema_migrations WHERE version = '0085_network_tunnel_ca'")
            [0][0]
                .as<int>(),
        1);
    const auto foreign_keys =
        tx.exec("SELECT count(*), count(*) FILTER (WHERE confdeltype = 'c') FROM pg_constraint "
                "WHERE contype = 'f' AND conrelid IN (SELECT oid FROM pg_class WHERE relname LIKE "
                "'network_%')");
    EXPECT_EQ(foreign_keys[0][0].as<int>(), 8);
    EXPECT_EQ(foreign_keys[0][1].as<int>(), 4);
    const auto defaults = tx.exec_params(
        "INSERT INTO network_connections (customer_id, workspace_id, source_kind, source_id, "
        "target_kind, target_id) "
        "VALUES ($1::uuid, $2::uuid, 'app', $3::uuid, 'service', $4::uuid) RETURNING "
        "ports::text, credential_scope IS NULL, desired, generation, created_by IS NULL, "
        "created_at = updated_at, id IS NOT NULL",
        scope.customer_id,
        scope.workspace_id,
        source,
        target);
    EXPECT_EQ(defaults[0][0].as<std::string>(), "{}");
    EXPECT_TRUE(defaults[0][1].as<bool>());
    EXPECT_EQ(defaults[0][2].as<std::string>(), "present");
    EXPECT_EQ(defaults[0][3].as<int>(), 1);
    EXPECT_TRUE(defaults[0][4].as<bool>());
    EXPECT_TRUE(defaults[0][5].as<bool>());
    EXPECT_TRUE(defaults[0][6].as<bool>());
    tx.commit();
    const auto bad_egress = [&](const std::string& fields, const std::string& values) {
        pqxx::work bad(connection);
        bad.exec_params("INSERT INTO network_egress_rules (customer_id, workspace_id, ports, " +
                            fields + ") VALUES ($1::uuid, $2::uuid, '{443}', " + values + ")",
                        scope.customer_id,
                        scope.workspace_id);
    };
    EXPECT_THROW(bad_egress("cidr, dns_name", "'0.0.0.0/0', 'example.test'"),
                 pqxx::check_violation);
    EXPECT_THROW(bad_egress("cidr, dns_name", "NULL, NULL"), pqxx::check_violation);
    EXPECT_THROW(bad_egress("cidr, source_kind", "'0.0.0.0/0', 'app'"), pqxx::check_violation);
    EXPECT_THROW(bad_egress("cidr, desired", "'0.0.0.0/0', 'deleted'"), pqxx::check_violation);
}

TEST_F(PostgresNetwork, RolloutSeedsOnlyExistingAppPathsOnce) {
    auto stopped = store->create_web_app({.customer_id = scope.customer_id,
                                          .workspace_id = scope.workspace_id,
                                          .name = "stopped",
                                          .mode = "image",
                                          .image = "client:v1",
                                          .port = 3000,
                                          .host = "stopped.test"});
    ASSERT_TRUE(stopped);
    auto removed = store->create_web_app({.customer_id = scope.customer_id,
                                          .workspace_id = scope.workspace_id,
                                          .name = "removed",
                                          .mode = "image",
                                          .image = "client:v1",
                                          .port = 3000,
                                          .host = "removed.test"});
    ASSERT_TRUE(removed);
    auto service = store->create_service({.customer_id = scope.customer_id,
                                          .kind = "postgres",
                                          .name = "database",
                                          .workspace_id = scope.workspace_id});
    ASSERT_TRUE(service);
    auto gone = store->create_service({.customer_id = scope.customer_id,
                                       .kind = "postgres",
                                       .name = "gone",
                                       .workspace_id = scope.workspace_id});
    ASSERT_TRUE(gone);
    ASSERT_TRUE(store->withdraw_service(gone->id));
    pqxx::connection connection(uri);
    {
        pqxx::work tx(connection);
        tx.exec_params("UPDATE apps SET desired = 'stopped' WHERE id = $1::uuid", stopped->id);
        tx.exec_params("UPDATE apps SET desired = 'deleted' WHERE id = $1::uuid", removed->id);
        tx.exec_params("UPDATE workspaces SET desired = 'stopped' WHERE id = $1::uuid",
                       scope.workspace_id);
        tx.exec("DELETE FROM schema_migrations WHERE version = '0084_network'");
        tx.exec("DROP TABLE network_guard");
        tx.exec(schema_sql());
        tx.commit();
    }
    auto paths = store->list_network_connections(scope);
    ASSERT_TRUE(paths);
    ASSERT_EQ(paths->size(), 4u);
    std::set<std::pair<std::string, std::string>> edges;
    for (const auto& row : *paths) {
        EXPECT_EQ(row.desired, "present");
        EXPECT_EQ(row.generation, 1);
        EXPECT_FALSE(row.created_by);
        EXPECT_TRUE(row.spec.ports.empty());
        EXPECT_TRUE(row.spec.credential_scope.is_null());
        edges.insert({row.spec.source_id, row.spec.target_id});
    }
    EXPECT_EQ(edges,
              (std::set<std::pair<std::string, std::string>>{{source, target},
                                                             {source, service->id},
                                                             {stopped->id, target},
                                                             {stopped->id, service->id}}));
    EXPECT_TRUE(store->list_network_connections(other_scope)->empty());
    EXPECT_TRUE(store->list_network_connections(foreign_scope)->empty());
    EXPECT_TRUE(store->list_network_egress_rules(scope)->empty());
    ASSERT_TRUE(store->withdraw_network_connection(scope, paths->front().id, actor));
    auto later = store->create_web_app({.customer_id = scope.customer_id,
                                        .workspace_id = scope.workspace_id,
                                        .name = "later",
                                        .mode = "image",
                                        .image = "client:v1",
                                        .port = 3000,
                                        .host = "later.test"});
    ASSERT_TRUE(later);
    {
        pqxx::work tx(connection);
        tx.exec(schema_sql());
        tx.exec(schema_sql());
        tx.commit();
    }
    EXPECT_EQ(store->list_network_connections(scope)->size(), 4u);
    EXPECT_EQ(store->get_network_connection(scope, paths->front().id)->desired, "absent");
    EXPECT_TRUE(store->list_network_egress_rules(scope)->empty());
}

TEST_F(PostgresNetwork, NotificationsCoverAllDesiredWritesAndExcludeStatusAndRollback) {
    pqxx::connection listener(uri);
    std::vector<std::string> received;
    listener.listen("lake_network",
                    [&](pqxx::notification note) { received.emplace_back(note.payload); });
    const auto expect_note = [&](const std::string& id) {
        const auto count = received.size();
        listener.await_notification(5, 0);
        ASSERT_EQ(received.size(), count + 1);
        EXPECT_EQ(received.back(), id);
    };
    auto c = store->create_network_connection(scope, connection_spec(), actor);
    ASSERT_TRUE(c);
    expect_note(c->id);
    ASSERT_TRUE(store->update_network_connection(scope, c->id, connection_spec(), actor));
    expect_note(c->id);
    ASSERT_TRUE(store->withdraw_network_connection(scope, c->id, actor));
    expect_note(c->id);
    auto e = store->create_network_egress_rule(scope, egress_spec(), actor);
    ASSERT_TRUE(e);
    expect_note(e->id);
    ASSERT_TRUE(store->update_network_egress_rule(scope, e->id, egress_spec(), actor));
    expect_note(e->id);
    ASSERT_TRUE(store->withdraw_network_egress_rule(scope, e->id, actor));
    expect_note(e->id);
    auto t = store->create_network_tunnel_grant(scope, tunnel_spec(), actor, {});
    ASSERT_TRUE(t);
    expect_note(t->id);
    ASSERT_TRUE(store->update_network_tunnel_grant(scope, t->id, tunnel_spec(), actor));
    expect_note(t->id);
    ASSERT_TRUE(store->revoke_network_tunnel_grant(scope, t->id, actor));
    expect_note(t->id);
    auto p = store->create_network_peer(scope, peer_spec(), actor);
    ASSERT_TRUE(p);
    expect_note(p->id);
    ASSERT_TRUE(store->update_network_peer(scope, p->id, peer_spec(), actor));
    expect_note(p->id);
    ASSERT_TRUE(store->withdraw_network_peer(scope, p->id, actor));
    expect_note(p->id);
    observe("connection", c->id);
    observe("egress_rule", e->id);
    observe("tunnel_grant", t->id);
    observe("peer", p->id);
    pqxx::connection connection(uri);
    {
        pqxx::work tx(connection);
        tx.exec_params(
            "UPDATE network_connections SET generation = generation + 1 WHERE id = $1::uuid",
            c->id);
        // Intentionally abort: this notification must never be delivered.
    }
    listener.await_notification(0, 100000);
    EXPECT_EQ(received.size(), 12u);
    EXPECT_EQ(store->get_network_connection(scope, c->id)->generation, 3);
}

TEST_F(PostgresNetwork, AuditFailureRollsBackTheDesiredMutationAndNotification) {
    auto existing = store->create_network_connection(scope, connection_spec(), actor);
    ASSERT_TRUE(existing);
    pqxx::connection connection(uri);
    {
        pqxx::work tx(connection);
        tx.exec("CREATE FUNCTION refuse_network_audit() RETURNS trigger LANGUAGE plpgsql AS $$ "
                "BEGIN RAISE EXCEPTION 'network audit refused'; END $$");
        tx.exec("CREATE TRIGGER refuse_network_audit BEFORE INSERT ON audit_events "
                "FOR EACH ROW EXECUTE FUNCTION refuse_network_audit()");
        tx.commit();
    }
    pqxx::connection listener(uri);
    int notifications = 0;
    listener.listen("lake_network", [&](pqxx::notification) { ++notifications; });
    auto result = store->update_network_connection(scope,
                                                   existing->id,
                                                   {.source_kind = "app",
                                                    .source_id = source,
                                                    .target_kind = "service",
                                                    .target_id = target,
                                                    .ports = {443}},
                                                   other_actor);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, "store_error");
    auto current = store->get_network_connection(scope, existing->id);
    ASSERT_TRUE(current);
    EXPECT_EQ(current->generation, 1);
    EXPECT_TRUE(current->spec.ports.empty());
    EXPECT_EQ(store->list_audit_events({})->size(), 1u);
    listener.await_notification(0, 100000);
    EXPECT_EQ(notifications, 0);
    EXPECT_FALSE(store->create_network_peer(scope, peer_spec(), actor));
    EXPECT_TRUE(store->list_network_peers(scope)->empty());
}

TEST_F(PostgresNetwork, ExpiredGrantCannotBeRenewedOrUsedAsAConnectionSource) {
    auto grant = store->create_network_tunnel_grant(scope, tunnel_spec(), actor, {});
    ASSERT_TRUE(grant);
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec_params("UPDATE network_tunnel_grants SET expires_at = now() - interval '1 second' "
                       "WHERE id = $1::uuid",
                       grant->id);
        tx.commit();
    }
    EXPECT_EQ(
        store->update_network_tunnel_grant(scope, grant->id, tunnel_spec(), actor).error().code,
        "conflict");
    EXPECT_EQ(store->revoke_network_tunnel_grant(scope, grant->id, actor).error().code, "conflict");
    auto spec = connection_spec();
    spec.source_kind = "tunnel";
    spec.source_id = grant->id;
    EXPECT_EQ(store->create_network_connection(scope, spec, actor).error().code, "invalid_input");
    EXPECT_FALSE(store->get_network_tunnel_grant(scope, grant->id)->revoked_at);
    EXPECT_EQ(store->list_audit_events({})->size(), 1u);
}

TEST_F(PostgresNetwork, ConcurrentWritersKeepGenerationsAndTheAuditChain) {
    auto created = store->create_network_connection(scope, connection_spec(), actor);
    ASSERT_TRUE(created);
    const auto change = [&] {
        auto writer = make_postgres_store(uri, test_kek);
        return writer->update_network_connection(
            scope, created->id, connection_spec(), other_actor);
    };
    auto a = std::async(std::launch::async, change);
    auto b = std::async(std::launch::async, change);
    auto first = a.get();
    auto second = b.get();
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ((std::set<std::int64_t>{first->generation, second->generation}),
              (std::set<std::int64_t>{2, 3}));
    EXPECT_EQ(store->get_network_connection(scope, created->id)->generation, 3);
    auto events = store->list_audit_events({});
    ASSERT_TRUE(events);
    ASSERT_EQ(events->size(), 3u);
    EXPECT_TRUE(lake::audit::verify(*store, *events));
    auto reopened = make_postgres_store(uri, test_kek);
    EXPECT_EQ(reopened->get_network_connection(scope, created->id)->generation, 3);
}

} // namespace
