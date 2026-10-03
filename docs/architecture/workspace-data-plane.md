# Workspace Data Plane

Refs [lake#311](https://github.com/oramasearch/lake/issues/311). Decisions: Steve,
2026-09-29. This document describes the target contract and the events-first implementation;
the inventory below deliberately includes existing violations that require separate changes.

## Ownership and Routing

Everything except multitenancy, dispatch, and billing belongs in a workspace Zetta. This includes
credentials, secrets, metrics, application state, content, history, logs, and events. Billing is an
exception whose design remains open; metering is workspace data, not an implicit billing exception.
A customer can own multiple workspaces. Lake's control stores retain customer/workspace identities,
memberships, grants, workspace-to-service routing, and content-free agent dispatch.

`workspace_id` is the immutable `workspaces.id` of a customer's workspace. Creates require it.
Missing, malformed, unknown, foreign-customer, non-Zetta, or withdrawn IDs return `invalid_input`
with `/workspace_id`. A known workspace without its Ready endpoint and mounted credential returns
`workspace_not_ready`. The person requests a workspace by name and environment on Workspaces.
There is no implicit default and no event fallback to Lake's Zetta or PostgreSQL.

Tasks, apps, and builds persist this routing identity in the control store in this first step;
their other data remains a separately tracked migration. Task claims return the ID and the task's
workspace URL. Child tasks must pass it explicitly. App builds must name their app's workspace.
Workspace callers validate ownership, resolve `workspaces.zetta_service_id`, and use that
service's endpoint and mounted credential. The operator reconciles desired state; Lake does
not provision Kubernetes resources. See [the workspace contract](../control-plane/workspaces.md)
for migration 0039, notifications, the schema guard and the approved dev-cluster replacement.

Migration assigns existing rows only when a customer has exactly one live Zetta service and that
service is its current default. Each assignment is recorded. Unresolved object/customer IDs are
reported for explicit remediation; an unresolved row cannot publish or read workspace events.
New rows always require a valid workspace. No event payload column is introduced.

Before cutover, inspect `workspace_assignments WHERE outcome = 'needs_selection'`; the rows
identify the object table, object ID and customer. Migration warnings report the same IDs.
This PR does not inspect a deployed database or choose among ambiguous workspaces. Resolve those
rows in an explicitly reviewed operator migration before serving traffic; the application has no
reassignment endpoint, and the routing trigger rejects later changes, including null-to-ID changes.
The migration test covers sole-default, multiple-workspace and withdrawn-workspace customers and
verifies that rerunning the migration preserves its assignment record.

## Navigation

One global workspace selector appears next to the space in the top bar, including when the space
has only one workspace. Design, Train, and Ship URLs have the form `/w/<workspace_id>/train/data`.
Switching workspaces preserves the page path. Links, active navigation ownership, breadcrumbs,
and forms preserve this identity. Forms and capability calls send `workspace_id` explicitly.
The path is authoritative; a contradictory form or customer ID is refused.

Old page URLs redirect to the last valid workspace remembered in a cookie, or the space's only
workspace. Multiple choices without a valid remembered ID require selection. A remembered ID is
validated against the current caller and customer. New workspace opens `/workspaces`, with
name/environment creation and typed-name deletion. The selector lists workspace rows, including
Pending workspaces; it does not infer workspaces from services.

## Events and Retention

| Location | Topic | Retention | Lifecycle |
|---|---|---|---|
| Lake bus | `agent-tasks.<agent>` | 1 hour and 10,000 events | One per configured library agent; payload is only `{task_id, customer_id, agent, kind}`. |
| Selected workspace | `agent-events` | 24 hours and 10,000 events | One per workspace, created lazily; never one per turn. |
| App's workspace | `app-events.<app>` | 24 hours and 5,000 events | Dropped when the app is deleted. |
| Build's workspace | `build-events.<build>` | 24 hours and 5,000 events | Dropped after a terminal build's 24-hour replay window. |

Creation sets explicit Zetta `settings.retention_ms` and `settings.max_events`. Existing managed
topics receive the same mutable retention settings. Zetta enforces both limits on publish and
maintenance; Lake does not implement a second event store. Longer history requires changing the
workspace topic's retention or its persistent Zetta store, never copying history to PostgreSQL.

Streams lists the selected workspace's topics and the space's per-agent dispatch rows. Workspace
topic isolation is structural, including two workspaces belonging to the same customer. Following
one task may still select its correlation ID within its own workspace topic. Dispatch inspection
continues to hide other customers' task IDs.

The scheduled owner loop reaps terminal build topics and orphaned app/build topics. Workspace
deletion removes the workspace Zetta and its topics. The bus inventory pass logs obsolete exposed
topics before dropping them; only configured `agent-tasks.*` remain. Reaped counts are operational
counters; moving their durable storage is covered by the metrics follow-up.

The authorized Zetta checkout currently bounds `receive_events.wait_ms` at 30,000 in
`src/core/api/pubsub_capabilities.cpp`. A 60-second upstream poll needs a Zetta change; repeating
two 30-second calls would not satisfy the idle audit-rate requirement. Agent-runtime must back
off after an empty poll and adopt the longer poll once supported. This dependency must remain
explicit in rollout verification.

## Cutover

There is no dual-publish phase. Coordinate schema application, Lake, and agent clients: apply the
recorded workspace assignments, remediate unresolved rows, stop old clients, deploy Lake with
workspace routing and bus cleanup, and deploy clients that pass the task's workspace ID on every
workspace call and delegated create. Agent-runtime is the first client follow-up, followed by each
library agent repository. The operator must vendor the schema and use its new guard before Lake
starts. Cross-repository code is outside this worktree and is tracked in the follow-ups.

## Moving a Table

Every workspace item in the audit below moves through one mechanism,
[workspace records](workspace-records.md): sealed records in `lake_*` collections of the
workspace Zetta's keyed record store (zetta#1231), reached through the workspace caller, with an
operator one-shot copy and verification before any table is dropped. That document gives the
Zetta primitive for each record shape, the C++ interface, the migration and cutover sequence, and
the checklist each follow-up applies. Conditional writes (`if_absent`, `if_revision`) are decided by
the engine, so mutable rows, one-use codes and claims use the same mechanism.

## Storage Audit

This is a source inventory of `db/schema.sql`, Lake's Zetta callers, and instance-local stores.
`multitenancy/dispatch` stays; `workspace` moves. Mixed rows require a split: only the stated
routing/authorization fields stay. Migration guards are schema bookkeeping, not application data.
The reported deployment had seven `orama_control` datasets, but Lake's current source addresses
only the three listed below; no names are invented for the four unobserved datasets.

| PostgreSQL table | Classification | Reason |
|---|---|---|
| `schema_migrations` | multitenancy/dispatch | Control-schema version bookkeeping. |
| `workspace_assignments` | multitenancy/dispatch | Recorded legacy routing assignments and unresolved ownership; no event bodies. |
| `workspace_data_plane_guard` | multitenancy/dispatch | Migration 0038 application marker. |
| `workspaces` | multitenancy/dispatch | Customer workspace identity, desired lifecycle and operator routing/status contract. |
| `workspaces_guard` | multitenancy/dispatch | Migration 0039 application marker for the operator schema Job. |
| `repository_names_guard` | multitenancy/dispatch | Migration 0040 application marker for active repository-name uniqueness. |
| `app_authorization_codes` | multitenancy/dispatch | Hashed, short-lived, single-use protected-app browser handoff codes. |
| `app_forward_auth_guard` | multitenancy/dispatch | Migration 0041 application marker for protected-app forward authentication. |
| `users` | multitenancy/dispatch | Identity reference for customer membership. |
| `organizations` | multitenancy/dispatch | Tenant identity and organization boundary. |
| `customers` | multitenancy/dispatch | Space ownership and tenant routing. |
| `repositories` | workspace | Repository metadata and lifecycle belong to its workspace. [Follow-up](https://github.com/oramasearch/lake/issues/315). |
| `objects` | multitenancy/dispatch | Policy object identities and ancestry for grants. |
| `teams` | multitenancy/dispatch | Membership group identities. |
| `memberships` | multitenancy/dispatch | Tenant and team membership. |
| `grants` | multitenancy/dispatch | Authorization grants. |
| `invitations` | multitenancy/dispatch | Pending tenant membership. |
| `lake_identity` | workspace | Signing credentials require explicit custody outside the control store. [Follow-up](https://github.com/oramasearch/lake/issues/316). |
| `audit_events` | workspace | Signed call history and details are event data. [Follow-up](https://github.com/oramasearch/lake/issues/317). |
| `audit_retention` | workspace | Audit-history storage policy follows its history. [Follow-up](https://github.com/oramasearch/lake/issues/318). |
| `repository_refs` | workspace | Repository reference state. [Follow-up](https://github.com/oramasearch/lake/issues/319). |
| `identity_documents` | workspace | Identity document bodies and verification evidence. [Follow-up](https://github.com/oramasearch/lake/issues/320). |
| `devices` | workspace | Device records and authentication state. [Follow-up](https://github.com/oramasearch/lake/issues/321). |
| `credentials` | workspace | Password, passkey, token and MFA credential material. [Follow-up](https://github.com/oramasearch/lake/issues/322). |
| `sessions` | workspace | Session bearer digests and authentication state. [Follow-up](https://github.com/oramasearch/lake/issues/323). |
| `keystore` | workspace | Encrypted private key custody. [Follow-up](https://github.com/oramasearch/lake/issues/324). |
| `oauth_providers` | workspace | Provider credentials and application configuration. [Follow-up](https://github.com/oramasearch/lake/issues/325). |
| `identity_oauth_flows` | workspace | Authentication exchange state. [Follow-up](https://github.com/oramasearch/lake/issues/326). |
| `identity_webauthn_challenges` | workspace | Authentication challenge state. [Follow-up](https://github.com/oramasearch/lake/issues/327). |
| `secrets_mounts` | workspace | Secret namespace and service configuration. [Follow-up](https://github.com/oramasearch/lake/issues/328). |
| `secrets_entries` | workspace | Secret names and lifecycle. [Follow-up](https://github.com/oramasearch/lake/issues/329). |
| `secrets_versions` | workspace | Encrypted secret bodies. [Follow-up](https://github.com/oramasearch/lake/issues/330). |
| `secrets_encryption_keys` | workspace | Customer key material and versions. [Follow-up](https://github.com/oramasearch/lake/issues/331). |
| `secrets_data_keys` | workspace | Wrapped data encryption keys. [Follow-up](https://github.com/oramasearch/lake/issues/332). |
| `sso_providers` | workspace | Federation provider configuration and secrets. [Follow-up](https://github.com/oramasearch/lake/issues/334). |
| `federation_issuers` | workspace | Issuer/application configuration; retain only membership grants centrally. [Follow-up](https://github.com/oramasearch/lake/issues/335). |
| `scim_users` | multitenancy/dispatch | External-to-local membership identity mapping. |
| `scim_groups` | multitenancy/dispatch | External-to-local team mapping. |
| `scim_group_members` | multitenancy/dispatch | Provisioned group membership. |
| `sso_provisioning_logs` | workspace | Provisioning history. [Follow-up](https://github.com/oramasearch/lake/issues/333). |
| `offline_recovery_grants` | multitenancy/dispatch | Revocable authorization grants. |
| `offline_recovery_attempts` | workspace | Account recovery workflow state. [Follow-up](https://github.com/oramasearch/lake/issues/336). |
| `offline_recovery_admin_approvals` | workspace | Recovery ceremony evidence. [Follow-up](https://github.com/oramasearch/lake/issues/337). |
| `offline_recovery_deliveries` | workspace | Recovery delivery payloads. [Follow-up](https://github.com/oramasearch/lake/issues/338). |
| `offline_recovery_notifications` | workspace | Device notification history. [Follow-up](https://github.com/oramasearch/lake/issues/339). |
| `offline_recovery_contacts` | workspace | Recovery contact configuration. [Follow-up](https://github.com/oramasearch/lake/issues/340). |
| `forge_records` | workspace | Reviews, issues, releases, comments and automation state. [Follow-up](https://github.com/oramasearch/lake/issues/341). |
| `pki_authorities` | workspace | Private root and intermediate credentials. [Follow-up](https://github.com/oramasearch/lake/issues/342). |
| `pki_certificates` | workspace | Issued certificate records. [Follow-up](https://github.com/oramasearch/lake/issues/343). |
| `pki_acme_orders` | workspace | Certificate issuance workflow. [Follow-up](https://github.com/oramasearch/lake/issues/344). |
| `pki_callers` | workspace | Service identity exchange evidence. [Follow-up](https://github.com/oramasearch/lake/issues/345). |
| `pki_recovery_ceremonies` | workspace | Root recovery workflow. [Follow-up](https://github.com/oramasearch/lake/issues/346). |
| `pki_recovery_shares` | workspace | Recovery-share evidence. [Follow-up](https://github.com/oramasearch/lake/issues/347). |
| `metering_usage` | workspace | Usage measurements, independent of billing design. [Follow-up](https://github.com/oramasearch/lake/issues/348). |
| `metering_build_records` | workspace | Build compute measurements. [Follow-up](https://github.com/oramasearch/lake/issues/349). |
| `drive_share_links` | workspace | Drive share links: token digest, folder, role, expiry, revocation. **Moved** to each workspace Zetta's `lake_drive_share_links` record collection by [lake#350](https://github.com/oramasearch/lake/issues/350) ([sharing.md](../api/sharing.md#where-links-live)); the synthetic `share-link:<id>` grant stays a grant, and `drive_share_link_routes` (multitenancy/dispatch: token digest → workspace for pre-move tokens) stays central. The table is read only by the one-shot copy until a later migration drops it. |
| `auth_applications` | workspace | Application identity-pool configuration. [Follow-up](https://github.com/oramasearch/lake/issues/351). |
| `auth_app_signing_keys` | workspace | Application signing credentials. [Follow-up](https://github.com/oramasearch/lake/issues/352). |
| `auth_app_users` | workspace | Application user records. [Follow-up](https://github.com/oramasearch/lake/issues/353). |
| `auth_app_organizations` | workspace | Application organization records. [Follow-up](https://github.com/oramasearch/lake/issues/354). |
| `auth_app_webhooks` | workspace | Webhook configuration and secrets. [Follow-up](https://github.com/oramasearch/lake/issues/355). |
| `auth_app_webhook_events` | workspace | Application event history. [Follow-up](https://github.com/oramasearch/lake/issues/356). |
| `auth_app_webhook_deliveries` | workspace | Delivery payloads and retry history. [Follow-up](https://github.com/oramasearch/lake/issues/357). |
| `agent_sessions` | workspace | Agent credential delegation and session state. [Follow-up](https://github.com/oramasearch/lake/issues/360). |
| `customer_storage_quotas` | multitenancy/dispatch | Tenant allocation policy; measurements move. |
| `platform_environments` | workspace | Repository environment definitions. [Follow-up](https://github.com/oramasearch/lake/issues/358). |
| `platform_mirror` | workspace | Deployment, log and environment state. [Follow-up](https://github.com/oramasearch/lake/issues/359). |
| `identity_taap_assertions` | workspace | Authentication replay evidence. [Follow-up](https://github.com/oramasearch/lake/issues/361). |
| `auth_app_authorization_codes` | workspace | Application authentication exchange state. [Follow-up](https://github.com/oramasearch/lake/issues/363). |
| `secrets_dynamic_leases` | workspace | Generated credentials and lease lifecycle. [Follow-up](https://github.com/oramasearch/lake/issues/362). |
| `custodial_identities` | workspace | Encrypted identity signing seeds. [Follow-up](https://github.com/oramasearch/lake/issues/364). |
| `services` | multitenancy/dispatch | Customer workspace/service identities and provisioning routing. |
| `service_status` | multitenancy/dispatch | Workspace endpoint and readiness routing. |
| `stores` | workspace | Bucket/filesystem/volume application definitions. [Follow-up](https://github.com/oramasearch/lake/issues/365). |
| `store_status` | workspace | Storage application status. [Follow-up](https://github.com/oramasearch/lake/issues/366). |
| `agent_launches` | workspace | Agent inputs/configuration; split content-free dispatch. [Follow-up](https://github.com/oramasearch/lake/issues/367). |
| `agent_launch_status` | workspace | Agent results, messages and credential digests. [Follow-up](https://github.com/oramasearch/lake/issues/368). |
| `agent_schedules` | workspace | Retired but retained application schedule records. [Follow-up](https://github.com/oramasearch/lake/issues/369). |
| `ask_messages` | workspace | Conversation bodies and answers. **Moved** to each workspace Zetta's `lake_ask_messages` record collection by [lake#370](https://github.com/oramasearch/lake/issues/370) ([workspace-records.md](workspace-records.md)); the table is read only by the one-shot copy until a later migration drops it. |
| `agent_cron_schedules` | workspace | Scheduled agent inputs and application settings. [Follow-up](https://github.com/oramasearch/lake/issues/371). |
| `agent_cron_schedule_status` | workspace | Schedule credentials and application status. [Follow-up](https://github.com/oramasearch/lake/issues/373). |
| `agent_cron_runs` | workspace | Run history and results. [Follow-up](https://github.com/oramasearch/lake/issues/372). |
| `agent_tasks` | workspace | Bodies and conversation data move; keep IDs/workspace/agent/dispatch deadlines. [Follow-up](https://github.com/oramasearch/lake/issues/374). |
| `agent_task_status` | workspace | Answers, messages and credential hashes move; keep dispatch claim/terminal state. [Follow-up](https://github.com/oramasearch/lake/issues/375). |
| `opportunities` | workspace | Auto-Suggest output and decisions. **Moved** to each workspace Zetta's `lake_opportunities` record collection by [lake#376](https://github.com/oramasearch/lake/issues/376) ([workspace-records.md](workspace-records.md)): saved in the workspace the scan ran in, status changes are conditional writes; the table is read only by the one-shot copy until a later migration drops it. |
| `apps` | workspace | App configuration, environment, Secret refs and token hashes; workspace ID added here. [Follow-up](https://github.com/oramasearch/lake/issues/377). |
| `builds` | workspace | Build inputs, results and token hashes; workspace ID added here. [Follow-up](https://github.com/oramasearch/lake/issues/378). |
| `stream_owners` | workspace | Moved: topic creators are sealed records in the topic's workspace Zetta (`lake_stream_owners`), so ownership follows the workspace incarnation; migration `0047_stream_owner_workspaces` records each row's workspace for the copy, and the table is dropped once it is verified ([lake#379](https://github.com/oramasearch/lake/issues/379), [streams.md](../control-plane/streams.md)). |
| `identity_core_guard`, `identity_mfa_guard`, `secrets_guard`, `sso_federation_guard`, `offline_recovery_guard`, `forge_guard`, `pki_guard`, `metering_guard`, `drive_guard`, `auth_app_guard`, `agent_guard`, `settings_guard`, `platform_guard`, `identity_exit_guard`, `token_attenuation_guard`, `session_lookup_guard`, `custodial_identity_guard`, `services_guard`, `service_catalog_guard`, `sqlite_services_guard`, `analytics_services_guard`, `stores_guard`, `credentials_guard`, `agents_guard`, `agent_cron_guard`, `agent_tasks_guard`, `agent_task_events_guard`, `web_apps_guard`, `web_apps_operator_contract_guard`, `builds_guard`, `streams_guard` | multitenancy/dispatch | Operator migration guards contain application timestamps only. |

| Zetta or other storage | Classification | Reason |
|---|---|---|
| Lake `agent-tasks.*` | multitenancy/dispatch | Content-free dispatch; explicit retention in this change. |
| Lake `agent-events` | workspace | Turn events move in this change. |
| Lake `app-events.*` | workspace | App events move in this change. |
| Lake `build-events.*` | workspace | Build logs/events move in this change. |
| Legacy `agent-turn.*` exposed topics | workspace | Obsolete per-turn history; cutover cleanup. |
| `orama_control.deployments` | workspace | Repository deployment state; operator-owned producer. [Follow-up](https://github.com/oramasearch/orama-operator/issues/210). |
| `orama_control.environment_env` | workspace | Environment values and application configuration. [Follow-up](https://github.com/oramasearch/orama-operator/issues/211). |
| `orama_control.domains` | workspace | Application domain configuration and status. [Follow-up](https://github.com/oramasearch/orama-operator/issues/212). |
| Retired `orama_control.workspaces` | multitenancy/dispatch | Superseded workspace routing, no longer accessed by Lake. |
| Lake Zetta users/tokens | workspace | Service credentials need a custody/bootstrap design. [Follow-up](https://github.com/oramasearch/zetta/issues/1172). |
| Lake Zetta audit history | workspace | History of control calls is still event data. [Follow-up](https://github.com/oramasearch/zetta/issues/1173). |
| Mounted service/store connections | workspace | Service passwords, tokens and storage credentials. [Follow-up](https://github.com/oramasearch/orama-operator/issues/213). |
| Mounted agent bus/service credentials | workspace | Agent authentication secrets. [Follow-up](https://github.com/oramasearch/orama-operator/issues/214). |
| `LAKE_STORE_ROOT/<customer>/store` | workspace | Liquid repository objects and history. [Follow-up](https://github.com/oramasearch/lake/issues/380). |
| `LAKE_STORE_ROOT/<customer>/index` | workspace | Repository and artifact search indexes. [Follow-up](https://github.com/oramasearch/lake/issues/381). |
| Instance registry storage | workspace | OCI, npm, Python and debug artifacts. [Follow-up](https://github.com/oramasearch/lake/issues/383). |
| Instance transparency/TUF/timestamp storage | workspace | Signed audit history, trust objects and evidence. [Follow-up](https://github.com/oramasearch/lake/issues/382). |
| Loki application/build/platform logs | workspace | Application log history, despite external storage. [Follow-up](https://github.com/oramasearch/orama-operator/issues/215). |
| Prometheus metrics and request logs | workspace | Operational metrics and usage need workspace routing. [Follow-up](https://github.com/oramasearch/lake/issues/384). |

Each workspace item outside the event move has an owning-repository follow-up linked above.
Identity bootstrap, shared operational history, and billing need explicit designs;
this first step does not silently classify credentials or metrics as control-plane exceptions.

### Client and Rollout Follow-ups

- [agent-runtime](https://github.com/oramasearch/agent-runtime/issues/6): client cutover, explicit IDs and idle backoff; implement first.
- [agent-ask-orama](https://github.com/oramasearch/agent-ask-orama/issues/12): pass the explicit task workspace through every data-plane call.
- [agent-project](https://github.com/oramasearch/agent-project/issues/17): pass the explicit task workspace through every data-plane call.
- [agent-application](https://github.com/oramasearch/agent-application/issues/3): pass the explicit task workspace through every data-plane call.
- [agent-experiments](https://github.com/oramasearch/agent-experiments/issues/3): pass the explicit task workspace through every data-plane call.
- [agent-autosuggest](https://github.com/oramasearch/agent-autosuggest/issues/11): pass the explicit task workspace through every data-plane call.
- [agent-creator](https://github.com/oramasearch/agent-creator/issues/9): pass the explicit task workspace through every data-plane call.
- [agent-eval-generator](https://github.com/oramasearch/agent-eval-generator/issues/6): pass the explicit task workspace through every data-plane call.
- [agent-autosuggest-corpus](https://github.com/oramasearch/agent-autosuggest-corpus/issues/6): pass the explicit task workspace through every data-plane call.
- [agent-project-corpus](https://github.com/oramasearch/agent-project-corpus/issues/6): pass the explicit task workspace through every data-plane call.
- [Zetta 60-second receive contract](https://github.com/oramasearch/zetta/issues/1174): upstream poll limit and timeout support.
- [Operator schema and event clients](https://github.com/oramasearch/orama-operator/issues/216): migration/guard handoff and workspace-aware app/build event publishers.

## Verification and Changes

Required proof: native static and shared builds, fresh local PostgreSQL fixtures, complete CTest
suites, and UI token gates. Focused coverage must check missing/unknown/foreign workspace IDs,
immutable routing, ambiguous migration reporting, two customers and two workspaces within one
customer, retention across a synthetic week, app/build reaping, bus cutover, and navigation/form
workspace preservation. Agent deployment and upstream long-poll acceptance are separate rollout
dependencies. Change topic defaults with their tests and this table in the same commit.
