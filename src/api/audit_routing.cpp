#include "lake/api/audit_routing.hpp"

#include <algorithm>
#include <array>
#include <cctype>

#include "lake/api/audit_recorder.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/control/store.hpp"

namespace lake::api {
namespace {

// Control-plane and identity capabilities, and those that span the workspaces of a space or the
// whole instance: their events stay in the central log (decision 52). Sorted.
constexpr std::array central = std::to_array<std::string_view>({
    "accept_invitation",
    // The prepaid balance is billing, in the control plane (lake#1114, decision 56).
    "account_balance",
    // Which key an account signs with is identity (lake#762).
    "account_identity",
    // ACME (RFC 8555): lake's certificate authority for workload identity.
    "acme_authorization",
    "acme_certificate",
    "acme_finalize_order",
    "acme_http01_challenge",
    "acme_http01_response",
    "acme_new_account",
    "acme_new_order",
    "acme_order",
    "append_transparency_entries",
    "approve_admin_recovery",
    "audit",
    "begin_admin_recovery",
    // An application's sign-in flow is identity: who signed in, before any workspace is chosen.
    "begin_application_authorization",
    "begin_email_recovery",
    "begin_identity_proof",
    "begin_own_identity",
    "begin_passkey_registration",
    "begin_passkey_sign_in",
    "begin_root_recovery",
    // Billing is account configuration in the control plane (decision 56, lake#564).
    "bill_preview",
    "billing_account",
    "cancel_recovery",
    "check_agent_certificate",
    "check_cached_read",
    "claim_recovery_deliveries",
    "claim_root_recovery_share",
    "complete_recovery",
    "complete_root_recovery",
    // Account recovery's custody of a repository key is identity, not the repository's data.
    "configure_recovery_contact",
    "convert_personal_space",
    "copy_workspace_records",
    // A browser key is a credential, kept with the others in the control plane (lake#1152).
    "create_browser_key",
    // Coupon codes are billing, in the control plane (lake#1156, decision 56).
    "create_coupon",
    "create_customer",
    "create_federation_issuer",
    "create_grant",
    "create_invitation",
    "create_membership",
    "create_object",
    "create_organization",
    "create_organization_space",
    "create_pki_authority",
    "create_team",
    "create_token",
    "create_user",
    "create_workspace",
    "delete_account",
    "delete_organization",
    "delete_workspace",
    "disable_coupon",
    "disable_mfa",
    "drive_shared_with_me",
    "effective_access",
    "enroll_mfa",
    "entity_effective_access",
    "exchange_application_authorization_code",
    "exchange_cloud_attestation",
    "exchange_jwt_attestation",
    "exchange_kubernetes_attestation",
    "export_audit",
    "export_transparency_replication_batch",
    "get_customer",
    "get_doc",
    "get_invoice",
    "get_membership",
    "get_object",
    "get_organization",
    "get_team",
    "get_user",
    "get_workspace",
    // Credits from Orama are billing, in the control plane (lake#1134, decision 56).
    "grant_balance_credit",
    "identity",
    "initialize_tuf",
    "issue_agent_certificate",
    "issue_client_certificate",
    "issue_device_certificate",
    "issue_invoice",
    "issue_jwt_svid",
    "issue_network_tunnel_certificate",
    "issue_offline_grant",
    "issue_ssh_host_certificate",
    "issue_ssh_user_certificate",
    "issue_x509_svid",
    "join_organization",
    // Dynamic-secret leases are published to lake's transparency log, which commits to the
    // central chain when the call is made; they stay central until the log can name a workspace
    // chain's event.
    "lease_dynamic_secret",
    "list_agent_library",
    "list_applications",
    "list_browser_keys",
    "list_coupons",
    "list_customers",
    "list_devices",
    "list_docs",
    "list_federation_issuers",
    "list_grants",
    // Tenant image routes span the workspaces of a space (lake#383).
    "list_image_repositories",
    "list_invitations",
    "list_invoices",
    "list_memberships",
    "list_objects",
    "list_offline_grants",
    "list_organizations",
    "list_packages",
    "list_recovery_notifications",
    "list_service_kinds",
    "list_services",
    "list_sessions",
    "list_sso_providers",
    "list_sso_provisioning_logs",
    "list_store_providers",
    "list_teams",
    "list_users",
    "list_workspaces",
    "mcp_setup",
    "metering_usage",
    "mint_agent_session_key",
    // lake#380: the operator's move of a workspace's repositories into its own store.
    "move_workspace_store",
    "my_audit",
    "my_permissions",
    "price_list",
    "publish_transparency_checkpoint",
    "publish_tuf_metadata",
    "record_invoice_payment",
    "recover_repository_key",
    "recover_with_code",
    "redeem_coupon",
    "refresh_account_balance",
    "register_oauth_provider",
    "register_passkey",
    "register_sso_provider",
    "remove_member",
    // lake#380: the operator's removal of a space's old store once its workspaces are switched.
    "remove_space_store",
    "renew_dynamic_secret",
    "renew_timestamp_evidence",
    "replicate_transparency_log",
    "repository_recovery_status",
    // The operator's volumes are control plane (migration 0069, operator#259, operator#260).
    "request_volume_snapshot",
    "resend_invoice",
    "resolve_model",
    "revoke_agent_certificate",
    "revoke_agent_session",
    "revoke_balance_credit",
    "revoke_browser_key",
    "revoke_device",
    "revoke_dynamic_secret",
    "revoke_grant",
    "revoke_invitation",
    "revoke_offline_grant",
    "revoke_session",
    "revoke_user_sessions",
    "rotate_agent_certificate",
    "rotate_customer_kek",
    "rotate_identity",
    "rotate_tuf_root",
    "scim_delete_group",
    "scim_delete_user",
    "scim_get_group",
    "scim_get_user",
    "scim_list_groups",
    "scim_list_users",
    "scim_patch_group",
    "scim_patch_user",
    "scim_upsert_group",
    "scim_upsert_user",
    "search",
    "server_info",
    "set_audit_retention",
    "set_auto_top_up",
    "set_balance_overdraft",
    // An account's billing contact is billing configuration (lake#857).
    "set_billing_contact",
    "set_customer_credential_policy",
    "set_member_role",
    "set_price_list",
    "set_storage_quota",
    // Usage limits are account configuration beside the storage quota (lake#564).
    "set_usage_limit",
    "set_workspace_storage",
    "sign_in_passkey",
    "sign_in_password",
    "sign_in_saml",
    "sign_in_sso",
    "sign_in_taap",
    "sign_out_everywhere",
    "sign_up_password",
    "start_oauth",
    "start_sso_oidc",
    "start_sso_saml",
    "start_top_up",
    "start_workspace",
    "test_sso_provider",
    "timestamp_checkpoint",
    "timestamp_evidence",
    "transparency_checkpoint",
    "transparency_consistency_proof",
    "transparency_identity_scan",
    "transparency_inclusion_proof",
    "transparency_log_configuration",
    "transparency_proof_bundle",
    "transparency_tile",
    "tuf_metadata",
    // Links to Orama's own tunnel client, for any signed-in person: no workspace's data (lake#745).
    "tunnel_client_downloads",
    // Removing a wrong image route is control plane: the route is multitenancy (lake#383).
    "unroute_image_repository",
    "update_workspace",
    "upsert_scim_group",
    "upsert_scim_user",
    // A reading of usage across an account's workspaces (lake#564).
    "usage_limits",
    "usage_over_time",
    "use_own_identity",
    "verify_agent_session_drip",
    "verify_audit",
    "verify_mfa",
    "viewer",
    "void_invoice",
    "volume_snapshots",
    "workspace_status",
});

// Capabilities about a workspace's data: their events go to that workspace's audit. Sorted.
constexpr std::array workspace = std::to_array<std::string_view>({
    "agent_turn",
    "app_logs",
    "append_drive_upload",
    "application_jwks",
    "approve_agent_proposal",
    "attach_model_provenance",
    "begin_drive_upload",
    "begin_mcp_connection_sign_in",
    "bind_service_to_app",
    "browse_stream",
    "build_app",
    "build_image",
    "build_logs",
    "call_mcp_tool",
    "cancel_build",
    "change_diff",
    "claim_agent_task",
    "commit_repository_files",
    "compare_runs",
    "compile_pipeline",
    "complete_mcp_connection_sign_in",
    "confirm_ask_proposal",
    "confirm_solution",
    "connect_service",
    "create_agent_from_proposal",
    "create_app",
    "create_application_organization",
    "create_application_webhook",
    "create_domain",
    "create_drive",
    "create_drive_folder",
    "create_drive_folder_entry",
    "create_drive_share_link",
    "create_drive_shortcut",
    "create_encryption_key",
    "create_image_repository",
    "create_issue",
    "create_issue_comment",
    "create_mcp_connection",
    "create_mcp_connection_agent",
    "create_methodology",
    "create_network_connection",
    "create_network_egress_rule",
    "create_pipeline",
    "create_projects_from_opportunities",
    "create_release",
    "create_repository",
    "create_review",
    "create_review_comment",
    "create_secret_mount",
    "create_service",
    "create_service_and_bind_to_app",
    "create_solution",
    "create_store",
    "create_stream",
    "create_transcript",
    "create_webhook",
    "decrypt",
    "delete_app",
    "delete_application_organization",
    "delete_application_user",
    "delete_ask_conversation",
    "delete_dataset",
    "delete_domain",
    "delete_drive",
    "delete_drive_entry",
    "delete_drive_path",
    "delete_index",
    "delete_mcp_connection",
    "delete_methodology",
    "delete_model",
    "delete_repository",
    "delete_run",
    "delete_secret_mount",
    "delete_secret_versions",
    "delete_service",
    "delete_solution",
    "delete_store",
    "delete_stream",
    "delete_stream_messages",
    "delete_transcript",
    "deliver_webhook",
    "deploy_app",
    "derive_encryption_key",
    "destroy_secret_versions",
    "diff_drive_versions",
    "dismiss_opportunity",
    "download_drive_file",
    "download_drive_share_link",
    "draft_methodology",
    "drive_browse",
    "drive_conflict",
    "drive_effective_access",
    "drive_history",
    "drive_preview",
    "drive_share_people",
    "duplicate_methodology",
    "duplicate_solution",
    "edit_pipeline",
    "encrypt",
    "environment_logs",
    "export_application_user",
    "export_repository",
    "find_missing",
    "finish_drive_upload",
    "get_agent_task",
    "get_app",
    "get_application",
    "get_artifact",
    "get_ask_conversation",
    "get_build",
    "get_change",
    "get_dataset",
    "get_drive",
    "get_drive_entry",
    "get_encryption_key",
    "get_environment",
    "get_experiment",
    "get_experiment_analysis",
    "get_mcp_connection",
    "get_methodology",
    "get_methodology_deployment",
    "get_methodology_run",
    "get_model",
    "get_pipeline",
    "get_repository",
    "get_run_metrics",
    "get_secret_mount",
    "get_solution",
    "get_stream",
    "get_transcript",
    "grant_network_tunnel",
    "hmac",
    "import_github_repository",
    "import_repository",
    "issue_application_jwt",
    "issue_assignees",
    "latest_agent_task",
    "launch_agent",
    "list_agent_launches",
    "list_agent_revisions",
    "list_agent_schedules",
    "list_agent_streams",
    "list_application_organizations",
    "list_application_users",
    "list_application_webhook_events",
    "list_application_webhooks",
    "list_apps",
    "list_artifacts",
    "list_branch_protections",
    "list_builds",
    "list_changes",
    "list_conflicts",
    "list_datasets",
    "list_deployments",
    "list_drive_entries",
    "list_drives",
    "list_encryption_keys",
    "list_environments",
    "list_experiment_trials",
    "list_experiments",
    "list_exports",
    "list_issue_comments",
    "list_issues",
    "list_mcp_connection_calls",
    "list_mcp_connection_tools",
    "list_mcp_connections",
    "list_methodology_deployments",
    "list_methodology_runs",
    "list_network_connections",
    "list_network_egress_rules",
    "list_network_peers",
    "list_network_tunnel_grants",
    "list_network_tunnel_sessions",
    "list_opportunities",
    "list_releases",
    "list_repositories",
    "list_review_comments",
    "list_reviews",
    "list_runs",
    "list_secret_mounts",
    "list_secrets",
    "list_solutions",
    "list_stores",
    "list_streams",
    "list_transcripts",
    "list_webhook_deliveries",
    "list_webhooks",
    "load_drive_dataset",
    "load_drive_model",
    "map_review_anchors",
    // The operator's delivery of a secret to a Kubernetes Secret: the mount's workspace (lake#625).
    // A served model's stage (lake#690) and a run's artifact chunks: the workspace of the
    // deployment or run.
    "methodology_deployment_input",
    "methodology_run_artifact",
    "methodology_run_input",
    "methodology_run_output",
    "mint_sealed_secret",
    // A model call through the model gateway: the workspace the agent's run belongs to (lake#248).
    "model_completion",
    "model_traffic",
    "move_drive_entry",
    "move_drive_path",
    "network_activity",
    "network_rules",
    "predict",
    "prepare_repository_upload",
    "preview_drive_share_link",
    "promote_trial",
    "publish_agent_event",
    "publish_app_event",
    "publish_build_event",
    "publish_stream_message",
    "pull",
    "push",
    "put_secret",
    // The quickstart process (lake#1128): its record is in the workspace it runs in.
    "quickstart_snippets",
    "quickstart_status",
    "read_chunk",
    "read_drive_file",
    "read_mcp_tool",
    "read_secret",
    "receive_agent_events",
    "receive_agent_tasks",
    "receive_app_events",
    "receive_build_events",
    "recent_drive_entries",
    "record_model_usage",
    "refresh_mcp_connection",
    "refs",
    "register_application",
    "reindex_artifacts",
    "remove_network_connection",
    "rename_drive",
    "rename_repository",
    "render_pipeline",
    "replicate_repository",
    "repository_file",
    "repository_refs",
    "repository_tree",
    "resolve_conflict",
    "resolve_dataset_file",
    "resolve_drive_share_link",
    "resolve_model_file",
    "restart_app",
    "restore_drive_version",
    "retry_application_webhook",
    "retry_webhook_delivery",
    "revoke_drive_share_link",
    "revoke_network_tunnel",
    "rewrap",
    "rollback_app",
    "rotate_application_signing_key",
    "rotate_encryption_key",
    "rotate_network_connection_credential",
    "run_methodology",
    "run_pipeline",
    "scale_app",
    "schedule_agent",
    "search_drives",
    "search_index",
    "secret_metadata",
    "send_ask_message",
    "service_connection",
    "set_agent_streams",
    "set_app_auth",
    "set_app_env",
    "set_app_stores",
    "set_app_zetta_binding",
    "set_branch_protection",
    "set_environment_env",
    "set_mcp_tool_auto_approve",
    "set_repository_visibility",
    "set_service_stores",
    "settle_ask_turn",
    "share_drive_folder",
    "sign",
    "stage_repository_file",
    "start_app",
    "start_experiment",
    "start_quickstart",
    "start_solution",
    "stop_agent_launch",
    "stop_app",
    "stop_experiment",
    "stop_experiment_trial",
    "stop_methodology_run",
    "store_connection",
    "suspend_application_user",
    "tail_experiment_events",
    "test_mcp_connection",
    "train_quickstart",
    // Dictation is the speech-to-text usage of the workspace it is dictated in (lake#613).
    "transcribe",
    "transfer_repository",
    "unbind_service_from_app",
    "undelete_secret_versions",
    "unschedule_agent",
    "unset_app_env",
    "unset_environment_env",
    "update_application_organization",
    "update_application_theme",
    "update_issue",
    "update_mcp_connection",
    "update_methodology",
    "update_review",
    "update_solution",
    "update_stream",
    "update_stream_message",
    "update_transcript",
    "upload_chunks",
    "upload_drive_entries",
    "upload_drive_files",
    "validate_methodology",
    "verify",
    "verify_model_provenance",
    "workspace_act",
    "workspace_openapi",
    "workspace_query",
    "workspace_sql",
});

bool listed(std::span<const std::string_view> names, std::string_view name) {
    return std::ranges::binary_search(names, name);
}

// An id lake could have made: the routing lookups are never asked about anything else.
bool uuid_shaped(std::string_view value) {
    if (value.size() != 36) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        const bool dash = index == 8 || index == 13 || index == 18 || index == 23;
        if (dash ? value[index] != '-'
                 : std::isxdigit(static_cast<unsigned char>(value[index])) == 0) {
            return false;
        }
    }
    return true;
}

std::string text(const nlohmann::json& input, const char* key) {
    if (!input.is_object()) {
        return {};
    }
    const auto found = input.find(key);
    return found != input.end() && found->is_string() && uuid_shaped(found->get<std::string>())
               ? found->get<std::string>()
               : std::string();
}

// The workspace a workspace capability's input names through its object. Each read is one
// indexed PostgreSQL lookup of a routing row; no workspace is called.
std::string object_workspace(const Capability* capability,
                             const nlohmann::json& input,
                             const ApiContext& context) {
    control::Store& store = *context.store;
    const std::string object = capability ? capability->object : std::string();
    const auto repository_workspace = [&store](const std::string& id) -> std::string {
        if (id.empty()) {
            return {};
        }
        const auto repository = store.get_repository(id);
        return repository ? repository->workspace_id : std::string();
    };
    for (const char* key : {"repository_id", "repository"}) {
        if (auto found = repository_workspace(text(input, key)); !found.empty()) {
            return found;
        }
    }
    if (object == "repository") {
        if (auto found = repository_workspace(text(input, "id")); !found.empty()) {
            return found;
        }
    }
    if (const std::string mount = text(input, "mount_id"); !mount.empty()) {
        if (const auto route = store.get_secret_mount_route(mount)) {
            return route->workspace_id;
        }
    }
    std::string application = text(input, "application_id");
    if (application.empty() && object == "application") {
        application = text(input, "id");
    }
    if (!application.empty()) {
        if (const auto found = store.get_application(application)) {
            return found->workspace_id;
        }
    }
    if (const std::string app = text(input, "app_id"); !app.empty()) {
        if (const auto found = store.get_web_app(app)) {
            return found->workspace_id;
        }
    }
    if (const std::string build = text(input, "build_id"); !build.empty()) {
        if (const auto found = store.get_build(build)) {
            return found->workspace_id;
        }
    }
    // An app or build credential acts for its app or build (Registry's token checks set these).
    const std::string& audited = context.audit_object;
    if (audited.starts_with("app/") && uuid_shaped(audited.substr(4))) {
        if (const auto found = store.get_web_app(audited.substr(4))) {
            return found->workspace_id;
        }
    }
    if (audited.starts_with("build/") && uuid_shaped(audited.substr(6))) {
        if (const auto found = store.get_build(audited.substr(6))) {
            return found->workspace_id;
        }
    }
    if (audited.starts_with("repository/")) {
        const std::string rest = audited.substr(11);
        const std::string id = rest.substr(0, rest.find('/'));
        if (auto found = uuid_shaped(id) ? repository_workspace(id) : std::string();
            !found.empty()) {
            return found;
        }
    }
    return {};
}

// The workspace as a route: live, and in the space the event names (when it names one).
AuditRoute live_workspace(const ApiContext& context,
                          const std::string& workspace_id,
                          const std::string& customer_id) {
    if (workspace_id.empty() || context.audit_recorder == nullptr || context.store == nullptr) {
        return {};
    }
    const auto workspace = context.store->get_workspace(workspace_id);
    if (!workspace || workspace->desired == "deleted" ||
        (!customer_id.empty() && workspace->customer_id != customer_id)) {
        return {};
    }
    return AuditRoute{AuditLog::Workspace, workspace->customer_id, workspace->id};
}

} // namespace

