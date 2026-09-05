#include "lake/control/memory_store.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <random>
#include <set>

#include "lake/control/handles.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/policy.hpp"
#include "secrets_crypto.hpp"

namespace lake::control {

std::string new_uuid_v4() {
    thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<unsigned> byte(0, 255);
    unsigned char bytes[16];
    for (auto& b : bytes)
        b = static_cast<unsigned char>(byte(rng));
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40); // version 4
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80); // RFC 4122 variant
    std::string out;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out += '-';
        out += std::format("{:02x}", bytes[i]);
    }
    return out;
}

std::string iso8601_at(std::chrono::system_clock::time_point value) {
    return std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::seconds>(value));
}

std::string now_iso8601() {
    return iso8601_at(std::chrono::system_clock::now());
}

namespace {

std::string lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

nlohmann::json nullable(const std::string& s) {
    return s.empty() ? nlohmann::json(nullptr) : nlohmann::json(s);
}

bool valid_object_type(std::string_view type) {
    static const std::set<std::string_view> types = {
        "customer",
        "organization",
        "team",
        "member",
        "repository",
        "path",
        "environment",
        "secret_mount",
        "secret",
    };
    return types.contains(type);
}

bool valid_policy_role(std::string_view role) {
    static const std::set<std::string_view> roles = {
        "read",
        "write",
        "admin",
        "deploy",
        "review",
        "share",
    };
    return roles.contains(role);
}

bool valid_membership_role(std::string_view role) {
    return role == "owner" || role == "admin" || role == "member";
}

std::string policy_role_for_membership(std::string_view role) {
    return role == "member" ? "read" : "admin";
}

} // namespace

nlohmann::json to_json(const User& u) {
    return {{"id", u.id},
            {"taap_id", nullable(u.taap_id)},
            {"email", nullable(u.email)},
            {"display_name", u.display_name},
            {"created_at", u.created_at},
            {"updated_at", u.updated_at}};
}
nlohmann::json to_json(const Organization& o) {
    return {{"id", o.id},
            {"taap_id", nullable(o.taap_id)},
            {"display_name", o.display_name},
            {"created_at", o.created_at},
            {"updated_at", o.updated_at}};
}
nlohmann::json to_json(const Customer& c) {
    return {{"id", c.id},
            {"customer_type", c.customer_type},
            {"user_id", nullable(c.user_id)},
            {"organization_id", nullable(c.organization_id)},
            {"name", c.name},
            {"created_at", c.created_at},
            {"updated_at", c.updated_at}};
}

nlohmann::json to_json(const Repository& r) {
    return {{"id", r.id},
            {"customer_id", r.customer_id},
            {"name", r.name},
            {"description", r.description},
            {"visibility", r.visibility},
            {"deleted_at", nullable(r.deleted_at)},
            {"retention_until", nullable(r.retention_until)},
            {"created_at", r.created_at},
            {"updated_at", r.updated_at}};
}

nlohmann::json to_json(const RepositoryRef& reference) {
    return {{"repository_id", reference.repository_id},
            {"name", reference.name},
            {"head", reference.head},
            {"updated_at", reference.updated_at}};
}

nlohmann::json to_json(const ForgeRecord& record) {
    return {{"id", record.id},
            {"repository_id", record.repository_id},
            {"kind", record.kind},
            {"value", record.value},
            {"created_at", record.created_at},
            {"updated_at", record.updated_at}};
}

nlohmann::json to_json(const DriveShareLink& link) {
    return {{"id", link.id},
            {"repository_id", link.repository_id},
            {"object_id", link.object_id},
            {"node_id", link.node_id},
            {"path", link.path},
            {"role", link.role},
            {"expires_at", link.expires_at},
            {"revoked_at", nullable(link.revoked_at)},
            {"created_by_taap_id", link.created_by_taap_id},
            {"created_at", link.created_at}};
}

nlohmann::json to_json(const Team& team) {
    return {{"id", team.id},
            {"organization_id", team.organization_id},
            {"name", team.name},
            {"display_name", team.display_name},
            {"created_at", team.created_at},
            {"updated_at", team.updated_at}};
}

nlohmann::json to_json(const Membership& membership) {
    return {{"id", membership.id},
            {"organization_id", membership.organization_id},
            {"team_id", nullable(membership.team_id)},
            {"principal_type", membership.principal_type},
            {"principal_id", membership.principal_id},
            {"principal_taap_id", membership.principal_taap_id},
            {"role", membership.role},
            {"attestation_digest", membership.attestation_digest},
            {"granted_by_taap_id", membership.granted_by_taap_id},
            {"created_at", membership.created_at},
            {"updated_at", membership.updated_at}};
}

nlohmann::json to_json(const Invitation& invitation) {
    return {{"id", invitation.id},
            {"organization_id", invitation.organization_id},
            {"team_id", nullable(invitation.team_id)},
            {"invitee_type", invitation.invitee_type},
            {"invitee", invitation.invitee},
            {"role", invitation.role},
            {"expires_at", invitation.expires_at},
            {"status", invitation.status},
            {"accepted_membership_id", nullable(invitation.accepted_membership_id)},
            {"created_by_taap_id", invitation.created_by_taap_id},
            {"created_at", invitation.created_at},
            {"updated_at", invitation.updated_at}};
}

nlohmann::json to_json(const PolicyObject& object) {
    return {{"id", object.id},
            {"object_type", object.object_type},
            {"external_id", object.external_id},
            {"parent_id", nullable(object.parent_id)},
            {"inherit", object.inherit},
            {"created_at", object.created_at},
            {"updated_at", object.updated_at}};
}

nlohmann::json to_json(const Grant& grant) {
    return {{"id", grant.id},
            {"principal_taap_id", grant.principal_taap_id},
            {"object_id", grant.object_id},
            {"role", grant.role},
            {"attestation_digest", grant.attestation_digest},
            {"granted_by_taap_id", grant.granted_by_taap_id},
            {"created_at", grant.created_at},
            {"updated_at", grant.updated_at}};
}

nlohmann::json to_json(const SecretMount& mount) {
    return {{"id", mount.id},
            {"customer_id", mount.customer_id},
            {"organization_id", mount.organization_id},
            {"name", mount.name},
            {"description", mount.description},
            {"created_at", mount.created_at},
            {"updated_at", mount.updated_at}};
}

nlohmann::json to_json(const SecretVersion& version) {
    return {{"mount_id", version.mount_id},
            {"name", version.name},
            {"version", version.version},
            {"value", version.value},
            {"custom_metadata", version.custom_metadata},
            {"created_at", version.created_at},
            {"deleted_at", nullable(version.deleted_at)},
            {"destroyed_at", nullable(version.destroyed_at)}};
}

nlohmann::json to_json(const SecretMetadata& metadata) {
    nlohmann::json versions = nlohmann::json::array();
    for (const SecretVersionMetadata& version : metadata.versions) {
        versions.push_back({{"version", version.version},
                            {"created_at", version.created_at},
                            {"deleted_at", nullable(version.deleted_at)},
                            {"destroyed_at", nullable(version.destroyed_at)}});
    }
    return {{"mount_id", metadata.mount_id},
            {"name", metadata.name},
            {"current_version", metadata.current_version},
            {"custom_metadata", metadata.custom_metadata},
            {"versions", std::move(versions)},
            {"created_at", metadata.created_at},
            {"updated_at", metadata.updated_at}};
}

nlohmann::json to_json(const EncryptionKey& key) {
    return {{"id", key.id},
            {"mount_id", key.mount_id},
            {"name", key.name},
            {"derived_from", nullable(key.derived_from)},
            {"current_version", key.current_version},
            {"created_at", key.created_at},
            {"updated_at", key.updated_at}};
}

nlohmann::json to_json(const TransitValue& value) {
    return {
        {"value", value.value}, {"key_version", value.key_version}, {"algorithm", value.algorithm}};
}

nlohmann::json to_json(const TransitVerification& value) {
    return {
        {"valid", value.valid}, {"key_version", value.key_version}, {"algorithm", value.algorithm}};
}

nlohmann::json to_json(const DerivedEncryptionKey& value) {
    nlohmann::json output = to_json(value.key);
    output["parent_version"] = value.parent_version;
    return output;
}

nlohmann::json to_json(const CustomerKekRotation& value) {
    return {{"customer_id", value.customer_id},
            {"version", value.version},
            {"data_keys_rewrapped", value.data_keys_rewrapped}};
}

Result<void> InMemoryStore::ping() {
    return {};
}

Result<User> InMemoryStore::create_user(const NewUser& in) {
    std::lock_guard lock(mutex_);
    for (const auto& u : users_) {
        if (!in.taap_id.empty() && u.taap_id == in.taap_id)
            return std::unexpected(StoreError{"conflict", "taap_id already exists"});
        if (!in.email.empty() && lower(u.email) == lower(in.email))
            return std::unexpected(StoreError{"conflict", "email already exists"});
    }
    const std::string now = now_iso8601();
    users_.push_back(User{new_uuid_v4(), in.taap_id, in.email, in.display_name, now, now});
    return users_.back();
}

Result<std::vector<User>> InMemoryStore::list_users(int limit) {
    std::lock_guard lock(mutex_);
    std::vector<User> out(users_.rbegin(), users_.rend());
    if (limit > 0 && out.size() > static_cast<std::size_t>(limit))
        out.resize(static_cast<std::size_t>(limit));
    return out;
}

audit::Result<audit::Identity> InMemoryStore::current_lake_identity() {
    std::lock_guard lock(mutex_);
    for (const audit::Identity& identity : identities_) {
        if (identity.current) {
            return identity;
        }
    }
    return std::unexpected(audit::Error{"not_found", "no lake identity exists"});
}

audit::Result<audit::Identity> InMemoryStore::lake_identity_by_taap_id(const std::string& taap_id) {
    std::lock_guard lock(mutex_);
    for (const audit::Identity& identity : identities_) {
        if (identity.taap_id == taap_id) {
            return identity;
        }
    }
    return std::unexpected(audit::Error{"not_found", "no lake identity for " + taap_id});
}

audit::Result<void> InMemoryStore::create_lake_identity(const audit::Identity& identity) {
    std::lock_guard lock(mutex_);
    for (const audit::Identity& existing : identities_) {
        if (existing.current || existing.taap_id == identity.taap_id) {
            return std::unexpected(audit::Error{"conflict", "a lake identity already exists"});
        }
    }
    identities_.push_back(identity);
    return {};
}

audit::Result<void> InMemoryStore::rotate_lake_identity(const audit::Identity& previous,
                                                        const audit::Identity& replacement) {
    std::lock_guard lock(mutex_);
    for (audit::Identity& existing : identities_) {
        if (existing.id == previous.id && existing.current) {
            existing.current = false;
            identities_.push_back(replacement);
            return {};
        }
    }
    return std::unexpected(audit::Error{"conflict", "the lake identity changed before rotation"});
}

Result<void> InMemoryStore::append_audit_event(const audit::Event& event) {
    std::lock_guard lock(mutex_);
    const std::string previous_digest = audit_events_.empty() ? "" : audit_events_.back().digest;
    if (event.previous_digest != previous_digest) {
        return std::unexpected(
            StoreError{"conflict", "audit event predecessor changed before append"});
    }
    audit_events_.push_back(event);
    return {};
}

Result<audit::Event> InMemoryStore::latest_audit_event() {
    std::lock_guard lock(mutex_);
    if (audit_events_.empty()) {
        return std::unexpected(StoreError{"not_found", "no audit events exist"});
    }
    return audit_events_.back();
}

Result<std::vector<audit::Event>> InMemoryStore::list_audit_events(const audit::Filter& filter) {
    std::lock_guard lock(mutex_);
    std::vector<audit::Event> events;
    for (const audit::Event& event : audit_events_) {
        if (!filter.principal.empty() && event.draft.principal != filter.principal) {
            continue;
        }
        if (!filter.object.empty() && event.draft.object != filter.object) {
            continue;
        }
        if (!filter.outcome.empty() && event.draft.outcome != filter.outcome) {
            continue;
        }
        if (!filter.from.empty() && event.occurred_at < filter.from) {
            continue;
        }
        if (!filter.to.empty() && event.occurred_at > filter.to) {
            continue;
        }
        events.push_back(event);
        if (filter.limit > 0 && events.size() >= static_cast<std::size_t>(filter.limit)) {
            break;
        }
    }
    return events;
}

Result<void> InMemoryStore::set_audit_retention_days(const std::string& customer_id, int days) {
    std::lock_guard lock(mutex_);
    if (days < audit::retention_floor_days) {
        return std::unexpected(
            StoreError{"invalid_input", "audit retention cannot be below 400 days"});
    }
    if (customer_id.empty()) {
        return std::unexpected(StoreError{"invalid_input", "customer_id is required"});
    }
    for (const Customer& customer : customers_) {
        if (customer.id == customer_id) {
            for (auto& retention : audit_retentions_) {
                if (retention.first == customer_id) {
                    if (days < retention.second) {
                        return std::unexpected(
                            StoreError{"conflict", "audit retention cannot be reduced"});
                    }
                    retention.second = days;
                    return {};
                }
            }
            audit_retentions_.emplace_back(customer_id, days);
            return {};
        }
    }
    return std::unexpected(StoreError{"not_found", "no customer " + customer_id});
}

Result<MeteringUsage> InMemoryStore::upsert_metering_usage(const MeteringUpdate& update) {
    if (update.customer_id.empty() || update.hour.empty()) {
        return std::unexpected(StoreError{"invalid_input", "customer_id and hour are required"});
    }
    std::lock_guard lock(mutex_);
    auto found = std::ranges::find_if(metering_usage_, [&](const MeteringUsage& usage) {
        return usage.customer_id == update.customer_id && usage.hour == update.hour;
    });
    if (found == metering_usage_.end()) {
        metering_usage_.push_back(MeteringUsage{.customer_id = update.customer_id,
                                                .hour = update.hour,
                                                .stored_bytes = update.stored_bytes.value_or(0),
                                                .transfer_in_bytes = update.transfer_in_bytes,
                                                .transfer_out_bytes = update.transfer_out_bytes});
        return metering_usage_.back();
    }
    if (update.stored_bytes) {
        found->stored_bytes = *update.stored_bytes;
    }
    found->transfer_in_bytes += update.transfer_in_bytes;
    found->transfer_out_bytes += update.transfer_out_bytes;
    return *found;
}

Result<bool> InMemoryStore::record_metered_build(const MeteredBuild& build) {
    if (build.customer_id.empty() || build.result_id.empty() || build.hour.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "customer_id, result_id, and hour are required"});
    }
    std::lock_guard lock(mutex_);
    if (!metered_builds_.emplace(build.customer_id, build.result_id).second) {
        return false;
    }
    auto found = std::ranges::find_if(metering_usage_, [&](const MeteringUsage& usage) {
        return usage.customer_id == build.customer_id && usage.hour == build.hour;
    });
    if (found == metering_usage_.end()) {
        metering_usage_.push_back(MeteringUsage{.customer_id = build.customer_id,
                                                .hour = build.hour,
                                                .compute_seconds = build.compute_seconds});
    } else {
        found->compute_seconds += build.compute_seconds;
    }
    return true;
}

Result<std::vector<MeteringUsage>>
InMemoryStore::list_metering_usage(const std::string& customer_id,
                                   const std::string& from_hour,
                                   const std::string& to_hour,
                                   int limit) {
    if (customer_id.empty() || limit < 1) {
        return std::unexpected(StoreError{"invalid_input", "customer_id and limit are required"});
    }
    std::lock_guard lock(mutex_);
    std::vector<MeteringUsage> result;
    for (const MeteringUsage& usage : metering_usage_) {
        if (usage.customer_id != customer_id) {
            continue;
        }
        if (!from_hour.empty() && usage.hour < from_hour) {
            continue;
        }
        if (!to_hour.empty() && usage.hour > to_hour) {
            continue;
        }
        result.push_back(usage);
    }
    std::ranges::sort(result, {}, &MeteringUsage::hour);
    if (result.size() > static_cast<std::size_t>(limit)) {
        result.resize(static_cast<std::size_t>(limit));
    }
    return result;
}

Result<User> InMemoryStore::get_user(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (const auto& u : users_)
        if (u.id == id)
            return u;
    return std::unexpected(StoreError{"not_found", "no user " + id});
}

Result<Organization> InMemoryStore::create_organization(const NewOrganization& in) {
    std::lock_guard lock(mutex_);
    for (const auto& o : organizations_) {
        if (!in.taap_id.empty() && o.taap_id == in.taap_id)
            return std::unexpected(StoreError{"conflict", "taap_id already exists"});
    }
    const std::string now = now_iso8601();
    organizations_.push_back(Organization{new_uuid_v4(), in.taap_id, in.display_name, now, now});
    std::string parent_id;
    if (!in.customer_id.empty()) {
        const auto parent = std::ranges::find_if(policy_objects_, [&](const PolicyObject& object) {
            return object.object_type == "customer" && object.external_id == in.customer_id;
        });
        if (parent == policy_objects_.end()) {
            organizations_.pop_back();
            return std::unexpected(
                StoreError{"invalid_input", "customer_id does not reference a policy object"});
        }
        parent_id = parent->id;
    }
    policy_objects_.push_back(PolicyObject{
        new_uuid_v4(), "organization", organizations_.back().id, parent_id, now, now, true});
    return organizations_.back();
}

Result<std::vector<Organization>> InMemoryStore::list_organizations(int limit) {
    std::lock_guard lock(mutex_);
    std::vector<Organization> out(organizations_.rbegin(), organizations_.rend());
    if (limit > 0 && out.size() > static_cast<std::size_t>(limit))
        out.resize(static_cast<std::size_t>(limit));
    return out;
}

Result<Organization> InMemoryStore::get_organization(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (const auto& o : organizations_)
        if (o.id == id)
            return o;
    return std::unexpected(StoreError{"not_found", "no organization " + id});
}

Result<Customer> InMemoryStore::create_customer(const NewCustomer& in) {
    std::lock_guard lock(mutex_);
    const bool individual = in.customer_type == "individual";
    if (!individual && in.customer_type != "organization") {
        return std::unexpected(
            StoreError{"invalid_input", "customer_type must be individual or organization"});
    }
    if (individual ? (in.user_id.empty() || !in.organization_id.empty())
                   : (in.organization_id.empty() || !in.user_id.empty())) {
        return std::unexpected(StoreError{"invalid_input",
                                          "an individual has exactly user_id, an organization "
                                          "exactly organization_id (customer_type_check)"});
    }
    if (individual && std::none_of(users_.begin(), users_.end(), [&](const User& u) {
            return u.id == in.user_id;
        })) {
        return std::unexpected(StoreError{"invalid_input", "user_id does not reference a user"});
    }
    if (!individual &&
        std::none_of(organizations_.begin(), organizations_.end(), [&](const Organization& o) {
            return o.id == in.organization_id;
        })) {
        return std::unexpected(
            StoreError{"invalid_input", "organization_id does not reference an organization"});
    }
    if (!valid_handle(in.name))
        return std::unexpected(StoreError{"invalid_input", "name must match customer_name_format"});
    for (const auto& c : customers_) {
        if (c.name == in.name)
            return std::unexpected(
                StoreError{"conflict", "name already taken (customer_name_unique)"});
        if (individual && c.user_id == in.user_id)
            return std::unexpected(StoreError{"conflict", "this user already has a customer"});
        if (!individual && c.organization_id == in.organization_id)
            return std::unexpected(
                StoreError{"conflict", "this organization already has a customer"});
    }
    const std::string now = now_iso8601();
    customers_.push_back(Customer{
        new_uuid_v4(), in.customer_type, in.user_id, in.organization_id, in.name, now, now});
    policy_objects_.push_back(
        PolicyObject{new_uuid_v4(), "customer", customers_.back().id, {}, now, now, true});
    if (!individual) {
        for (auto& object : policy_objects_) {
            if (object.object_type == "organization" && object.external_id == in.organization_id) {
                object.parent_id = policy_objects_.back().id;
                object.updated_at = now;
            }
        }
    }
    return customers_.back();
}

Result<std::vector<Customer>> InMemoryStore::list_customers(int limit) {
    std::lock_guard lock(mutex_);
    std::vector<Customer> out(customers_.rbegin(), customers_.rend());
    if (limit > 0 && out.size() > static_cast<std::size_t>(limit))
        out.resize(static_cast<std::size_t>(limit));
    return out;
}

