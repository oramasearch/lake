#pragma once
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/audit/audit.hpp"

// The control-plane store: users and organizations (identity), customers (the
// tenant that owns everything). One interface, two implementations — Postgres
// (production, the schema in db/schema.sql) and in-memory (tests, local demos).
// No HTTP type, no libpq type: the Postgres implementation lives in its own
// library.
namespace lake::control {

struct StoreError {
    std::string code; // not_found | conflict | invalid_input | store_unavailable
                      // | store_error
    std::string message;
};
template <class T> using Result = std::expected<T, StoreError>;

struct User {
    std::string id, taap_id, email, display_name, created_at, updated_at;
};
struct Organization {
    std::string id, taap_id, display_name, created_at, updated_at;
};
struct Customer {
    std::string id, customer_type, user_id, organization_id, name, created_at, updated_at;
};
struct Repository {
    std::string id, customer_id, name, description, visibility, deleted_at, retention_until,
        created_at, updated_at;
};
struct RepositoryRef {
    std::string repository_id, name, head, updated_at;
};
struct RepositoryDeletion {
    std::string deleted_at, retention_until;
};
struct Team {
    std::string id, organization_id, name, display_name, created_at, updated_at;
};
struct Membership {
    std::string id, organization_id, team_id, principal_type, principal_id, principal_taap_id, role,
        attestation_digest, granted_by_taap_id, created_at, updated_at;
};
struct Invitation {
    std::string id, organization_id, team_id, invitee_type, invitee, role, expires_at, status,
        accepted_membership_id, created_by_taap_id, created_at, updated_at;
};
struct PolicyObject {
    std::string id, object_type, external_id, parent_id, created_at, updated_at;
    bool inherit = true;
};
struct Grant {
    std::string id, principal_taap_id, object_id, role, attestation_digest, granted_by_taap_id,
        created_at, updated_at;
};
struct ObjectPathEntry {
    std::string requested_object_id;
    PolicyObject object;
    int depth = 0;
};
struct NewUser {
    std::string taap_id, email, display_name;
};
struct NewOrganization {
    std::string taap_id, display_name, customer_id;
};
struct NewCustomer {
    std::string customer_type, user_id, organization_id, name;
};
struct NewRepository {
    std::string customer_id, name, description,
        visibility; // visibility empty = private
};
struct NewTeam {
    std::string organization_id, name, display_name, acting_principal, request_id;
};
struct NewMembership {
    std::string organization_id, team_id, principal_type, principal_id, principal_taap_id, role,
        attestation_digest, granted_by_taap_id, request_id;
};
struct NewInvitation {
    std::string organization_id, team_id, invitee_type, invitee, role, expires_at,
        created_by_taap_id, request_id;
};
struct AcceptInvitation {
    std::string invitation_id, principal_type, principal_id, principal_taap_id, attestation_digest,
        accepted_by_taap_id, request_id;
};
struct NewPolicyObject {
    std::string object_type, external_id, parent_id, acting_principal, request_id;
    bool inherit = true;
};
struct NewGrant {
    std::string principal_taap_id, object_id, role, attestation_digest, granted_by_taap_id,
        request_id;
};

// Identity-core records deliberately carry the same pool_id from the first
// lake-owned pool. The auth-as-a-service work can therefore add pools without
// duplicating identity tables.
struct Device {
    std::string id, user_id, name, revoked_at, created_at, updated_at;
};
struct Session {
    std::string id, user_id, device_id, pool_id, kind, name, reason, expires_at, revoked_at,
        created_at, last_used_at;
    std::vector<std::string> scopes;
};
struct IssuedSession {
    Session session;
    std::string secret;
};
struct Principal {
    std::string user_id, taap_id, session_id, device_id, pool_id, kind;
    std::vector<std::string> scopes;
};
struct NewPasswordUser {
    std::string pool_id, email, password, display_name, device_name, identity_document;
};
struct PasswordSignIn {
    std::string pool_id, email, password, totp_code, device_name;
};
struct MfaEnrollment {
    std::string secret;
    std::vector<std::string> recovery_codes;
};
struct NewToken {
    std::string user_id, pool_id, name, reason;
    std::vector<std::string> scopes;
    int expires_in_seconds = 0;
    bool is_static = false;
};
struct WebAuthnChallenge {
    std::string user_id, pool_id, ceremony, challenge, rp_id, origin;
};
struct PasskeyRegistration {
    std::string user_id, pool_id, challenge, client_data_json, attestation_object, name;
};
struct PasskeyAssertion {
    std::string pool_id, challenge, credential_id, client_data_json, authenticator_data, signature,
        device_name;
};
struct OAuthIdentity {
    std::string pool_id, provider, subject, email, display_name, device_name, identity_document;
};
struct OAuthProvider {
    std::string pool_id, provider, issuer, client_id, client_secret;
    std::vector<std::string> scopes;
};
struct OAuthFlow {
    std::string pool_id, provider, state, code_verifier, nonce, device_name, identity_document;
};

nlohmann::json to_json(const User&);
nlohmann::json to_json(const Organization&);
nlohmann::json to_json(const Customer&);
nlohmann::json to_json(const Repository&);
nlohmann::json to_json(const RepositoryRef&);
nlohmann::json to_json(const Team&);
nlohmann::json to_json(const Membership&);
nlohmann::json to_json(const Invitation&);
nlohmann::json to_json(const PolicyObject&);
nlohmann::json to_json(const Grant&);
std::string new_uuid_v4();
std::string now_iso8601();
nlohmann::json to_json(const Device&);
nlohmann::json to_json(const Session&);

class Store {
public:
    virtual ~Store() = default;
    [[nodiscard]] virtual std::string kind() const = 0; // "postgres" | "memory"
    virtual Result<void> ping() = 0;

