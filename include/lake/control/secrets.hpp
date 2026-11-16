// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lake::control {

// A secret mount (docs/api/secrets.md): a namespace of versioned secrets and named encryption keys,
// in one workspace of the customer's space (lake#328). Its owner is the customer itself (an
// account, `organization_id` empty) or an organization inside the customer's namespace. The mount
// itself, its secrets, their versions, its named keys and every data key live in that workspace's
// Zetta (control/secret_records.hpp); the control store keeps only the content-free route below.
struct SecretMount {
    std::string id;
    std::string customer_id;
    std::string organization_id; // empty: the customer (account) owns the mount
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
    std::string organization_id; // empty (NULL): the customer owns the mount (migration 0071)
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

nlohmann::json to_json(const SecretMount& value);
nlohmann::json to_json(const SecretMountRoute& value);
// The external id of the policy environment a mount hangs under, below its owner's object:
// `secrets/<customer>/<organization>` for an organization's mount (unchanged since 0009) and
// `secrets/<customer>` for an account's.
std::string secret_mount_environment(const SecretMountRoute& route);
nlohmann::json to_json(const SecretVersion& value);
nlohmann::json to_json(const SecretMetadata& value);
nlohmann::json to_json(const DynamicSecretLease& value);
nlohmann::json to_json(const EncryptionKey& value);
nlohmann::json to_json(const TransitValue& value);
nlohmann::json to_json(const TransitVerification& value);
nlohmann::json to_json(const DerivedEncryptionKey& value);
nlohmann::json to_json(const CustomerKekRotation& value);

} // namespace lake::control