Result<Customer> InMemoryStore::get_customer(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (const auto& c : customers_)
        if (c.id == id)
            return c;
    return std::unexpected(StoreError{"not_found", "no customer " + id});
}

} // namespace lake::control

namespace lake::control {

nlohmann::json to_json(const Device& d) {
    return {{"id", d.id},
            {"user_id", d.user_id},
            {"name", d.name},
            {"passkey_credential_id", nullable(d.passkey_credential_id)},
            {"passkey_binding_attestation", d.passkey_binding_attestation},
            {"revoked_at", nullable(d.revoked_at)},
            {"created_at", d.created_at},
            {"updated_at", d.updated_at}};
}

nlohmann::json to_json(const Session& s) {
    return {{"id", s.id},
            {"user_id", nullable(s.user_id)},
            {"device_id", nullable(s.device_id)},
            {"pool_id", s.pool_id},
            {"kind", s.kind},
            {"name", s.name},
            {"scopes", s.scopes},
            {"reason", s.reason},
            {"expires_at", nullable(s.expires_at)},
            {"revoked_at", nullable(s.revoked_at)},
            {"created_at", s.created_at},
            {"last_used_at", nullable(s.last_used_at)}};
}

nlohmann::json to_json(const OfflineGrant& value) {
    return {{"id", value.id},
            {"user_id", value.user_id},
            {"principal_taap_id", value.principal_taap_id},
            {"device_id", value.device_id},
            {"scopes", value.scopes},
            {"issued_at", value.issued_at},
            {"expires_at", value.expires_at},
            {"revoked_at", nullable(value.revoked_at)},
            {"credential", value.credential}};
}

nlohmann::json to_json(const RecoveryAttempt& value) {
    return {{"id", value.id},
            {"user_id", value.user_id},
            {"organization_id", nullable(value.organization_id)},
            {"method", value.method},
            {"status", value.status},
            {"ready_at", value.ready_at},
            {"expires_at", value.expires_at},
            {"completed_at", nullable(value.completed_at)},
            {"approval_count", value.approval_count}};
}

nlohmann::json to_json(const RecoveryDelivery& value) {
    return {{"id", value.id},
            {"recovery_id", value.recovery_id},
            {"email", value.email},
            {"token", value.token},
            {"kind", value.kind},
            {"created_at", value.created_at},
            {"delivered_at", nullable(value.delivered_at)}};
}

nlohmann::json to_json(const RecoveryNotification& value) {
    return {{"id", value.id},
            {"recovery_id", value.recovery_id},
            {"user_id", value.user_id},
            {"device_id", value.device_id},
            {"kind", value.kind},
            {"created_at", value.created_at}};
}

nlohmann::json to_json(const RecoveryContact& value, bool include_wrapped_key) {
    nlohmann::json output = {{"repository_id", value.repository_id},
                             {"contact_taap_id", value.contact_taap_id},
                             {"created_at", value.created_at},
                             {"updated_at", value.updated_at}};
    if (include_wrapped_key) {
        output["wrapped_key"] = value.wrapped_key;
    }
    return output;
}

nlohmann::json to_json(const SsoProvider& provider) {
    return {{"id", provider.id},
            {"organization_id", provider.organization_id},
            {"pool_id", provider.pool_id},
            {"name", provider.name},
            {"protocol", provider.protocol},
            {"issuer", provider.issuer},
            {"metadata", provider.metadata},
            {"signing_certificate", provider.signing_certificate},
            {"client_id", provider.client_id},
            {"attribute_mapping", provider.attribute_mapping},
            {"jit_provisioning", provider.jit_provisioning},
            {"enabled", provider.enabled},
            {"tested_at", nullable(provider.tested_at)},
            {"created_at", provider.created_at},
            {"updated_at", provider.updated_at}};
}

nlohmann::json to_json(const FederationIssuer& issuer) {
    return {{"id", issuer.id},
            {"organization_id", issuer.organization_id},
            {"issuer", issuer.issuer},
            {"claim", issuer.claim},
            {"created_at", issuer.created_at}};
}

nlohmann::json to_json(const ScimUser& user) {
    return {{"provider_id", user.provider_id},
            {"external_id", user.external_id},
            {"user_id", user.user_id},
            {"active", user.active},
            {"attributes", user.attributes},
            {"created_at", user.created_at},
            {"updated_at", user.updated_at}};
}

nlohmann::json to_json(const ScimGroup& group) {
    return {{"provider_id", group.provider_id},
            {"external_id", group.external_id},
            {"team_id", group.team_id},
            {"display_name", group.display_name},
            {"attributes", group.attributes},
            {"created_at", group.created_at},
            {"updated_at", group.updated_at}};
}

nlohmann::json to_json(const ProvisioningLog& log) {
    return {{"id", log.id},
            {"provider_id", log.provider_id},
            {"resource", log.resource},
            {"operation", log.operation},
            {"external_id", log.external_id},
            {"outcome", log.outcome},
            {"details", log.details},
            {"created_at", log.created_at}};
}

namespace {

Device&
add_device(std::vector<Device>& devices, const std::string& user_id, const std::string& name) {
    const std::string now = now_iso8601();
    devices.push_back(
        Device{new_uuid_v4(), user_id, name.empty() ? "browser" : name, "", now, now});
    return devices.back();
}

IssuedSession add_session(std::vector<Session>& sessions,
                          const std::string& user_id,
                          const std::string& pool_id,
                          const std::string& device_id,
                          const std::string& kind,
                          const std::string& name,
                          const std::vector<std::string>& scopes,
                          const std::string& reason,
                          int lifetime) {
    const std::string secret = identity_crypto::random_secret();
    const std::string hash = identity_crypto::hash_secret(secret);
    if (secret.empty() || hash.empty())
        return {};
    const std::string now = now_iso8601();
    std::string expires_at;
    if (lifetime > 0) {
        const auto expiry = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count() +
                            lifetime;
        expires_at = std::to_string(expiry);
    }
    sessions.push_back(Session{new_uuid_v4(),
                               user_id,
                               device_id,
                               pool_id,
                               kind,
                               name,
                               reason,
                               expires_at,
                               "",
                               now,
                               "",
                               scopes});
    sessions.back().reason += "\n" + hash;
    return {sessions.back(), secret};
}

std::string session_hash(const Session& session) {
    const auto newline = session.reason.rfind('\n');
    return newline == std::string::npos ? "" : session.reason.substr(newline + 1);
}

std::string session_reason(const Session& session) {
    const auto newline = session.reason.rfind('\n');
    return newline == std::string::npos ? session.reason : session.reason.substr(0, newline);
}

} // namespace

Result<IssuedSession> InMemoryStore::sign_up_password(const NewPasswordUser& input) {
    if (input.email.empty() || input.password.empty())
        return std::unexpected(StoreError{"invalid_input", "email and password are required"});
    std::lock_guard lock(mutex_);
    const std::string pool = input.pool_id.empty() ? "lake" : input.pool_id;
    for (auto& credential : passwords_) {
        if (credential.pool_id == pool && lower(credential.email) == lower(input.email))
            return std::unexpected(StoreError{"conflict", "email already exists"});
    }
    const std::string identity_secret = identity_crypto::random_secret();
    if (identity_secret.empty())
        return std::unexpected(StoreError{"store_error", "could not create identity"});
    const std::string identity = "taap:1:ed25519:" + identity_secret;
    const std::string now = now_iso8601();
    users_.push_back(User{new_uuid_v4(), identity, input.email, input.display_name, now, now});
    const std::string password_hash = identity_crypto::hash_secret(input.password);
    if (password_hash.empty())
        return std::unexpected(StoreError{"store_error", "could not protect password"});
    passwords_.push_back(
        PasswordCredential{users_.back().id, pool, input.email, password_hash, ""});
    Device& device = add_device(devices_, users_.back().id, input.device_name);
    IssuedSession issued =
        add_session(sessions_, users_.back().id, pool, device.id, "browser", "", {"*"}, "", 86400);
    if (issued.secret.empty())
        return std::unexpected(StoreError{"store_error", "could not issue session"});
    issued.session.reason.clear();
    return issued;
}

Result<IssuedSession> InMemoryStore::sign_in_password(const PasswordSignIn& input) {
    std::lock_guard lock(mutex_);
    const std::string pool = input.pool_id.empty() ? "lake" : input.pool_id;
    for (const auto& credential : passwords_) {
        if (credential.pool_id != pool || lower(credential.email) != lower(input.email))
            continue;
        if (!identity_crypto::verify_secret(credential.password, input.password))
            return std::unexpected(StoreError{"unauthorized", "invalid email or password"});
        if (!credential.totp_secret.empty())
            return std::unexpected(StoreError{"mfa_required", "TOTP verification is required"});
        Device& device = add_device(devices_, credential.user_id, input.device_name);
        IssuedSession issued = add_session(
            sessions_, credential.user_id, pool, device.id, "browser", "", {"*"}, "", 86400);
        if (issued.secret.empty())
            return std::unexpected(StoreError{"store_error", "could not issue session"});
        issued.session.reason.clear();
        return issued;
    }
    return std::unexpected(StoreError{"unauthorized", "invalid email or password"});
}

Result<MfaEnrollment> InMemoryStore::enroll_mfa(const std::string& user_id,
                                                const std::string& pool_id) {
    MfaEnrollment enrollment;
    enrollment.secret = identity_crypto::new_totp_secret();
    if (enrollment.secret.empty()) {
        return std::unexpected(StoreError{"store_error", "could not create TOTP secret"});
    }
    for (int index = 0; index < 8; ++index) {
        const std::string code = identity_crypto::random_secret().substr(0, 12);
        if (code.size() != 12) {
            return std::unexpected(
                StoreError{"store_error", "could not create MFA recovery codes"});
        }
        enrollment.recovery_codes.push_back(code);
    }
    std::lock_guard lock(mutex_);
    for (auto& credential : passwords_) {
        if (credential.user_id == user_id && credential.pool_id == pool_id) {
            credential.totp_secret = enrollment.secret;
            credential.recovery_hashes.clear();
            for (const std::string& code : enrollment.recovery_codes) {
                const std::string hash = identity_crypto::hash_secret(code);
                if (hash.empty()) {
                    credential.totp_secret.clear();
                    credential.recovery_hashes.clear();
                    return std::unexpected(
                        StoreError{"store_error", "could not protect MFA recovery codes"});
                }
                credential.recovery_hashes.push_back(hash);
            }
            return enrollment;
        }
    }
    return std::unexpected(StoreError{"not_found", "password credential not found"});
}

Result<void> InMemoryStore::disable_mfa(const std::string& user_id,
                                        const std::string& pool_id,
                                        const std::string& code) {
    std::lock_guard lock(mutex_);
    for (auto& credential : passwords_) {
        if (credential.user_id != user_id || credential.pool_id != pool_id ||
            credential.totp_secret.empty()) {
            continue;
        }
        const bool valid_totp = identity_crypto::verify_totp(credential.totp_secret, code);
        const auto recovery = std::find_if(
            credential.recovery_hashes.begin(),
            credential.recovery_hashes.end(),
            [&](const std::string& hash) { return identity_crypto::verify_secret(hash, code); });
        if (valid_totp || recovery != credential.recovery_hashes.end()) {
            credential.totp_secret.clear();
            credential.recovery_hashes.clear();
            return {};
        }
    }
    return std::unexpected(StoreError{"unauthorized", "a fresh MFA verification is required"});
}

Result<IssuedSession> InMemoryStore::sign_in_totp(const PasswordSignIn& input) {
    std::lock_guard lock(mutex_);
    const std::string pool = input.pool_id.empty() ? "lake" : input.pool_id;
    for (auto& credential : passwords_) {
        if (credential.pool_id != pool || lower(credential.email) != lower(input.email))
            continue;
        const bool valid_totp =
            identity_crypto::verify_totp(credential.totp_secret, input.totp_code);
        auto recovery =
            std::find_if(credential.recovery_hashes.begin(),
                         credential.recovery_hashes.end(),
                         [&](const std::string& hash) {
                             return identity_crypto::verify_secret(hash, input.totp_code);
                         });
        if (!identity_crypto::verify_secret(credential.password, input.password) ||
            (!valid_totp && recovery == credential.recovery_hashes.end())) {
            return std::unexpected(StoreError{"unauthorized", "invalid credentials or TOTP code"});
        }
        if (!valid_totp) {
            credential.recovery_hashes.erase(recovery);
        }
        Device& device = add_device(devices_, credential.user_id, input.device_name);
        IssuedSession issued = add_session(
            sessions_, credential.user_id, pool, device.id, "browser", "", {"*"}, "", 86400);
        if (issued.secret.empty())
            return std::unexpected(StoreError{"store_error", "could not issue session"});
        issued.session.reason.clear();
        return issued;
    }
    return std::unexpected(StoreError{"unauthorized", "invalid credentials or TOTP code"});
}

Result<WebAuthnChallenge> InMemoryStore::begin_passkey_registration(const std::string& user_id,
                                                                    const std::string& pool_id,
                                                                    const std::string& rp_id,
                                                                    const std::string& origin) {
    if (user_id.empty() || pool_id.empty() || rp_id.empty() || origin.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "WebAuthn registration scope is incomplete"});
    }
    std::lock_guard lock(mutex_);
    if (std::none_of(
            users_.begin(), users_.end(), [&](const User& user) { return user.id == user_id; })) {
        return std::unexpected(StoreError{"not_found", "user not found"});
    }
    WebAuthnChallenge challenge{
        user_id, pool_id, "registration", identity_crypto::random_secret(), rp_id, origin};
    if (challenge.challenge.empty()) {
        return std::unexpected(StoreError{"store_error", "could not generate WebAuthn challenge"});
    }
    webauthn_challenges_.push_back(
        {challenge, std::chrono::steady_clock::now() + std::chrono::minutes(5), false});
    return challenge;
}

Result<void> InMemoryStore::register_passkey(const PasskeyRegistration& input) {
    identity_crypto::VerifiedPasskeyRegistration verified;
    std::string subject_taap_id;
    {
        std::lock_guard lock(mutex_);
        WebAuthnChallengeRecord* challenge = nullptr;
        for (WebAuthnChallengeRecord& candidate : webauthn_challenges_) {
            if (candidate.challenge.challenge == input.challenge &&
                candidate.challenge.ceremony == "registration" &&
                candidate.challenge.user_id == input.user_id &&
                candidate.challenge.pool_id == input.pool_id && !candidate.used &&
                candidate.expires_at > std::chrono::steady_clock::now()) {
                candidate.used = true;
                challenge = &candidate;
                break;
            }
        }
        if (challenge == nullptr) {
            return std::unexpected(
                StoreError{"unauthorized", "WebAuthn challenge is invalid or expired"});
        }
        const auto registration =
            identity_crypto::verify_passkey_registration(input.client_data_json,
                                                         input.attestation_object,
                                                         input.challenge,
                                                         challenge->challenge.rp_id,
                                                         challenge->challenge.origin);
        if (!registration) {
            return std::unexpected(StoreError{"unauthorized", registration.error()});
        }
        verified = *registration;
        for (const auto& credential : passkeys_) {
            if (credential.pool_id == input.pool_id &&
                credential.credential_id == verified.credential_id) {
                return std::unexpected(StoreError{"conflict", "passkey already registered"});
            }
        }
        const auto user = std::find_if(users_.begin(), users_.end(), [&](const User& candidate) {
            return candidate.id == input.user_id;
        });
        if (user == users_.end() || user->taap_id.empty()) {
            return std::unexpected(StoreError{"not_found", "user TAAP identity not found"});
        }
        subject_taap_id = user->taap_id;
    }
    const auto binding = audit::sign_passkey_binding(
        *this, subject_taap_id, verified.credential_id, input.pool_id, input.identity_options);
    if (!binding) {
        return std::unexpected(StoreError{binding.error().code, binding.error().message});
    }
    std::lock_guard lock(mutex_);
    for (const auto& credential : passkeys_) {
        if (credential.pool_id == input.pool_id &&
            credential.credential_id == verified.credential_id) {
            return std::unexpected(StoreError{"conflict", "passkey already registered"});
        }
    }
    Device& device = add_device(devices_, input.user_id, input.name);
    passkeys_.push_back(PasskeyCredential{input.user_id,
                                          input.pool_id,
                                          verified.credential_id,
                                          verified.public_key_pem,
                                          input.name,
                                          device.id,
                                          "",
                                          *binding,
                                          verified.sign_count});
    return {};
}

Result<WebAuthnChallenge> InMemoryStore::begin_passkey_sign_in(const std::string& pool_id,
                                                               const std::string& rp_id,
                                                               const std::string& origin) {
    if (pool_id.empty() || rp_id.empty() || origin.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "WebAuthn assertion scope is incomplete"});
    }
    std::lock_guard lock(mutex_);
    WebAuthnChallenge challenge{
        "", pool_id, "assertion", identity_crypto::random_secret(), rp_id, origin};
    if (challenge.challenge.empty()) {
        return std::unexpected(StoreError{"store_error", "could not generate WebAuthn challenge"});
    }
    webauthn_challenges_.push_back(
        {challenge, std::chrono::steady_clock::now() + std::chrono::minutes(5), false});
    return challenge;
}

Result<IssuedSession> InMemoryStore::sign_in_passkey(const PasskeyAssertion& input) {
    std::lock_guard lock(mutex_);
    WebAuthnChallengeRecord* challenge = nullptr;
    for (WebAuthnChallengeRecord& candidate : webauthn_challenges_) {
        if (candidate.challenge.challenge == input.challenge &&
            candidate.challenge.ceremony == "assertion" &&
            candidate.challenge.pool_id == input.pool_id && !candidate.used &&
            candidate.expires_at > std::chrono::steady_clock::now()) {
            candidate.used = true;
            challenge = &candidate;
            break;
        }
    }
    if (challenge == nullptr) {
        return std::unexpected(
            StoreError{"unauthorized", "WebAuthn challenge is invalid or expired"});
    }
    for (PasskeyCredential& credential : passkeys_) {
        if (credential.pool_id != input.pool_id ||
            credential.credential_id != input.credential_id || !credential.revoked_at.empty()) {
            continue;
        }
        const auto sign_count =
            identity_crypto::verify_passkey_assertion(input.client_data_json,
                                                      input.authenticator_data,
                                                      input.signature,
                                                      credential.public_key_pem,
                                                      input.challenge,
                                                      challenge->challenge.rp_id,
                                                      challenge->challenge.origin);
        if (!sign_count) {
            return std::unexpected(StoreError{"unauthorized", sign_count.error()});
        }
        if ((*sign_count != 0 || credential.sign_count != 0) &&
            *sign_count <= credential.sign_count) {
            return std::unexpected(
                StoreError{"unauthorized", "passkey sign count did not advance"});
        }
        credential.sign_count = *sign_count;
        Device& device = add_device(devices_, credential.user_id, input.device_name);
        IssuedSession issued = add_session(sessions_,
                                           credential.user_id,
                                           input.pool_id,
                                           device.id,
                                           "browser",
                                           "",
                                           {"*"},
                                           "",
                                           86400);
        if (issued.secret.empty())
            return std::unexpected(StoreError{"store_error", "could not issue session"});
        issued.session.reason.clear();
        return issued;
    }
    return std::unexpected(StoreError{"unauthorized", "unknown passkey"});
}

Result<IssuedSession> InMemoryStore::sign_in_oauth(const OAuthIdentity& input) {
    if (input.provider.empty() || input.subject.empty())
        return std::unexpected(StoreError{"invalid_input", "provider and subject are required"});
    std::lock_guard lock(mutex_);
    const std::string pool = input.pool_id.empty() ? "lake" : input.pool_id;
    const std::string subject = input.provider + ":" + input.subject;
    for (const auto& credential : oauth_credentials_) {
        if (credential.pool_id == pool && credential.subject == subject) {
            Device& device = add_device(devices_, credential.user_id, input.device_name);
            IssuedSession issued = add_session(
                sessions_, credential.user_id, pool, device.id, "browser", "", {"*"}, "", 86400);
            if (issued.secret.empty())
                return std::unexpected(StoreError{"store_error", "could not issue session"});
            issued.session.reason.clear();
            return issued;
        }
    }
    if (input.identity_document.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "a TAAP identity document is required for sign-up"});
    }
    const std::string now = now_iso8601();
    const std::string identity = "taap:1:ed25519:" + identity_crypto::random_secret();
    users_.push_back(User{new_uuid_v4(), identity, input.email, input.display_name, now, now});
    oauth_credentials_.push_back(OAuthCredential{users_.back().id, pool, subject});
    Device& device = add_device(devices_, users_.back().id, input.device_name);
    IssuedSession issued =
        add_session(sessions_, users_.back().id, pool, device.id, "browser", "", {"*"}, "", 86400);
    if (issued.secret.empty())
        return std::unexpected(StoreError{"store_error", "could not issue session"});
    issued.session.reason.clear();
    return issued;
}

