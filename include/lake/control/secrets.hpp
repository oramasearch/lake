#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lake::control {

struct SecretMount {
    std::string id;
    std::string customer_id;
    std::string organization_id;
    std::string name;
    std::string description;
    std::string created_at;
    std::string updated_at;
};

struct NewSecretMount {
    std::string customer_id;
    std::string organization_id;
    std::string name;
    std::string description;
    std::string acting_principal;
    std::string request_id;
};

struct SecretVersion {
    std::string mount_id;
    std::string name;
    std::uint64_t version = 0;
    nlohmann::json value = nlohmann::json::object();
    nlohmann::json custom_metadata = nlohmann::json::object();
    std::string created_at;
    std::string deleted_at;
    std::string destroyed_at;
};

struct SecretVersionMetadata {
    std::uint64_t version = 0;
    std::string created_at;
    std::string deleted_at;
    std::string destroyed_at;
};

struct SecretMetadata {
    std::string mount_id;
    std::string name;
    std::uint64_t current_version = 0;
    nlohmann::json custom_metadata = nlohmann::json::object();
    std::vector<SecretVersionMetadata> versions;
    std::string created_at;
    std::string updated_at;
};

struct PutSecret {
    std::string mount_id;
    std::string name;
    nlohmann::json value = nlohmann::json::object();
    nlohmann::json custom_metadata = nlohmann::json::object();
    std::uint64_t check_and_set = 0;
};

enum class SecretVersionAction {
    Delete,
    Undelete,
    Destroy,
};

struct EncryptionKey {
    std::string id;
    std::string mount_id;
    std::string name;
    std::string derived_from;
    std::uint64_t current_version = 0;
    std::string created_at;
    std::string updated_at;
};

struct NewEncryptionKey {
    std::string mount_id;
    std::string name;
};

struct TransitValue {
    std::string value;
    std::uint64_t key_version = 0;
    std::string algorithm;
};

struct TransitVerification {
    bool valid = false;
    std::uint64_t key_version = 0;
    std::string algorithm;
};

struct DerivedEncryptionKey {
    EncryptionKey key;
    std::uint64_t parent_version = 0;
};

struct CustomerKekRotation {
    std::string customer_id;
    std::uint64_t version = 0;
    std::uint64_t data_keys_rewrapped = 0;
};

nlohmann::json to_json(const SecretMount& value);
nlohmann::json to_json(const SecretVersion& value);
nlohmann::json to_json(const SecretMetadata& value);
nlohmann::json to_json(const EncryptionKey& value);
nlohmann::json to_json(const TransitValue& value);
nlohmann::json to_json(const TransitVerification& value);
nlohmann::json to_json(const DerivedEncryptionKey& value);
nlohmann::json to_json(const CustomerKekRotation& value);

} // namespace lake::control
