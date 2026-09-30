#pragma once
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "lake/control/platform_store.hpp"
#include "lake/control/store.hpp"

namespace lake::control {

// In-memory store enforcing the same constraints as db/schema.sql: one customer
// per identity, individual xor organization, one global handle namespace, the
// handle format.
class InMemoryStore final : public Store, public PlatformStore {
public:
    std::string kind() const override {
        return "memory";
    }
    Result<void> ping() override;
    Result<PlatformEnvironment> ensure_platform_environment(const std::string& repository_id,
                                                            const std::string& name) override;
    Result<PlatformEnvironment> get_platform_environment(const std::string& repository_id,
                                                         const std::string& name) override;
    Result<std::vector<PlatformEnvironment>>
    list_platform_environments(const std::string& repository_id) override;
    Result<void> upsert_platform_mirror(const PlatformMirrorRow& row) override;
    Result<std::vector<PlatformMirrorRow>> list_platform_mirror(const std::string& kind) override;
    Result<User> create_user(const NewUser&) override;
    Result<std::vector<User>> list_users(int limit) override;
    Result<User> get_user(const std::string& id) override;
    Result<Organization> create_organization(const NewOrganization&) override;
    Result<std::vector<Organization>> list_organizations(int limit) override;
    Result<Organization> get_organization(const std::string& id) override;
    Result<Customer> create_customer(const NewCustomer&) override;
    Result<std::vector<Customer>> list_customers(int limit,
                                                 const std::string& user_id = {}) override;
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
    Result<ForgeRecord> create_forge_record(const NewForgeRecord&) override;
    Result<ForgeRecord> get_forge_record(const std::string& id) override;
    Result<std::vector<ForgeRecord>> list_forge_records(const std::string& repository_id,
                                                        const std::string& kind,
                                                        int limit) override;
    Result<ForgeRecord> update_forge_record(const std::string& id,
                                            const nlohmann::json& value) override;
    Result<DriveShareLink> create_drive_share_link(const NewDriveShareLink&) override;
    Result<DriveShareLink> get_drive_share_link(const std::string& id) override;
    Result<DriveShareLink>
    get_drive_share_link_by_token_hash(const std::string& token_hash) override;
    Result<std::vector<DriveShareLink>> list_drive_share_links(const std::string& repository_id,
                                                               const std::string& object_id,
                                                               int limit) override;
    Result<DriveShareLink> revoke_drive_share_link(const std::string& id) override;
    Result<Team> create_team(const NewTeam&) override;
    Result<std::vector<Team>> list_teams(const std::string& organization_id, int limit) override;
    Result<Team> get_team(const std::string& id) override;
    Result<Membership> create_membership(const NewMembership&) override;
    Result<Membership> get_membership(const std::string& id) override;
    Result<std::vector<Membership>> list_memberships(const std::string& organization_id,
                                                     const std::string& team_id,
                                                     int limit) override;
    Result<Membership> set_member_role(const std::string& id,
                                       const std::string& role,
                                       const std::string& actor) override;
    Result<void> remove_member(const std::string& id, const std::string& actor) override;
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
    Result<Application> create_application(const NewApplication& value) override;
    Result<std::vector<Application>> list_applications(const std::string& customer_id,
                                                       int limit) override;
    Result<Application> get_application(const std::string& id) override;
    Result<Application> get_application_by_pool(const std::string& pool_id) override;
    Result<Application>
    get_application_by_domain(const std::string& custom_sign_in_domain) override;
    Result<Application> update_application_theme(const std::string& id,
                                                 const nlohmann::json& theme) override;
    Result<ApplicationSigningKey>
    rotate_application_signing_key(const ApplicationSigningKey& value) override;
    Result<ApplicationSigningKey>
    current_application_signing_key(const std::string& application_id) override;
    Result<std::vector<ApplicationSigningKey>>
    list_application_signing_keys(const std::string& application_id) override;
    Result<ApplicationUser> register_application_user(const std::string& application_id,
                                                      const std::string& user_id) override;
    Result<ApplicationUser> get_application_user(const std::string& application_id,
                                                 const std::string& user_id) override;
    Result<std::vector<ApplicationUser>> list_application_users(const std::string& application_id,
                                                                const std::string& query,
                                                                int limit) override;
    Result<ApplicationUser> set_application_user_status(const std::string& application_id,
                                                        const std::string& user_id,
                                                        const std::string& status) override;
    Result<void> delete_application_user(const std::string& application_id,
                                         const std::string& user_id) override;
    Result<ApplicationOrganization>
    create_application_organization(const NewApplicationOrganization& value) override;
    Result<std::vector<ApplicationOrganization>>
    list_application_organizations(const std::string& application_id, int limit) override;
    Result<ApplicationOrganization> update_application_organization(
        const std::string& id, const std::string& name, const nlohmann::json& metadata) override;
    Result<void> delete_application_organization(const std::string& id) override;
    Result<ApplicationWebhook>
    create_application_webhook(const NewApplicationWebhook& value) override;
    Result<ApplicationWebhook> get_application_webhook(const std::string& id) override;
    Result<std::vector<ApplicationWebhook>>
    list_application_webhooks(const std::string& application_id, int limit) override;
    Result<ApplicationWebhookEvent>
    create_application_webhook_event(const std::string& application_id,
                                     const std::string& event,
                                     const nlohmann::json& payload) override;
    Result<ApplicationWebhookEvent> get_application_webhook_event(const std::string& id) override;
    Result<std::vector<ApplicationWebhookEvent>>
    list_application_webhook_events(const std::string& application_id, int limit) override;
    Result<ApplicationWebhookDelivery>
    create_application_webhook_delivery(const NewApplicationWebhookDelivery& value) override;
    Result<std::vector<ApplicationWebhookDelivery>>
    list_application_webhook_deliveries(const std::string& event_id) override;
    Result<void>
    create_application_authorization_code(const ApplicationAuthorizationCode& value) override;
    Result<ApplicationAuthorizationCode>
    consume_application_authorization_code(const std::string& application_id,
                                           const std::string& code_hash) override;
    audit::Result<audit::Identity> current_lake_identity() override;
    audit::Result<audit::Identity> lake_identity_by_taap_id(const std::string& taap_id) override;
    audit::Result<void> create_lake_identity(const audit::Identity& identity) override;
    audit::Result<void> rotate_lake_identity(const audit::Identity& previous,
                                             const audit::Identity& replacement) override;
    Result<void> append_audit_event(const audit::Event& event) override;
    Result<audit::Event> latest_audit_event() override;
    Result<std::vector<audit::Event>> list_audit_events(const audit::Filter& filter) override;
    Result<void> set_audit_retention_days(const std::string& customer_id, int days) override;
    Result<MeteringUsage> upsert_metering_usage(const MeteringUpdate& update) override;
    Result<bool> record_metered_build(const MeteredBuild& build) override;
    Result<std::vector<MeteringUsage>> list_metering_usage(const std::string& customer_id,
                                                           const std::string& from_hour,
                                                           const std::string& to_hour,
                                                           int limit) override;
    Result<std::optional<std::uint64_t>> storage_quota(const std::string& customer_id) override;
    Result<std::vector<Workspace>> list_workspaces(const std::string& customer_id) override;
    Result<Workspace> get_workspace(const std::string& id) override;
    Result<Workspace> create_workspace(const NewWorkspace&) override;
    Result<Workspace> update_workspace(const std::string& id, const WorkspacePatch&) override;
    Result<std::vector<ManagedService>> list_services(const std::string& customer_id) override;
    Result<ManagedService> get_service(const std::string& id) override;
    Result<ManagedService> create_service(const NewManagedService&) override;
    Result<ManagedService> withdraw_service(const std::string& id) override;
    // What the operator writes to service_status; tests and local demos call it directly.
    Result<void> set_service_status(const std::string& id,
                                    std::string phase,
                                    std::string message,
                                    std::string endpoint);
    Result<std::vector<WebApp>> list_web_apps(const std::string& customer_id) override;
    Result<WebApp> get_web_app(const std::string& id) override;
    Result<WebApp> resolve_web_app_auth_host(const std::string& host,
                                             const std::string& workspace_id) override;
    Result<WebApp> authorize_web_app(const std::string& app_id,
                                     const std::string& user_id) override;
    Result<WebApp> authorize_web_app_session(const std::string& host,
                                             const std::string& workspace_id,
                                             const std::string& app_id,
                                             const std::string& user_id) override;
    Result<void> create_web_app_authorization_code(const WebAppAuthorizationCode& value) override;
    Result<WebAppAuthorization>
    consume_web_app_authorization_code(const std::string& host,
                                       const std::string& workspace_id,
                                       const std::string& code_hash) override;
    Result<void> sweep_web_app_authorization_codes() override;
    Result<WebApp> create_web_app(const NewWebApp&) override;
    Result<WebApp> update_web_app(const std::string& id, const WebAppPatch&) override;
    Result<void> purge_deleted_web_apps() override;
    Result<void> set_web_app_token_hashes(const std::string& id, nlohmann::json hashes);
    // What the operator writes back after reconciling a desired row. Tests and local demos call
    // this directly; the production operator updates the same columns in Postgres.
    Result<void> set_web_app_status(const std::string& id,
                                    std::string phase,
                                    std::string url,
                                    bool ready,
                                    std::string message,
                                    std::int64_t observed_generation);
    Result<std::vector<Build>> list_builds(const std::string& customer_id) override;
    Result<void> create_stream_owner(const StreamOwner&) override;
    Result<std::string> stream_owner(const StreamOwner&) override;
    Result<void> delete_stream_owner(const StreamOwner&) override;
    Result<std::vector<AgentTaskStreamScope>>
    list_agent_task_stream_scopes(const std::string& customer_id) override;
    Result<Build> get_build(const std::string& id) override;
    Result<Build> create_build(const NewBuild&) override;
    Result<Build> cancel_build(const std::string& id) override;
    // The production operator updates these columns directly after reconciling the Job.
    Result<void> set_build_status(const std::string& id, BuildStatus status);
    Result<std::vector<ManagedStore>> list_stores(const std::string& customer_id) override;
    Result<ManagedStore> get_store(const std::string& id) override;
    Result<ManagedStore> create_store(const NewManagedStore&) override;
    Result<ManagedStore> withdraw_store(const std::string& id) override;
    // What the operator writes to store_status; tests and local demos call it directly.
    Result<void> set_store_status(const std::string& id,
                                  std::string phase,
                                  std::string message,
                                  std::string endpoint);
    Result<AgentLaunch> create_agent_launch(const NewAgentLaunch&) override;
    Result<AgentLaunch> get_agent_launch(const std::string& id) override;
    Result<std::vector<AgentLaunch>> list_agent_launches(const std::string& customer_id,
                                                         const std::string& agent_id,
                                                         int limit) override;
    Result<AgentLaunch> withdraw_agent_launch(const std::string& id) override;
    // What the operator writes to agent_launch_status; tests and local demos call it directly.
    Result<void> set_agent_launch_status(const std::string& id,
                                         std::string phase,
                                         std::string message,
                                         std::string endpoint,
                                         std::string credential_sha256,
                                         std::optional<int> exit_code = std::nullopt,
                                         nlohmann::json result = nullptr);
    Result<AgentCronSchedule> create_agent_cron_schedule(const NewAgentCronSchedule&) override;
    Result<AgentCronSchedule> get_agent_cron_schedule(const std::string& id) override;
    Result<std::vector<AgentCronSchedule>>
    list_agent_cron_schedules(const std::string& customer_id, const std::string& agent_id) override;
    Result<AgentCronSchedule> withdraw_agent_cron_schedule(const std::string& id) override;
    Result<std::vector<AgentLaunch>> list_agent_cron_runs(const std::string& customer_id,
                                                          const std::string& agent_id,
                                                          int limit) override;
    // What the operator writes to agent_cron_schedule_status and agent_cron_runs; tests and local
    // demos call them directly. A run is keyed by its Job's uid.
    Result<void> set_agent_cron_schedule_status(const std::string& id,
                                                std::string phase,
                                                std::string message,
                                                std::string credential_sha256);
    Result<void> put_agent_cron_run(const std::string& schedule_id,
                                    const std::string& run_id,
                                    const std::string& job_name,
                                    std::string phase,
                                    std::string message,
                                    std::optional<int> exit_code = std::nullopt,
                                    nlohmann::json result = nullptr);
    Result<AgentTask> create_agent_task(const AgentTask&) override;
    Result<AgentTask> get_agent_task(const std::string& id) override;
    Result<AgentTask> latest_agent_task(const std::string& customer_id,
                                        const std::string& kind,
                                        const std::string& workspace_id) override;
    Result<AgentTask> latest_agent_conversation_task(const std::string& customer_id,
                                                     const std::string& conversation_id,
                                                     const std::string& agent,
                                                     const std::string& kind,
                                                     const std::string& workspace_id) override;
    Result<AgentTask> claim_agent_task(const std::string& id,
                                       const std::string& agent,
                                       const std::string& credential_sha256,
                                       std::uint64_t events_from = 0,
                                       int lease_seconds = 600) override;
    Result<void> renew_agent_task_lease(const std::string& id,
                                        const std::string& credential_sha256) override;
    Result<void> mark_agent_task_notice_published(const std::string& id,
                                                  int attempts,
                                                  const std::string& phase) override;
    Result<AgentTask> record_agent_task_event(const std::string& id,
                                              const std::string& agent,
                                              const std::string& phase,
                                              const std::string& message,
                                              const nlohmann::json& result) override;
    Result<void> fail_agent_task(const std::string& id, const std::string& message) override;
    Result<void> mark_agent_task_published(const std::string& id) override;
    Result<StaleAgentTasks> stale_agent_tasks(std::chrono::seconds republish_after) override;
    // Moves a task's clocks back, so tests reach a deadline without waiting for it.
    void age_agent_task(const std::string& id, std::chrono::seconds by);
    Result<void> record_opportunities(const std::vector<Opportunity>&) override;
    Result<std::vector<Opportunity>> list_opportunities(const std::string& customer_id) override;
    Result<Opportunity> set_opportunity_status(const std::string& id,
                                               const std::string& status,
                                               const std::string& project) override;
    // Sets a task's status directly; tests and local demos call it.
    Result<void> set_agent_task_status(const std::string& id,
                                       std::string phase,
                                       std::string message,
                                       std::string credential_sha256,
                                       nlohmann::json result = nullptr);
    Result<AskMessage> append_ask_message(const AskMessage&) override;
    Result<std::vector<AskMessage>> ask_conversation(const std::string& conversation_id,
                                                     const std::string& user_id,
                                                     int limit) override;
    Result<std::uint64_t> set_storage_quota(const std::string& customer_id,
                                            std::uint64_t quota_bytes) override;
    Result<PkiAuthorityRecord> create_pki_authority(const NewPkiAuthorityRecord& value,
                                                    const std::string& instance_kek) override;
    Result<PkiAuthorityMaterial> get_pki_authority(const std::string& customer_id,
                                                   const std::string& instance_kek) override;
    Result<PkiCallerRecord> create_pki_caller(const NewPkiCallerRecord& value) override;
    Result<std::vector<PkiCallerRecord>> list_pki_callers(const std::string& customer_id,
                                                          int limit) override;
    Result<PkiCertificateRecord>
    create_pki_certificate(const NewPkiCertificateRecord& value) override;
    Result<PkiCertificateRecord> get_pki_certificate(const std::string& id) override;
    Result<PkiCertificateRecord> revoke_pki_certificate(const std::string& id) override;
    Result<AgentSession> create_agent_session(const NewAgentSession& value) override;
    Result<AgentSession> revoke_agent_session(const std::string& customer_id,
                                              const std::string& id) override;
    Result<bool> customer_allows_non_expiring_credentials(const std::string& customer_id) override;
    Result<void> set_customer_non_expiring_credentials(const std::string& customer_id,
                                                       bool allowed) override;
    Result<AgentSession> get_agent_session(const std::string& customer_id,
                                           const std::string& id) override;
    Result<PkiRecoveryCeremonyRecord>
    create_pki_recovery_ceremony(const NewPkiRecoveryCeremonyRecord& value) override;
    Result<PkiRecoveryCeremonyRecord>
    get_pki_recovery_ceremony(const std::string& customer_id,
                              const std::string& ceremony_id) override;
    Result<PkiRecoveryCeremonyRecord>
    complete_pki_recovery_ceremony(const std::string& customer_id,
                                   const std::string& ceremony_id,
                                   const std::vector<PkiRecoveryShareEvidence>& evidence) override;
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
    Result<IssuedDynamicSecret> lease_dynamic_secret(const NewDynamicSecretLease& value,
                                                     const std::string& instance_kek) override;
    Result<DynamicSecretLease> get_dynamic_secret_lease(const std::string& id) override;
    Result<DynamicSecretLease> renew_dynamic_secret_lease(const std::string& id,
                                                          int lifetime_seconds) override;
    Result<DynamicSecretLease> revoke_dynamic_secret_lease(const std::string& id) override;
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
    Result<IssuedSession> sign_in_taap(const TaapSignIn&) override;
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
    Result<IssuedSession> sign_in_clerk(const ClerkIdentity&) override;
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
    Result<OfflineGrant> issue_offline_grant(const NewOfflineGrant& grant) override;
    Result<std::vector<OfflineGrant>> list_offline_grants(const std::string& user_id) override;
    Result<OfflineGrant> get_offline_grant(const std::string& id) override;
    Result<OfflineGrant> revoke_offline_grant(const std::string& user_id,
                                              const std::string& id) override;
    Result<IssuedSession> recover_with_code(const std::string& pool_id,
                                            const std::string& email,
                                            const std::string& code,
                                            const std::string& device_name) override;
    Result<RecoveryAttempt> begin_email_recovery(const std::string& pool_id,
                                                 const std::string& email,
                                                 int cooling_seconds) override;
    Result<RecoveryAttempt> begin_admin_recovery(const std::string& organization_id,
                                                 const std::string& user_id,
                                                 const std::string& requested_by) override;
    Result<RecoveryAttempt> get_recovery_attempt(const std::string& recovery_id) override;
    Result<RecoveryAttempt> approve_admin_recovery(const std::string& recovery_id,
                                                   const std::string& approved_by) override;
    Result<IssuedSession> complete_recovery(const std::string& recovery_id,
                                            const std::string& token,
                                            const std::string& device_name) override;
    Result<std::vector<RecoveryDelivery>> claim_recovery_deliveries(int limit) override;
    Result<std::vector<RecoveryNotification>>
    list_recovery_notifications(const std::string& user_id) override;
    Result<RecoveryContact> configure_recovery_contact(const RecoveryContact& contact) override;
    Result<RecoveryContact> get_recovery_contact(const std::string& repository_id) override;
    Result<SsoProvider> register_sso_provider(const NewSsoProvider&) override;
    Result<std::vector<SsoProvider>> list_sso_providers(const std::string& organization_id,
                                                        int limit) override;
    Result<SsoProvider> get_sso_provider(const std::string& id) override;
    Result<OAuthProvider> get_sso_oidc_provider(const std::string& id) override;
    Result<SsoProvider> authenticate_scim(const std::string& bearer_token) override;
    Result<SsoProvider> test_sso_provider(const std::string& id) override;
    Result<FederationIssuer> create_federation_issuer(const NewFederationIssuer&) override;
    Result<std::vector<FederationIssuer>>
    list_federation_issuers(const std::string& organization_id, int limit) override;
    Result<bool> allows_federation_claim(const std::string& organization_id,
                                         const std::string& issuer,
                                         const std::string& claim) override;
    Result<IssuedSession> sign_in_sso(const SsoIdentity&) override;
    Result<std::vector<ScimUser>> list_scim_users(const std::string& provider_id,
                                                  int limit) override;
    Result<ScimUser> get_scim_user(const std::string& provider_id,
                                   const std::string& resource_id) override;
    Result<ScimUser> upsert_scim_user(const NewScimUser&) override;
    Result<ScimUser> delete_scim_user(const std::string& provider_id,
                                      const std::string& resource_id) override;
    Result<std::vector<ScimGroup>> list_scim_groups(const std::string& provider_id,
                                                    int limit) override;
    Result<ScimGroup> get_scim_group(const std::string& provider_id,
                                     const std::string& resource_id) override;
    Result<ScimGroup> upsert_scim_group(const NewScimGroup&) override;
    Result<void> delete_scim_group(const std::string& provider_id,
                                   const std::string& resource_id) override;
    Result<std::vector<ProvisioningLog>> list_sso_provisioning_logs(const std::string& provider_id,
                                                                    int limit) override;

private:
    bool user_can_access_web_app_locked(const WebApp& app, const std::string& user_id) const;
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
    std::vector<ForgeRecord> forge_records_;
    std::vector<DriveShareLink> drive_share_links_;
    std::vector<Team> teams_;
    std::vector<Membership> memberships_;
    std::vector<Invitation> invitations_;
    std::vector<PolicyObject> policy_objects_;
    std::vector<Grant> grants_;
    std::vector<Application> applications_;
    std::vector<ApplicationSigningKey> application_signing_keys_;
    std::vector<ApplicationUser> application_users_;
    std::vector<ApplicationOrganization> application_organizations_;
    std::vector<ApplicationWebhook> application_webhooks_;
    std::vector<ApplicationWebhookEvent> application_webhook_events_;
    std::vector<ApplicationWebhookDelivery> application_webhook_deliveries_;
    struct ApplicationAuthorizationCodeRecord {
        ApplicationAuthorizationCode code;
        std::chrono::steady_clock::time_point expires_at;
        bool used = false;
    };
    std::vector<ApplicationAuthorizationCodeRecord> application_authorization_codes_;
    std::vector<audit::Identity> identities_;
    std::vector<audit::Event> audit_events_;
    std::vector<std::pair<std::string, int>> audit_retentions_;
    std::vector<MeteringUsage> metering_usage_;
    std::map<std::string, std::uint64_t> storage_quotas_;
    std::vector<ManagedService> services_;
    std::vector<Workspace> workspaces_;
    std::vector<WebApp> web_apps_;
    struct WebAppAuthorizationCodeRecord {
        WebAppAuthorizationCode code;
        std::chrono::steady_clock::time_point expires_at;
        bool used = false;
    };
    std::vector<WebAppAuthorizationCodeRecord> web_app_authorization_codes_;
    std::vector<Build> builds_;
    std::vector<StreamOwner> stream_owners_;
    std::vector<ManagedStore> stores_;
    std::set<std::pair<std::string, std::string>> metered_builds_;
    std::vector<PkiAuthorityRecord> pki_authorities_;
    std::vector<PkiCallerRecord> pki_callers_;
    std::vector<PkiCertificateRecord> pki_certificates_;
    std::vector<AgentSession> agent_sessions_;
    std::set<std::string> non_expiring_customers_;
    std::vector<AgentLaunch> agent_launches_;
    std::vector<AgentCronSchedule> agent_cron_schedules_;
    std::vector<std::pair<std::string, AgentLaunch>> agent_cron_runs_; // schedule id, run
    std::vector<AskMessage> ask_messages_;
    std::vector<AgentTask> agent_tasks_;
    struct TaskClock {
        std::chrono::steady_clock::time_point created, published, started, lease_until;
    };
    std::map<std::string, TaskClock> agent_task_clocks_;
    std::vector<Opportunity> opportunities_;
    std::vector<PkiRecoveryCeremonyRecord> pki_recovery_ceremonies_;
    std::map<std::string, std::vector<PkiRecoveryShareEvidence>> pki_recovery_evidence_;
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
    std::vector<DynamicSecretLease> dynamic_secret_leases_;
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
    std::set<std::string> taap_assertions_;
    std::vector<PasskeyCredential> passkeys_;
    std::vector<OAuthCredential> oauth_credentials_;
    // user id -> the TAAP identity lake minted for a Clerk sign-in (custodial_identities).
    std::map<std::string, ClerkIdentity> custodial_identities_;
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
    std::vector<OfflineGrant> offline_grants_;
    struct RecoveryRecord {
        RecoveryAttempt attempt;
        std::string token_hash;
        std::vector<std::string> approvals;
        std::chrono::system_clock::time_point ready_at;
        std::chrono::system_clock::time_point expires_at;
    };
    std::vector<RecoveryRecord> recovery_attempts_;
    std::vector<RecoveryDelivery> recovery_deliveries_;
    std::vector<RecoveryNotification> recovery_notifications_;
    std::vector<RecoveryContact> recovery_contacts_;
    std::vector<PlatformEnvironment> platform_environments_;
    std::vector<PlatformMirrorRow> platform_mirror_;
    std::vector<SsoProvider> sso_providers_;
    std::vector<std::pair<std::string, std::string>> sso_scim_tokens_;
    std::vector<std::pair<std::string, std::string>> sso_oidc_secrets_;
    std::vector<FederationIssuer> federation_issuers_;
    struct SsoCredential {
        std::string provider_id;
        std::string subject;
        std::string user_id;
    };
    std::vector<SsoCredential> sso_credentials_;
    std::vector<ScimUser> scim_users_;
    std::vector<ScimGroup> scim_groups_;
    struct ScimGroupMember {
        std::string provider_id;
        std::string group_external_id;
        std::string user_external_id;
        std::string membership_id;
    };
    std::vector<ScimGroupMember> scim_group_members_;
    void remove_scim_user_access(const ScimUser& user, const std::string& now);
    std::vector<ProvisioningLog> provisioning_logs_;
};

std::string new_uuid_v4();
std::string iso8601_at(std::chrono::system_clock::time_point value);
std::string now_iso8601();

} // namespace lake::control