Result<void> InMemoryStore::register_oauth_provider(const OAuthProvider& input) {
    if (input.pool_id.empty() || input.provider.empty() || input.issuer.empty() ||
        input.client_id.empty() || input.client_secret.empty()) {
        return std::unexpected(StoreError{
            "invalid_input", "pool, provider, issuer, client id, and client secret are required"});
    }
    std::lock_guard lock(mutex_);
    for (OAuthProvider& provider : oauth_providers_) {
        if (provider.pool_id == input.pool_id && provider.provider == input.provider) {
            provider = input;
            return {};
        }
    }
    oauth_providers_.push_back(input);
    return {};
}

Result<OAuthProvider> InMemoryStore::get_oauth_provider(const std::string& pool_id,
                                                        const std::string& provider) {
    std::lock_guard lock(mutex_);
    for (const OAuthProvider& candidate : oauth_providers_) {
        if (candidate.pool_id == pool_id && candidate.provider == provider) {
            return candidate;
        }
    }
    return std::unexpected(StoreError{"not_found", "OAuth provider is not registered"});
}

Result<void> InMemoryStore::begin_oauth(const OAuthFlow& input) {
    if (input.pool_id.empty() || input.provider.empty() || input.state.empty() ||
        input.code_verifier.empty() || input.nonce.empty()) {
        return std::unexpected(StoreError{"invalid_input", "OAuth flow material is incomplete"});
    }
    std::lock_guard lock(mutex_);
    const bool provider_exists = std::any_of(
        oauth_providers_.begin(), oauth_providers_.end(), [&](const OAuthProvider& provider) {
            return provider.pool_id == input.pool_id && provider.provider == input.provider;
        });
    const bool sso_provider_exists =
        input.provider.starts_with("sso-") &&
        std::ranges::any_of(sso_providers_, [&](const SsoProvider& provider) {
            return provider.id == input.provider.substr(std::string("sso-").size()) &&
                   provider.pool_id == input.pool_id && provider.protocol == "oidc" &&
                   provider.enabled;
        });
    if (!provider_exists && !sso_provider_exists) {
        return std::unexpected(StoreError{"not_found", "OAuth provider is not registered"});
    }
    oauth_flows_.push_back(
        {input, std::chrono::steady_clock::now() + std::chrono::minutes(5), false});
    return {};
}

Result<OAuthFlow> InMemoryStore::consume_oauth(const std::string& provider,
                                               const std::string& state) {
    std::lock_guard lock(mutex_);
    for (OAuthFlowRecord& record : oauth_flows_) {
        if (record.flow.provider == provider && record.flow.state == state && !record.used &&
            record.expires_at > std::chrono::steady_clock::now()) {
            record.used = true;
            return record.flow;
        }
    }
    return std::unexpected(StoreError{"unauthorized", "OAuth state is invalid or expired"});
}

Result<IssuedSession> InMemoryStore::create_token(const NewToken& input) {
    if (input.name.empty())
        return std::unexpected(StoreError{"invalid_input", "name is required"});
    if (input.is_static && input.reason.empty())
        return std::unexpected(StoreError{"invalid_input", "a static token requires a reason"});
    std::lock_guard lock(mutex_);
    const int lifetime = input.is_static ? 0
                                         : (input.expires_in_seconds > 0 ? input.expires_in_seconds
                                                                         : 60 * 60 * 24 * 30);
    IssuedSession issued = add_session(sessions_,
                                       input.user_id,
                                       input.pool_id,
                                       "",
                                       input.is_static ? "static" : "api_token",
                                       input.name,
                                       input.scopes,
                                       input.reason,
                                       lifetime);
    if (issued.secret.empty())
        return std::unexpected(StoreError{"store_error", "could not issue token"});
    issued.session.reason = input.reason;
    return issued;
}

Result<std::vector<Session>> InMemoryStore::list_sessions(const std::string& user_id,
                                                          const std::string& pool_id) {
    std::lock_guard lock(mutex_);
    std::vector<Session> result;
    for (auto it = sessions_.rbegin(); it != sessions_.rend(); ++it) {
        if (it->user_id == user_id && it->pool_id == pool_id) {
            Session copy = *it;
            copy.reason = session_reason(copy);
            result.push_back(std::move(copy));
        }
    }
    return result;
}

Result<void> InMemoryStore::revoke_session(const std::string& user_id,
                                           const std::string& session_id) {
    std::lock_guard lock(mutex_);
    for (auto& session : sessions_)
        if (session.id == session_id && session.user_id == user_id) {
            session.revoked_at = now_iso8601();
            return {};
        }
    return std::unexpected(StoreError{"not_found", "session not found"});
}

Result<std::vector<Device>> InMemoryStore::list_devices(const std::string& user_id) {
    std::lock_guard lock(mutex_);
    std::vector<Device> result;
    for (auto it = devices_.rbegin(); it != devices_.rend(); ++it) {
        if (it->user_id == user_id) {
            result.push_back(*it);
            const auto passkey =
                std::find_if(passkeys_.begin(), passkeys_.end(), [&](const auto& credential) {
                    return credential.device_id == it->id;
                });
            if (passkey != passkeys_.end()) {
                result.back().passkey_credential_id = passkey->credential_id;
                result.back().passkey_binding_attestation = passkey->binding_attestation;
            }
        }
    }
    return result;
}

Result<void> InMemoryStore::revoke_device(const std::string& user_id,
                                          const std::string& device_id) {
    std::lock_guard lock(mutex_);
    for (auto& device : devices_) {
        if (device.id == device_id && device.user_id == user_id) {
            device.revoked_at = now_iso8601();
            device.updated_at = device.revoked_at;
            for (PasskeyCredential& passkey : passkeys_) {
                if (passkey.device_id == device_id && passkey.revoked_at.empty()) {
                    passkey.revoked_at = device.revoked_at;
                }
            }
            return {};
        }
    }
    return std::unexpected(StoreError{"not_found", "device not found"});
}

Result<Principal> InMemoryStore::context_for(const std::string& secret) {
    std::lock_guard lock(mutex_);
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    for (auto& session : sessions_) {
        if (!session.revoked_at.empty() ||
            !identity_crypto::verify_secret(session_hash(session), secret))
            continue;
        if (!session.expires_at.empty() && std::stoll(session.expires_at) < now)
            continue;
        for (const auto& device : devices_)
            if (device.id == session.device_id && !device.revoked_at.empty())
                return std::unexpected(StoreError{"unauthorized", "device revoked"});
        const auto user = std::ranges::find_if(
            users_, [&](const User& row) { return row.id == session.user_id; });
        if (user == users_.end() || user->taap_id.empty())
            return std::unexpected(StoreError{"unauthorized", "session identity is unavailable"});
        session.last_used_at = now_iso8601();
        return Principal{session.user_id,
                         user->taap_id,
                         session.id,
                         session.device_id,
                         session.pool_id,
                         session.kind,
                         session.scopes};
    }
    return std::unexpected(StoreError{"unauthorized", "invalid, expired, or revoked credential"});
}

Result<SsoProvider> InMemoryStore::register_sso_provider(const NewSsoProvider& input) {
    if (input.organization_id.empty() || input.name.empty() || input.issuer.empty() ||
        input.scim_token.empty() || (input.protocol != "saml" && input.protocol != "oidc")) {
        return std::unexpected(StoreError{"invalid_input", "provider configuration is incomplete"});
    }
    if ((input.protocol == "saml" &&
         (input.metadata.empty() || input.signing_certificate.empty())) ||
        (input.protocol == "oidc" && (input.client_id.empty() || input.client_secret.empty()))) {
        return std::unexpected(
            StoreError{"invalid_input", "provider protocol configuration is incomplete"});
    }
    std::lock_guard lock(mutex_);
    if (std::none_of(
            organizations_.begin(), organizations_.end(), [&](const Organization& organization) {
                return organization.id == input.organization_id;
            })) {
        return std::unexpected(StoreError{"not_found", "organization does not exist"});
    }
    const std::string now = now_iso8601();
    for (SsoProvider& provider : sso_providers_) {
        if (provider.organization_id == input.organization_id && provider.name == input.name) {
            provider.pool_id = input.pool_id.empty() ? "lake" : input.pool_id;
            provider.protocol = input.protocol;
            provider.issuer = input.issuer;
            provider.metadata = input.metadata;
            provider.signing_certificate = input.signing_certificate;
            provider.client_id = input.client_id;
            provider.attribute_mapping = input.attribute_mapping;
            provider.jit_provisioning = input.jit_provisioning;
            provider.enabled = true;
            provider.updated_at = now;
            sso_scim_tokens_.erase(
                std::remove_if(sso_scim_tokens_.begin(),
                               sso_scim_tokens_.end(),
                               [&](const auto& entry) { return entry.first == provider.id; }),
                sso_scim_tokens_.end());
            const std::string token_hash = identity_crypto::hash_secret(input.scim_token);
            if (token_hash.empty()) {
                return std::unexpected(StoreError{"store_error", "could not hash SCIM token"});
            }
            sso_scim_tokens_.emplace_back(provider.id, token_hash);
            sso_oidc_secrets_.erase(
                std::remove_if(sso_oidc_secrets_.begin(),
                               sso_oidc_secrets_.end(),
                               [&](const auto& entry) { return entry.first == provider.id; }),
                sso_oidc_secrets_.end());
            if (input.protocol == "oidc") {
                sso_oidc_secrets_.emplace_back(provider.id, input.client_secret);
            }
            return provider;
        }
    }
    SsoProvider provider{new_uuid_v4(),
                         input.organization_id,
                         input.pool_id.empty() ? "lake" : input.pool_id,
                         input.name,
                         input.protocol,
                         input.issuer,
                         input.metadata,
                         input.signing_certificate,
                         input.client_id,
                         input.attribute_mapping,
                         input.jit_provisioning,
                         true,
                         "",
                         now,
                         now};
    const std::string token_hash = identity_crypto::hash_secret(input.scim_token);
    if (token_hash.empty()) {
        return std::unexpected(StoreError{"store_error", "could not hash SCIM token"});
    }
    sso_scim_tokens_.emplace_back(provider.id, token_hash);
    if (input.protocol == "oidc") {
        sso_oidc_secrets_.emplace_back(provider.id, input.client_secret);
    }
    sso_providers_.push_back(provider);
    return provider;
}

Result<std::vector<SsoProvider>>
InMemoryStore::list_sso_providers(const std::string& organization_id, int limit) {
    std::lock_guard lock(mutex_);
    std::vector<SsoProvider> result;
    for (auto iterator = sso_providers_.rbegin(); iterator != sso_providers_.rend(); ++iterator) {
        if (organization_id.empty() || iterator->organization_id == organization_id) {
            result.push_back(*iterator);
        }
        if (limit > 0 && result.size() >= static_cast<std::size_t>(limit)) {
            break;
        }
    }
    return result;
}

Result<SsoProvider> InMemoryStore::get_sso_provider(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto provider = std::find_if(sso_providers_.begin(),
                                       sso_providers_.end(),
                                       [&](const SsoProvider& item) { return item.id == id; });
    if (provider == sso_providers_.end()) {
        return std::unexpected(StoreError{"not_found", "SSO provider does not exist"});
    }
    return *provider;
}

Result<OAuthProvider> InMemoryStore::get_sso_oidc_provider(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto provider =
        std::find_if(sso_providers_.begin(), sso_providers_.end(), [&](const SsoProvider& item) {
            return item.id == id && item.protocol == "oidc" && item.enabled;
        });
    const auto secret = std::find_if(sso_oidc_secrets_.begin(),
                                     sso_oidc_secrets_.end(),
                                     [&](const auto& item) { return item.first == id; });
    if (provider == sso_providers_.end() || secret == sso_oidc_secrets_.end()) {
        return std::unexpected(StoreError{"not_found", "OIDC provider is unavailable"});
    }
    return OAuthProvider{provider->pool_id,
                         "sso-" + provider->id,
                         provider->issuer,
                         provider->client_id,
                         secret->second,
                         {"openid", "email", "profile"}};
}

Result<SsoProvider> InMemoryStore::authenticate_scim(const std::string& bearer_token) {
    std::lock_guard lock(mutex_);
    for (const auto& [provider_id, token_hash] : sso_scim_tokens_) {
        if (!identity_crypto::verify_secret(token_hash, bearer_token)) {
            continue;
        }
        const auto provider = std::ranges::find_if(sso_providers_, [&](const SsoProvider& item) {
            return item.id == provider_id && item.enabled;
        });
        if (provider != sso_providers_.end()) {
            return *provider;
        }
    }
    return std::unexpected(StoreError{"unauthorized", "SCIM bearer token is invalid"});
}

Result<SsoProvider> InMemoryStore::test_sso_provider(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto provider = std::find_if(sso_providers_.begin(),
                                       sso_providers_.end(),
                                       [&](const SsoProvider& item) { return item.id == id; });
    if (provider == sso_providers_.end()) {
        return std::unexpected(StoreError{"not_found", "SSO provider does not exist"});
    }
    if ((provider->protocol == "saml" &&
         (provider->metadata.empty() || provider->signing_certificate.empty())) ||
        (provider->protocol == "oidc" && provider->client_id.empty())) {
        return std::unexpected(StoreError{"invalid_input", "SSO provider is not testable"});
    }
    provider->tested_at = now_iso8601();
    provider->updated_at = provider->tested_at;
    return *provider;
}

Result<FederationIssuer> InMemoryStore::create_federation_issuer(const NewFederationIssuer& input) {
    if (input.organization_id.empty() || input.issuer.empty() || input.claim.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "organization, issuer, and claim are required"});
    }
    std::lock_guard lock(mutex_);
    for (const FederationIssuer& item : federation_issuers_) {
        if (item.organization_id == input.organization_id && item.issuer == input.issuer &&
            item.claim == input.claim) {
            return std::unexpected(
                StoreError{"conflict", "federation issuer claim already exists"});
        }
    }
    FederationIssuer issuer{
        new_uuid_v4(), input.organization_id, input.issuer, input.claim, now_iso8601()};
    federation_issuers_.push_back(issuer);
    return issuer;
}

Result<std::vector<FederationIssuer>>
InMemoryStore::list_federation_issuers(const std::string& organization_id, int limit) {
    std::lock_guard lock(mutex_);
    std::vector<FederationIssuer> result;
    for (auto iterator = federation_issuers_.rbegin(); iterator != federation_issuers_.rend();
         ++iterator) {
        if (organization_id.empty() || iterator->organization_id == organization_id) {
            result.push_back(*iterator);
        }
        if (limit > 0 && result.size() >= static_cast<std::size_t>(limit)) {
            break;
        }
    }
    return result;
}

Result<bool> InMemoryStore::allows_federation_claim(const std::string& organization_id,
                                                    const std::string& issuer,
                                                    const std::string& claim) {
    std::lock_guard lock(mutex_);
    return std::ranges::any_of(federation_issuers_, [&](const FederationIssuer& value) {
        return value.organization_id == organization_id && value.issuer == issuer &&
               value.claim == claim;
    });
}

Result<IssuedSession> InMemoryStore::sign_in_sso(const SsoIdentity& input) {
    if (input.provider_id.empty() || input.issuer.empty() || input.subject.empty() ||
        !input.claims.is_object()) {
        return std::unexpected(StoreError{"invalid_input", "SSO assertion is incomplete"});
    }
    std::lock_guard lock(mutex_);
    const auto provider =
        std::find_if(sso_providers_.begin(), sso_providers_.end(), [&](const SsoProvider& item) {
            return item.id == input.provider_id;
        });
    if (provider == sso_providers_.end() || !provider->enabled ||
        provider->issuer != input.issuer) {
        return std::unexpected(StoreError{"unauthorized", "SSO issuer is not accepted"});
    }
    for (auto claim = input.claims.begin(); claim != input.claims.end(); ++claim) {
        const bool allowed =
            std::any_of(federation_issuers_.begin(),
                        federation_issuers_.end(),
                        [&](const FederationIssuer& rule) {
                            return rule.organization_id == provider->organization_id &&
                                   rule.issuer == input.issuer && rule.claim == claim.key();
                        });
        if (!allowed) {
            return std::unexpected(
                StoreError{"unauthorized", "issuer is not allowed to assert " + claim.key()});
        }
    }
    auto scim = std::find_if(scim_users_.begin(), scim_users_.end(), [&](const ScimUser& user) {
        return user.provider_id == input.provider_id && user.external_id == input.subject;
    });
    std::string user_id;
    if (scim != scim_users_.end()) {
        if (!scim->active) {
            return std::unexpected(StoreError{"unauthorized", "provisioned user is inactive"});
        }
        user_id = scim->user_id;
    } else {
        if (!provider->jit_provisioning || input.taap_id.empty()) {
            return std::unexpected(StoreError{"unauthorized", "SSO user is not provisioned"});
        }
        const std::string now = now_iso8601();
        users_.push_back(
            User{new_uuid_v4(), input.taap_id, input.email, input.display_name, now, now});
        user_id = users_.back().id;
        scim_users_.push_back(
            ScimUser{input.provider_id, input.subject, user_id, true, input.claims, now, now});
    }
    const auto user = std::find_if(
        users_.begin(), users_.end(), [&](const User& item) { return item.id == user_id; });
    if (user == users_.end() || user->taap_id.empty()) {
        return std::unexpected(StoreError{"unauthorized", "SSO user has no TAAP identity"});
    }
    const auto credential = std::find_if(
        sso_credentials_.begin(), sso_credentials_.end(), [&](const SsoCredential& item) {
            return item.provider_id == input.provider_id && item.subject == input.subject;
        });
    if (credential == sso_credentials_.end()) {
        sso_credentials_.push_back(SsoCredential{input.provider_id, input.subject, user_id});
    }
    Device& device = add_device(devices_, user_id, input.device_name);
    return add_session(
        sessions_, user_id, provider->pool_id, device.id, "browser", "", {"*"}, "", 86400);
}

Result<std::vector<ScimUser>> InMemoryStore::list_scim_users(const std::string& provider_id,
                                                             int limit) {
    std::lock_guard lock(mutex_);
    std::vector<ScimUser> result;
    for (const ScimUser& user : scim_users_) {
        if (user.provider_id == provider_id) {
            result.push_back(user);
        }
        if (limit > 0 && result.size() >= static_cast<std::size_t>(limit)) {
            break;
        }
    }
    return result;
}

Result<ScimUser> InMemoryStore::get_scim_user(const std::string& provider_id,
                                              const std::string& resource_id) {
    std::lock_guard lock(mutex_);
    const auto user = std::ranges::find_if(scim_users_, [&](const ScimUser& item) {
        return item.provider_id == provider_id &&
               (item.user_id == resource_id || item.external_id == resource_id);
    });
    if (user == scim_users_.end()) {
        return std::unexpected(StoreError{"not_found", "SCIM user does not exist"});
    }
    return *user;
}

void InMemoryStore::remove_scim_user_access(const ScimUser& user, const std::string& now) {
    for (Session& session : sessions_) {
        if (session.user_id == user.user_id && session.revoked_at.empty()) {
            session.revoked_at = now;
        }
    }
    std::vector<std::string> membership_ids;
    for (const ScimGroupMember& member : scim_group_members_) {
        if (member.provider_id == user.provider_id && member.user_external_id == user.external_id) {
            membership_ids.push_back(member.membership_id);
        }
    }
    for (const std::string& membership_id : membership_ids) {
        const auto membership = std::ranges::find_if(
            memberships_, [&](const Membership& item) { return item.id == membership_id; });
        if (membership == memberships_.end()) {
            continue;
        }
        const auto object = std::ranges::find_if(policy_objects_, [&](const PolicyObject& item) {
            return item.object_type == "team" && item.external_id == membership->team_id;
        });
        if (object != policy_objects_.end()) {
            grants_.erase(std::remove_if(grants_.begin(),
                                         grants_.end(),
                                         [&](const Grant& grant) {
                                             return grant.object_id == object->id &&
                                                    grant.principal_taap_id ==
                                                        membership->principal_taap_id &&
                                                    grant.attestation_digest ==
                                                        membership->attestation_digest;
                                         }),
                          grants_.end());
        }
        policy_objects_.erase(std::remove_if(policy_objects_.begin(),
                                             policy_objects_.end(),
                                             [&](const PolicyObject& item) {
                                                 return item.object_type == "member" &&
                                                        item.external_id == membership_id;
                                             }),
                              policy_objects_.end());
        memberships_.erase(membership);
    }
    scim_group_members_.erase(std::remove_if(scim_group_members_.begin(),
                                             scim_group_members_.end(),
                                             [&](const ScimGroupMember& member) {
                                                 return member.provider_id == user.provider_id &&
                                                        member.user_external_id == user.external_id;
                                             }),
                              scim_group_members_.end());
}

