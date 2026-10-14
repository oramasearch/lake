#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lake::control {

// Every operation is scoped to both the customer and workspace. Authorization belongs to the
// caller; the store enforces ownership and records every successful mutation in central audit.
struct NetworkScope {
    std::string customer_id;
    std::string workspace_id;
};

struct NetworkActor {
    // Empty only for a system/operator write. A person must name their persisted user and TAAP id.
    std::string user_id;
    std::string principal;
    std::string request_id;
};

struct NetworkStatus {
    std::string phase;
    std::string message;
    std::int64_t observed_generation = 0;
    std::string updated_at;
    std::string credential_secret; // connection status only
    std::string endpoint;          // tunnel grant status only
};

struct NetworkMetadata {
    std::string id;
    std::string customer_id;
    std::string workspace_id;
    std::int64_t generation = 1;
    std::optional<std::string> created_by;
    std::string created_at;
    std::string updated_at;
    // No status row yet is distinct from an operator-written empty phase.
    std::optional<NetworkStatus> status;
};

struct NetworkConnectionSpec {
    std::string source_kind;
    std::string source_id;
    std::string target_kind;
    std::string target_id;
    std::vector<int> ports;
    nlohmann::json credential_scope = nullptr;
};

struct NetworkConnection : NetworkMetadata {
    NetworkConnectionSpec spec;
    std::string desired = "present";
};

struct NetworkEgressRuleSpec {
    std::optional<std::string> source_kind;
    std::optional<std::string> source_id;
    std::optional<std::string> cidr;
    std::optional<std::string> dns_name;
    std::vector<int> ports;
};

struct NetworkEgressRule : NetworkMetadata {
    NetworkEgressRuleSpec spec;
    std::string desired = "present";
};

struct NetworkTunnelGrantSpec {
    std::string user_id;
    std::string target_kind;
    std::string target_id;
    int port = 0;
    std::string expires_at;
};

struct NetworkTunnelGrant : NetworkMetadata {
    NetworkTunnelGrantSpec spec;
    std::optional<std::string> revoked_at;
};

struct NetworkPeerSpec {
    std::string name;
    std::string remote;
    nlohmann::json exports = nlohmann::json::array();
    nlohmann::json imports = nlohmann::json::array();
};

struct NetworkPeer : NetworkMetadata {
    NetworkPeerSpec spec;
    std::string desired = "present";
};

} // namespace lake::control
