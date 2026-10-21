#pragma once
#include <chrono>
#include <map>
#include <mutex>
#include <set>
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
    Result<NetworkConnection> create_network_connection(const NetworkScope&,
                                                        const NetworkConnectionSpec&,
                                                        const NetworkActor&) override;
    Result<NetworkConnection> update_network_connection(const NetworkScope&,
                                                        const std::string& id,
                                                        const NetworkConnectionSpec&,
                                                        const NetworkActor&) override;
    Result<NetworkConnection> withdraw_network_connection(const NetworkScope&,
                                                          const std::string& id,
                                                          const NetworkActor&) override;
    Result<NetworkConnection> get_network_connection(const NetworkScope&,
                                                     const std::string& id) override;
    Result<std::vector<NetworkConnection>> list_network_connections(const NetworkScope&) override;
    Result<NetworkEgressRule> create_network_egress_rule(const NetworkScope&,
                                                         const NetworkEgressRuleSpec&,
                                                         const NetworkActor&) override;
    Result<NetworkEgressRule> update_network_egress_rule(const NetworkScope&,
                                                         const std::string& id,
                                                         const NetworkEgressRuleSpec&,
                                                         const NetworkActor&) override;
    Result<NetworkEgressRule> withdraw_network_egress_rule(const NetworkScope&,
                                                           const std::string& id,
                                                           const NetworkActor&) override;
    Result<NetworkEgressRule> get_network_egress_rule(const NetworkScope&,
                                                      const std::string& id) override;
    Result<std::vector<NetworkEgressRule>> list_network_egress_rules(const NetworkScope&) override;
    Result<NetworkTunnelGrant> create_network_tunnel_grant(const NetworkScope&,
                                                           const NetworkTunnelGrantSpec&,
                                                           const NetworkActor&) override;
    Result<NetworkTunnelGrant> update_network_tunnel_grant(const NetworkScope&,
                                                           const std::string& id,
                                                           const NetworkTunnelGrantSpec&,
                                                           const NetworkActor&) override;
    Result<NetworkTunnelGrant> revoke_network_tunnel_grant(const NetworkScope&,
                                                           const std::string& id,
                                                           const NetworkActor&) override;
    Result<NetworkTunnelGrant> get_network_tunnel_grant(const NetworkScope&,
                                                        const std::string& id) override;
    Result<std::vector<NetworkTunnelGrant>>
    list_network_tunnel_grants(const NetworkScope&) override;
    Result<NetworkPeer>
    create_network_peer(const NetworkScope&, const NetworkPeerSpec&, const NetworkActor&) override;
    Result<NetworkPeer> update_network_peer(const NetworkScope&,
                                            const std::string& id,
                                            const NetworkPeerSpec&,
                                            const NetworkActor&) override;
    Result<NetworkPeer>
    withdraw_network_peer(const NetworkScope&, const std::string& id, const NetworkActor&) override;
    Result<NetworkPeer> get_network_peer(const NetworkScope&, const std::string& id) override;
    Result<std::vector<NetworkPeer>> list_network_peers(const NetworkScope&) override;
    // Operator observation for tests and local demos, never a desired-state mutation.
    Result<void> set_network_connection_status(const std::string& id, const NetworkStatus&);
    Result<void> set_network_egress_rule_status(const std::string& id, const NetworkStatus&);
    Result<void> set_network_tunnel_grant_status(const std::string& id, const NetworkStatus&);
    Result<void> set_network_peer_status(const std::string& id, const NetworkStatus&);
    Result<User> create_user(const NewUser&) override;
    Result<std::vector<User>> list_users(int limit) override;
    Result<User> get_user(const std::string& id) override;
    Result<std::map<std::string, std::string>>
    principal_names(const std::vector<std::string>& taap_ids) override;
    Result<Organization> create_organization(const NewOrganization&) override;
    Result<std::vector<Organization>> list_organizations(int limit) override;
    Result<Organization> get_organization(const std::string& id) override;
    Result<OrganizationDeletion> delete_organization(const std::string& id, bool dry_run) override;
    Result<AccountDeletion> delete_account(const std::string& user_id, bool dry_run) override;
    Result<Customer> create_customer(const NewCustomer&) override;
    Result<std::vector<Customer>> list_customers(int limit,
                                                 const std::string& user_id = {}) override;
    Result<Customer> get_customer(const std::string& id) override;
    Result<Customer> organization_customer(const std::string& organization_id) override;
    Result<Repository> create_repository(const NewRepository&) override;
    Result<std::vector<Repository>> list_repositories(const std::string& customer_id,
                                                      int limit,
                                                      const std::string& workspace_id = {}) override;
    Result<Repository> get_repository(const std::string& id) override;
    Result<std::vector<Repository>> repositories_by_liquid_id(const std::string& liquid_id) override;
    Result<void> set_repository_liquid_id(const std::string& id,
                                          const std::string& liquid_id) override;
    Result<std::vector<Repository>> repositories_without_liquid_id(int limit) override;
    Result<Repository> rename_repository(const std::string& id, const std::string& name) override;
    Result<Repository> set_repository_visibility(const std::string& id,
                                                 const std::string& visibility) override;
    Result<Repository> delete_repository(const std::string& id,
                                         const RepositoryDeletion& deletion) override;
    Result<void>
    assign_legacy_audit_events(const std::vector<std::string>& workspace_actions) override;
    Result<std::vector<LegacyAuditEvent>> legacy_audit_events(const std::string& after_id,
                                                              int limit) override;
    Result<DriveShareLinkRoute> drive_share_link_route(const std::string& token_hash) override;
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
    Result<Invitation> revoke_invitation(const std::string& id, bool dry_run) override;
    Result<OrganizationSpace> create_organization_space(const NewOrganizationSpace&) override;
    Result<OrganizationSpace> convert_personal_space(const ConvertPersonalSpace&) override;
    Result<std::vector<Membership>> list_user_memberships(const std::string& user_id,
                                                          int limit) override;
    Result<std::vector<Invitation>> list_invitee_invitations(const std::string& email,
                                                             const std::vector<std::string>& handles,
                                                             int limit) override;
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
    Result<Grant> get_grant(const std::string& id) override;
    Result<Grant> revoke_grant(const std::string& id, bool dry_run) override;
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
    Result<std::string> encrypt_application_credential(const std::string& purpose,
                                                       const std::string& plaintext) override;
    Result<std::string> decrypt_application_credential(const std::string& purpose,
                                                       const std::string& envelope) override;
    Result<int> revoke_application_user_sessions(const std::string& application_id,
                                                 const std::string& user_id) override;
    Result<void> delete_application_user_credentials(const std::string& application_id,
                                                     const std::string& user_id) override;
    audit::Result<audit::Identity> current_lake_identity() override;
    audit::Result<audit::Identity> lake_identity_by_taap_id(const std::string& taap_id) override;
    audit::Result<void> create_lake_identity(const audit::Identity& identity) override;
    audit::Result<void> rotate_lake_identity(const audit::Identity& previous,
                                             const audit::Identity& replacement) override;
    Result<void> append_audit_event(const audit::Event& event) override;
    Result<audit::Event> latest_audit_event() override;
    Result<std::vector<audit::Event>> list_audit_events(const audit::Filter& filter) override;
    Result<void> set_audit_retention_days(const std::string& customer_id, int days) override;
    Result<std::optional<std::uint64_t>> storage_quota(const std::string& customer_id) override;
    Result<std::vector<Workspace>> list_workspaces(const std::string& customer_id) override;
    Result<Workspace> get_workspace(const std::string& id) override;
    Result<Workspace> create_workspace(const NewWorkspace&) override;
    Result<Workspace> update_workspace(const std::string& id, const WorkspacePatch&) override;
    Result<Workspace> mark_workspace_store_moved(const std::string& id) override;
    Result<WorkspaceDeletion> delete_workspace_cascade(const WorkspaceDeletionRequest&) override;
    Result<WorkspaceDeletion> get_workspace_deletion(const std::string& workspace_id) override;
    Result<std::vector<WorkspaceDeletion>> pending_workspace_deletions(int limit) override;
    Result<void> complete_workspace_deletion(const std::string& workspace_id) override;
    Result<OciRepositoryRoute> create_oci_repository_route(const std::string& customer_id,
                                                           const std::string& name,
                                                           const std::string& workspace_id) override;
    Result<OciRepositoryRoute> get_oci_repository_route(const std::string& customer_id,
                                                        const std::string& name) override;
    Result<std::vector<OciRepositoryRoute>>
    list_oci_repository_routes(const std::string& customer_id,
                               const std::string& workspace_id) override;
    Result<std::vector<OciRepositoryRoute>>
    delete_oci_repository_route(const std::string& customer_id, const std::string& name,
                                const std::string& to_workspace_id = {}) override;
    Result<Workspace> set_workspace_storage(const std::string& id, std::int32_t storage_gi) override;
    Result<VolumeSnapshotPolicy> request_volume_snapshot(const VolumeSnapshotRequest&) override;
    Result<std::vector<VolumeSnapshotPolicy>> volume_snapshots() override;
    // The operator's workspace observation (phase, observed generation). Tests call this
    // directly; the production operator updates the same columns in Postgres.
    Result<void> set_workspace_status(const std::string& id, std::string phase);
    // The namespace the operator reports a workspace runs in (`workspaces.namespace`); tests call
    // this directly.
    Result<void> set_workspace_namespace(const std::string& id, std::string namespace_name);
    // What the operator writes about a workspace's volumes and a target's snapshots (migration
    // 0069); tests call these directly.
    Result<void> set_workspace_storage_status(const std::string& id, std::int32_t observed_gi,
                                              std::string phase, std::string message);
    Result<void> set_volume_snapshot_status(const std::string& target, VolumeSnapshotStatus status);
    Result<std::vector<ManagedService>> list_services(const std::string& customer_id) override;
    Result<ManagedService> get_service(const std::string& id) override;
    Result<ManagedService> create_service(const NewManagedService&) override;
    Result<ManagedService> withdraw_service(const std::string& id) override;
    Result<ManagedService> set_service_stores(const std::string& service_id,
                                              const std::vector<StoreMountRequest>&) override;
    Result<std::vector<StoreMount>>
    list_store_mounts(const std::string& customer_id,
                      const std::string& workspace_id = {}) override;
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
    Result<std::vector<AgentTaskStreamScope>>
    list_agent_task_stream_scopes(const std::string& customer_id) override;
    Result<Build> get_build(const std::string& id) override;
    Result<Build> create_build(const NewBuild&) override;
    Result<Build> cancel_build(const std::string& id) override;
    // The production operator updates these columns directly after reconciling the Job.
    Result<void> set_build_status(const std::string& id, BuildStatus status);
    // Moves a build's request time back, as if it had been running `seconds`; tests only.
    Result<void> age_build(const std::string& id, std::int64_t seconds);
    Result<std::vector<ManagedStore>> list_stores(const std::string& customer_id,
                                                  const std::string& workspace_id = {}) override;
    Result<ManagedStore> get_store(const std::string& id) override;
    Result<ManagedStore> create_store(const NewManagedStore&) override;
    Result<ManagedStore> withdraw_store(const std::string& id) override;
    // What the operator writes to store_status; tests and local demos call it directly.
    Result<void> set_store_status(const std::string& id,
                                  std::string phase,
                                  std::string message,
                                  std::string endpoint);
    // What the operator writes to store_status.used_bytes (migration 0080); tests call it.
    Result<void> set_store_used_bytes(const std::string& id, std::optional<std::int64_t> bytes);
    Result<AgentLaunch> create_agent_launch(const NewAgentLaunch&) override;
    Result<AgentLaunch> get_agent_launch(const std::string& id) override;
    Result<std::vector<AgentLaunch>> list_agent_launches(const std::string& customer_id,
                                                         const std::string& agent_id,
                                                         int limit,
                                                         const std::string& workspace_id = {}) override;
    Result<AgentLaunch> withdraw_agent_launch(const std::string& id) override;
    Result<std::size_t> expire_unstarted_agent_launches(std::int64_t seconds,
                                                        const std::string& reason) override;
    Result<AgentWorkloadsWithdrawn> withdraw_agent_workloads(const std::string& customer_id,
                                                             const std::string& workspace_id,
                                                             const std::string& agent_id,
                                                             const std::string& reason) override;
    // Moves a launch's creation time back, as if it had been waiting `seconds`; tests only.
    Result<void> age_agent_launch(const std::string& id, std::int64_t seconds);
    // What the operator writes to agent_launch_status; tests and local demos call it directly.
    Result<void> set_agent_launch_status(const std::string& id,
                                         std::string phase,
                                         std::string message,
                                         std::string endpoint,
                                         std::string credential_sha256,
                                         std::optional<int> exit_code = std::nullopt,
                                         nlohmann::json result = nullptr);
    Result<MethodologyRun> create_methodology_run(const NewMethodologyRun&) override;
    Result<MethodologyRun> get_methodology_run(const std::string& id) override;
    Result<std::vector<MethodologyRun>> list_methodology_runs(const std::string& customer_id,
                                                              const std::string& workspace_id,
                                                              const std::string& methodology_id,
                                                              int limit) override;
    Result<MethodologyRun> withdraw_methodology_run(const std::string& id,
                                                    const std::string& reason) override;
    Result<std::size_t> expire_unstarted_methodology_runs(std::int64_t seconds,
                                                          const std::string& reason) override;
    Result<void> put_methodology_run_status(const std::string& id,
                                            const MethodologyRunStatus& status) override;
    // Moves a run's creation `seconds` into the past, as age_agent_launch does.
    Result<void> age_methodology_run(const std::string& id, std::int64_t seconds);
    Result<MethodologyDeployment>
    create_methodology_deployment(const NewMethodologyDeployment&) override;
    Result<MethodologyDeployment> get_methodology_deployment(const std::string& id) override;
    Result<std::vector<MethodologyDeployment>>
    list_methodology_deployments(const std::string& customer_id,
                                 const std::string& workspace_id,
                                 const std::string& model_id,
                                 int limit) override;
    Result<MethodologyDeployment> withdraw_methodology_deployment(const std::string& id,
                                                                  const std::string& reason) override;
    Result<void> put_methodology_deployment_status(const std::string& id,
                                                   const MethodologyDeploymentStatus& status) override;
    Result<AgentCronSchedule> create_agent_cron_schedule(const NewAgentCronSchedule&) override;
    Result<AgentCronSchedule> get_agent_cron_schedule(const std::string& id) override;
    Result<std::vector<AgentCronSchedule>>
    list_agent_cron_schedules(const std::string& customer_id, const std::string& agent_id,
                              const std::string& workspace_id = {}) override;
    Result<AgentCronSchedule> withdraw_agent_cron_schedule(const std::string& id) override;
    Result<std::vector<AgentLaunch>> list_agent_cron_runs(const std::string& customer_id,
                                                          const std::string& agent_id,
                                                          int limit,
                                                          const std::string& workspace_id = {}) override;
    // What the operator writes to agent_cron_schedule_status and agent_cron_runs; tests and local
    // demos call them directly. A run is keyed by its Job's uid.
    Result<void> set_agent_cron_schedule_status(const std::string& id,
                                                std::string phase,
                                                std::string message,
                                                std::string credential_sha256);
    // Moves a cron run's start `seconds` earlier, so a test can give it a duration (lake#658).
    Result<void> age_agent_cron_run(const std::string& run_id, std::int64_t seconds);
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
    Result<std::vector<AgentTask>>
    list_conversation_agent_tasks(const std::string& customer_id,
                                  const std::string& workspace_id,
                                  const std::string& conversation_id,
                                  const std::string& requested_by_user_id) override;
    Result<std::size_t> delete_agent_tasks(const std::string& customer_id,
                                           const std::vector<std::string>& ids) override;
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
                                              const std::string& phase) override;
    Result<void> fail_agent_task(const std::string& id, const std::string& notice) override;
    Result<void> mark_agent_task_published(const std::string& id) override;
    Result<StaleAgentTasks> stale_agent_tasks(std::chrono::seconds republish_after) override;
    // Moves a task's clocks back, so tests reach a deadline without waiting for it.
    void age_agent_task(const std::string& id, std::chrono::seconds by);
    // Sets a task's dispatch status directly; tests and local demos call it. What the agent wrote
    // is a record in the task's workspace (control/agent_task_records.hpp).
    Result<void> set_agent_task_status(const std::string& id,
                                       std::string phase,
                                       std::string credential_sha256 = {});
    Result<std::vector<UsageLimit>> usage_limits(const std::string& customer_id) override;
    Result<UsageLimit> set_usage_limit(const UsageLimit& limit) override;
    Result<std::vector<BillingPlan>> billing_plans() override;
    Result<std::vector<WorkloadResources>> workload_resources() override;
    Result<void> put_workload_resources(const WorkloadResources& row) override;
    Result<void> put_mcp_connection_dispatch(const McpConnectionDispatch& row) override;
    Result<void> remove_mcp_connection_dispatch(const std::string& workspace_id,
                                                const std::string& connection_id) override;
    Result<std::vector<McpConnectionDispatch>> mcp_stream_dispatch(int limit) override;
    Result<std::vector<McpConnectionDispatch>>
    mcp_exported_dispatch(const std::string& workspace_id) override;
    Result<std::vector<AppTransfer>> app_transfer(const std::string& since_hour) override;
    Result<void> add_app_transfer(const AppTransfer& row) override;
    Result<BillingPlan> put_billing_plan(const BillingPlan& plan) override;
    Result<Invoice> create_invoice(const Invoice& invoice) override;
    Result<std::vector<Invoice>> list_invoices(const std::string& customer_id) override;
    Result<Invoice> get_invoice(const std::string& id) override;
    Result<Invoice> set_invoice_state(const std::string& id,
                                      const std::string& state,
                                      const std::string& provider_reference,
                                      const std::string& note) override;
    Result<Invoice> set_invoice_provider_status(const std::string& id,
                                                const std::string& from,
                                                const std::string& to) override;
    Result<std::string> billing_provider_customer(const std::string& customer_id,
                                                  const std::string& provider) override;
    Result<std::string>
    set_billing_provider_customer(const std::string& customer_id,
                                  const std::string& provider,
                                  const std::string& provider_customer_id) override;
    Result<std::string>
    replace_billing_provider_customer(const std::string& customer_id,
                                      const std::string& provider,
                                      const std::string& expected_id,
                                      const std::string& provider_customer_id) override;
    Result<PaymentEventResult> apply_payment_event(const PaymentEvent& event) override;
    Result<std::optional<BillingContact>> billing_contact(const std::string& customer_id) override;
    Result<BillingContact> set_billing_contact(const BillingContact& contact) override;
    Result<CloudOrder> create_cloud_order(const NewCloudOrder& order) override;
    Result<CloudOrder> get_cloud_order(const std::string& id) override;
    Result<CloudOrder> transition_cloud_order(const CloudOrderTransition& change) override;
    Result<CloudOrderPaymentResult>
    apply_cloud_order_payment(const CloudOrderPayment& payment) override;
    Result<std::vector<CloudOrder>> claim_cloud_order_emails(int limit, int lease_seconds) override;
    Result<void> record_cloud_order_email(const CloudOrderEmailOutcome& outcome) override;
    Result<std::vector<CloudOrderEvent>> unaudited_cloud_order_events(int limit) override;
    Result<void> mark_cloud_order_events_audited(const std::vector<std::int64_t>& ids) override;
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
    Result<SecretMountRoute> create_secret_mount_route(const SecretMountRoute& value) override;
    Result<SecretMountRoute> get_secret_mount_route(const std::string& mount_id) override;
    Result<void> delete_secret_mount_route(const std::string& mount_id) override;
    Result<std::vector<SecretMountRoute>>
    list_secret_mount_routes(const std::string& customer_id,
                             const std::string& workspace_id) override;
    Result<WrappedDataKey> wrap_secret_data_key(const std::string& customer_id,
                                                const std::string& owner_id,
                                                std::uint64_t key_version,
                                                const std::string& raw_key,
                                                const std::string& instance_kek) override;
    Result<std::string> unwrap_secret_data_key(const std::string& customer_id,
                                               const std::string& owner_id,
                                               std::uint64_t key_version,
                                               const WrappedDataKey& wrapped,
                                               const std::string& instance_kek) override;
    Result<CustomerKekState> customer_kek_state(const std::string& customer_id) override;
    Result<CustomerKekRotation> rotate_customer_kek(const std::string& customer_id,
                                                    const std::string& instance_kek) override;
    Result<void> retire_customer_kek(const std::string& customer_id,
                                     std::uint64_t previous_version) override;
    Result<SecretLeaseRoute> record_secret_lease_route(const SecretLeaseRoute& value) override;
    Result<SecretLeaseRoute> get_secret_lease_route(const std::string& lease_id) override;
    Result<void> reschedule_secret_lease(const std::string& lease_id,
                                         std::int64_t due_us) override;
    Result<void> delete_secret_lease_route(const std::string& lease_id) override;
    Result<std::vector<SecretLeaseRoute>> claim_due_secret_leases(int limit,
                                                                  int hold_seconds) override;
    Result<IssuedSession> sign_up_password(const NewPasswordUser&) override;
    Result<IssuedSession> sign_in_password(const PasswordSignIn&) override;
    Result<IssuedSession> sign_in_taap(const TaapSignIn&) override;
    Result<MfaEnrollment> enroll_mfa(const std::string& user_id,
                                     const std::string& pool_id) override;
    Result<void> disable_mfa(const std::string& user_id,
                             const std::string& pool_id,
                             const std::string& code) override;
    Result<bool> mfa_enrolled(const std::string& user_id, const std::string& pool_id) override;
    Result<void> verify_mfa_code(const std::string& user_id,
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
    Result<ClerkUserProvisioned> provision_clerk_user(const ClerkIdentity&) override;
    Result<std::string> update_clerk_user(const ClerkIdentity&) override;
    Result<std::string> disable_clerk_user(const std::string& subject) override;
    Result<bool> clerk_webhook_applied(const std::string& delivery_id) override;
    Result<void> record_clerk_webhook(const std::string& delivery_id,
                                      const std::string& event_type,
                                      const std::string& subject) override;
    // provision_clerk_user under the lock.
    Result<ClerkUserProvisioned> clerk_user_locked(const ClerkIdentity&);
    Result<AccountIdentity> account_identity(const std::string& user_id) override;
    Result<ManagedKey> managed_key(const std::string& user_id) override;
    Result<void> create_identity_link_challenge(const IdentityLinkChallenge&) override;
    Result<IdentityLinkChallenge>
    consume_identity_link_challenge(const std::string& user_id,
                                    const std::string& challenge) override;
    Result<AccountIdentity> link_own_identity(const IdentityLink&) override;
    Result<std::string> account_for_identity(const std::string& taap_id) override;
    Result<void> register_oauth_provider(const OAuthProvider&) override;
    Result<OAuthProvider> get_oauth_provider(const std::string& pool_id,
                                             const std::string& provider) override;
    Result<void> begin_oauth(const OAuthFlow&) override;
    Result<void> begin_saml(const SamlRequest&) override;
    Result<SamlRequest> consume_saml(const SamlAnswer&) override;
    Result<OAuthFlow> consume_oauth(const std::string& provider, const std::string& state) override;
    Result<IssuedSession> create_token(const NewToken&) override;
    Result<std::vector<Session>> list_sessions(const std::string& user_id,
                                               const std::string& pool_id) override;
    Result<void> revoke_session(const std::string& user_id, const std::string& session_id) override;
    Result<std::vector<Device>> list_devices(const std::string& user_id) override;
    Result<void> revoke_device(const std::string& user_id, const std::string& device_id) override;
    Result<Principal> context_for(const std::string& secret) override;
    Result<Principal> resolve_principal(const std::string& secret);
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
    Result<void> check_network_scope_locked(const NetworkScope&) const;
    Result<void> check_network_target_locked(const NetworkScope&,
                                             const std::string& kind,
                                             const std::string& id) const;
    template <class Row>
    Result<Row> change_network(const NetworkScope&,
                               const std::string& id,
                               const nlohmann::json& spec,
                               const NetworkActor&,
                               const std::string& operation,
                               std::vector<Row>& rows);
    template <class Row>
    Result<Row>
    get_network(const NetworkScope&, const std::string& id, const std::vector<Row>& rows);
    template <class Row>
    Result<std::vector<Row>> list_network(const NetworkScope&, const std::vector<Row>& rows);
    template <class Row>
    Result<void>
    set_network_status(const std::string& id, const NetworkStatus&, std::vector<Row>& rows);
    std::vector<NetworkConnection> network_connections_;
    std::vector<NetworkEgressRule> network_egress_rules_;
    std::vector<NetworkTunnelGrant> network_tunnel_grants_;
    std::vector<NetworkPeer> network_peers_;
    bool user_can_access_web_app_locked(const WebApp& app, const std::string& user_id) const;
    // The organization's own space shares its organization-scope memberships' grants.
    void derive_space_grant_locked(const Membership& membership, const std::string& role);
    void drop_space_grant_locked(const Membership& membership, const std::string& role);
    Result<std::string> customer_kek_locked(const std::string& customer_id, bool create);
    Result<WrappedDataKey> wrap_data_key_locked(const std::string& customer_id,
                                                const std::string& owner_id,
                                                std::uint64_t key_version,
                                                const std::string& raw_key);
    std::mutex mutex_;
    std::vector<User> users_;
    std::vector<Organization> organizations_;
    // Organizations deleted by delete_organization and their spaces (lake#645): kept, never read.
    std::set<std::string> deleted_organizations_, deleted_customers_;
    // Accounts deleted by delete_account (lake#810): kept, email and name erased, never read.
    std::set<std::string> deleted_users_;
    std::vector<Customer> customers_;
    std::vector<Repository> repositories_; // routing rows: no description (lake#315 part 2)
    // Audit event id -> (workspace, outcome), as workspace_assignments holds them (lake#317).
    std::map<std::string, std::pair<std::string, std::string>> legacy_audit_assignments_;
    std::map<std::string, DriveShareLinkRoute> drive_share_link_routes_; // by token digest
    std::vector<Team> teams_;
    std::vector<Membership> memberships_;
    std::vector<Invitation> invitations_;
    std::vector<PolicyObject> policy_objects_;
    std::vector<Grant> grants_;
    std::vector<Application> applications_; // routing rows: no settings (lake#351)
    // The envelope's data key per credential purpose, as the keystore table holds them.
    std::map<std::string, std::string> application_credential_keys_;
    std::vector<audit::Identity> identities_;
    std::vector<audit::Event> audit_events_;
    std::vector<std::pair<std::string, int>> audit_retentions_;
    std::map<std::string, std::uint64_t> storage_quotas_;
    std::map<std::pair<std::string, std::string>, UsageLimit> usage_limits_;
    std::map<std::string, BillingPlan> billing_plans_;
    std::map<std::pair<std::string, std::string>, WorkloadResources> workload_resources_;
    // By (workspace, connection).
    std::map<std::pair<std::string, std::string>, McpConnectionDispatch> mcp_connection_dispatch_;
    std::map<std::pair<std::string, std::string>, AppTransfer> app_transfer_; // (hour, app)
    std::vector<Invoice> invoices_;
    std::map<std::pair<std::string, std::string>, std::string> billing_provider_customers_;
    std::set<std::pair<std::string, std::string>> payment_events_;
    std::map<std::string, BillingContact> billing_contacts_; // by customer
    // Cloud orders (migration 0089), their events, and when each order's next email is due.
    std::vector<CloudOrder> cloud_orders_;
    std::vector<CloudOrderEvent> cloud_order_events_;
    std::map<std::string, std::chrono::system_clock::time_point> cloud_order_email_due_;
    void cloud_order_event_locked(const std::string& id,
                                  const std::string& from,
                                  const std::string& to,
                                  const std::string& actor,
                                  const std::string& detail);
    std::vector<ManagedService> services_;
    std::vector<Workspace> workspaces_;
    std::vector<WorkspaceDeletion> workspace_deletions_;
    std::vector<OciRepositoryRoute> oci_repository_routes_;
    // volume_snapshot_policies by target (migration 0069) and the operator's status rows, which
    // outlive their policy as they do in PostgreSQL.
    std::map<std::string, VolumeSnapshotPolicy> volume_snapshot_policies_{
        {"store", VolumeSnapshotPolicy{.target = "store"}}};
    std::map<std::string, VolumeSnapshotStatus> volume_snapshot_status_;
    std::vector<WebApp> web_apps_;
    struct WebAppAuthorizationCodeRecord {
        WebAppAuthorizationCode code;
        std::chrono::steady_clock::time_point expires_at;
        bool used = false;
    };
    std::vector<WebAppAuthorizationCodeRecord> web_app_authorization_codes_;
    std::vector<Build> builds_;
    std::vector<ManagedStore> stores_;
    // store_mounts (migration 0080): the rows only; the names are read at list time.
    std::vector<StoreMount> store_mounts_;
    // With mutex_ held: checks then replaces what one workload mounts, as the Postgres store does.
    Result<void> check_store_mounts_locked(const std::string& object_kind,
                                           const std::string& object_id,
                                           const std::string& workspace_id,
                                           const std::vector<StoreMountRequest>& mounts) const;
    void drop_workspace_store_mounts_locked(const std::string& workspace_id);
    void put_store_mounts_locked(const std::string& object_kind,
                                 const std::string& object_id,
                                 const std::vector<StoreMountRequest>& mounts);
    std::vector<StoreMount> store_mounts_locked(const std::string& customer_id,
                                                const std::string& workspace_id,
                                                const std::string& store_id) const;
    std::vector<PkiAuthorityRecord> pki_authorities_;
    std::vector<PkiCallerRecord> pki_callers_;
    std::vector<PkiCertificateRecord> pki_certificates_;
    std::vector<AgentSession> agent_sessions_;
    std::set<std::string> non_expiring_customers_;
    std::vector<AgentLaunch> agent_launches_;
    std::vector<MethodologyRun> methodology_runs_;
    std::vector<MethodologyDeployment> methodology_deployments_;
    std::vector<AgentCronSchedule> agent_cron_schedules_;
    std::vector<std::pair<std::string, AgentLaunch>> agent_cron_runs_; // schedule id, run
    std::vector<AgentTask> agent_tasks_;
    struct TaskClock {
        std::chrono::steady_clock::time_point created, published, started, lease_until;
    };
    std::map<std::string, TaskClock> agent_task_clocks_;
    std::vector<PkiRecoveryCeremonyRecord> pki_recovery_ceremonies_;
    std::map<std::string, std::vector<PkiRecoveryShareEvidence>> pki_recovery_evidence_;
    struct CustomerKekRecord {
        std::string customer_id;
        std::string key;
        std::uint64_t version = 1;
        // The KEK a rotation replaced, kept until retire_customer_kek.
        std::string previous_key;
        std::uint64_t previous_version = 0;
    };
    std::vector<CustomerKekRecord> customer_keks_;
    std::vector<SecretMountRoute> secret_mount_routes_;
    std::vector<SecretLeaseRoute> secret_lease_routes_;
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
    // Accounts disabled by a Clerk user.deleted (lake#765), and the deliveries applied.
    std::set<std::string> disabled_users_;
    std::set<std::string> clerk_webhook_deliveries_;
    // user id -> when its managed seed was destroyed by using its own identity (lake#762).
    std::map<std::string, std::string> managed_seed_destroyed_at_;
    struct IdentityLinkChallengeRecord {
        IdentityLinkChallenge challenge;
        std::chrono::system_clock::time_point expires_at;
        bool used = false;
    };
    std::vector<IdentityLinkChallengeRecord> identity_link_challenges_;
    // user id -> its own identity, linked as the managed key's successor.
    std::map<std::string, IdentityLink> identity_links_;
    std::map<std::string, std::string> identity_linked_at_;
    std::vector<WebAuthnChallengeRecord> webauthn_challenges_;
    std::vector<OAuthProvider> oauth_providers_;
    struct OAuthFlowRecord {
        OAuthFlow flow;
        std::chrono::steady_clock::time_point expires_at;
        bool used = false;
    };
    std::vector<OAuthFlowRecord> oauth_flows_;
    struct SamlRequestRecord {
        SamlRequest request;
        std::chrono::steady_clock::time_point expires_at;
        bool used = false;
    };
    std::vector<SamlRequestRecord> saml_requests_;
    // (provider id, assertion id) to the assertion's expiry in Unix seconds.
    std::map<std::pair<std::string, std::string>, std::int64_t> saml_assertions_;
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