Result<ScimUser> InMemoryStore::upsert_scim_user(const NewScimUser& input) {
    if (input.provider_id.empty() || input.external_id.empty() || input.taap_id.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "provider, external id, and TAAP identity are required"});
    }
    std::lock_guard lock(mutex_);
    if (std::none_of(
            sso_providers_.begin(), sso_providers_.end(), [&](const SsoProvider& provider) {
                return provider.id == input.provider_id;
            })) {
        return std::unexpected(StoreError{"not_found", "SSO provider does not exist"});
    }
    const std::string now = now_iso8601();
    const auto record =
        std::find_if(scim_users_.begin(), scim_users_.end(), [&](const ScimUser& user) {
            return user.provider_id == input.provider_id && user.external_id == input.external_id;
        });
    if (record != scim_users_.end()) {
        const auto user = std::find_if(
            users_.begin(), users_.end(), [&](User& item) { return item.id == record->user_id; });
        if (user != users_.end()) {
            user->email = input.email;
            user->display_name = input.display_name;
            user->updated_at = now;
        }
        record->active = input.active;
        record->attributes = input.attributes;
        record->updated_at = now;
        if (!input.active) {
            remove_scim_user_access(*record, now);
        }
        provisioning_logs_.push_back(ProvisioningLog{new_uuid_v4(),
                                                     input.provider_id,
                                                     "user",
                                                     input.active ? "update" : "deactivate",
                                                     input.external_id,
                                                     "success",
                                                     {},
                                                     now});
        return *record;
    }
    users_.push_back(User{new_uuid_v4(), input.taap_id, input.email, input.display_name, now, now});
    ScimUser created{input.provider_id,
                     input.external_id,
                     users_.back().id,
                     input.active,
                     input.attributes,
                     now,
                     now};
    scim_users_.push_back(created);
    provisioning_logs_.push_back(ProvisioningLog{new_uuid_v4(),
                                                 input.provider_id,
                                                 "user",
                                                 input.active ? "create" : "deactivate",
                                                 input.external_id,
                                                 "success",
                                                 {},
                                                 now});
    return created;
}

Result<ScimUser> InMemoryStore::delete_scim_user(const std::string& provider_id,
                                                 const std::string& resource_id) {
    std::lock_guard lock(mutex_);
    const auto user = std::ranges::find_if(scim_users_, [&](const ScimUser& item) {
        return item.provider_id == provider_id &&
               (item.user_id == resource_id || item.external_id == resource_id);
    });
    if (user == scim_users_.end()) {
        return std::unexpected(StoreError{"not_found", "SCIM user does not exist"});
    }
    const std::string now = now_iso8601();
    user->active = false;
    user->updated_at = now;
    remove_scim_user_access(*user, now);
    provisioning_logs_.push_back(ProvisioningLog{
        new_uuid_v4(), provider_id, "user", "deactivate", user->external_id, "success", {}, now});
    return *user;
}

Result<std::vector<ScimGroup>> InMemoryStore::list_scim_groups(const std::string& provider_id,
                                                               int limit) {
    std::lock_guard lock(mutex_);
    std::vector<ScimGroup> result;
    for (const ScimGroup& group : scim_groups_) {
        if (group.provider_id == provider_id) {
            result.push_back(group);
        }
        if (limit > 0 && result.size() >= static_cast<std::size_t>(limit)) {
            break;
        }
    }
    return result;
}

Result<ScimGroup> InMemoryStore::get_scim_group(const std::string& provider_id,
                                                const std::string& resource_id) {
    std::lock_guard lock(mutex_);
    const auto group = std::ranges::find_if(scim_groups_, [&](const ScimGroup& item) {
        return item.provider_id == provider_id &&
               (item.team_id == resource_id || item.external_id == resource_id);
    });
    if (group == scim_groups_.end()) {
        return std::unexpected(StoreError{"not_found", "SCIM group does not exist"});
    }
    return *group;
}

Result<ScimGroup> InMemoryStore::upsert_scim_group(const NewScimGroup& input) {
    if (input.provider_id.empty() || input.external_id.empty() || input.team_name.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "provider, external id, and team name are required"});
    }
    std::lock_guard lock(mutex_);
    const auto provider =
        std::find_if(sso_providers_.begin(), sso_providers_.end(), [&](const SsoProvider& item) {
            return item.id == input.provider_id;
        });
    if (provider == sso_providers_.end()) {
        return std::unexpected(StoreError{"not_found", "SSO provider does not exist"});
    }
    const std::string now = now_iso8601();
    auto group = std::find_if(scim_groups_.begin(), scim_groups_.end(), [&](const ScimGroup& item) {
        return item.provider_id == input.provider_id && item.external_id == input.external_id;
    });
    const bool creating = group == scim_groups_.end();
    if (group == scim_groups_.end()) {
        if (!valid_handle(input.team_name)) {
            return std::unexpected(StoreError{"invalid_input", "team name is invalid"});
        }
        teams_.push_back(Team{new_uuid_v4(),
                              provider->organization_id,
                              input.team_name,
                              input.display_name,
                              now,
                              now});
        const auto parent = std::find_if(
            policy_objects_.begin(), policy_objects_.end(), [&](const PolicyObject& item) {
                return item.object_type == "organization" &&
                       item.external_id == provider->organization_id;
            });
        if (parent == policy_objects_.end()) {
            return std::unexpected(
                StoreError{"store_error", "organization policy object is missing"});
        }
        policy_objects_.push_back(
            PolicyObject{new_uuid_v4(), "team", teams_.back().id, parent->id, now, now, true});
        scim_groups_.push_back(ScimGroup{input.provider_id,
                                         input.external_id,
                                         teams_.back().id,
                                         input.display_name,
                                         input.attributes,
                                         now,
                                         now});
        group = std::prev(scim_groups_.end());
    } else {
        group->display_name = input.display_name;
        group->attributes = input.attributes;
        group->updated_at = now;
    }
    const auto team_object =
        std::find_if(policy_objects_.begin(), policy_objects_.end(), [&](const PolicyObject& item) {
            return item.object_type == "team" && item.external_id == group->team_id;
        });
    if (team_object == policy_objects_.end()) {
        return std::unexpected(StoreError{"store_error", "team policy object is missing"});
    }
    std::vector<std::string> removed_membership_ids;
    for (const ScimGroupMember& member : scim_group_members_) {
        const bool belongs_to_group = member.provider_id == input.provider_id &&
                                      member.group_external_id == input.external_id;
        const bool is_still_member =
            std::ranges::find(input.member_external_ids, member.user_external_id) !=
            input.member_external_ids.end();
        if (belongs_to_group && !is_still_member) {
            removed_membership_ids.push_back(member.membership_id);
        }
    }
    for (const std::string& membership_id : removed_membership_ids) {
        const auto membership =
            std::find_if(memberships_.begin(), memberships_.end(), [&](const Membership& item) {
                return item.id == membership_id;
            });
        if (membership == memberships_.end()) {
            continue;
        }
        grants_.erase(std::remove_if(grants_.begin(),
                                     grants_.end(),
                                     [&](const Grant& grant) {
                                         return grant.object_id == team_object->id &&
                                                grant.principal_taap_id ==
                                                    membership->principal_taap_id;
                                     }),
                      grants_.end());
        policy_objects_.erase(std::remove_if(policy_objects_.begin(),
                                             policy_objects_.end(),
                                             [&](const PolicyObject& item) {
                                                 return item.object_type == "member" &&
                                                        item.external_id == membership_id;
                                             }),
                              policy_objects_.end());
        memberships_.erase(membership);
    }
    scim_group_members_.erase(std::remove_if(scim_group_members_.begin(),
                                             scim_group_members_.end(),
                                             [&](const ScimGroupMember& member) {
                                                 return std::ranges::find(removed_membership_ids,
                                                                          member.membership_id) !=
                                                        removed_membership_ids.end();
                                             }),
                              scim_group_members_.end());
    for (const std::string& external_id : input.member_external_ids) {
        const auto user =
            std::find_if(scim_users_.begin(), scim_users_.end(), [&](const ScimUser& item) {
                return item.provider_id == input.provider_id && item.external_id == external_id &&
                       item.active;
            });
        if (user == scim_users_.end()) {
            return std::unexpected(StoreError{"invalid_input", "SCIM group member is not active"});
        }
        const bool mapped = std::any_of(scim_group_members_.begin(),
                                        scim_group_members_.end(),
                                        [&](const ScimGroupMember& member) {
                                            return member.provider_id == input.provider_id &&
                                                   member.group_external_id == input.external_id &&
                                                   member.user_external_id == external_id;
                                        });
        if (mapped) {
            continue;
        }
        const auto lake_user = std::find_if(users_.begin(), users_.end(), [&](const User& item) {
            return item.id == user->user_id;
        });
        if (lake_user == users_.end() || lake_user->taap_id.empty()) {
            return std::unexpected(StoreError{"invalid_input", "SCIM user has no TAAP identity"});
        }
        Membership membership{new_uuid_v4(),
                              provider->organization_id,
                              group->team_id,
                              "user",
                              lake_user->id,
                              lake_user->taap_id,
                              "member",
                              "scim:" + input.provider_id + ":" + input.external_id,
                              provider->issuer,
                              now,
                              now};
        memberships_.push_back(membership);
        policy_objects_.push_back(
            PolicyObject{new_uuid_v4(), "member", membership.id, team_object->id, now, now, true});
        grants_.push_back(Grant{new_uuid_v4(),
                                lake_user->taap_id,
                                team_object->id,
                                "read",
                                membership.attestation_digest,
                                membership.granted_by_taap_id,
                                now,
                                now});
        scim_group_members_.push_back(
            ScimGroupMember{input.provider_id, input.external_id, external_id, membership.id});
    }
    provisioning_logs_.push_back(ProvisioningLog{new_uuid_v4(),
                                                 input.provider_id,
                                                 "group",
                                                 creating ? "create" : "update",
                                                 input.external_id,
                                                 "success",
                                                 {},
                                                 now});
    return *group;
}

Result<void> InMemoryStore::delete_scim_group(const std::string& provider_id,
                                              const std::string& resource_id) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find_if(scim_groups_, [&](const ScimGroup& item) {
        return item.provider_id == provider_id &&
               (item.team_id == resource_id || item.external_id == resource_id);
    });
    if (found == scim_groups_.end()) {
        return std::unexpected(StoreError{"not_found", "SCIM group does not exist"});
    }
    const ScimGroup group = *found;
    const auto team_object = std::ranges::find_if(policy_objects_, [&](const PolicyObject& item) {
        return item.object_type == "team" && item.external_id == group.team_id;
    });
    if (team_object != policy_objects_.end()) {
        const std::string object_id = team_object->id;
        grants_.erase(std::remove_if(grants_.begin(),
                                     grants_.end(),
                                     [&](const Grant& grant) {
                                         return grant.object_id == object_id &&
                                                grant.attestation_digest ==
                                                    "scim:" + provider_id + ":" + group.external_id;
                                     }),
                      grants_.end());
        policy_objects_.erase(team_object);
    }
    std::vector<std::string> membership_ids;
    for (const ScimGroupMember& member : scim_group_members_) {
        if (member.provider_id == provider_id && member.group_external_id == group.external_id) {
            membership_ids.push_back(member.membership_id);
        }
    }
    policy_objects_.erase(std::remove_if(policy_objects_.begin(),
                                         policy_objects_.end(),
                                         [&](const PolicyObject& object) {
                                             return object.object_type == "member" &&
                                                    std::ranges::find(membership_ids,
                                                                      object.external_id) !=
                                                        membership_ids.end();
                                         }),
                          policy_objects_.end());
    memberships_.erase(std::remove_if(memberships_.begin(),
                                      memberships_.end(),
                                      [&](const Membership& membership) {
                                          return std::ranges::find(membership_ids, membership.id) !=
                                                 membership_ids.end();
                                      }),
                       memberships_.end());
    scim_group_members_.erase(std::remove_if(scim_group_members_.begin(),
                                             scim_group_members_.end(),
                                             [&](const ScimGroupMember& member) {
                                                 return member.provider_id == provider_id &&
                                                        member.group_external_id ==
                                                            group.external_id;
                                             }),
                              scim_group_members_.end());
    teams_.erase(std::remove_if(teams_.begin(),
                                teams_.end(),
                                [&](const Team& team) { return team.id == group.team_id; }),
                 teams_.end());
    scim_groups_.erase(found);
    provisioning_logs_.push_back(ProvisioningLog{new_uuid_v4(),
                                                 provider_id,
                                                 "group",
                                                 "deactivate",
                                                 group.external_id,
                                                 "success",
                                                 {},
                                                 now_iso8601()});
    return {};
}

Result<std::vector<ProvisioningLog>>
InMemoryStore::list_sso_provisioning_logs(const std::string& provider_id, int limit) {
    std::lock_guard lock(mutex_);
    std::vector<ProvisioningLog> result;
    for (auto iterator = provisioning_logs_.rbegin(); iterator != provisioning_logs_.rend();
         ++iterator) {
        if (provider_id.empty() || iterator->provider_id == provider_id) {
            result.push_back(*iterator);
        }
        if (limit > 0 && result.size() >= static_cast<std::size_t>(limit)) {
            break;
        }
    }
    return result;
}

} // namespace lake::control

