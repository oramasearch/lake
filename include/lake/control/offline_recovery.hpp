#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lake::control {

struct OfflineGrant {
    std::string id;
    std::string user_id;
    std::string principal_taap_id;
    std::string device_id;
    std::vector<std::string> scopes;
    std::string issued_at;
    std::string expires_at;
    std::string revoked_at;
    nlohmann::json credential;
};

struct NewOfflineGrant {
    std::string id;
    std::string user_id;
    std::string principal_taap_id;
    std::string device_id;
    std::vector<std::string> scopes;
    int duration_seconds = 0;
    nlohmann::json credential;
};

struct RecoveryAttempt {
    std::string id;
    std::string user_id;
    std::string organization_id;
    std::string method;
    std::string status;
    std::string ready_at;
    std::string expires_at;
    std::string completed_at;
    int approval_count = 0;
};

struct RecoveryDelivery {
    std::string id;
    std::string recovery_id;
    std::string email;
    std::string token;
    std::string kind;
    std::string created_at;
    std::string delivered_at;
};

struct RecoveryNotification {
    std::string id;
    std::string recovery_id;
    std::string user_id;
    std::string device_id;
    std::string kind;
    std::string created_at;
};

struct RecoveryContact {
    std::string repository_id;
    std::string contact_taap_id;
    std::string wrapped_key;
    std::string created_at;
    std::string updated_at;
};

nlohmann::json to_json(const OfflineGrant& value);
nlohmann::json to_json(const RecoveryAttempt& value);
nlohmann::json to_json(const RecoveryDelivery& value);
nlohmann::json to_json(const RecoveryNotification& value);
nlohmann::json to_json(const RecoveryContact& value, bool include_wrapped_key = false);

} // namespace lake::control
