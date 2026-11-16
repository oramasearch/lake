// SPDX-License-Identifier: MIT

#include "lake/control/memory_store.hpp"
#include "network_internal.hpp"

#include <algorithm>
#include <type_traits>

namespace lake::control {
namespace {
template <class Row> bool in_scope(const Row& row, const NetworkScope& scope) {
    return row.customer_id == scope.customer_id && row.workspace_id == scope.workspace_id;
}

template <class Row> bool ended(const Row& row) {
    if constexpr (std::is_same_v<Row, NetworkTunnelGrant>) {
        const auto expires = network_detail::instant(row.spec.expires_at);
        const double now =
            std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        return row.revoked_at.has_value() || !expires || *expires <= now;
    } else {
        return row.desired != "present";
    }
}
} // namespace

Result<void> InMemoryStore::check_network_scope_locked(const NetworkScope& scope) const {
    const auto workspace = std::ranges::find(workspaces_, scope.workspace_id, &Workspace::id);
    if (workspace == workspaces_.end() || workspace->customer_id != scope.customer_id ||
        workspace->desired == "deleted") {
        return std::unexpected(StoreError{"invalid_input", "no live workspace in this customer"});
    }
    return {};
}

Result<void> InMemoryStore::check_network_target_locked(const NetworkScope& scope,
                                                        const std::string& kind,
                                                        const std::string& id) const {
    bool found = false;
    if (kind == "app") {
        found = std::ranges::any_of(web_apps_, [&](const auto& row) {
            return row.id == id && in_scope(row, scope) && row.desired != "deleted";
        });
    } else if (kind == "service") {
        found = std::ranges::any_of(services_, [&](const auto& row) {
            return row.id == id && in_scope(row, scope) && row.desired == "present";
        });
    } else if (kind == "agent") {
        found = std::ranges::any_of(agent_launches_, [&](const auto& row) {
            return row.id == id && in_scope(row, scope) && row.desired == "present";
        });
    } else if (kind == "tunnel") {
        found = std::ranges::any_of(network_tunnel_grants_, [&](const auto& row) {
            return row.id == id && in_scope(row, scope) && !ended(row);
        });
    }
    if (!found) {
        return std::unexpected(
            StoreError{"invalid_input", "network workload is not live in this workspace"});
    }
    return {};
}

template <class Row>
Result<Row> InMemoryStore::change_network(const NetworkScope& scope,
                                          const std::string& id,
                                          const nlohmann::json& spec,
                                          const NetworkActor& actor,
                                          const std::string& operation,
                                          std::vector<Row>& rows,
                                          const NetworkTunnelGrantLimits* grant_limits) {
    if (auto checked = network_detail::validate_scope(scope); !checked) {
        return std::unexpected(checked.error());
    }
    const bool create = operation == "create";
    if (!create && !network_detail::uuid(id)) {
        return std::unexpected(StoreError{"invalid_input", "network id must be a UUID"});
    }
    auto signer = network_detail::prepare_audit(*this, actor, actor.identity_options);
    if (!signer) {
        return std::unexpected(signer.error());
    }
    std::lock_guard lock(mutex_);
    if (auto checked = check_network_scope_locked(scope); !checked) {
        return std::unexpected(checked.error());
    }
    auto existing = std::ranges::find(rows, id, &Row::id);
    Row result;
    if (!create) {
        if (existing == rows.end() || !in_scope(*existing, scope)) {
            return std::unexpected(StoreError{"not_found", "no network record in this workspace"});
        }
        if (ended(*existing)) {
            return std::unexpected(StoreError{"conflict", "the network record has ended"});
        }
        result = *existing;
        ++result.generation;
    } else {
        result.id = new_uuid_v4();
        result.customer_id = scope.customer_id;
        result.workspace_id = scope.workspace_id;
        result.created_at = now_iso8601();
        if (!actor.user_id.empty()) {
            result.created_by = actor.user_id;
        }
    }
    result.updated_at = now_iso8601();
    if (create || operation == "update") {
        auto checked =
            network_detail::validate(network_detail::table<Row>(),
                                     spec,
                                     [&](const std::string& kind, const std::string& target) {
                                         return check_network_target_locked(scope, kind, target);
                                     });
        if (!checked) {
            return std::unexpected(checked.error());
        }
        if constexpr (std::is_same_v<Row, NetworkTunnelGrant>) {
            if (std::ranges::none_of(users_, [&](const User& user) {
                    return user.id == spec.at("user_id").get<std::string>();
                })) {
                return std::unexpected(StoreError{"invalid_input", "no tunnel user"});
            }
            // lake#983: counted under the store's lock, as PostgreSQL counts under the account's
            // row lock.
            if (create && grant_limits != nullptr) {
                std::size_t in_workspace = 0;
                std::size_t in_account = 0;
                for (const NetworkTunnelGrant& grant : rows) {
                    if (grant.customer_id != scope.customer_id ||
                        !network_tunnel_grant_active(grant)) {
                        continue;
                    }
                    ++in_account;
                    if (grant.workspace_id == scope.workspace_id) {
                        ++in_workspace;
                    }
                }
                if (auto refused = network_detail::tunnel_grant_limit_refusal(
                        *grant_limits, in_workspace, in_account)) {
                    return std::unexpected(*refused);
                }
            }
        }
        auto value = network_detail::metadata_json(result);
        value.update(spec);
        value["desired"] = "present";
        value["revoked_at"] = nullptr;
        const auto status = result.status;
        result = network_detail::decode<Row>(value);
        result.status = status;
    } else if constexpr (std::is_same_v<Row, NetworkTunnelGrant>) {
        result.revoked_at = result.updated_at;
    } else {
        result.desired = "absent";
    }
    auto event =
        network_detail::sign_change(*this,
                                    *signer,
                                    actor,
                                    scope,
                                    network_detail::table<Row>(),
                                    result.id,
                                    operation,
                                    result.generation,
                                    audit_events_.empty() ? "" : audit_events_.back().digest,
                                    actor.identity_options);
    if (!event) {
        return std::unexpected(event.error());
    }
    // Both changes happen under the same lock after validation and signing succeed.
    if (create) {
        rows.push_back(result);
    } else {
        *existing = result;
    }
    audit_events_.push_back(*event);
    return result;
}

template <class Row>
Result<Row> InMemoryStore::get_network(const NetworkScope& scope,
                                       const std::string& id,
                                       const std::vector<Row>& rows) {
    if (auto checked = network_detail::validate_scope(scope); !checked) {
        return std::unexpected(checked.error());
    }
    if (!network_detail::uuid(id)) {
        return std::unexpected(StoreError{"invalid_input", "network id must be a UUID"});
    }
    std::lock_guard lock(mutex_);
    const auto row = std::ranges::find(rows, id, &Row::id);
    if (row == rows.end() || !in_scope(*row, scope)) {
        return std::unexpected(StoreError{"not_found", "no network record in this workspace"});
    }
    return *row;
}

template <class Row>
Result<std::vector<Row>> InMemoryStore::list_network(const NetworkScope& scope,
                                                     const std::vector<Row>& rows) {
    if (auto checked = network_detail::validate_scope(scope); !checked) {
        return std::unexpected(checked.error());
    }
    std::lock_guard lock(mutex_);
    std::vector<Row> result;
    for (const auto& row : rows) {
        if (in_scope(row, scope)) {
            result.push_back(row);
        }
    }
    std::ranges::sort(result, [](const auto& a, const auto& b) {
        return std::tie(a.created_at, a.id) < std::tie(b.created_at, b.id);
    });
    return result;
}

template <class Row>
Result<void> InMemoryStore::set_network_status(const std::string& id,
                                               const NetworkStatus& status,
                                               std::vector<Row>& rows) {
    std::lock_guard lock(mutex_);
    auto row = std::ranges::find(rows, id, &Row::id);
    if (row == rows.end()) {
        return std::unexpected(StoreError{"not_found", "no network record"});
    }
    row->status = status;
    row->status->updated_at = now_iso8601();
    return {};
}

Result<NetworkConnection> InMemoryStore::create_network_connection(
    const NetworkScope& scope, const NetworkConnectionSpec& spec, const NetworkActor& actor) {
    return change_network(
        scope, "", network_detail::encode(spec), actor, "create", network_connections_);
}

Result<NetworkConnection>
InMemoryStore::update_network_connection(const NetworkScope& scope,
                                         const std::string& id,
                                         const NetworkConnectionSpec& spec,
                                         const NetworkActor& actor) {
    return change_network(
        scope, id, network_detail::encode(spec), actor, "update", network_connections_);
}

Result<NetworkConnection> InMemoryStore::withdraw_network_connection(const NetworkScope& scope,
                                                                     const std::string& id,
                                                                     const NetworkActor& actor) {
    return change_network(scope, id, nullptr, actor, "withdraw", network_connections_);
}

Result<NetworkConnection> InMemoryStore::get_network_connection(const NetworkScope& scope,
                                                                const std::string& id) {
    return get_network(scope, id, network_connections_);
}

Result<std::vector<NetworkConnection>>
InMemoryStore::list_network_connections(const NetworkScope& scope) {
    return list_network(scope, network_connections_);
}

Result<void> InMemoryStore::set_network_connection_status(const std::string& id,
                                                          const NetworkStatus& status) {
    return set_network_status(id, status, network_connections_);
}

Result<NetworkEgressRule> InMemoryStore::create_network_egress_rule(
    const NetworkScope& scope, const NetworkEgressRuleSpec& spec, const NetworkActor& actor) {
    return change_network(
        scope, "", network_detail::encode(spec), actor, "create", network_egress_rules_);
}

Result<NetworkEgressRule>
InMemoryStore::update_network_egress_rule(const NetworkScope& scope,
                                          const std::string& id,
                                          const NetworkEgressRuleSpec& spec,
                                          const NetworkActor& actor) {
    return change_network(
        scope, id, network_detail::encode(spec), actor, "update", network_egress_rules_);
}

Result<NetworkEgressRule> InMemoryStore::withdraw_network_egress_rule(const NetworkScope& scope,
                                                                      const std::string& id,
                                                                      const NetworkActor& actor) {
    return change_network(scope, id, nullptr, actor, "withdraw", network_egress_rules_);
}

Result<NetworkEgressRule> InMemoryStore::get_network_egress_rule(const NetworkScope& scope,
                                                                 const std::string& id) {
    return get_network(scope, id, network_egress_rules_);
}

Result<std::vector<NetworkEgressRule>>
InMemoryStore::list_network_egress_rules(const NetworkScope& scope) {
    return list_network(scope, network_egress_rules_);
}

Result<void> InMemoryStore::set_network_egress_rule_status(const std::string& id,
                                                           const NetworkStatus& status) {
    return set_network_status(id, status, network_egress_rules_);
}

Result<NetworkTunnelGrant>
InMemoryStore::create_network_tunnel_grant(const NetworkScope& scope,
                                           const NetworkTunnelGrantSpec& spec,
                                           const NetworkActor& actor,
                                           const NetworkTunnelGrantLimits& limits) {
    return change_network(
        scope, "", network_detail::encode(spec), actor, "create", network_tunnel_grants_, &limits);
}

Result<NetworkTunnelGrant>
InMemoryStore::update_network_tunnel_grant(const NetworkScope& scope,
                                           const std::string& id,
                                           const NetworkTunnelGrantSpec& spec,
                                           const NetworkActor& actor) {
    return change_network(
        scope, id, network_detail::encode(spec), actor, "update", network_tunnel_grants_);
}

Result<NetworkTunnelGrant> InMemoryStore::revoke_network_tunnel_grant(const NetworkScope& scope,
                                                                      const std::string& id,
                                                                      const NetworkActor& actor) {
    return change_network(scope, id, nullptr, actor, "revoke", network_tunnel_grants_);
}

Result<NetworkTunnelGrant> InMemoryStore::get_network_tunnel_grant(const NetworkScope& scope,
                                                                   const std::string& id) {
    return get_network(scope, id, network_tunnel_grants_);
}

Result<std::vector<NetworkTunnelGrant>>
InMemoryStore::list_network_tunnel_grants(const NetworkScope& scope) {
    return list_network(scope, network_tunnel_grants_);
}

Result<void> InMemoryStore::set_network_tunnel_grant_status(const std::string& id,
                                                            const NetworkStatus& status) {
    return set_network_status(id, status, network_tunnel_grants_);
}

Result<NetworkPeer> InMemoryStore::create_network_peer(const NetworkScope& scope,
                                                       const NetworkPeerSpec& spec,
                                                       const NetworkActor& actor) {
    return change_network(scope, "", network_detail::encode(spec), actor, "create", network_peers_);
}

Result<NetworkPeer> InMemoryStore::update_network_peer(const NetworkScope& scope,
                                                       const std::string& id,
                                                       const NetworkPeerSpec& spec,
                                                       const NetworkActor& actor) {
    return change_network(scope, id, network_detail::encode(spec), actor, "update", network_peers_);
}

Result<NetworkPeer> InMemoryStore::withdraw_network_peer(const NetworkScope& scope,
                                                         const std::string& id,
                                                         const NetworkActor& actor) {
    return change_network(scope, id, nullptr, actor, "withdraw", network_peers_);
}

Result<NetworkPeer> InMemoryStore::get_network_peer(const NetworkScope& scope,
                                                    const std::string& id) {
    return get_network(scope, id, network_peers_);
}

Result<std::vector<NetworkPeer>> InMemoryStore::list_network_peers(const NetworkScope& scope) {
    return list_network(scope, network_peers_);
}

Result<void> InMemoryStore::set_network_peer_status(const std::string& id,
                                                    const NetworkStatus& status) {
    return set_network_status(id, status, network_peers_);
}
} // namespace lake::control
