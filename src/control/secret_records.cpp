#include "lake/control/secret_records.hpp"

namespace lake::control {
namespace {

using nlohmann::json;

std::string text(const json& body, const char* field) {
    const auto found = body.find(field);
    return found != body.end() && found->is_string() ? found->get<std::string>() : std::string();
}

std::uint64_t number(const json& body, const char* field) {
    const auto found = body.find(field);
    if (found == body.end() || !found->is_number_integer() || found->get<std::int64_t>() < 0) {
        return 0;
    }
    return found->get<std::uint64_t>();
}

std::string or_created(std::string updated, std::int64_t at_us) {
    return updated.empty() ? record_instant(at_us) : updated;
}

} // namespace

std::string SecretMountCodec::lookup(const std::string& customer_id) {
    return "customer:" + customer_id;
}

json SecretMountCodec::encode(const SecretMount& mount) {
    return {{"kind", "mount"},
            {"customer_id", mount.customer_id},
            {"organization_id", mount.organization_id},
            {"workspace_id", mount.workspace_id},
            {"name", mount.name},
            {"description", mount.description},
            {"updated_at", mount.updated_at}};
}

std::optional<SecretMount> SecretMountCodec::decode(const StoredRecord& record) {
    if (record.schema_version != 1 || !record.body.is_object() ||
        text(record.body, "kind") != "mount") {
        return std::nullopt;
    }
    SecretMount mount;
    mount.id = record.id;
    mount.customer_id = text(record.body, "customer_id");
    mount.organization_id = text(record.body, "organization_id");
    mount.workspace_id = text(record.body, "workspace_id");
    mount.name = text(record.body, "name");
    mount.description = text(record.body, "description");
    mount.at_us = record.at_us;
    mount.created_at = record_instant(record.at_us);
    mount.updated_at = or_created(text(record.body, "updated_at"), record.at_us);
    // An empty organization is a mount its account owns (the workspace's customer).
    if (mount.customer_id.empty() || mount.name.empty()) {
        return std::nullopt;
    }
    return mount;
}

std::string secret_mount_owner(const SecretMount& mount) {
    return mount.organization_id.empty() ? "customer:" + mount.customer_id
                                         : "organization:" + mount.organization_id;
}

std::string secret_mount_name_key(const WorkspaceRecords& records, const SecretMount& mount) {
    return records.index_of(SecretMountCodec::collection,
                            secret_mount_owner(mount) + "\nname:" + mount.name);
}

std::string secret_mount_name_lookup(const SecretMount& mount) {
    // An organization's claims keep the lookup they were written with.
    return mount.organization_id.empty() ? "name-claims:" + secret_mount_owner(mount)
                                         : "name-claims:" + mount.organization_id;
}

json secret_mount_name_claim(const std::string& mount_id) {
    return {{"kind", "name_claim"}, {"mount_id", mount_id}};
}

std::optional<std::string> secret_mount_name_claimed(const StoredRecord& record) {
    if (!record.body.is_object() || text(record.body, "kind") != "name_claim") {
        return std::nullopt;
    }
    std::string mount_id = text(record.body, "mount_id");
    if (mount_id.empty()) {
        return std::nullopt;
    }
    return mount_id;
}

std::string SecretEntryCodec::lookup(const std::string& mount_id) {
    return "mount:" + mount_id;
}

std::string SecretEntryCodec::key(const WorkspaceRecords& records, const std::string& mount_id,
                                  const std::string& name) {
    return records.index_of(collection, "mount:" + mount_id + "\nname:" + name);
}

json SecretEntryCodec::encode(const SecretEntry& entry) {
    return {{"mount_id", entry.mount_id},
            {"name", entry.name},
            {"current_version", entry.current_version},
            {"custom_metadata", entry.custom_metadata},
            {"updated_at", entry.updated_at}};
}

std::optional<SecretEntry> SecretEntryCodec::decode(const StoredRecord& record) {
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    SecretEntry entry;
    entry.mount_id = text(record.body, "mount_id");
    entry.name = text(record.body, "name");
    entry.current_version = number(record.body, "current_version");
    entry.custom_metadata = record.body.value("custom_metadata", json::object());
    entry.at_us = record.at_us;
    entry.created_at = record_instant(record.at_us);
    entry.updated_at = or_created(text(record.body, "updated_at"), record.at_us);
    entry.revision = record.revision;
    entry.lake_revision = record.lake_revision;
    if (entry.mount_id.empty() || entry.name.empty() || !entry.custom_metadata.is_object()) {
        return std::nullopt;
    }
    return entry;
}

std::string SecretVersionCodec::lookup(const std::string& mount_id, const std::string& name) {
    return "mount:" + mount_id + "\nname:" + name;
}

std::string SecretVersionCodec::key(const WorkspaceRecords& records, const std::string& mount_id,
                                    const std::string& name, std::uint64_t version) {
    return records.index_of(collection, "mount:" + mount_id + "\nname:" + name +
                                            "\nversion:" + std::to_string(version));
}

json SecretVersionCodec::encode(const SecretVersionRecord& version) {
    return {{"mount_id", version.mount_id},
            {"name", version.name},
            {"version", version.version},
            {"ciphertext", version.ciphertext ? json(*version.ciphertext) : json(nullptr)},
            {"metadata", version.metadata ? *version.metadata : json(nullptr)},
            {"deleted_at", version.deleted_at},
            {"destroyed_at", version.destroyed_at}};
}

std::optional<SecretVersionRecord> SecretVersionCodec::decode(const StoredRecord& record) {
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    SecretVersionRecord version;
    version.mount_id = text(record.body, "mount_id");
    version.name = text(record.body, "name");
    version.version = number(record.body, "version");
    if (const auto found = record.body.find("ciphertext");
        found != record.body.end() && found->is_string()) {
        version.ciphertext = found->get<std::string>();
    }
    if (const auto found = record.body.find("metadata");
        found != record.body.end() && found->is_object()) {
        version.metadata = *found;
    }
    version.deleted_at = text(record.body, "deleted_at");
    version.destroyed_at = text(record.body, "destroyed_at");
    version.at_us = record.at_us;
    version.created_at = record_instant(record.at_us);
    version.revision = record.revision;
    version.lake_revision = record.lake_revision;
    if (version.mount_id.empty() || version.name.empty() || version.version == 0 ||
        (version.destroyed_at.empty() && !version.ciphertext) ||
        (!version.destroyed_at.empty() && version.ciphertext)) {
        return std::nullopt;
    }
    return version;
}

std::string EncryptionKeyCodec::lookup(const std::string& mount_id) {
    return "mount:" + mount_id;
}

std::string EncryptionKeyCodec::key(const WorkspaceRecords& records, const std::string& mount_id,
                                    const std::string& name) {
    return records.index_of(collection, "mount:" + mount_id + "\nname:" + name);
}

json EncryptionKeyCodec::encode(const EncryptionKey& key) {
    return {{"id", key.id},
            {"mount_id", key.mount_id},
            {"name", key.name},
            {"derived_from", key.derived_from},
            {"current_version", key.current_version},
            {"updated_at", key.updated_at}};
}

std::optional<EncryptionKeyRecord> EncryptionKeyCodec::decode(const StoredRecord& record) {
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    EncryptionKeyRecord decoded;
    EncryptionKey& key = decoded.key;
    key.id = text(record.body, "id");
    key.mount_id = text(record.body, "mount_id");
    key.name = text(record.body, "name");
    key.derived_from = text(record.body, "derived_from");
    key.current_version = number(record.body, "current_version");
    key.created_at = record_instant(record.at_us);
    key.updated_at = or_created(text(record.body, "updated_at"), record.at_us);
    decoded.at_us = record.at_us;
    decoded.revision = record.revision;
    decoded.lake_revision = record.lake_revision;
    if (key.id.empty() || key.mount_id.empty() || key.name.empty() || key.current_version == 0) {
        return std::nullopt;
    }
    return decoded;
}

std::string SecretDataKeyCodec::lookup(const std::string& customer_id) {
    return "customer:" + customer_id;
}

std::string SecretDataKeyCodec::key(const WorkspaceRecords& records, std::string_view owner_kind,
                                    const std::string& owner_id, std::uint64_t key_version) {
    return records.index_of(collection, std::string(owner_kind) + ":" + owner_id +
                                            "\nversion:" + std::to_string(key_version));
}

json SecretDataKeyCodec::encode(const SecretDataKey& key) {
    return {{"customer_id", key.customer_id},
            {"owner_kind", key.owner_kind},
            {"owner_id", key.owner_id},
            {"key_version", key.key_version},
            {"kek_version", key.wrapped.kek_version},
            {"wrapped_key", key.wrapped.wrapped_key}};
}

std::optional<SecretDataKey> SecretDataKeyCodec::decode(const StoredRecord& record) {
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    SecretDataKey key;
    key.customer_id = text(record.body, "customer_id");
    key.owner_kind = text(record.body, "owner_kind");
    key.owner_id = text(record.body, "owner_id");
    key.key_version = number(record.body, "key_version");
    key.wrapped.kek_version = number(record.body, "kek_version");
    key.wrapped.wrapped_key = text(record.body, "wrapped_key");
    key.at_us = record.at_us;
    key.revision = record.revision;
    key.lake_revision = record.lake_revision;
    if (key.customer_id.empty() || (key.owner_kind != "mount" && key.owner_kind != "encryption") ||
        key.owner_id.empty() || key.key_version == 0 || key.wrapped.kek_version == 0 ||
        key.wrapped.wrapped_key.empty()) {
        return std::nullopt;
    }
    return key;
}

std::string SecretLeaseCodec::lookup(const std::string& mount_id) {
    return "mount:" + mount_id;
}

json SecretLeaseCodec::encode(const SecretLeaseRecord& lease) {
    return {{"mount_id", lease.mount_id},
            {"role", lease.role},
            {"ciphertext", lease.ciphertext ? json(*lease.ciphertext) : json(nullptr)},
            {"expires_us", lease.expires_us},
            {"revoked_at", lease.revoked_at},
            {"expired_at", lease.expired_at},
            {"renewals", lease.renewals}};
}

std::optional<SecretLeaseRecord> SecretLeaseCodec::decode(const StoredRecord& record) {
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    SecretLeaseRecord lease;
    lease.id = record.id;
    lease.mount_id = text(record.body, "mount_id");
    lease.role = text(record.body, "role");
    if (const auto found = record.body.find("ciphertext");
        found != record.body.end() && found->is_string()) {
        lease.ciphertext = found->get<std::string>();
    }
    if (const auto found = record.body.find("expires_us");
        found != record.body.end() && found->is_number_integer()) {
        lease.expires_us = found->get<std::int64_t>();
    }
    lease.revoked_at = text(record.body, "revoked_at");
    lease.expired_at = text(record.body, "expired_at");
    lease.renewals = static_cast<int>(number(record.body, "renewals"));
    lease.at_us = record.at_us;
    lease.revision = record.revision;
    lease.lake_revision = record.lake_revision;
    if (lease.mount_id.empty() || lease.role.empty() || lease.expires_us <= 0 ||
        (!lease.settled() && !lease.ciphertext)) {
        return std::nullopt;
    }
    return lease;
}

DynamicSecretLease to_lease(const SecretLeaseRecord& lease, const std::string& workspace_id) {
    return DynamicSecretLease{lease.id,
                              lease.mount_id,
                              workspace_id,
                              lease.role,
                              record_instant(lease.expires_us),
                              lease.revoked_at,
                              lease.expired_at,
                              lease.renewals,
                              record_instant(lease.at_us)};
}

} // namespace lake::control