namespace lake::control {

Result<Repository> InMemoryStore::create_repository(const NewRepository& in) {
    std::lock_guard lock(mutex_);
    if (std::none_of(customers_.begin(), customers_.end(), [&](const Customer& c) {
            return c.id == in.customer_id;
        })) {
        return std::unexpected(
            StoreError{"invalid_input", "customer_id does not reference a customer"});
    }
    if (!valid_handle(in.name))
        return std::unexpected(
            StoreError{"invalid_input", "name must match repository_name_format"});
    const std::string visibility = in.visibility.empty() ? "private" : in.visibility;
    if (visibility != "private" && visibility != "organization" && visibility != "public") {
        return std::unexpected(StoreError{"invalid_input", "visibility must be private or public"});
    }
    for (const auto& r : repositories_) {
        if (r.customer_id == in.customer_id && r.name == in.name) {
            return std::unexpected(
                StoreError{"conflict", "this customer already has a repository named " + in.name});
        }
    }
    const std::string now = now_iso8601();
    repositories_.push_back(Repository{
        new_uuid_v4(), in.customer_id, in.name, in.description, visibility, "", "", now, now});
    const auto parent = std::ranges::find_if(policy_objects_, [&](const PolicyObject& object) {
        return object.object_type == "customer" && object.external_id == in.customer_id;
    });
    if (parent == policy_objects_.end()) {
        repositories_.pop_back();
        return std::unexpected(StoreError{"store_error", "customer policy object is missing"});
    }
    policy_objects_.push_back(PolicyObject{
        new_uuid_v4(), "repository", repositories_.back().id, parent->id, now, now, true});
    return repositories_.back();
}

Result<std::vector<Repository>> InMemoryStore::list_repositories(const std::string& customer_id,
                                                                 int limit) {
    std::lock_guard lock(mutex_);
    std::vector<Repository> out;
    for (auto it = repositories_.rbegin(); it != repositories_.rend(); ++it) {
        if (customer_id.empty() || it->customer_id == customer_id)
            out.push_back(*it);
    }
    if (limit > 0 && out.size() > static_cast<std::size_t>(limit))
        out.resize(static_cast<std::size_t>(limit));
    return out;
}

Result<Repository> InMemoryStore::get_repository(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (const auto& r : repositories_)
        if (r.id == id)
            return r;
    return std::unexpected(StoreError{"not_found", "no repository " + id});
}

namespace {

std::vector<Repository>::iterator repository_by_id(std::vector<Repository>& repositories,
                                                   const std::string& id) {
    return std::find_if(repositories.begin(),
                        repositories.end(),
                        [&](const Repository& repository) { return repository.id == id; });
}

bool repository_name_taken(const std::vector<Repository>& repositories,
                           const std::string& customer_id,
                           const std::string& name,
                           const std::string& except_id) {
    return std::any_of(repositories.begin(), repositories.end(), [&](const Repository& repository) {
        return repository.id != except_id && repository.customer_id == customer_id &&
               repository.name == name;
    });
}

} // namespace

Result<Repository> InMemoryStore::rename_repository(const std::string& id,
                                                    const std::string& name) {
    std::lock_guard lock(mutex_);
    if (!valid_handle(name)) {
        return std::unexpected(
            StoreError{"invalid_input", "name must match repository_name_format"});
    }
    const auto repository = repository_by_id(repositories_, id);
    if (repository == repositories_.end()) {
        return std::unexpected(StoreError{"not_found", "no repository " + id});
    }
    if (!repository->deleted_at.empty()) {
        return std::unexpected(StoreError{"conflict", "repository is tombstoned"});
    }
    if (repository_name_taken(repositories_, repository->customer_id, name, id)) {
        return std::unexpected(
            StoreError{"conflict", "this customer already has a repository named " + name});
    }
    repository->name = name;
    repository->updated_at = now_iso8601();
    return *repository;
}

Result<Repository> InMemoryStore::transfer_repository(const std::string& id,
                                                      const std::string& customer_id) {
    std::lock_guard lock(mutex_);
    if (std::ranges::none_of(
            customers_, [&](const Customer& customer) { return customer.id == customer_id; })) {
        return std::unexpected(
            StoreError{"invalid_input", "customer_id does not reference a customer"});
    }
    const auto repository = repository_by_id(repositories_, id);
    if (repository == repositories_.end()) {
        return std::unexpected(StoreError{"not_found", "no repository " + id});
    }
    if (!repository->deleted_at.empty()) {
        return std::unexpected(StoreError{"conflict", "repository is tombstoned"});
    }
    if (repository_name_taken(repositories_, customer_id, repository->name, id)) {
        return std::unexpected(
            StoreError{"conflict", "destination customer already has that repository name"});
    }
    repository->customer_id = customer_id;
    repository->updated_at = now_iso8601();
    return *repository;
}

Result<Repository> InMemoryStore::set_repository_visibility(const std::string& id,
                                                            const std::string& visibility) {
    std::lock_guard lock(mutex_);
    if (visibility != "private" && visibility != "organization" && visibility != "public") {
        return std::unexpected(
            StoreError{"invalid_input", "visibility must be private, organization, or public"});
    }
    const auto repository = repository_by_id(repositories_, id);
    if (repository == repositories_.end()) {
        return std::unexpected(StoreError{"not_found", "no repository " + id});
    }
    if (!repository->deleted_at.empty()) {
        return std::unexpected(StoreError{"conflict", "repository is tombstoned"});
    }
    repository->visibility = visibility;
    repository->updated_at = now_iso8601();
    return *repository;
}

Result<Repository> InMemoryStore::delete_repository(const std::string& id,
                                                    const RepositoryDeletion& deletion) {
    std::lock_guard lock(mutex_);
    const auto repository = repository_by_id(repositories_, id);
    if (repository == repositories_.end()) {
        return std::unexpected(StoreError{"not_found", "no repository " + id});
    }
    if (!repository->deleted_at.empty()) {
        return std::unexpected(StoreError{"conflict", "repository is already tombstoned"});
    }
    repository->deleted_at = deletion.deleted_at;
    repository->retention_until = deletion.retention_until;
    repository->updated_at = deletion.deleted_at;
    return *repository;
}

Result<std::vector<RepositoryRef>>
InMemoryStore::list_repository_refs(const std::string& repository_id) {
    std::lock_guard lock(mutex_);
    if (repository_by_id(repositories_, repository_id) == repositories_.end()) {
        return std::unexpected(StoreError{"not_found", "no repository " + repository_id});
    }
    std::vector<RepositoryRef> references;
    for (const auto& reference : repository_refs_) {
        if (reference.repository_id == repository_id) {
            references.push_back(reference);
        }
    }
    return references;
}

Result<RepositoryRef> InMemoryStore::set_repository_ref(const std::string& repository_id,
                                                        const std::string& name,
                                                        const std::string& head) {
    std::lock_guard lock(mutex_);
    if (repository_by_id(repositories_, repository_id) == repositories_.end()) {
        return std::unexpected(StoreError{"not_found", "no repository " + repository_id});
    }
    for (auto& reference : repository_refs_) {
        if (reference.repository_id == repository_id && reference.name == name) {
            reference.head = head;
            reference.updated_at = now_iso8601();
            return reference;
        }
    }
    repository_refs_.push_back(RepositoryRef{repository_id, name, head, now_iso8601()});
    return repository_refs_.back();
}

Result<ForgeRecord> InMemoryStore::create_forge_record(const NewForgeRecord& input) {
    std::lock_guard lock(mutex_);
    if (repository_by_id(repositories_, input.repository_id) == repositories_.end()) {
        return std::unexpected(
            StoreError{"invalid_input", "repository_id does not reference a repository"});
    }
    if (input.kind.empty() || !input.value.is_object()) {
        return std::unexpected(
            StoreError{"invalid_input", "forge record kind and object value are required"});
    }
    const std::string now = now_iso8601();
    forge_records_.push_back(
        ForgeRecord{new_uuid_v4(), input.repository_id, input.kind, now, now, input.value});
    return forge_records_.back();
}

Result<ForgeRecord> InMemoryStore::get_forge_record(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto record = std::ranges::find(forge_records_, id, &ForgeRecord::id);
    if (record == forge_records_.end()) {
        return std::unexpected(StoreError{"not_found", "no forge record " + id});
    }
    return *record;
}

Result<std::vector<ForgeRecord>> InMemoryStore::list_forge_records(const std::string& repository_id,
                                                                   const std::string& kind,
                                                                   int limit) {
    std::lock_guard lock(mutex_);
    std::vector<ForgeRecord> result;
    for (auto record = forge_records_.rbegin(); record != forge_records_.rend(); ++record) {
        if (record->repository_id == repository_id && (kind.empty() || record->kind == kind)) {
            result.push_back(*record);
        }
        if (limit > 0 && result.size() >= static_cast<std::size_t>(limit)) {
            break;
        }
    }
    return result;
}

Result<ForgeRecord> InMemoryStore::update_forge_record(const std::string& id,
                                                       const nlohmann::json& value) {
    std::lock_guard lock(mutex_);
    if (!value.is_object()) {
        return std::unexpected(StoreError{"invalid_input", "forge record value must be an object"});
    }
    const auto record = std::ranges::find(forge_records_, id, &ForgeRecord::id);
    if (record == forge_records_.end()) {
        return std::unexpected(StoreError{"not_found", "no forge record " + id});
    }
    record->value = value;
    record->updated_at = now_iso8601();
    return *record;
}

Result<DriveShareLink> InMemoryStore::create_drive_share_link(const NewDriveShareLink& input) {
    std::lock_guard lock(mutex_);
    if (repository_by_id(repositories_, input.repository_id) == repositories_.end()) {
        return std::unexpected(
            StoreError{"invalid_input", "repository_id does not reference a repository"});
    }
    const auto object = std::ranges::find(policy_objects_, input.object_id, &PolicyObject::id);
    if (object == policy_objects_.end() || object->object_type != "path") {
        return std::unexpected(
            StoreError{"invalid_input", "object_id does not reference a drive folder"});
    }
    if (input.token_hash.empty() || input.expires_at.empty() ||
        (input.role != "read" && input.role != "write")) {
        return std::unexpected(StoreError{
            "invalid_input", "share link requires a token, expiry, and read or write role"});
    }
    if (std::ranges::any_of(drive_share_links_, [&](const DriveShareLink& link) {
            return link.token_hash == input.token_hash;
        })) {
        return std::unexpected(StoreError{"conflict", "share link token already exists"});
    }
    const std::string now = now_iso8601();
    const std::string id = new_uuid_v4();
    drive_share_links_.push_back(DriveShareLink{id,
                                                input.repository_id,
                                                input.object_id,
                                                input.node_id,
                                                input.path,
                                                input.role,
                                                input.token_hash,
                                                input.expires_at,
                                                "",
                                                input.created_by_taap_id,
                                                now});
    grants_.push_back(Grant{new_uuid_v4(),
                            "share-link:" + id,
                            input.object_id,
                            input.role,
                            "drive_share_link/" + id,
                            input.created_by_taap_id,
                            now,
                            now});
    return drive_share_links_.back();
}

Result<DriveShareLink> InMemoryStore::get_drive_share_link(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto link = std::ranges::find(drive_share_links_, id, &DriveShareLink::id);
    if (link == drive_share_links_.end()) {
        return std::unexpected(StoreError{"not_found", "no drive share link " + id});
    }
    return *link;
}

Result<DriveShareLink>
InMemoryStore::get_drive_share_link_by_token_hash(const std::string& token_hash) {
    std::lock_guard lock(mutex_);
    const auto link =
        std::ranges::find(drive_share_links_, token_hash, &DriveShareLink::token_hash);
    if (link == drive_share_links_.end()) {
        return std::unexpected(StoreError{"not_found", "share link is not recognized"});
    }
    return *link;
}

Result<std::vector<DriveShareLink>> InMemoryStore::list_drive_share_links(
    const std::string& repository_id, const std::string& object_id, int limit) {
    std::lock_guard lock(mutex_);
    std::vector<DriveShareLink> output;
    for (auto link = drive_share_links_.rbegin(); link != drive_share_links_.rend(); ++link) {
        if (link->repository_id == repository_id &&
            (object_id.empty() || link->object_id == object_id)) {
            output.push_back(*link);
        }
        if (limit > 0 && output.size() >= static_cast<std::size_t>(limit)) {
            break;
        }
    }
    return output;
}

Result<DriveShareLink> InMemoryStore::revoke_drive_share_link(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto link = std::ranges::find(drive_share_links_, id, &DriveShareLink::id);
    if (link == drive_share_links_.end()) {
        return std::unexpected(StoreError{"not_found", "no drive share link " + id});
    }
    if (!link->revoked_at.empty()) {
        return std::unexpected(StoreError{"conflict", "drive share link is already revoked"});
    }
    link->revoked_at = now_iso8601();
    return *link;
}

Result<Team> InMemoryStore::create_team(const NewTeam& in) {
    std::lock_guard lock(mutex_);
    if (!valid_handle(in.name)) {
        return std::unexpected(StoreError{"invalid_input", "name must match team_name_format"});
    }
    if (std::ranges::none_of(organizations_, [&](const Organization& organization) {
            return organization.id == in.organization_id;
        })) {
        return std::unexpected(
            StoreError{"invalid_input", "organization_id does not reference an organization"});
    }
    if (std::ranges::any_of(teams_, [&](const Team& team) {
            return team.organization_id == in.organization_id && team.name == in.name;
        })) {
        return std::unexpected(
            StoreError{"conflict", "this organization already has a team named " + in.name});
    }
    const auto parent = std::ranges::find_if(policy_objects_, [&](const PolicyObject& object) {
        return object.object_type == "organization" && object.external_id == in.organization_id;
    });
    if (parent == policy_objects_.end()) {
        return std::unexpected(StoreError{"store_error", "organization policy object is missing"});
    }
    const std::string now = now_iso8601();
    teams_.push_back(Team{new_uuid_v4(), in.organization_id, in.name, in.display_name, now, now});
    policy_objects_.push_back(
        PolicyObject{new_uuid_v4(), "team", teams_.back().id, parent->id, now, now, true});
    return teams_.back();
}

Result<std::vector<Team>> InMemoryStore::list_teams(const std::string& organization_id, int limit) {
    std::lock_guard lock(mutex_);
    std::vector<Team> out;
    for (auto team = teams_.rbegin(); team != teams_.rend(); ++team) {
        if (organization_id.empty() || team->organization_id == organization_id) {
            out.push_back(*team);
        }
    }
    if (limit > 0 && out.size() > static_cast<std::size_t>(limit)) {
        out.resize(static_cast<std::size_t>(limit));
    }
    return out;
}

Result<Team> InMemoryStore::get_team(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (const auto& team : teams_) {
        if (team.id == id) {
            return team;
        }
    }
    return std::unexpected(StoreError{"not_found", "no team " + id});
}

Result<Membership> InMemoryStore::create_membership(const NewMembership& in) {
    std::lock_guard lock(mutex_);
    if ((in.principal_type != "user" && in.principal_type != "workload") ||
        in.principal_id.empty() || in.principal_taap_id.empty()) {
        return std::unexpected(StoreError{
            "invalid_input", "a user or workload principal and TAAP identity are required"});
    }
    if (!valid_membership_role(in.role) || in.attestation_digest.empty() ||
        in.granted_by_taap_id.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "role and attestation evidence are required"});
    }
    if (in.principal_type == "user") {
        const auto user = std::ranges::find_if(users_, [&](const User& row) {
            return row.id == in.principal_id && row.taap_id == in.principal_taap_id;
        });
        if (user == users_.end()) {
            return std::unexpected(
                StoreError{"invalid_input", "user principal does not match its TAAP identity"});
        }
    }
    const auto organization = std::ranges::find_if(
        organizations_, [&](const Organization& row) { return row.id == in.organization_id; });
    if (organization == organizations_.end()) {
        return std::unexpected(
            StoreError{"invalid_input", "organization_id does not reference an organization"});
    }
    if (!in.team_id.empty()) {
        const auto team = std::ranges::find_if(teams_, [&](const Team& row) {
            return row.id == in.team_id && row.organization_id == in.organization_id;
        });
        if (team == teams_.end()) {
            return std::unexpected(
                StoreError{"invalid_input", "team_id does not belong to the organization"});
        }
    }
    if (std::ranges::any_of(memberships_, [&](const Membership& row) {
            return row.organization_id == in.organization_id && row.team_id == in.team_id &&
                   row.principal_type == in.principal_type && row.principal_id == in.principal_id;
        })) {
        return std::unexpected(
            StoreError{"conflict", "the principal is already a member of this scope"});
    }
    const auto parent = std::ranges::find_if(policy_objects_, [&](const PolicyObject& object) {
        const std::string type = in.team_id.empty() ? "organization" : "team";
        const std::string external_id = in.team_id.empty() ? in.organization_id : in.team_id;
        return object.object_type == type && object.external_id == external_id;
    });
    if (parent == policy_objects_.end()) {
        return std::unexpected(StoreError{"store_error", "membership policy parent is missing"});
    }
    const std::string parent_object_id = parent->id;
    const std::string now = now_iso8601();
    memberships_.push_back(Membership{new_uuid_v4(),
                                      in.organization_id,
                                      in.team_id,
                                      in.principal_type,
                                      in.principal_id,
                                      in.principal_taap_id,
                                      in.role,
                                      in.attestation_digest,
                                      in.granted_by_taap_id,
                                      now,
                                      now});
    policy_objects_.push_back(PolicyObject{
        new_uuid_v4(), "member", memberships_.back().id, parent_object_id, now, now, true});
    grants_.push_back(Grant{new_uuid_v4(),
                            in.principal_taap_id,
                            parent_object_id,
                            policy_role_for_membership(in.role),
                            in.attestation_digest,
                            in.granted_by_taap_id,
                            now,
                            now});
    return memberships_.back();
}

Result<std::vector<Membership>> InMemoryStore::list_memberships(const std::string& organization_id,
                                                                const std::string& team_id,
                                                                int limit) {
    std::lock_guard lock(mutex_);
    std::vector<Membership> out;
    for (auto membership = memberships_.rbegin(); membership != memberships_.rend(); ++membership) {
        if ((organization_id.empty() || membership->organization_id == organization_id) &&
            (team_id.empty() || membership->team_id == team_id)) {
            out.push_back(*membership);
        }
    }
    if (limit > 0 && out.size() > static_cast<std::size_t>(limit)) {
        out.resize(static_cast<std::size_t>(limit));
    }
    return out;
}

Result<Invitation> InMemoryStore::create_invitation(const NewInvitation& in) {
    std::lock_guard lock(mutex_);
    if ((in.invitee_type != "email" && in.invitee_type != "handle") || in.invitee.empty() ||
        !valid_membership_role(in.role) || in.expires_at.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "invitee, role, and expiry are required"});
    }
    if (in.expires_at <= now_iso8601()) {
        return std::unexpected(StoreError{"invalid_input", "expires_at must be in the future"});
    }
    if (std::ranges::none_of(organizations_, [&](const Organization& row) {
            return row.id == in.organization_id;
        })) {
        return std::unexpected(
            StoreError{"invalid_input", "organization_id does not reference an organization"});
    }
    if (!in.team_id.empty() && std::ranges::none_of(teams_, [&](const Team& row) {
            return row.id == in.team_id && row.organization_id == in.organization_id;
        })) {
        return std::unexpected(
            StoreError{"invalid_input", "team_id does not belong to the organization"});
    }
    const std::string now = now_iso8601();
    invitations_.push_back(Invitation{new_uuid_v4(),
                                      in.organization_id,
                                      in.team_id,
                                      in.invitee_type,
                                      lower(in.invitee),
                                      in.role,
                                      in.expires_at,
                                      "pending",
                                      {},
                                      in.created_by_taap_id,
                                      now,
                                      now});
    return invitations_.back();
}

Result<std::vector<Invitation>> InMemoryStore::list_invitations(const std::string& organization_id,
                                                                const std::string& team_id,
                                                                int limit) {
    std::lock_guard lock(mutex_);
    std::vector<Invitation> out;
    for (auto invitation = invitations_.rbegin(); invitation != invitations_.rend(); ++invitation) {
        if ((organization_id.empty() || invitation->organization_id == organization_id) &&
            (team_id.empty() || invitation->team_id == team_id)) {
            out.push_back(*invitation);
        }
    }
    if (limit > 0 && out.size() > static_cast<std::size_t>(limit)) {
        out.resize(static_cast<std::size_t>(limit));
    }
    return out;
}

Result<Invitation> InMemoryStore::get_invitation(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (const auto& invitation : invitations_) {
        if (invitation.id == id) {
            return invitation;
        }
    }
    return std::unexpected(StoreError{"not_found", "no invitation " + id});
}

Result<Membership> InMemoryStore::accept_invitation(const AcceptInvitation& in) {
    std::lock_guard lock(mutex_);
    const auto invitation = std::ranges::find_if(
        invitations_, [&](const Invitation& row) { return row.id == in.invitation_id; });
    if (invitation == invitations_.end()) {
        return std::unexpected(StoreError{"not_found", "no invitation " + in.invitation_id});
    }
    if (invitation->status != "pending") {
        return std::unexpected(StoreError{"conflict", "invitation is not pending"});
    }
    if (invitation->expires_at <= now_iso8601()) {
        return std::unexpected(StoreError{"conflict", "invitation has expired"});
    }
    if ((in.principal_type != "user" && in.principal_type != "workload") ||
        in.principal_id.empty() || in.principal_taap_id.empty() || in.attestation_digest.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "principal and attestation evidence are required"});
    }
    if (in.principal_type == "user" && std::ranges::none_of(users_, [&](const User& row) {
            return row.id == in.principal_id && row.taap_id == in.principal_taap_id;
        })) {
        return std::unexpected(
            StoreError{"invalid_input", "user principal does not match its TAAP identity"});
    }
    const auto parent = std::ranges::find_if(policy_objects_, [&](const PolicyObject& object) {
        const std::string type = invitation->team_id.empty() ? "organization" : "team";
        const std::string external_id =
            invitation->team_id.empty() ? invitation->organization_id : invitation->team_id;
        return object.object_type == type && object.external_id == external_id;
    });
    if (parent == policy_objects_.end()) {
        return std::unexpected(StoreError{"store_error", "membership policy parent is missing"});
    }
    const std::string parent_object_id = parent->id;
    if (std::ranges::any_of(memberships_, [&](const Membership& row) {
            return row.organization_id == invitation->organization_id &&
                   row.team_id == invitation->team_id && row.principal_type == in.principal_type &&
                   row.principal_id == in.principal_id;
        })) {
        return std::unexpected(
            StoreError{"conflict", "the principal is already a member of this scope"});
    }
    const std::string now = now_iso8601();
    memberships_.push_back(Membership{new_uuid_v4(),
                                      invitation->organization_id,
                                      invitation->team_id,
                                      in.principal_type,
                                      in.principal_id,
                                      in.principal_taap_id,
                                      invitation->role,
                                      in.attestation_digest,
                                      in.accepted_by_taap_id,
                                      now,
                                      now});
    policy_objects_.push_back(PolicyObject{
        new_uuid_v4(), "member", memberships_.back().id, parent_object_id, now, now, true});
    grants_.push_back(Grant{new_uuid_v4(),
                            in.principal_taap_id,
                            parent_object_id,
                            policy_role_for_membership(invitation->role),
                            in.attestation_digest,
                            in.accepted_by_taap_id,
                            now,
                            now});
    invitation->status = "accepted";
    invitation->accepted_membership_id = memberships_.back().id;
    invitation->updated_at = now;
    return memberships_.back();
}

Result<PolicyObject> InMemoryStore::create_policy_object(const NewPolicyObject& in) {
    std::lock_guard lock(mutex_);
    if (!valid_object_type(in.object_type) || in.external_id.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "valid object_type and external_id are required"});
    }
    if (in.parent_id.empty() && in.object_type != "customer") {
        return std::unexpected(
            StoreError{"invalid_input", "only a customer policy object may have no parent"});
    }
    if (std::ranges::any_of(policy_objects_, [&](const PolicyObject& object) {
            return object.object_type == in.object_type && object.external_id == in.external_id;
        })) {
        return std::unexpected(StoreError{"conflict", "policy object already exists"});
    }
    if (!in.parent_id.empty() &&
        std::ranges::none_of(policy_objects_, [&](const PolicyObject& object) {
            return object.id == in.parent_id;
        })) {
        return std::unexpected(
            StoreError{"invalid_input", "parent_id does not reference a policy object"});
    }
    if (!in.parent_id.empty()) {
        const auto parent = std::ranges::find_if(
            policy_objects_, [&](const PolicyObject& object) { return object.id == in.parent_id; });
        if (!object_parent_allowed(in.object_type, parent->object_type)) {
            return std::unexpected(
                StoreError{"invalid_input", "object type is not valid under this parent"});
        }
    }
    const std::string now = now_iso8601();
    policy_objects_.push_back(PolicyObject{
        new_uuid_v4(), in.object_type, in.external_id, in.parent_id, now, now, in.inherit});
    return policy_objects_.back();
}

Result<PolicyObject> InMemoryStore::get_policy_object(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (const auto& object : policy_objects_) {
        if (object.id == id) {
            return object;
        }
    }
    return std::unexpected(StoreError{"not_found", "no policy object " + id});
}

Result<PolicyObject> InMemoryStore::find_policy_object(const std::string& object_type,
                                                       const std::string& external_id) {
    std::lock_guard lock(mutex_);
    for (const auto& object : policy_objects_) {
        if (object.object_type == object_type && object.external_id == external_id) {
            return object;
        }
    }
    return std::unexpected(StoreError{"not_found", "no " + object_type + " object " + external_id});
}

Result<std::vector<PolicyObject>> InMemoryStore::list_policy_objects(const std::string& parent_id,
                                                                     const std::string& object_type,
                                                                     int limit) {
    std::lock_guard lock(mutex_);
    std::vector<PolicyObject> out;
    for (auto object = policy_objects_.rbegin(); object != policy_objects_.rend(); ++object) {
        if ((parent_id.empty() || object->parent_id == parent_id) &&
            (object_type.empty() || object->object_type == object_type)) {
            out.push_back(*object);
        }
    }
    if (limit > 0 && out.size() > static_cast<std::size_t>(limit)) {
        out.resize(static_cast<std::size_t>(limit));
    }
    return out;
}

Result<std::vector<ObjectPathEntry>>
InMemoryStore::policy_paths(const std::vector<std::string>& requested_object_ids) {
    std::lock_guard lock(mutex_);
    std::vector<ObjectPathEntry> out;
    for (const auto& requested_id : requested_object_ids) {
        std::string current_id = requested_id;
        std::set<std::string> visited;
        int depth = 0;
        while (!current_id.empty() && !visited.contains(current_id)) {
            visited.insert(current_id);
            const auto object = std::ranges::find_if(
                policy_objects_, [&](const PolicyObject& row) { return row.id == current_id; });
            if (object == policy_objects_.end()) {
                break;
            }
            out.push_back(ObjectPathEntry{requested_id, *object, depth});
            if (!object->inherit) {
                break;
            }
            current_id = object->parent_id;
            ++depth;
        }
    }
    return out;
}

