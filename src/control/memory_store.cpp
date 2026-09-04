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

std::string now_iso8601() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%FT%T}Z", now);
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
    if (!provider_exists) {
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

} // namespace lake::control
