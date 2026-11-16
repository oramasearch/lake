// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "lake/control/secrets.hpp"
#include "lake/control/workspace_records.hpp"

namespace lake::control {

// Secrets in the workspace Zetta of their mount (docs/api/secrets.md, lake#328-#332). Each retired
// table is one collection of that workspace's record store, sealed like every lake record
// (docs/architecture/workspace-records.md), and every body that held ciphertext in PostgreSQL
// holds the same ciphertext here: the envelope is unchanged. LAKE_INSTANCE_KEK wraps the customer
// KEK (keystore), the customer KEK wraps each data key (`secrets_data_keys`), and a mount's data
// key seals its secret versions while a named key's versions are themselves data keys.
//
// Names are never record keys: a key that a name selects is the blind index of the name in its
// collection (WorkspaceRecords::index_of), so the workspace holds no secret, mount or key name in
// the clear.

// `secrets_mounts`: one record per mount, keyed by the mount id, listed by its space. A mount's
// name is claimed in the same collection by a record keyed by the blind index of its owner and
// name, written `if_absent`, which keeps names unique within the workspace and owner.
struct SecretMountCodec {
    static constexpr RecordCollection collection{"secrets_mounts", 1};
    static std::string lookup(const std::string& customer_id);
    static nlohmann::json encode(const SecretMount& mount);
    static std::optional<SecretMount> decode(const StoredRecord& record);
};
// The owner a name claim is scoped to: `organization:<id>` for an organization's mount (the
// value claims have always used) and `customer:<id>` for an account's. A UUID never contains a
// colon, so the two cannot collide.
std::string secret_mount_owner(const SecretMount& mount);
std::string secret_mount_name_key(const WorkspaceRecords& records, const SecretMount& mount);
std::string secret_mount_name_lookup(const SecretMount& mount);
nlohmann::json secret_mount_name_claim(const std::string& mount_id);
// The mount a name claim names; nullopt for a record that is not a claim.
std::optional<std::string> secret_mount_name_claimed(const StoredRecord& record);

// `secrets_entries`: one record per secret name, keyed by the blind index of mount and name. It
// holds the current version and the custom metadata; versions move it only forward.
struct SecretEntry {
    std::string mount_id;
    std::string name;
    std::uint64_t current_version = 0;
    nlohmann::json custom_metadata = nlohmann::json::object();
    std::string created_at;
    std::string updated_at;
    std::int64_t at_us = 0;
    std::int64_t revision = 0;
    std::int64_t lake_revision = 0;
};
struct SecretEntryCodec {
    static constexpr RecordCollection collection{"secrets_entries", 1};
    static std::string lookup(const std::string& mount_id);
    static std::string key(const WorkspaceRecords& records, const std::string& mount_id,
                           const std::string& name);
    static nlohmann::json encode(const SecretEntry& entry);
    static std::optional<SecretEntry> decode(const StoredRecord& record);
};

// `secrets_versions`: one record per version, keyed by the blind index of mount, name and number,
// written `if_absent` so a number is used once, and found by mount and name. `ciphertext` is the
// AES-256-GCM seal under the mount data key with associated data
// "lake secret/<mount>/<name>/<version>"; it is null once destroyed. `metadata` is the custom
// metadata the version's put left on the entry, so a later writer can complete a put that stopped
// between writing the version and moving the entry.
struct SecretVersionRecord {
    std::string mount_id;
    std::string name;
    std::uint64_t version = 0;
    std::optional<std::string> ciphertext;
    std::optional<nlohmann::json> metadata;
    std::string created_at;
    std::string deleted_at;
    std::string destroyed_at;
    std::int64_t at_us = 0;
    std::int64_t revision = 0;
    std::int64_t lake_revision = 0;
};
struct SecretVersionCodec {
    static constexpr RecordCollection collection{"secrets_versions", 1};
    static std::string lookup(const std::string& mount_id, const std::string& name);
    static std::string key(const WorkspaceRecords& records, const std::string& mount_id,
                           const std::string& name, std::uint64_t version);
    static nlohmann::json encode(const SecretVersionRecord& version);
    static std::optional<SecretVersionRecord> decode(const StoredRecord& record);
};

// `secrets_encryption_keys`: one record per named key, keyed by the blind index of mount and
// name, listed by mount. It keeps the key's id, which every transit ciphertext names.
struct EncryptionKeyRecord {
    EncryptionKey key;
    std::int64_t at_us = 0;
    std::int64_t revision = 0;
    std::int64_t lake_revision = 0;
};
struct EncryptionKeyCodec {
    static constexpr RecordCollection collection{"secrets_encryption_keys", 1};
    static std::string lookup(const std::string& mount_id);
    static std::string key(const WorkspaceRecords& records, const std::string& mount_id,
                           const std::string& name);
    static nlohmann::json encode(const EncryptionKey& key);
    static std::optional<EncryptionKeyRecord> decode(const StoredRecord& record);
};

// `secrets_data_keys`: one record per data key version, keyed by the blind index of its owner and
// version, listed by space (a customer KEK rotation rewraps them all). `owner_kind` is `mount` (a
// mount's data key, version 1) or `encryption` (a named key's versions).
struct SecretDataKey {
    std::string customer_id;
    std::string owner_kind;
    std::string owner_id;
    std::uint64_t key_version = 0;
    WrappedDataKey wrapped;
    std::int64_t at_us = 0;
    std::int64_t revision = 0;
    std::int64_t lake_revision = 0;
};
struct SecretDataKeyCodec {
    static constexpr RecordCollection collection{"secrets_data_keys", 1};
    static std::string lookup(const std::string& customer_id);
    static std::string key(const WorkspaceRecords& records, std::string_view owner_kind,
                           const std::string& owner_id, std::uint64_t key_version);
    static nlohmann::json encode(const SecretDataKey& key);
    static std::optional<SecretDataKey> decode(const StoredRecord& record);
};

// `secrets_dynamic_leases` (lake#362): one record per lease, keyed by the lease id and listed by
// mount. `ciphertext` is the generated credential sealed under the mount data key with associated
// data "lake dynamic secret/<mount>/<lease id>"; it is null once the lease is revoked or has been
// settled as expired. Every change after creation (renew, revoke, expire) is a conditional write
// at the revision read, so a renewal computed from an active copy never lands on a revoked or
// expired lease.
struct SecretLeaseRecord {
    std::string id;
    std::string mount_id;
    std::string role;
    std::optional<std::string> ciphertext;
    std::int64_t expires_us = 0;
    std::string revoked_at;
    std::string expired_at;
    int renewals = 0;
    std::int64_t at_us = 0;
    std::int64_t revision = 0;
    std::int64_t lake_revision = 0;

    // Revoked, or settled as expired: nothing can change it any more.
    bool settled() const {
        return !revoked_at.empty() || !expired_at.empty();
    }
    // Usable now: neither settled nor past its expiry.
    bool active(std::int64_t now_us) const {
        return !settled() && expires_us > now_us;
    }
};
struct SecretLeaseCodec {
    static constexpr RecordCollection collection{"secrets_dynamic_leases", 1};
    static std::string lookup(const std::string& mount_id);
    static nlohmann::json encode(const SecretLeaseRecord& lease);
    static std::optional<SecretLeaseRecord> decode(const StoredRecord& record);
};
// The capability's view of a lease record.
DynamicSecretLease to_lease(const SecretLeaseRecord& lease, const std::string& workspace_id);

} // namespace lake::control