std::string_view to_string(AuditLog log) {
    return log == AuditLog::Workspace ? "workspace" : "central";
}

std::span<const std::string_view> central_audit_capabilities() {
    return central;
}

std::span<const std::string_view> workspace_audit_capabilities() {
    return workspace;
}

std::vector<std::string> workspace_audit_actions() {
    std::vector<std::string> actions(workspace.begin(), workspace.end());
    for (const std::string_view direct : {"deployment_phase", "push_change", "reference_update"}) {
        actions.emplace_back(direct);
    }
    // Workspace events recorded before lake#561 renamed these experiment capabilities
    // (get_experiment, start_experiment, stop_experiment) keep their names.
    for (const std::string_view renamed : {"get_experiment_study", "start_study", "stop_study"}) {
        actions.emplace_back(renamed);
    }
    std::ranges::sort(actions);
    return actions;
}

std::optional<AuditLog> audit_log_of(std::string_view capability) {
    if (listed(central, capability)) {
        return AuditLog::Central;
    }
    // Each named engine capability is a workspace_query or workspace_act call (lake#650).
    if (listed(workspace, capability) || !engine_capability_of(capability).empty()) {
        return AuditLog::Workspace;
    }
    return std::nullopt;
}

std::string acted_workspace(const Capability* capability,
                            std::string_view name,
                            const nlohmann::json& input,
                            const ApiContext& context,
                            std::string_view outcome,
                            bool handled) {
    if (!handled || outcome == "denied" || context.store == nullptr) {
        return {};
    }
    if (!context.audit_workspace_id.empty()) {
        return context.audit_workspace_id;
    }
    return audit_log_of(name) == AuditLog::Workspace ? object_workspace(capability, input, context)
                                                     : std::string();
}