Result<Grant> InMemoryStore::create_grant(const NewGrant& in) {
    std::lock_guard lock(mutex_);
    if (in.principal_taap_id.empty() || !valid_policy_role(in.role) ||
        in.attestation_digest.empty() || in.granted_by_taap_id.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "principal, role, and attestation evidence are required"});
    }
    const auto object = std::ranges::find_if(
        policy_objects_, [&](const PolicyObject& row) { return row.id == in.object_id; });
    if (object == policy_objects_.end()) {
        return std::unexpected(
            StoreError{"invalid_input", "object_id does not reference a policy object"});
    }
    if (!role_valid_for_object_type(object->object_type, in.role)) {
        return std::unexpected(
            StoreError{"invalid_input", "role is not valid for this object type"});
    }
    if (std::ranges::any_of(grants_, [&](const Grant& grant) {
            return grant.principal_taap_id == in.principal_taap_id &&
                   grant.object_id == in.object_id && grant.role == in.role;
        })) {
        return std::unexpected(StoreError{"conflict", "grant already exists"});
    }
    const std::string now = now_iso8601();
    grants_.push_back(Grant{new_uuid_v4(),
                            in.principal_taap_id,
                            in.object_id,
                            in.role,
                            in.attestation_digest,
                            in.granted_by_taap_id,
                            now,
                            now});
    return grants_.back();
}

Result<std::vector<Grant>>
InMemoryStore::list_grants(const std::vector<std::string>& object_ids,
                           const std::optional<std::string>& principal_taap_id,
                           int limit) {
    std::lock_guard lock(mutex_);
    std::set<std::string> wanted(object_ids.begin(), object_ids.end());
    std::vector<Grant> out;
    for (auto grant = grants_.rbegin(); grant != grants_.rend(); ++grant) {
        if ((wanted.empty() || wanted.contains(grant->object_id)) &&
            (!principal_taap_id || grant->principal_taap_id == *principal_taap_id)) {
            out.push_back(*grant);
        }
    }
    if (limit > 0 && out.size() > static_cast<std::size_t>(limit)) {
        out.resize(static_cast<std::size_t>(limit));
    }
    return out;
}

Result<std::string> InMemoryStore::customer_kek_locked(const std::string& customer_id,
                                                       bool create) {
    auto record = std::ranges::find_if(customer_keks_, [&](const CustomerKekRecord& value) {
        return value.customer_id == customer_id;
    });
    if (record != customer_keks_.end()) {
        return record->key;
    }
    if (!create) {
        return std::unexpected(StoreError{"not_found", "no customer key exists"});
    }
    auto generated = secrets_crypto::random_key();
    if (!generated) {
        return std::unexpected(generated.error());
    }
    std::string key(reinterpret_cast<const char*>(generated->data()), generated->size());
    secrets_crypto::cleanse(*generated);
    customer_keks_.push_back(CustomerKekRecord{customer_id, key, 1});
    return key;
}

Result<PkiAuthorityRecord> InMemoryStore::create_pki_authority(const NewPkiAuthorityRecord& input,
                                                               const std::string& instance_kek) {
    static_cast<void>(instance_kek);
    std::lock_guard lock(mutex_);
    const auto customer = std::ranges::find_if(
        customers_, [&](const Customer& value) { return value.id == input.customer_id; });
    if (customer == customers_.end()) {
        return std::unexpected(StoreError{"not_found", "no customer " + input.customer_id});
    }
    if (std::ranges::any_of(pki_authorities_, [&](const PkiAuthorityRecord& value) {
            return value.customer_id == input.customer_id;
        })) {
        return std::unexpected(StoreError{"conflict", "customer authority already exists"});
    }
    auto raw_kek = customer_kek_locked(input.customer_id, true);
    if (!raw_kek || raw_kek->size() != secrets_crypto::Key{}.size()) {
        return std::unexpected(raw_kek ? StoreError{"store_error", "customer key is invalid"}
                                       : raw_kek.error());
    }
    secrets_crypto::Key kek{};
    std::copy(raw_kek->begin(), raw_kek->end(), kek.begin());
    const std::string root_associated = "lake pki authority/" + input.customer_id + "/root";
    const std::string intermediate_associated =
        "lake pki authority/" + input.customer_id + "/intermediate";
    auto root = secrets_crypto::seal(kek, input.root_private_key_pem, root_associated);
    auto intermediate =
        secrets_crypto::seal(kek, input.intermediate_private_key_pem, intermediate_associated);
    secrets_crypto::cleanse(kek);
    if (!root || !intermediate) {
        return std::unexpected(!root ? root.error() : intermediate.error());
    }
    const std::string now = now_iso8601();
    PkiAuthorityRecord record{new_uuid_v4(),
                              input.customer_id,
                              input.root_certificate_pem,
                              input.intermediate_chain_pem,
                              *root,
                              *intermediate,
                              now,
                              now};
    pki_authorities_.push_back(record);
    return record;
}

Result<PkiAuthorityMaterial> InMemoryStore::get_pki_authority(const std::string& customer_id,
                                                              const std::string& instance_kek) {
    static_cast<void>(instance_kek);
    std::lock_guard lock(mutex_);
    const auto record =
        std::ranges::find_if(pki_authorities_, [&](const PkiAuthorityRecord& value) {
            return value.customer_id == customer_id;
        });
    if (record == pki_authorities_.end()) {
        return std::unexpected(StoreError{"not_found", "customer authority does not exist"});
    }
    auto raw_kek = customer_kek_locked(customer_id, false);
    if (!raw_kek || raw_kek->size() != secrets_crypto::Key{}.size()) {
        return std::unexpected(raw_kek ? StoreError{"store_error", "customer key is invalid"}
                                       : raw_kek.error());
    }
    secrets_crypto::Key kek{};
    std::copy(raw_kek->begin(), raw_kek->end(), kek.begin());
    const std::string root_associated = "lake pki authority/" + customer_id + "/root";
    const std::string intermediate_associated =
        "lake pki authority/" + customer_id + "/intermediate";
    auto root = secrets_crypto::open(kek, record->root_key_ciphertext, root_associated);
    auto intermediate =
        secrets_crypto::open(kek, record->intermediate_key_ciphertext, intermediate_associated);
    secrets_crypto::cleanse(kek);
    if (!root || !intermediate) {
        return std::unexpected(!root ? root.error() : intermediate.error());
    }
    return PkiAuthorityMaterial{record->customer_id,
                                record->root_certificate_pem,
                                record->intermediate_chain_pem,
                                std::move(*root),
                                std::move(*intermediate)};
}

Result<PkiCallerRecord> InMemoryStore::create_pki_caller(const NewPkiCallerRecord& input) {
    std::lock_guard lock(mutex_);
    const auto customer = std::ranges::find_if(
        customers_, [&](const Customer& value) { return value.id == input.customer_id; });
    static const std::set<std::string> attestation_types{
        "kubernetes", "oidc", "jwt", "aws", "azure"};
    if (customer == customers_.end()) {
        return std::unexpected(StoreError{"not_found", "customer does not exist"});
    }
    if (input.workload_taap_id.empty() || !attestation_types.contains(input.attestation_type) ||
        input.subject.empty() || !input.attributes.is_object() ||
        input.expires_at <= now_iso8601()) {
        return std::unexpected(StoreError{"invalid_input", "caller record is invalid"});
    }
    PkiCallerRecord record{new_uuid_v4(),
                           input.customer_id,
                           input.workload_taap_id,
                           input.attestation_type,
                           input.issuer,
                           input.subject,
                           input.attributes,
                           input.expires_at,
                           now_iso8601()};
    pki_callers_.push_back(record);
    return record;
}

Result<std::vector<PkiCallerRecord>> InMemoryStore::list_pki_callers(const std::string& customer_id,
                                                                     int limit) {
    if (limit < 1 || limit > 100) {
        return std::unexpected(StoreError{"invalid_input", "limit must be between 1 and 100"});
    }
    std::lock_guard lock(mutex_);
    std::vector<PkiCallerRecord> output;
    for (auto record = pki_callers_.rbegin();
         record != pki_callers_.rend() && output.size() < limit;
         ++record) {
        if (record->customer_id == customer_id) {
            output.push_back(*record);
        }
    }
    return output;
}

Result<PkiRecoveryCeremonyRecord>
InMemoryStore::create_pki_recovery_ceremony(const NewPkiRecoveryCeremonyRecord& input) {
    std::lock_guard lock(mutex_);
    const auto authority =
        std::ranges::find_if(pki_authorities_, [&](const PkiAuthorityRecord& value) {
            return value.customer_id == input.customer_id;
        });
    if (authority == pki_authorities_.end()) {
        return std::unexpected(StoreError{"not_found", "customer authority does not exist"});
    }
    if (input.id.empty() || input.threshold < 2 || input.share_count < input.threshold ||
        input.share_count > 255 || input.expires_at <= now_iso8601()) {
        return std::unexpected(StoreError{"invalid_input", "recovery ceremony is invalid"});
    }
    if (std::ranges::any_of(pki_recovery_ceremonies_,
                            [&](const auto& value) { return value.id == input.id; })) {
        return std::unexpected(StoreError{"conflict", "recovery ceremony already exists"});
    }
    PkiRecoveryCeremonyRecord record{input.id,
                                     input.customer_id,
                                     authority->id,
                                     input.threshold,
                                     input.share_count,
                                     std::nullopt,
                                     input.expires_at,
                                     now_iso8601()};
    pki_recovery_ceremonies_.push_back(record);
    return record;
}

Result<PkiRecoveryCeremonyRecord>
InMemoryStore::get_pki_recovery_ceremony(const std::string& customer_id,
                                         const std::string& ceremony_id) {
    std::lock_guard lock(mutex_);
    const auto ceremony = std::ranges::find_if(pki_recovery_ceremonies_, [&](const auto& value) {
        return value.id == ceremony_id && value.customer_id == customer_id;
    });
    if (ceremony == pki_recovery_ceremonies_.end()) {
        return std::unexpected(StoreError{"not_found", "recovery ceremony does not exist"});
    }
    return *ceremony;
}

Result<PkiRecoveryCeremonyRecord> InMemoryStore::complete_pki_recovery_ceremony(
    const std::string& customer_id,
    const std::string& ceremony_id,
    const std::vector<PkiRecoveryShareEvidence>& evidence) {
    std::lock_guard lock(mutex_);
    auto ceremony = std::ranges::find_if(pki_recovery_ceremonies_, [&](const auto& value) {
        return value.id == ceremony_id && value.customer_id == customer_id;
    });
    if (ceremony == pki_recovery_ceremonies_.end()) {
        return std::unexpected(StoreError{"not_found", "recovery ceremony does not exist"});
    }
    const std::string now = now_iso8601();
    if (ceremony->recovered_at || ceremony->expires_at <= now) {
        return std::unexpected(StoreError{"conflict", "recovery ceremony is not active"});
    }
    std::set<int> indices;
    for (const PkiRecoveryShareEvidence& share : evidence) {
        if (share.index < 1 || share.digest.empty() || !indices.insert(share.index).second) {
            return std::unexpected(StoreError{"invalid_input", "recovery evidence is invalid"});
        }
    }
    if (static_cast<int>(evidence.size()) < ceremony->threshold) {
        return std::unexpected(StoreError{"invalid_input", "recovery quorum is not present"});
    }
    ceremony->recovered_at = now;
    pki_recovery_evidence_[ceremony_id] = evidence;
    return *ceremony;
}

Result<void> InMemoryStore::store_data_key_locked(const std::string& customer_id,
                                                  const std::string& owner_id,
                                                  std::uint64_t version,
                                                  const std::string& key) {
    auto customer_key = customer_kek_locked(customer_id, true);
    if (!customer_key || customer_key->size() != secrets_crypto::Key{}.size()) {
        return std::unexpected(customer_key ? StoreError{"store_error", "customer key is invalid"}
                                            : customer_key.error());
    }
    secrets_crypto::Key kek{};
    std::copy(customer_key->begin(), customer_key->end(), kek.begin());
    const std::string associated =
        "lake data key/" + customer_id + "/" + owner_id + "/" + std::to_string(version);
    auto wrapped = secrets_crypto::seal(kek, key, associated);
    secrets_crypto::cleanse(kek);
    if (!wrapped) {
        return std::unexpected(wrapped.error());
    }
    data_keys_.push_back(DataKeyRecord{customer_id, owner_id, version, *wrapped});
    return {};
}

Result<std::string> InMemoryStore::data_key_locked(const std::string& customer_id,
                                                   const std::string& owner_id,
                                                   std::uint64_t version) {
    const auto record = std::ranges::find_if(data_keys_, [&](const DataKeyRecord& value) {
        return value.customer_id == customer_id && value.owner_id == owner_id &&
               value.version == version;
    });
    if (record == data_keys_.end()) {
        return std::unexpected(StoreError{"not_found", "no data key version exists"});
    }
    auto customer_key = customer_kek_locked(customer_id, false);
    if (!customer_key || customer_key->size() != secrets_crypto::Key{}.size()) {
        return std::unexpected(customer_key ? StoreError{"store_error", "customer key is invalid"}
                                            : customer_key.error());
    }
    secrets_crypto::Key kek{};
    std::copy(customer_key->begin(), customer_key->end(), kek.begin());
    const std::string associated =
        "lake data key/" + customer_id + "/" + owner_id + "/" + std::to_string(version);
    auto opened = secrets_crypto::open(kek, record->wrapped_key, associated);
    secrets_crypto::cleanse(kek);
    return opened;
}

Result<SecretMount> InMemoryStore::create_secret_mount(const NewSecretMount& input,
                                                       const std::string& instance_kek) {
    static_cast<void>(instance_kek);
    std::lock_guard lock(mutex_);
    if (input.customer_id.empty() || input.organization_id.empty() || !valid_handle(input.name)) {
        return std::unexpected(
            StoreError{"invalid_input",
                       "customer_id, organization_id, and a lowercase mount name are required"});
    }
    const auto customer = std::ranges::find_if(
        customers_, [&](const Customer& value) { return value.id == input.customer_id; });
    const auto organization = std::ranges::find_if(organizations_, [&](const Organization& value) {
        return value.id == input.organization_id;
    });
    const auto customer_object =
        std::ranges::find_if(policy_objects_, [&](const PolicyObject& value) {
            return value.object_type == "customer" && value.external_id == input.customer_id;
        });
    const auto organization_object =
        std::ranges::find_if(policy_objects_, [&](const PolicyObject& value) {
            return value.object_type == "organization" &&
                   value.external_id == input.organization_id;
        });
    if (customer == customers_.end() || organization == organizations_.end() ||
        customer_object == policy_objects_.end() || organization_object == policy_objects_.end() ||
        organization_object->parent_id != customer_object->id) {
        return std::unexpected(
            StoreError{"invalid_input", "organization_id is not inside the customer namespace"});
    }
    if (std::ranges::any_of(secret_mounts_, [&](const SecretMount& value) {
            return value.customer_id == input.customer_id &&
                   value.organization_id == input.organization_id && value.name == input.name;
        })) {
        return std::unexpected(StoreError{"conflict", "secret mount already exists"});
    }
    const std::string now = now_iso8601();
    SecretMount mount{new_uuid_v4(),
                      input.customer_id,
                      input.organization_id,
                      input.name,
                      input.description,
                      now,
                      now};
    const std::string environment_external =
        "secrets/" + input.customer_id + "/" + input.organization_id;
    auto environment = std::ranges::find_if(policy_objects_, [&](const PolicyObject& value) {
        return value.object_type == "environment" && value.external_id == environment_external;
    });
    if (environment == policy_objects_.end()) {
        policy_objects_.push_back(PolicyObject{new_uuid_v4(),
                                               "environment",
                                               environment_external,
                                               organization_object->id,
                                               now,
                                               now,
                                               true});
        environment = std::prev(policy_objects_.end());
    }
    policy_objects_.push_back(
        PolicyObject{new_uuid_v4(), "secret_mount", mount.id, environment->id, now, now, true});
    auto data_key = secrets_crypto::random_key();
    if (!data_key) {
        policy_objects_.pop_back();
        return std::unexpected(data_key.error());
    }
    const std::string raw(reinterpret_cast<const char*>(data_key->data()), data_key->size());
    auto stored = store_data_key_locked(input.customer_id, mount.id, 1, raw);
    secrets_crypto::cleanse(*data_key);
    if (!stored) {
        policy_objects_.pop_back();
        return std::unexpected(stored.error());
    }
    secret_mounts_.push_back(mount);
    return mount;
}

Result<std::vector<SecretMount>> InMemoryStore::list_secret_mounts(
    const std::string& customer_id, const std::string& organization_id, int limit) {
    std::lock_guard lock(mutex_);
    std::vector<SecretMount> output;
    for (auto mount = secret_mounts_.rbegin(); mount != secret_mounts_.rend(); ++mount) {
        if ((customer_id.empty() || mount->customer_id == customer_id) &&
            (organization_id.empty() || mount->organization_id == organization_id)) {
            output.push_back(*mount);
        }
    }
    if (limit > 0 && output.size() > static_cast<std::size_t>(limit)) {
        output.resize(static_cast<std::size_t>(limit));
    }
    return output;
}

Result<SecretMount> InMemoryStore::get_secret_mount(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto mount = std::ranges::find_if(
        secret_mounts_, [&](const SecretMount& value) { return value.id == id; });
    if (mount == secret_mounts_.end()) {
        return std::unexpected(StoreError{"not_found", "no secret mount " + id});
    }
    return *mount;
}

Result<SecretVersion> InMemoryStore::put_secret(const PutSecret& input,
                                                const std::string& instance_kek) {
    static_cast<void>(instance_kek);
    std::lock_guard lock(mutex_);
    if (input.name.empty() || input.name.size() > 1024 || !input.value.is_object() ||
        !input.custom_metadata.is_object()) {
        return std::unexpected(StoreError{
            "invalid_input", "secret name, object value, and object metadata are required"});
    }
    const auto mount = std::ranges::find_if(
        secret_mounts_, [&](const SecretMount& value) { return value.id == input.mount_id; });
    if (mount == secret_mounts_.end()) {
        return std::unexpected(StoreError{"not_found", "no secret mount " + input.mount_id});
    }
    auto entry = std::ranges::find_if(secret_entries_, [&](const SecretEntryRecord& value) {
        return value.metadata.mount_id == input.mount_id && value.metadata.name == input.name;
    });
    const std::uint64_t current =
        entry == secret_entries_.end() ? 0 : entry->metadata.current_version;
    if (input.check_and_set != 0 && input.check_and_set != current) {
        return std::unexpected(StoreError{"conflict", "secret version changed"});
    }
    const std::uint64_t version = current + 1;
    auto raw_key = data_key_locked(mount->customer_id, mount->id, 1);
    if (!raw_key || raw_key->size() != secrets_crypto::Key{}.size()) {
        return std::unexpected(raw_key ? StoreError{"store_error", "mount data key is invalid"}
                                       : raw_key.error());
    }
    secrets_crypto::Key key{};
    std::copy(raw_key->begin(), raw_key->end(), key.begin());
    const std::string associated =
        "lake secret/" + mount->id + "/" + input.name + "/" + std::to_string(version);
    auto ciphertext = secrets_crypto::seal(key, input.value.dump(), associated);
    secrets_crypto::cleanse(key);
    if (!ciphertext) {
        return std::unexpected(ciphertext.error());
    }
    const std::string now = now_iso8601();
    if (entry == secret_entries_.end()) {
        SecretMetadata metadata{mount->id, input.name, 0, input.custom_metadata, {}, now, now};
        secret_entries_.push_back(SecretEntryRecord{metadata, {}});
        entry = std::prev(secret_entries_.end());
        const auto mount_object =
            std::ranges::find_if(policy_objects_, [&](const PolicyObject& value) {
                return value.object_type == "secret_mount" && value.external_id == mount->id;
            });
        policy_objects_.push_back(PolicyObject{new_uuid_v4(),
                                               "secret",
                                               mount->id + "/" + input.name,
                                               mount_object->id,
                                               now,
                                               now,
                                               true});
    }
    if (!input.custom_metadata.empty()) {
        entry->metadata.custom_metadata = input.custom_metadata;
    }
    entry->metadata.current_version = version;
    entry->metadata.updated_at = now;
    entry->metadata.versions.push_back(SecretVersionMetadata{version, now, "", ""});
    SecretVersion result{
        mount->id, input.name, version, input.value, entry->metadata.custom_metadata, now, "", ""};
    entry->versions.push_back(SecretVersionRecord{result, *ciphertext});
    return result;
}

