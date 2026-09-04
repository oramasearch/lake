#pragma once
#include <chrono>
#include <mutex>
#include <vector>

#include "lake/control/store.hpp"

namespace lake::control {

// In-memory store enforcing the same constraints as db/schema.sql: one customer
// per identity, individual xor organization, one global handle namespace, the
// handle format.
class InMemoryStore final : public Store {
public:
    std::string kind() const override {
        return "memory";
    }
    Result<void> ping() override;
    Result<User> create_user(const NewUser&) override;
    Result<std::vector<User>> list_users(int limit) override;
    Result<User> get_user(const std::string& id) override;
    Result<Organization> create_organization(const NewOrganization&) override;
    Result<std::vector<Organization>> list_organizations(int limit) override;
    Result<Organization> get_organization(const std::string& id) override;
    Result<Customer> create_customer(const NewCustomer&) override;
    Result<std::vector<Customer>> list_customers(int limit) override;
    Result<Customer> get_customer(const std::string& id) override;
    Result<Repository> create_repository(const NewRepository&) override;
    Result<std::vector<Repository>> list_repositories(const std::string& customer_id,
                                                      int limit) override;
    Result<Repository> get_repository(const std::string& id) override;
    Result<Repository> rename_repository(const std::string& id, const std::string& name) override;
    Result<Repository> transfer_repository(const std::string& id,
                                           const std::string& customer_id) override;
    Result<Repository> set_repository_visibility(const std::string& id,
                                                 const std::string& visibility) override;
    Result<Repository> delete_repository(const std::string& id,
                                         const RepositoryDeletion& deletion) override;
    Result<std::vector<RepositoryRef>>
    list_repository_refs(const std::string& repository_id) override;
    Result<RepositoryRef> set_repository_ref(const std::string& repository_id,
                                             const std::string& name,
                                             const std::string& head) override;
    Result<Team> create_team(const NewTeam&) override;
    Result<std::vector<Team>> list_teams(const std::string& organization_id, int limit) override;
    Result<Team> get_team(const std::string& id) override;
    Result<Membership> create_membership(const NewMembership&) override;
    Result<std::vector<Membership>> list_memberships(const std::string& organization_id,
                                                     const std::string& team_id,
                                                     int limit) override;
    Result<Membership> accept_invitation(const AcceptInvitation&) override;
    Result<Invitation> create_invitation(const NewInvitation&) override;
    Result<std::vector<Invitation>> list_invitations(const std::string& organization_id,
                                                     const std::string& team_id,
                                                     int limit) override;
    Result<Invitation> get_invitation(const std::string& id) override;
    Result<PolicyObject> create_policy_object(const NewPolicyObject&) override;
    Result<PolicyObject> get_policy_object(const std::string& id) override;
    Result<PolicyObject> find_policy_object(const std::string& object_type,
                                            const std::string& external_id) override;
    Result<std::vector<PolicyObject>> list_policy_objects(const std::string& parent_id,
                                                          const std::string& object_type,
                                                          int limit) override;
    Result<std::vector<ObjectPathEntry>>
    policy_paths(const std::vector<std::string>& requested_object_ids) override;
    Result<Grant> create_grant(const NewGrant&) override;
    Result<std::vector<Grant>> list_grants(const std::vector<std::string>& object_ids,
                                           const std::optional<std::string>& principal_taap_id,
                                           int limit) override;
    audit::Result<audit::Identity> current_lake_identity() override;
    audit::Result<audit::Identity> lake_identity_by_taap_id(const std::string& taap_id) override;
    audit::Result<void> create_lake_identity(const audit::Identity& identity) override;
    audit::Result<void> rotate_lake_identity(const audit::Identity& previous,
                                             const audit::Identity& replacement) override;
    Result<void> append_audit_event(const audit::Event& event) override;
    Result<audit::Event> latest_audit_event() override;
    Result<std::vector<audit::Event>> list_audit_events(const audit::Filter& filter) override;
    Result<void> set_audit_retention_days(const std::string& customer_id, int days) override;
    Result<SecretMount> create_secret_mount(const NewSecretMount& value,
                                            const std::string& instance_kek) override;
    Result<std::vector<SecretMount>> list_secret_mounts(const std::string& customer_id,
                                                        const std::string& organization_id,
                                                        int limit) override;
    Result<SecretMount> get_secret_mount(const std::string& id) override;
    Result<SecretVersion> put_secret(const PutSecret& value,
                                     const std::string& instance_kek) override;
    Result<SecretVersion> read_secret(const std::string& mount_id,
                                      const std::string& name,
                                      std::uint64_t version,
                                      const std::string& instance_kek) override;
    Result<SecretMetadata> secret_metadata(const std::string& mount_id,
                                           const std::string& name) override;
    Result<SecretMetadata> change_secret_versions(const std::string& mount_id,
                                                  const std::string& name,
                                                  const std::vector<std::uint64_t>& versions,
                                                  SecretVersionAction action) override;
    Result<EncryptionKey> create_encryption_key(const NewEncryptionKey& value,
                                                const std::string& instance_kek) override;
    Result<std::vector<EncryptionKey>> list_encryption_keys(const std::string& mount_id,
                                                            int limit) override;
    Result<EncryptionKey> get_encryption_key(const std::string& mount_id,
                                             const std::string& name) override;
    Result<EncryptionKey> rotate_encryption_key(const std::string& mount_id,
                                                const std::string& name,
                                                const std::string& instance_kek) override;
    Result<TransitValue> encrypt(const std::string& mount_id,
                                 const std::string& name,
                                 const std::string& plaintext,
                                 const std::string& instance_kek) override;
    Result<TransitValue> decrypt(const std::string& mount_id,
                                 const std::string& name,
                                 const std::string& ciphertext,
                                 const std::string& instance_kek) override;
    Result<TransitValue> rewrap(const std::string& mount_id,
                                const std::string& name,
                                const std::string& ciphertext,
                                const std::string& instance_kek) override;
    Result<TransitValue> sign(const std::string& mount_id,
                              const std::string& name,
                              const std::string& message,
                              std::uint64_t version,
                              const std::string& instance_kek) override;
    Result<TransitVerification> verify(const std::string& mount_id,
                                       const std::string& name,
                                       const std::string& message,
                                       const std::string& signature,
                                       std::uint64_t version,
                                       const std::string& instance_kek) override;
    Result<TransitValue> hmac(const std::string& mount_id,
                              const std::string& name,
                              const std::string& message,
                              std::uint64_t version,
                              const std::string& instance_kek) override;
    Result<DerivedEncryptionKey> derive_encryption_key(const std::string& mount_id,
                                                       const std::string& parent_name,
                                                       const std::string& name,
                                                       const std::string& context,
                                                       const std::string& instance_kek) override;
    Result<CustomerKekRotation> rotate_customer_kek(const std::string& customer_id,
                                                    const std::string& instance_kek) override;
    Result<IssuedSession> sign_up_password(const NewPasswordUser&) override;
    Result<IssuedSession> sign_in_password(const PasswordSignIn&) override;
    Result<MfaEnrollment> enroll_mfa(const std::string& user_id,
                                     const std::string& pool_id) override;
    Result<void> disable_mfa(const std::string& user_id,
                             const std::string& pool_id,
                             const std::string& code) override;
    Result<IssuedSession> sign_in_totp(const PasswordSignIn&) override;
    Result<WebAuthnChallenge> begin_passkey_registration(const std::string& user_id,
                                                         const std::string& pool_id,
                                                         const std::string& rp_id,
                                                         const std::string& origin) override;
    Result<void> register_passkey(const PasskeyRegistration&) override;
    Result<WebAuthnChallenge> begin_passkey_sign_in(const std::string& pool_id,
                                                    const std::string& rp_id,
                                                    const std::string& origin) override;
    Result<IssuedSession> sign_in_passkey(const PasskeyAssertion&) override;
    Result<IssuedSession> sign_in_oauth(const OAuthIdentity&) override;
    Result<void> register_oauth_provider(const OAuthProvider&) override;
    Result<OAuthProvider> get_oauth_provider(const std::string& pool_id,
                                             const std::string& provider) override;
    Result<void> begin_oauth(const OAuthFlow&) override;
    Result<OAuthFlow> consume_oauth(const std::string& provider, const std::string& state) override;
    Result<IssuedSession> create_token(const NewToken&) override;
    Result<std::vector<Session>> list_sessions(const std::string& user_id,
                                               const std::string& pool_id) override;
    Result<void> revoke_session(const std::string& user_id, const std::string& session_id) override;
    Result<std::vector<Device>> list_devices(const std::string& user_id) override;
    Result<void> revoke_device(const std::string& user_id, const std::string& device_id) override;
    Result<Principal> context_for(const std::string& secret) override;

private:
    Result<std::string> customer_kek_locked(const std::string& customer_id, bool create);
    Result<std::string> data_key_locked(const std::string& customer_id,
                                        const std::string& owner_id,
                                        std::uint64_t version);
    Result<void> store_data_key_locked(const std::string& customer_id,
                                       const std::string& owner_id,
                                       std::uint64_t version,
                                       const std::string& key);
    std::mutex mutex_;
    std::vector<User> users_;
    std::vector<Organization> organizations_;
    std::vector<Customer> customers_;
    std::vector<Repository> repositories_;
    std::vector<RepositoryRef> repository_refs_;
    std::vector<Team> teams_;
    std::vector<Membership> memberships_;
    std::vector<Invitation> invitations_;
    std::vector<PolicyObject> policy_objects_;
    std::vector<Grant> grants_;
    std::vector<audit::Identity> identities_;
    std::vector<audit::Event> audit_events_;
    std::vector<std::pair<std::string, int>> audit_retentions_;
    struct CustomerKekRecord {
        std::string customer_id;
        std::string key;
        std::uint64_t version = 1;
    };
    struct DataKeyRecord {
        std::string customer_id;
        std::string owner_id;
        std::uint64_t version = 1;
        std::string wrapped_key;
    };
    struct SecretVersionRecord {
        SecretVersion version;
        std::string ciphertext;
    };
    struct SecretEntryRecord {
        SecretMetadata metadata;
        std::vector<SecretVersionRecord> versions;
    };
    std::vector<CustomerKekRecord> customer_keks_;
    std::vector<DataKeyRecord> data_keys_;
    std::vector<SecretMount> secret_mounts_;
    std::vector<SecretEntryRecord> secret_entries_;
    std::vector<EncryptionKey> encryption_keys_;
    struct PasswordCredential {
        std::string user_id, pool_id, email, password, totp_secret;
        std::vector<std::string> recovery_hashes;
    };
    struct PasskeyCredential {
        std::string user_id, pool_id, credential_id, public_key_pem, name, device_id, revoked_at;
        nlohmann::json binding_attestation;
        std::uint32_t sign_count = 0;
    };
    struct OAuthCredential {
        std::string user_id, pool_id, subject;
    };
    struct WebAuthnChallengeRecord {
        WebAuthnChallenge challenge;
        std::chrono::steady_clock::time_point expires_at;
        bool used = false;
    };
    std::vector<PasswordCredential> passwords_;
    std::vector<PasskeyCredential> passkeys_;
    std::vector<OAuthCredential> oauth_credentials_;
    std::vector<WebAuthnChallengeRecord> webauthn_challenges_;
    std::vector<OAuthProvider> oauth_providers_;
    struct OAuthFlowRecord {
        OAuthFlow flow;
        std::chrono::steady_clock::time_point expires_at;
        bool used = false;
    };
    std::vector<OAuthFlowRecord> oauth_flows_;
    std::vector<Device> devices_;
    std::vector<Session> sessions_;
    std::vector<std::pair<std::string, std::string>> session_secrets_;
};

std::string new_uuid_v4();
std::string now_iso8601();

} // namespace lake::control