AuditRoute audit_route(std::string_view name,
                       const std::string& acted,
                       const ApiContext& context) {
    if (audit_log_of(name) != AuditLog::Workspace || context.audit_recorder == nullptr) {
        return {};
    }
    return live_workspace(context, acted, context.audit_customer_id);
}

AuditRoute audit_route(const Capability* capability,
                       std::string_view name,
                       const nlohmann::json& input,
                       const ApiContext& context,
                       std::string_view outcome,
                       bool handled) {
    if (audit_log_of(name) != AuditLog::Workspace || context.audit_recorder == nullptr) {
        return {};
    }
    return audit_route(name, acted_workspace(capability, name, input, context, outcome, handled),
                       context);
}

AuditRoute workspace_audit_route(const ApiContext& context, const std::string& workspace_id) {
    return live_workspace(context, workspace_id, {});
}

audit::Result<std::optional<audit::Event>> record_audit(const ApiContext& context,
                                                        const audit::EventDraft& draft,
                                                        const AuditRoute& route) {
    using Recorded = std::optional<audit::Event>;
    if (route.log == AuditLog::Workspace && context.audit_recorder != nullptr) {
        audit::EventDraft scoped = draft;
        if (scoped.customer_id.empty()) {
            scoped.customer_id = route.customer_id;
        }
        if (scoped.details.is_object() && !scoped.details.contains("workspace_id")) {
            scoped.details["workspace_id"] = route.workspace_id;
        }
        auto pending = audit::pending(scoped);
        const std::string event_id = pending.id;
        if (context.audit_recorder->add(
                route.customer_id, route.workspace_id, std::move(pending))) {
            return Recorded{};
        }
        // Delivery failure is control-plane evidence. Keep workspace payloads, object names
        // and request details out of the central chain, including on the sync entry points.
        const audit::EventDraft failure{
            .customer_id = route.customer_id,
            .kind = "audit_delivery",
            .principal = scoped.principal,
            .object = "workspace/" + route.workspace_id,
            .action = "audit_delivery_failed",
            .outcome = "failure",
            .request_id = scoped.request_id,
            .details = {{"workspace_id", route.workspace_id},
                        {"event_id", event_id},
                        {"operation_outcome", scoped.outcome},
                        {"error_code", "audit_queue_full"}},
        };
        const audit::IdentityOptions options{.configured_seed = context.identity_seed,
                                             .instance_kek = context.instance_kek};
        const auto evidence = context.store
                                  ? audit::record(*context.store, failure, options)
                                  : audit::Result<audit::Event>(std::unexpected(
                                        audit::Error{"store_unavailable", "no control store"}));
        if (!evidence) {
            return std::unexpected(
                audit::Error{"audit_queue_full",
                             "workspace audit queue is full; delivery failure evidence "
                             "could not be recorded"});
        }
        return std::unexpected(audit::Error{
            "audit_queue_full",
            "workspace audit queue is full; delivery failure recorded as " + evidence->id});
    }
    const audit::IdentityOptions options{.configured_seed = context.identity_seed,
                                         .instance_kek = context.instance_kek};
    auto recorded = audit::record(*context.store, draft, options);
    if (!recorded) {
        return std::unexpected(recorded.error());
    }
    return Recorded{std::move(*recorded)};
}

} // namespace lake::api