Result<SecretVersion> InMemoryStore::read_secret(const std::string& mount_id,
                                                 const std::string& name,
                                                 std::uint64_t version,
                                                 const std::string& instance_kek) {
    static_cast<void>(instance_kek);
    std::lock_guard lock(mutex_);
    const auto mount = std::ranges::find_if(
        secret_mounts_, [&](const SecretMount& value) { return value.id == mount_id; });
    const auto entry = std::ranges::find_if(secret_entries_, [&](const SecretEntryRecord& value) {
        return value.metadata.mount_id == mount_id && value.metadata.name == name;
    });
    if (mount == secret_mounts_.end() || entry == secret_entries_.end()) {
        return std::unexpected(StoreError{"not_found", "secret does not exist"});
    }
    const std::uint64_t selected = version == 0 ? entry->metadata.current_version : version;
    const auto stored =
        std::ranges::find_if(entry->versions, [&](const SecretVersionRecord& value) {
            return value.version.version == selected;
        });
    if (stored == entry->versions.end()) {
        return std::unexpected(StoreError{"not_found", "secret version does not exist"});
    }
    if (!stored->version.destroyed_at.empty()) {
        return std::unexpected(StoreError{"not_found", "secret version was destroyed"});
    }
    if (!stored->version.deleted_at.empty()) {
        return std::unexpected(StoreError{"not_found", "secret version is deleted"});
    }
    auto raw_key = data_key_locked(mount->customer_id, mount->id, 1);
    if (!raw_key || raw_key->size() != secrets_crypto::Key{}.size()) {
        return std::unexpected(raw_key ? StoreError{"store_error", "mount data key is invalid"}
                                       : raw_key.error());
    }
    secrets_crypto::Key key{};
    std::copy(raw_key->begin(), raw_key->end(), key.begin());
    const std::string associated =
        "lake secret/" + mount->id + "/" + name + "/" + std::to_string(selected);
    auto plaintext = secrets_crypto::open(key, stored->ciphertext, associated);
    secrets_crypto::cleanse(key);
    if (!plaintext) {
        return std::unexpected(plaintext.error());
    }
    nlohmann::json value = nlohmann::json::parse(*plaintext, nullptr, false);
    if (value.is_discarded() || !value.is_object()) {
        return std::unexpected(StoreError{"store_error", "stored secret is not valid JSON"});
    }
    SecretVersion result = stored->version;
    result.value = std::move(value);
    result.custom_metadata = entry->metadata.custom_metadata;
    return result;
}

Result<SecretMetadata> InMemoryStore::secret_metadata(const std::string& mount_id,
                                                      const std::string& name) {
    std::lock_guard lock(mutex_);
    const auto entry = std::ranges::find_if(secret_entries_, [&](const SecretEntryRecord& value) {
        return value.metadata.mount_id == mount_id && value.metadata.name == name;
    });
    if (entry == secret_entries_.end()) {
        return std::unexpected(StoreError{"not_found", "secret does not exist"});
    }
    return entry->metadata;
}

Result<SecretMetadata>
InMemoryStore::change_secret_versions(const std::string& mount_id,
                                      const std::string& name,
                                      const std::vector<std::uint64_t>& versions,
                                      SecretVersionAction action) {
    std::lock_guard lock(mutex_);
    if (versions.empty()) {
        return std::unexpected(StoreError{"invalid_input", "at least one version is required"});
    }
    auto entry = std::ranges::find_if(secret_entries_, [&](const SecretEntryRecord& value) {
        return value.metadata.mount_id == mount_id && value.metadata.name == name;
    });
    if (entry == secret_entries_.end()) {
        return std::unexpected(StoreError{"not_found", "secret does not exist"});
    }
    for (const std::uint64_t version : versions) {
        const auto stored =
            std::ranges::find_if(entry->versions, [&](const SecretVersionRecord& value) {
                return value.version.version == version;
            });
        if (stored == entry->versions.end()) {
            return std::unexpected(StoreError{"not_found", "secret version does not exist"});
        }
        if (action == SecretVersionAction::Undelete && !stored->version.destroyed_at.empty()) {
            return std::unexpected(
                StoreError{"conflict", "a destroyed secret version cannot be restored"});
        }
    }
    const std::string now = now_iso8601();
    for (const std::uint64_t version : versions) {
        auto stored = std::ranges::find_if(entry->versions, [&](const SecretVersionRecord& value) {
            return value.version.version == version;
        });
        if (action == SecretVersionAction::Delete && stored->version.destroyed_at.empty()) {
            stored->version.deleted_at = now;
        } else if (action == SecretVersionAction::Undelete) {
            stored->version.deleted_at.clear();
        } else if (action == SecretVersionAction::Destroy) {
            stored->ciphertext.clear();
            stored->version.value = nlohmann::json::object();
            stored->version.deleted_at = now;
            stored->version.destroyed_at = now;
        }
        auto metadata =
            std::ranges::find_if(entry->metadata.versions, [&](const SecretVersionMetadata& value) {
                return value.version == version;
            });
        metadata->deleted_at = stored->version.deleted_at;
        metadata->destroyed_at = stored->version.destroyed_at;
    }
    entry->metadata.updated_at = now;
    return entry->metadata;
}

Result<EncryptionKey> InMemoryStore::create_encryption_key(const NewEncryptionKey& input,
                                                           const std::string& instance_kek) {
    static_cast<void>(instance_kek);
    std::lock_guard lock(mutex_);
    if (!valid_handle(input.name)) {
        return std::unexpected(
            StoreError{"invalid_input", "encryption key name must be a lowercase handle"});
    }
    const auto mount = std::ranges::find_if(
        secret_mounts_, [&](const SecretMount& value) { return value.id == input.mount_id; });
    if (mount == secret_mounts_.end()) {
        return std::unexpected(StoreError{"not_found", "no secret mount " + input.mount_id});
    }
    if (std::ranges::any_of(encryption_keys_, [&](const EncryptionKey& value) {
            return value.mount_id == input.mount_id && value.name == input.name;
        })) {
        return std::unexpected(StoreError{"conflict", "encryption key already exists"});
    }
    const std::string now = now_iso8601();
    EncryptionKey key{new_uuid_v4(), input.mount_id, input.name, "", 1, now, now};
    auto raw = secrets_crypto::random_key();
    if (!raw) {
        return std::unexpected(raw.error());
    }
    const std::string material(reinterpret_cast<const char*>(raw->data()), raw->size());
    auto stored = store_data_key_locked(mount->customer_id, key.id, 1, material);
    secrets_crypto::cleanse(*raw);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    encryption_keys_.push_back(key);
    return key;
}

Result<std::vector<EncryptionKey>> InMemoryStore::list_encryption_keys(const std::string& mount_id,
                                                                       int limit) {
    std::lock_guard lock(mutex_);
    std::vector<EncryptionKey> output;
    for (auto key = encryption_keys_.rbegin(); key != encryption_keys_.rend(); ++key) {
        if (key->mount_id == mount_id) {
            output.push_back(*key);
        }
    }
    if (limit > 0 && output.size() > static_cast<std::size_t>(limit)) {
        output.resize(static_cast<std::size_t>(limit));
    }
    return output;
}

Result<EncryptionKey> InMemoryStore::get_encryption_key(const std::string& mount_id,
                                                        const std::string& name) {
    std::lock_guard lock(mutex_);
    const auto key = std::ranges::find_if(encryption_keys_, [&](const EncryptionKey& value) {
        return value.mount_id == mount_id && value.name == name;
    });
    if (key == encryption_keys_.end()) {
        return std::unexpected(StoreError{"not_found", "encryption key does not exist"});
    }
    return *key;
}

Result<EncryptionKey> InMemoryStore::rotate_encryption_key(const std::string& mount_id,
                                                           const std::string& name,
                                                           const std::string& instance_kek) {
    static_cast<void>(instance_kek);
    std::lock_guard lock(mutex_);
    auto key = std::ranges::find_if(encryption_keys_, [&](const EncryptionKey& value) {
        return value.mount_id == mount_id && value.name == name;
    });
    const auto mount = std::ranges::find_if(
        secret_mounts_, [&](const SecretMount& value) { return value.id == mount_id; });
    if (key == encryption_keys_.end() || mount == secret_mounts_.end()) {
        return std::unexpected(StoreError{"not_found", "encryption key does not exist"});
    }
    auto raw = secrets_crypto::random_key();
    if (!raw) {
        return std::unexpected(raw.error());
    }
    const std::uint64_t version = key->current_version + 1;
    const std::string material(reinterpret_cast<const char*>(raw->data()), raw->size());
    auto stored = store_data_key_locked(mount->customer_id, key->id, version, material);
    secrets_crypto::cleanse(*raw);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    key->current_version = version;
    key->updated_at = now_iso8601();
    return *key;
}

Result<TransitValue> InMemoryStore::encrypt(const std::string& mount_id,
                                            const std::string& name,
                                            const std::string& plaintext,
                                            const std::string& instance_kek) {
    static_cast<void>(instance_kek);
    std::lock_guard lock(mutex_);
    const auto key = std::ranges::find_if(encryption_keys_, [&](const EncryptionKey& value) {
        return value.mount_id == mount_id && value.name == name;
    });
    const auto mount = std::ranges::find_if(
        secret_mounts_, [&](const SecretMount& value) { return value.id == mount_id; });
    if (key == encryption_keys_.end() || mount == secret_mounts_.end()) {
        return std::unexpected(StoreError{"not_found", "encryption key does not exist"});
    }
    auto decoded = secrets_crypto::base64url_decode(plaintext);
    auto raw = data_key_locked(mount->customer_id, key->id, key->current_version);
    if (!decoded || !raw || raw->size() != secrets_crypto::Key{}.size()) {
        return std::unexpected(
            !decoded ? decoded.error()
                     : (!raw ? raw.error() : StoreError{"store_error", "data key is invalid"}));
    }
    secrets_crypto::Key material{};
    std::copy(raw->begin(), raw->end(), material.begin());
    auto encryption_key = secrets_crypto::derive(material, "lake transit encryption v1");
    secrets_crypto::cleanse(material);
    if (!encryption_key) {
        return std::unexpected(encryption_key.error());
    }
    const std::string associated =
        "lake transit/" + key->id + "/" + std::to_string(key->current_version);
    auto sealed = secrets_crypto::seal(*encryption_key, *decoded, associated);
    secrets_crypto::cleanse(*encryption_key);
    if (!sealed) {
        return std::unexpected(sealed.error());
    }
    return TransitValue{secrets_crypto::format_ciphertext({key->id, key->current_version, *sealed}),
                        key->current_version,
                        "aes-256-gcm"};
}

Result<TransitValue> InMemoryStore::decrypt(const std::string& mount_id,
                                            const std::string& name,
                                            const std::string& ciphertext,
                                            const std::string& instance_kek) {
    static_cast<void>(instance_kek);
    std::lock_guard lock(mutex_);
    const auto key = std::ranges::find_if(encryption_keys_, [&](const EncryptionKey& value) {
        return value.mount_id == mount_id && value.name == name;
    });
    const auto mount = std::ranges::find_if(
        secret_mounts_, [&](const SecretMount& value) { return value.id == mount_id; });
    auto parsed = secrets_crypto::parse_ciphertext(ciphertext);
    if (key == encryption_keys_.end() || mount == secret_mounts_.end()) {
        return std::unexpected(StoreError{"not_found", "encryption key does not exist"});
    }
    if (!parsed || parsed->key_id != key->id) {
        return std::unexpected(parsed ? StoreError{"invalid_input", "ciphertext names another key"}
                                      : parsed.error());
    }
    auto raw = data_key_locked(mount->customer_id, key->id, parsed->version);
    if (!raw || raw->size() != secrets_crypto::Key{}.size()) {
        return std::unexpected(raw ? StoreError{"store_error", "data key is invalid"}
                                   : raw.error());
    }
    secrets_crypto::Key material{};
    std::copy(raw->begin(), raw->end(), material.begin());
    auto encryption_key = secrets_crypto::derive(material, "lake transit encryption v1");
    secrets_crypto::cleanse(material);
    if (!encryption_key) {
        return std::unexpected(encryption_key.error());
    }
    const std::string associated =
        "lake transit/" + key->id + "/" + std::to_string(parsed->version);
    auto plaintext = secrets_crypto::open(*encryption_key, parsed->sealed, associated);
    secrets_crypto::cleanse(*encryption_key);
    if (!plaintext) {
        return std::unexpected(plaintext.error());
    }
    auto encoded = secrets_crypto::base64url_encode(*plaintext);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return TransitValue{*encoded, parsed->version, "base64url"};
}

Result<TransitValue> InMemoryStore::rewrap(const std::string& mount_id,
                                           const std::string& name,
                                           const std::string& ciphertext,
                                           const std::string& instance_kek) {
    static_cast<void>(instance_kek);
    std::lock_guard lock(mutex_);
    const auto key = std::ranges::find_if(encryption_keys_, [&](const EncryptionKey& value) {
        return value.mount_id == mount_id && value.name == name;
    });
    const auto mount = std::ranges::find_if(
        secret_mounts_, [&](const SecretMount& value) { return value.id == mount_id; });
    auto parsed = secrets_crypto::parse_ciphertext(ciphertext);
    if (key == encryption_keys_.end() || mount == secret_mounts_.end()) {
        return std::unexpected(StoreError{"not_found", "encryption key does not exist"});
    }
    if (!parsed || parsed->key_id != key->id) {
        return std::unexpected(parsed ? StoreError{"invalid_input", "ciphertext names another key"}
                                      : parsed.error());
    }
    auto old_raw = data_key_locked(mount->customer_id, key->id, parsed->version);
    auto new_raw = data_key_locked(mount->customer_id, key->id, key->current_version);
    if (!old_raw || !new_raw || old_raw->size() != secrets_crypto::Key{}.size() ||
        new_raw->size() != secrets_crypto::Key{}.size()) {
        return std::unexpected(
            !old_raw
                ? old_raw.error()
                : (!new_raw ? new_raw.error() : StoreError{"store_error", "data key is invalid"}));
    }
    secrets_crypto::Key old_material{};
    secrets_crypto::Key new_material{};
    std::copy(old_raw->begin(), old_raw->end(), old_material.begin());
    std::copy(new_raw->begin(), new_raw->end(), new_material.begin());
    auto old_key = secrets_crypto::derive(old_material, "lake transit encryption v1");
    auto new_key = secrets_crypto::derive(new_material, "lake transit encryption v1");
    secrets_crypto::cleanse(old_material);
    secrets_crypto::cleanse(new_material);
    if (!old_key || !new_key) {
        return std::unexpected(!old_key ? old_key.error() : new_key.error());
    }
    const std::string old_associated =
        "lake transit/" + key->id + "/" + std::to_string(parsed->version);
    auto plaintext = secrets_crypto::open(*old_key, parsed->sealed, old_associated);
    secrets_crypto::cleanse(*old_key);
    if (!plaintext) {
        secrets_crypto::cleanse(*new_key);
        return std::unexpected(plaintext.error());
    }
    const std::string new_associated =
        "lake transit/" + key->id + "/" + std::to_string(key->current_version);
    auto sealed = secrets_crypto::seal(*new_key, *plaintext, new_associated);
    secrets_crypto::cleanse(*new_key);
    if (!sealed) {
        return std::unexpected(sealed.error());
    }
    return TransitValue{secrets_crypto::format_ciphertext({key->id, key->current_version, *sealed}),
                        key->current_version,
                        "aes-256-gcm"};
}

Result<TransitValue> InMemoryStore::sign(const std::string& mount_id,
                                         const std::string& name,
                                         const std::string& message,
                                         std::uint64_t version,
                                         const std::string& instance_kek) {
    static_cast<void>(instance_kek);
    std::lock_guard lock(mutex_);
    const auto key = std::ranges::find_if(encryption_keys_, [&](const EncryptionKey& value) {
        return value.mount_id == mount_id && value.name == name;
    });
    const auto mount = std::ranges::find_if(
        secret_mounts_, [&](const SecretMount& value) { return value.id == mount_id; });
    if (key == encryption_keys_.end() || mount == secret_mounts_.end()) {
        return std::unexpected(StoreError{"not_found", "encryption key does not exist"});
    }
    const std::uint64_t selected = version == 0 ? key->current_version : version;
    auto decoded = secrets_crypto::base64url_decode(message);
    auto raw = data_key_locked(mount->customer_id, key->id, selected);
    if (!decoded || !raw || raw->size() != secrets_crypto::Key{}.size()) {
        return std::unexpected(
            !decoded ? decoded.error()
                     : (!raw ? raw.error() : StoreError{"store_error", "data key is invalid"}));
    }
    secrets_crypto::Key material{};
    std::copy(raw->begin(), raw->end(), material.begin());
    auto signing_key = secrets_crypto::derive(material, "lake transit signing v1");
    secrets_crypto::cleanse(material);
    if (!signing_key) {
        return std::unexpected(signing_key.error());
    }
    auto signature = secrets_crypto::sign(*signing_key, *decoded);
    secrets_crypto::cleanse(*signing_key);
    if (!signature) {
        return std::unexpected(signature.error());
    }
    return TransitValue{*signature, selected, "ed25519"};
}

Result<TransitVerification> InMemoryStore::verify(const std::string& mount_id,
                                                  const std::string& name,
                                                  const std::string& message,
                                                  const std::string& signature,
                                                  std::uint64_t version,
                                                  const std::string& instance_kek) {
    static_cast<void>(instance_kek);
    std::lock_guard lock(mutex_);
    const auto key = std::ranges::find_if(encryption_keys_, [&](const EncryptionKey& value) {
        return value.mount_id == mount_id && value.name == name;
    });
    const auto mount = std::ranges::find_if(
        secret_mounts_, [&](const SecretMount& value) { return value.id == mount_id; });
    if (key == encryption_keys_.end() || mount == secret_mounts_.end()) {
        return std::unexpected(StoreError{"not_found", "encryption key does not exist"});
    }
    const std::uint64_t selected = version == 0 ? key->current_version : version;
    auto decoded = secrets_crypto::base64url_decode(message);
    auto raw = data_key_locked(mount->customer_id, key->id, selected);
    if (!decoded || !raw || raw->size() != secrets_crypto::Key{}.size()) {
        return std::unexpected(
            !decoded ? decoded.error()
                     : (!raw ? raw.error() : StoreError{"store_error", "data key is invalid"}));
    }
    secrets_crypto::Key material{};
    std::copy(raw->begin(), raw->end(), material.begin());
    auto signing_key = secrets_crypto::derive(material, "lake transit signing v1");
    secrets_crypto::cleanse(material);
    if (!signing_key) {
        return std::unexpected(signing_key.error());
    }
    auto valid = secrets_crypto::verify(*signing_key, *decoded, signature);
    secrets_crypto::cleanse(*signing_key);
    if (!valid) {
        return std::unexpected(valid.error());
    }
    return TransitVerification{*valid, selected, "ed25519"};
}

Result<TransitValue> InMemoryStore::hmac(const std::string& mount_id,
                                         const std::string& name,
                                         const std::string& message,
                                         std::uint64_t version,
                                         const std::string& instance_kek) {
    static_cast<void>(instance_kek);
    std::lock_guard lock(mutex_);
    const auto key = std::ranges::find_if(encryption_keys_, [&](const EncryptionKey& value) {
        return value.mount_id == mount_id && value.name == name;
    });
    const auto mount = std::ranges::find_if(
        secret_mounts_, [&](const SecretMount& value) { return value.id == mount_id; });
    if (key == encryption_keys_.end() || mount == secret_mounts_.end()) {
        return std::unexpected(StoreError{"not_found", "encryption key does not exist"});
    }
    const std::uint64_t selected = version == 0 ? key->current_version : version;
    auto decoded = secrets_crypto::base64url_decode(message);
    auto raw = data_key_locked(mount->customer_id, key->id, selected);
    if (!decoded || !raw || raw->size() != secrets_crypto::Key{}.size()) {
        return std::unexpected(
            !decoded ? decoded.error()
                     : (!raw ? raw.error() : StoreError{"store_error", "data key is invalid"}));
    }
    secrets_crypto::Key material{};
    std::copy(raw->begin(), raw->end(), material.begin());
    auto hmac_key = secrets_crypto::derive(material, "lake transit hmac v1");
    secrets_crypto::cleanse(material);
    if (!hmac_key) {
        return std::unexpected(hmac_key.error());
    }
    auto digest = secrets_crypto::hmac(*hmac_key, *decoded);
    secrets_crypto::cleanse(*hmac_key);
    if (!digest) {
        return std::unexpected(digest.error());
    }
    return TransitValue{*digest, selected, "hmac-sha256"};
}