    virtual Result<User> create_user(const NewUser&) = 0;
    virtual Result<std::vector<User>> list_users(int limit) = 0;
    virtual Result<User> get_user(const std::string& id) = 0;

    virtual Result<Organization> create_organization(const NewOrganization&) = 0;
    virtual Result<std::vector<Organization>> list_organizations(int limit) = 0;
    virtual Result<Organization> get_organization(const std::string& id) = 0;

    virtual Result<Customer> create_customer(const NewCustomer&) = 0;
    virtual Result<std::vector<Customer>> list_customers(int limit) = 0;
    virtual Result<Customer> get_customer(const std::string& id) = 0;

    virtual Result<Repository> create_repository(const NewRepository&) = 0;
    // customer_id empty = every customer's repositories.
    virtual Result<std::vector<Repository>> list_repositories(const std::string& customer_id,
                                                              int limit) = 0;
    virtual Result<Repository> get_repository(const std::string& id) = 0;
    virtual Result<Repository> rename_repository(const std::string& id,
                                                 const std::string& name) = 0;
    virtual Result<Repository> transfer_repository(const std::string& id,
                                                   const std::string& customer_id) = 0;
    virtual Result<Repository> set_repository_visibility(const std::string& id,
                                                         const std::string& visibility) = 0;
    virtual Result<Repository> delete_repository(const std::string& id,
                                                 const RepositoryDeletion& deletion) = 0;
    virtual Result<std::vector<RepositoryRef>>
    list_repository_refs(const std::string& repository_id) = 0;
    virtual Result<RepositoryRef> set_repository_ref(const std::string& repository_id,
                                                     const std::string& name,
                                                     const std::string& head) = 0;

    virtual Result<Team> create_team(const NewTeam&) = 0;
    virtual Result<std::vector<Team>> list_teams(const std::string& organization_id, int limit) = 0;
    virtual Result<Team> get_team(const std::string& id) = 0;

    virtual Result<Membership> create_membership(const NewMembership&) = 0;
    virtual Result<std::vector<Membership>>
    list_memberships(const std::string& organization_id, const std::string& team_id, int limit) = 0;
    virtual Result<Membership> accept_invitation(const AcceptInvitation&) = 0;

    virtual Result<Invitation> create_invitation(const NewInvitation&) = 0;
    virtual Result<std::vector<Invitation>>
    list_invitations(const std::string& organization_id, const std::string& team_id, int limit) = 0;
    virtual Result<Invitation> get_invitation(const std::string& id) = 0;

