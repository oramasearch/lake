#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lake::control {

// A secret mount (docs/api/secrets.md): one organization's namespace of versioned secrets and named
// encryption keys, in one workspace of the customer's space (lake#328). The mount itself, its
// secrets, their versions, its named keys and every data key live in that workspace's Zetta
// (control/secret_records.hpp); the control store keeps only the content-free route below.
struct SecretMount {
    std::string id;
    std::string customer_id;
    std::string organization_id;
    std::string workspace_id;
    std::string name;
    std::string description;
    std::string created_at;
    std::string updated_at;
    std::int64_t at_us = 0;
};

// Where a mount lives: multitenancy and dispatch only (secret_mount_routes, migration 0057). It
// names no mount name, secret name, value or key; the workspace never changes.
struct SecretMountRoute {
    std::string mount_id;
    std::string customer_id;
    std::string organization_id;
    std::string workspace_id;
    std::string created_at;
};

// A data key wrapped by the customer KEK version `kek_version` (keystore, lake#324). The wrap's
// associated data is "lake data key/<customer>/<owner>/<key version>", unchanged since 0009, so a
// copied ciphertext opens where it is copied to.
struct WrappedDataKey {
    std::uint64_t kek_version = 0;
    std::string wrapped_key;
};

// The customer KEK versions a data key may be wrapped by: the current one and, while a rotation
// is rewrapping the workspaces' data keys, the one it replaced (0: none).
struct CustomerKekState {
    std::uint64_t version = 0;
    std::uint64_t previous_version = 0;
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

// A generated credential's lease (docs/api/secrets.md#dynamic-leases, lake#362). The lease and its
// sealed value live in the mount's workspace Zetta (control/secret_records.hpp); `expired_at` is
// set, and the value erased, when the expiry loop settles a lease whose lifetime ran out.
struct DynamicSecretLease {
    std::string id;
    std::string mount_id;
    std::string workspace_id;
    std::string role;
    std::string expires_at;
    std::string revoked_at;
    std::string expired_at;
    int renewals = 0;
    std::string created_at;
};

// Where a lease lives, and when the expiry loop must next look at it (secret_lease_routes,
// migration 0058, lake#362): multitenancy and dispatch only. It names no role, value or
// ciphertext; the workspace is the mount's and never changes. `due_us` is a hint, never later
// than the lease's expiry in its workspace record (which is the truth); 0 once the lease is
// revoked or expired there, when nothing is left to do.
struct SecretLeaseRoute {
    std::string lease_id;
    std::string mount_id;
    std::string customer_id;
    std::string workspace_id;
    std::int64_t due_us = 0;
    std::int64_t created_us = 0;
};

struct IssuedDynamicSecret {
    DynamicSecretLease lease;
    nlohmann::json value;
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
    // The version this rotation replaced; kept until every workspace data key is rewrapped.
    std::uint64_t previous_version = 0;
};

// Rows of the retired secrets tables (migration 0009), read only by the operator's one-shot copy
// (copy_workspace_records). `workspace_id` is the owner the copy writes to: the mount's route,
// empty while the mount has none (`needs_selection`) or its workspace was deleted
// (`workspace_deleted`).
struct LegacySecretOwner {
    std::string customer_id;
    std::string mount_id;
    std::string workspace_id;
    std::string outcome;
};

struct LegacySecretMount {
    SecretMount mount;
    LegacySecretOwner owner;
};

struct LegacySecretEntry {
    std::string id;
    std::string name;
    std::uint64_t current_version = 0;
    nlohmann::json custom_metadata = nlohmann::json::object();
    std::int64_t at_us = 0;
    std::string updated_at;
    LegacySecretOwner owner;
};

struct LegacySecretVersion {
    std::string id; // md5(entry id, version) as a UUID: the table has no id
    std::string name;
    std::uint64_t version = 0;
    std::string ciphertext; // empty once destroyed
    std::int64_t at_us = 0;
    std::string deleted_at;
    std::string destroyed_at;
    LegacySecretOwner owner;
};

struct LegacyEncryptionKey {
    EncryptionKey key;
    std::int64_t at_us = 0;
    LegacySecretOwner owner;
};

struct LegacySecretDataKey {
    std::string id; // md5(owner kind, owner id, version) as a UUID
    std::string owner_kind;
    std::string owner_id;
    std::uint64_t key_version = 0;
    WrappedDataKey wrapped; // kek_version: the customer's current KEK, which wraps every row
    std::int64_t at_us = 0;
    LegacySecretOwner owner;
};

// A row of the retired secrets_dynamic_leases table, read only by the one-shot copy. It follows
// its mount: `owner` is the mount's route, as for the other secrets rows.
struct LegacyDynamicSecretLease {
    std::string id;
    std::string role;
    std::string encrypted_value; // sealed under the mount data key, "lake dynamic secret/<m>/<id>"
    std::int64_t at_us = 0;
    std::int64_t expires_us = 0;
    std::string revoked_at;
    std::int64_t revoked_us = 0;
    int renewals = 0;
    LegacySecretOwner owner;
};

nlohmann::json to_json(const SecretMount& value);
nlohmann::json to_json(const SecretMountRoute& value);
nlohmann::json to_json(const SecretVersion& value);
nlohmann::json to_json(const SecretMetadata& value);
nlohmann::json to_json(const DynamicSecretLease& value);
nlohmann::json to_json(const EncryptionKey& value);
nlohmann::json to_json(const TransitValue& value);
nlohmann::json to_json(const TransitVerification& value);
nlohmann::json to_json(const DerivedEncryptionKey& value);
nlohmann::json to_json(const CustomerKekRotation& value);

} // namespace lake::control
