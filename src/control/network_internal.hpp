// SPDX-License-Identifier: MIT

#pragma once

#include "lake/control/store.hpp"
#include <functional>

namespace lake::control::network_detail {

struct Table {
    const char* name;
    const char* status;
    const char* status_key;
    const char* object_kind;
    std::vector<std::string> fields;
    bool tunnel;
};

template <class Row> const Table& table();
template <class Row> Row decode(const nlohmann::json& value);

template <> const Table& table<NetworkConnection>();
template <> NetworkConnection decode<NetworkConnection>(const nlohmann::json&);
nlohmann::json encode(const NetworkConnectionSpec&);
template <> const Table& table<NetworkEgressRule>();
template <> NetworkEgressRule decode<NetworkEgressRule>(const nlohmann::json&);
nlohmann::json encode(const NetworkEgressRuleSpec&);
template <> const Table& table<NetworkTunnelGrant>();
template <> NetworkTunnelGrant decode<NetworkTunnelGrant>(const nlohmann::json&);
nlohmann::json encode(const NetworkTunnelGrantSpec&);
template <> const Table& table<NetworkPeer>();
template <> NetworkPeer decode<NetworkPeer>(const nlohmann::json&);
nlohmann::json encode(const NetworkPeerSpec&);

bool uuid(std::string_view value);
Result<void> validate_scope(const NetworkScope&);
Result<double> instant(const std::string& value);
Result<void> validate(const Table&,
                      const nlohmann::json&,
                      const std::function<Result<void>(const std::string&, const std::string&)>&);
Result<audit::Identity> prepare_audit(Store&, const NetworkActor&, const audit::IdentityOptions&);
Result<audit::Event> sign_change(Store&,
                                 const audit::Identity&,
                                 const NetworkActor&,
                                 const NetworkScope&,
                                 const Table&,
                                 const std::string& id,
                                 const std::string& operation,
                                 std::int64_t generation,
                                 const std::string& previous,
                                 const audit::IdentityOptions&);
nlohmann::json metadata_json(const NetworkMetadata&);
// lake#983: the quota_exceeded refusal for a new tunnel grant when the workspace already holds
// `in_workspace` active grants or its account `in_account`; nothing when the grant fits.
std::optional<StoreError> tunnel_grant_limit_refusal(const NetworkTunnelGrantLimits&,
                                                     std::size_t in_workspace,
                                                     std::size_t in_account);

} // namespace lake::control::network_detail