    virtual Result<PolicyObject> create_policy_object(const NewPolicyObject&) = 0;
    virtual Result<PolicyObject> get_policy_object(const std::string& id) = 0;
    virtual Result<PolicyObject> find_policy_object(const std::string& object_type,
                                                    const std::string& external_id) = 0;
    virtual Result<std::vector<PolicyObject>> list_policy_objects(const std::string& parent_id,
                                                                  const std::string& object_type,
                                                                  int limit) = 0;
    virtual Result<std::vector<ObjectPathEntry>>
    policy_paths(const std::vector<std::string>& requested_object_ids) = 0;

    virtual Result<Grant> create_grant(const NewGrant&) = 0;
    virtual Result<std::vector<Grant>>
    list_grants(const std::vector<std::string>& object_ids,
                const std::optional<std::string>& principal_taap_id,
                int limit) = 0;

    virtual audit::Result<audit::Identity> current_lake_identity() = 0;
    virtual audit::Result<audit::Identity> lake_identity_by_taap_id(const std::string& taap_id) = 0;
    virtual audit::Result<void> create_lake_identity(const audit::Identity& identity) = 0;
    virtual audit::Result<void> rotate_lake_identity(const audit::Identity& previous,
                                                     const audit::Identity& replacement) = 0;

    virtual Result<void> append_audit_event(const audit::Event& event) = 0;
    virtual Result<audit::Event> latest_audit_event() = 0;
    virtual Result<std::vector<audit::Event>> list_audit_events(const audit::Filter& filter) = 0;
    virtual Result<void> set_audit_retention_days(const std::string& customer_id, int days) = 0;

    // Identity core. All bearer and cookie values are resolved here on every
    // request so a revoked session or device is rejected immediately.
    virtual Result<IssuedSession> sign_up_password(const NewPasswordUser&) = 0;
    virtual Result<IssuedSession> sign_in_password(const PasswordSignIn&) = 0;
    virtual Result<void> enroll_totp(const std::string& user_id,
                                     const std::string& pool_id,
                                     const std::string& secret) = 0;
    virtual Result<MfaEnrollment> enroll_mfa(const std::string& user_id,
                                             const std::string& pool_id) = 0;
    virtual Result<void> disable_mfa(const std::string& user_id,
                                     const std::string& pool_id,
                                     const std::string& code) = 0;
    virtual Result<IssuedSession> sign_in_totp(const PasswordSignIn&) = 0;
    virtual Result<WebAuthnChallenge> begin_passkey_registration(const std::string& user_id,
                                                                 const std::string& pool_id,
                                                                 const std::string& rp_id,
                                                                 const std::string& origin) = 0;
    virtual Result<void> register_passkey(const PasskeyRegistration&) = 0;
    virtual Result<WebAuthnChallenge> begin_passkey_sign_in(const std::string& pool_id,
                                                            const std::string& rp_id,
                                                            const std::string& origin) = 0;
    virtual Result<IssuedSession> sign_in_passkey(const PasskeyAssertion&) = 0;
    virtual Result<IssuedSession> sign_in_oauth(const OAuthIdentity&) = 0;
    virtual Result<void> register_oauth_provider(const OAuthProvider&) = 0;
    virtual Result<OAuthProvider> get_oauth_provider(const std::string& pool_id,
                                                     const std::string& provider) = 0;
    virtual Result<void> begin_oauth(const OAuthFlow&) = 0;
    virtual Result<OAuthFlow> consume_oauth(const std::string& provider,
                                            const std::string& state) = 0;
    virtual Result<IssuedSession> create_token(const NewToken&) = 0;
    virtual Result<std::vector<Session>> list_sessions(const std::string& user_id,
                                                       const std::string& pool_id) = 0;
    virtual Result<void> revoke_session(const std::string& user_id,
                                        const std::string& session_id) = 0;
    virtual Result<std::vector<Device>> list_devices(const std::string& user_id) = 0;
    virtual Result<void> revoke_device(const std::string& user_id,
                                       const std::string& device_id) = 0;
    virtual Result<Principal> context_for(const std::string& secret) = 0;
};

} // namespace lake::control