Result<DerivedEncryptionKey> InMemoryStore::derive_encryption_key(const std::string& mount_id,
                                                                  const std::string& parent_name,
                                                                  const std::string& name,
                                                                  const std::string& context,
                                                                  const std::string& instance_kek) {
    static_cast<void>(instance_kek);
    std::lock_guard lock(mutex_);
    if (!valid_handle(name)) {
        return std::unexpected(
            StoreError{"invalid_input", "derived key name must be a lowercase handle"});
    }
    const auto parent = std::ranges::find_if(encryption_keys_, [&](const EncryptionKey& value) {
        return value.mount_id == mount_id && value.name == parent_name;
    });
    const auto mount = std::ranges::find_if(
        secret_mounts_, [&](const SecretMount& value) { return value.id == mount_id; });
    if (parent == encryption_keys_.end() || mount == secret_mounts_.end()) {
        return std::unexpected(StoreError{"not_found", "parent encryption key does not exist"});
    }
    if (std::ranges::any_of(encryption_keys_, [&](const EncryptionKey& value) {
            return value.mount_id == mount_id && value.name == name;
        })) {
        return std::unexpected(StoreError{"conflict", "encryption key already exists"});
    }
    auto decoded_context = secrets_crypto::base64url_decode(context);
    auto raw = data_key_locked(mount->customer_id, parent->id, parent->current_version);
    if (!decoded_context || !raw || raw->size() != secrets_crypto::Key{}.size()) {
        return std::unexpected(
            !decoded_context
                ? decoded_context.error()
                : (!raw ? raw.error() : StoreError{"store_error", "data key is invalid"}));
    }
    secrets_crypto::Key material{};
    std::copy(raw->begin(), raw->end(), material.begin());
    auto derived = secrets_crypto::derive(material, "lake transit child v1/" + *decoded_context);
    secrets_crypto::cleanse(material);
    if (!derived) {
        return std::unexpected(derived.error());
    }
    const std::string now = now_iso8601();
    EncryptionKey child{new_uuid_v4(), mount_id, name, parent->id, 1, now, now};
    const std::string child_material(reinterpret_cast<const char*>(derived->data()),
                                     derived->size());
    auto stored = store_data_key_locked(mount->customer_id, child.id, 1, child_material);
    secrets_crypto::cleanse(*derived);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    const std::uint64_t parent_version = parent->current_version;
    encryption_keys_.push_back(child);
    return DerivedEncryptionKey{child, parent_version};
}

Result<CustomerKekRotation> InMemoryStore::rotate_customer_kek(const std::string& customer_id,
                                                               const std::string& instance_kek) {
    static_cast<void>(instance_kek);
    std::lock_guard lock(mutex_);
    auto current = std::ranges::find_if(customer_keks_, [&](const CustomerKekRecord& value) {
        return value.customer_id == customer_id;
    });
    if (current == customer_keks_.end() || current->key.size() != secrets_crypto::Key{}.size()) {
        return std::unexpected(StoreError{"not_found", "no customer key exists"});
    }
    auto generated = secrets_crypto::random_key();
    if (!generated) {
        return std::unexpected(generated.error());
    }
    secrets_crypto::Key old_key{};
    std::copy(current->key.begin(), current->key.end(), old_key.begin());
    std::vector<std::string> replacements;
    replacements.reserve(data_keys_.size());
    for (const DataKeyRecord& record : data_keys_) {
        if (record.customer_id != customer_id) {
            replacements.push_back({});
            continue;
        }
        const std::string associated = "lake data key/" + customer_id + "/" + record.owner_id +
                                       "/" + std::to_string(record.version);
        auto raw = secrets_crypto::open(old_key, record.wrapped_key, associated);
        if (!raw) {
            secrets_crypto::cleanse(old_key);
            secrets_crypto::cleanse(*generated);
            return std::unexpected(raw.error());
        }
        auto wrapped = secrets_crypto::seal(*generated, *raw, associated);
        if (!wrapped) {
            secrets_crypto::cleanse(old_key);
            secrets_crypto::cleanse(*generated);
            return std::unexpected(wrapped.error());
        }
        replacements.push_back(*wrapped);
    }
    std::vector<std::pair<std::string, std::string>> authority_replacements;
    authority_replacements.reserve(pki_authorities_.size());
    for (const PkiAuthorityRecord& record : pki_authorities_) {
        if (record.customer_id != customer_id) {
            authority_replacements.emplace_back();
            continue;
        }
        const std::string root_associated = "lake pki authority/" + customer_id + "/root";
        const std::string intermediate_associated =
            "lake pki authority/" + customer_id + "/intermediate";
        auto root = secrets_crypto::open(old_key, record.root_key_ciphertext, root_associated);
        auto intermediate = secrets_crypto::open(
            old_key, record.intermediate_key_ciphertext, intermediate_associated);
        if (!root || !intermediate) {
            secrets_crypto::cleanse(old_key);
            secrets_crypto::cleanse(*generated);
            return std::unexpected(!root ? root.error() : intermediate.error());
        }
        auto wrapped_root = secrets_crypto::seal(*generated, *root, root_associated);
        auto wrapped_intermediate =
            secrets_crypto::seal(*generated, *intermediate, intermediate_associated);
        if (!wrapped_root || !wrapped_intermediate) {
            secrets_crypto::cleanse(old_key);
            secrets_crypto::cleanse(*generated);
            return std::unexpected(!wrapped_root ? wrapped_root.error()
                                                 : wrapped_intermediate.error());
        }
        authority_replacements.emplace_back(*wrapped_root, *wrapped_intermediate);
    }
    secrets_crypto::cleanse(old_key);
    std::uint64_t rewrapped = 0;
    for (std::size_t index = 0; index < data_keys_.size(); ++index) {
        if (!replacements[index].empty()) {
            data_keys_[index].wrapped_key = std::move(replacements[index]);
            ++rewrapped;
        }
    }
    for (std::size_t index = 0; index < pki_authorities_.size(); ++index) {
        if (!authority_replacements[index].first.empty()) {
            pki_authorities_[index].root_key_ciphertext =
                std::move(authority_replacements[index].first);
            pki_authorities_[index].intermediate_key_ciphertext =
                std::move(authority_replacements[index].second);
            ++rewrapped;
        }
    }
    current->key.assign(reinterpret_cast<const char*>(generated->data()), generated->size());
    secrets_crypto::cleanse(*generated);
    ++current->version;
    return CustomerKekRotation{customer_id, current->version, rewrapped};
}

Result<OfflineGrant> InMemoryStore::issue_offline_grant(const NewOfflineGrant& input) {
    if (input.id.empty() || input.user_id.empty() || input.principal_taap_id.empty() ||
        input.device_id.empty() || input.scopes.empty() || input.duration_seconds <= 0 ||
        input.duration_seconds > 604800 || !input.credential.is_object()) {
        return std::unexpected(StoreError{"invalid_input", "offline grant is incomplete"});
    }
    std::lock_guard lock(mutex_);
    const bool device_exists = std::ranges::any_of(devices_, [&](const Device& device) {
        return device.id == input.device_id && device.user_id == input.user_id &&
               device.revoked_at.empty();
    });
    if (!device_exists) {
        return std::unexpected(StoreError{"not_found", "active device not found"});
    }
    const auto now = std::chrono::system_clock::now();
    const auto expires = now + std::chrono::seconds(input.duration_seconds);
    OfflineGrant grant{input.id,
                       input.user_id,
                       input.principal_taap_id,
                       input.device_id,
                       input.scopes,
                       now_iso8601(),
                       iso8601_at(expires),
                       {},
                       input.credential};
    offline_grants_.push_back(grant);
    return grant;
}

Result<std::vector<OfflineGrant>> InMemoryStore::list_offline_grants(const std::string& user_id) {
    std::lock_guard lock(mutex_);
    std::vector<OfflineGrant> output;
    for (const OfflineGrant& grant : offline_grants_) {
        if (grant.user_id == user_id) {
            output.push_back(grant);
        }
    }
    return output;
}

Result<OfflineGrant> InMemoryStore::get_offline_grant(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(offline_grants_, id, &OfflineGrant::id);
    if (found == offline_grants_.end()) {
        return std::unexpected(StoreError{"not_found", "offline grant not found"});
    }
    return *found;
}

Result<OfflineGrant> InMemoryStore::revoke_offline_grant(const std::string& user_id,
                                                         const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(offline_grants_, id, &OfflineGrant::id);
    if (found == offline_grants_.end() || found->user_id != user_id) {
        return std::unexpected(StoreError{"not_found", "offline grant not found"});
    }
    if (found->revoked_at.empty()) {
        found->revoked_at = now_iso8601();
    }
    return *found;
}

Result<IssuedSession> InMemoryStore::recover_with_code(const std::string& pool_id,
                                                       const std::string& email,
                                                       const std::string& code,
                                                       const std::string& device_name) {
    std::lock_guard lock(mutex_);
    for (PasswordCredential& credential : passwords_) {
        if (credential.pool_id != pool_id || lower(credential.email) != lower(email)) {
            continue;
        }
        const auto recovery =
            std::ranges::find_if(credential.recovery_hashes, [&](const std::string& hash) {
                return identity_crypto::verify_secret(hash, code);
            });
        if (recovery == credential.recovery_hashes.end()) {
            break;
        }
        credential.recovery_hashes.erase(recovery);
        Device& device = add_device(devices_, credential.user_id, device_name);
        IssuedSession issued = add_session(
            sessions_, credential.user_id, pool_id, device.id, "browser", "", {"*"}, "", 86400);
        if (issued.secret.empty()) {
            return std::unexpected(StoreError{"store_error", "could not issue recovery session"});
        }
        issued.session.reason.clear();
        return issued;
    }
    return std::unexpected(StoreError{"unauthorized", "invalid email or recovery code"});
}

namespace {

void add_recovery_notifications(std::vector<RecoveryNotification>& notifications,
                                const std::vector<Device>& devices,
                                const RecoveryAttempt& attempt,
                                const std::string& kind) {
    const std::string now = now_iso8601();
    for (const Device& device : devices) {
        if (device.user_id == attempt.user_id) {
            notifications.push_back(
                {new_uuid_v4(), attempt.id, attempt.user_id, device.id, kind, now});
        }
    }
}

} // namespace

Result<RecoveryAttempt> InMemoryStore::begin_email_recovery(const std::string& pool_id,
                                                            const std::string& email,
                                                            int cooling_seconds) {
    if (cooling_seconds < 3600) {
        return std::unexpected(
            StoreError{"invalid_input", "email recovery cooling period is too short"});
    }
    std::lock_guard lock(mutex_);
    const auto password = std::ranges::find_if(passwords_, [&](const PasswordCredential& value) {
        return value.pool_id == pool_id && lower(value.email) == lower(email);
    });
    if (password == passwords_.end()) {
        return std::unexpected(StoreError{"not_found", "account not found"});
    }
    const auto now = std::chrono::system_clock::now();
    const auto ready = now + std::chrono::seconds(cooling_seconds);
    const auto expires = ready + std::chrono::hours(24);
    const std::string token = identity_crypto::random_secret();
    const std::string token_hash = identity_crypto::hash_secret(token);
    if (token.empty() || token_hash.empty()) {
        return std::unexpected(StoreError{"store_error", "could not create recovery token"});
    }
    RecoveryAttempt attempt{new_uuid_v4(),
                            password->user_id,
                            {},
                            "email",
                            "cooling",
                            iso8601_at(ready),
                            iso8601_at(expires),
                            {},
                            0};
    recovery_attempts_.push_back({attempt, token_hash, {}, ready, expires});
    recovery_deliveries_.push_back(
        {new_uuid_v4(), attempt.id, email, token, "email_recovery_started", now_iso8601(), {}});
    add_recovery_notifications(
        recovery_notifications_, devices_, attempt, "email_recovery_started");
    return attempt;
}

Result<RecoveryAttempt> InMemoryStore::begin_admin_recovery(const std::string& organization_id,
                                                            const std::string& user_id,
                                                            const std::string& requested_by) {
    std::lock_guard lock(mutex_);
    const auto requester = std::ranges::find_if(memberships_, [&](const Membership& membership) {
        return membership.organization_id == organization_id &&
               membership.principal_taap_id == requested_by &&
               (membership.role == "owner" || membership.role == "admin");
    });
    const auto target = std::ranges::find_if(memberships_, [&](const Membership& membership) {
        return membership.organization_id == organization_id &&
               membership.principal_type == "user" && membership.principal_id == user_id;
    });
    if (requester == memberships_.end()) {
        return std::unexpected(
            StoreError{"unauthorized", "requester is not an organization admin"});
    }
    if (target == memberships_.end()) {
        return std::unexpected(StoreError{"not_found", "target user is not in the organization"});
    }
    const auto now = std::chrono::system_clock::now();
    const auto expires = now + std::chrono::hours(24);
    RecoveryAttempt attempt{new_uuid_v4(),
                            user_id,
                            organization_id,
                            "admin",
                            "pending_approval",
                            iso8601_at(now),
                            iso8601_at(expires),
                            {},
                            1};
    recovery_attempts_.push_back({attempt, {}, {requested_by}, now, expires});
    add_recovery_notifications(
        recovery_notifications_, devices_, attempt, "admin_recovery_requested");
    return attempt;
}

Result<RecoveryAttempt> InMemoryStore::get_recovery_attempt(const std::string& recovery_id) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find_if(recovery_attempts_, [&](const RecoveryRecord& value) {
        return value.attempt.id == recovery_id;
    });
    if (found == recovery_attempts_.end()) {
        return std::unexpected(StoreError{"not_found", "recovery attempt not found"});
    }
    return found->attempt;
}

Result<RecoveryAttempt> InMemoryStore::approve_admin_recovery(const std::string& recovery_id,
                                                              const std::string& approved_by) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find_if(recovery_attempts_, [&](const RecoveryRecord& value) {
        return value.attempt.id == recovery_id;
    });
    if (found == recovery_attempts_.end() || found->attempt.method != "admin") {
        return std::unexpected(StoreError{"not_found", "admin recovery not found"});
    }
    const auto admin = std::ranges::find_if(memberships_, [&](const Membership& membership) {
        return membership.organization_id == found->attempt.organization_id &&
               membership.principal_taap_id == approved_by &&
               (membership.role == "owner" || membership.role == "admin");
    });
    if (admin == memberships_.end()) {
        return std::unexpected(StoreError{"unauthorized", "approver is not an organization admin"});
    }
    if (std::ranges::find(found->approvals, approved_by) != found->approvals.end()) {
        return std::unexpected(
            StoreError{"conflict", "a different organization admin must approve recovery"});
    }
    if (std::chrono::system_clock::now() >= found->expires_at) {
        return std::unexpected(StoreError{"conflict", "admin recovery has expired"});
    }
    found->approvals.push_back(approved_by);
    found->attempt.approval_count = static_cast<int>(found->approvals.size());
    if (found->approvals.size() == 2) {
        const std::string token = identity_crypto::random_secret();
        found->token_hash = identity_crypto::hash_secret(token);
        if (token.empty() || found->token_hash.empty()) {
            return std::unexpected(StoreError{"store_error", "could not create recovery token"});
        }
        found->attempt.status = "approved";
        const auto user = std::ranges::find(users_, found->attempt.user_id, &User::id);
        if (user == users_.end()) {
            return std::unexpected(StoreError{"not_found", "target user not found"});
        }
        recovery_deliveries_.push_back({new_uuid_v4(),
                                        found->attempt.id,
                                        user->email,
                                        token,
                                        "admin_recovery_approved",
                                        now_iso8601(),
                                        {}});
        add_recovery_notifications(
            recovery_notifications_, devices_, found->attempt, "admin_recovery_approved");
    }
    return found->attempt;
}

Result<IssuedSession> InMemoryStore::complete_recovery(const std::string& recovery_id,
                                                       const std::string& token,
                                                       const std::string& device_name) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find_if(recovery_attempts_, [&](const RecoveryRecord& value) {
        return value.attempt.id == recovery_id;
    });
    if (found == recovery_attempts_.end() || found->attempt.status == "completed" ||
        found->token_hash.empty() || !identity_crypto::verify_secret(found->token_hash, token)) {
        return std::unexpected(StoreError{"unauthorized", "invalid recovery token"});
    }
    const auto now = std::chrono::system_clock::now();
    if (now < found->ready_at) {
        return std::unexpected(StoreError{"conflict", "recovery cooling period has not ended"});
    }
    if (now >= found->expires_at) {
        return std::unexpected(StoreError{"conflict", "recovery token has expired"});
    }
    if (found->attempt.method == "admin" && found->approvals.size() != 2) {
        return std::unexpected(StoreError{"conflict", "two administrator approvals are required"});
    }
    const std::string revoked_at = now_iso8601();
    for (Session& session : sessions_) {
        if (session.user_id == found->attempt.user_id && session.revoked_at.empty()) {
            session.revoked_at = revoked_at;
        }
    }
    const auto password = std::ranges::find_if(passwords_, [&](const PasswordCredential& value) {
        return value.user_id == found->attempt.user_id;
    });
    if (password == passwords_.end()) {
        return std::unexpected(StoreError{"not_found", "password credential not found"});
    }
    found->attempt.status = "completed";
    found->attempt.completed_at = revoked_at;
    add_recovery_notifications(
        recovery_notifications_, devices_, found->attempt, "account_recovery_completed");
    Device& device = add_device(devices_, found->attempt.user_id, device_name);
    IssuedSession issued = add_session(sessions_,
                                       found->attempt.user_id,
                                       password->pool_id,
                                       device.id,
                                       "browser",
                                       "",
                                       {"*"},
                                       "",
                                       86400);
    if (issued.secret.empty()) {
        return std::unexpected(StoreError{"store_error", "could not issue recovery session"});
    }
    issued.session.reason.clear();
    return issued;
}

Result<std::vector<RecoveryDelivery>> InMemoryStore::claim_recovery_deliveries(int limit) {
    std::lock_guard lock(mutex_);
    std::vector<RecoveryDelivery> output;
    for (RecoveryDelivery& delivery : recovery_deliveries_) {
        if (!delivery.delivered_at.empty()) {
            continue;
        }
        delivery.delivered_at = now_iso8601();
        output.push_back(delivery);
        delivery.token.clear();
        if (output.size() == static_cast<std::size_t>(limit)) {
            break;
        }
    }
    return output;
}

Result<std::vector<RecoveryNotification>>
InMemoryStore::list_recovery_notifications(const std::string& user_id) {
    std::lock_guard lock(mutex_);
    std::vector<RecoveryNotification> output;
    for (const RecoveryNotification& notification : recovery_notifications_) {
        if (notification.user_id == user_id) {
            output.push_back(notification);
        }
    }
    return output;
}

Result<RecoveryContact> InMemoryStore::configure_recovery_contact(const RecoveryContact& input) {
    if (input.repository_id.empty() || input.contact_taap_id.empty() || input.wrapped_key.empty()) {
        return std::unexpected(StoreError{"invalid_input", "recovery contact is incomplete"});
    }
    std::lock_guard lock(mutex_);
    if (std::ranges::none_of(repositories_, [&](const Repository& value) {
            return value.id == input.repository_id;
        })) {
        return std::unexpected(StoreError{"not_found", "repository not found"});
    }
    const auto found =
        std::ranges::find(recovery_contacts_, input.repository_id, &RecoveryContact::repository_id);
    const std::string now = now_iso8601();
    if (found != recovery_contacts_.end()) {
        found->contact_taap_id = input.contact_taap_id;
        found->wrapped_key = input.wrapped_key;
        found->updated_at = now;
        return *found;
    }
    recovery_contacts_.push_back(
        {input.repository_id, input.contact_taap_id, input.wrapped_key, now, now});
    return recovery_contacts_.back();
}

Result<RecoveryContact> InMemoryStore::get_recovery_contact(const std::string& repository_id) {
    std::lock_guard lock(mutex_);
    const auto found =
        std::ranges::find(recovery_contacts_, repository_id, &RecoveryContact::repository_id);
    if (found == recovery_contacts_.end()) {
        return std::unexpected(StoreError{"not_found", "repository has no recovery contact"});
    }
    return *found;
}

} // namespace lake::control
