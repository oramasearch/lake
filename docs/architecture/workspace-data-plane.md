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

Repository pages are the exception: they stay at their repository address and show the owning
workspace, because a repository names its own workspace and never changes it (lake#315). See
[the repository pages](../ui/repositories.md#the-owning-workspace).

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

The items records do not cover (the liquid stores, search indexes, registry and transparency log on
lake's store volume, and lake's telemetry: #380–#384; `repositories` and `repository_refs`,
lake#315 part 2 and #319, are done) are planned in [workspace storage plan](workspace-storage-plan.md), with the decisions Steve made
(decision 54, [lake#512](https://github.com/oramasearch/lake/issues/512)) and the issues in order.

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
| `workspace_deletions` | multitenancy/dispatch | One row per deleted workspace (`0065_workspace_deletions`, [lake#514](https://github.com/oramasearch/lake/issues/514)): who deleted it, when, the ids and names of the repositories, apps, builds and tenant image repositories it destroyed, and when lake finished removing what lives outside PostgreSQL. It outlives the workspace Zetta, so it cannot live there; it holds no content. [Deletion](../api/workspaces.md#deletion). |
| `workspaces_guard` | multitenancy/dispatch | Migration 0039 application marker for the operator schema Job. |
| `repository_names_guard` | multitenancy/dispatch | Migration 0040 application marker for active repository-name uniqueness. |
| `app_authorization_codes` | multitenancy/dispatch | Hashed, short-lived, single-use protected-app browser handoff codes. |
| `app_forward_auth_guard` | multitenancy/dispatch | Migration 0041 application marker for protected-app forward authentication. |
| `users` | multitenancy/dispatch | Identity reference for customer membership. |
| `organizations` | multitenancy/dispatch | Tenant identity and organization boundary. |
| `customers` | multitenancy/dispatch | Space ownership and tenant routing. |
| `repositories` | split | The row stays as routing (decision 54, S10): id, space, immutable `workspace_id` (`0051`, lake#315 part 1; unassigned rows are `needs_selection` until `0066`, lake#513, gives each its space's only live workspace), name, visibility and tombstone, which lake needs before it reads any workspace. **Moved** (lake#315 part 2, migration `0064`): the description, and later settings, are the repository's sealed record in `lake_repositories` of its workspace Zetta ([repositories.md](../api/repositories.md#where-the-description-lives-lake315-part-2)). New rows leave `description` empty; a later migration drops the column once the copy is verified. Forge records moved with [lake#341](https://github.com/oramasearch/lake/issues/341). |
| `objects` | multitenancy/dispatch | Policy object identities and ancestry for grants. |
| `teams` | multitenancy/dispatch | Membership group identities. |
| `memberships` | multitenancy/dispatch | Tenant and team membership. |
| `grants` | multitenancy/dispatch | Authorization grants. |
| `invitations` | multitenancy/dispatch | Pending tenant membership. |
| `lake_identity` | workspace | Signing credentials require explicit custody outside the control store. [Follow-up](https://github.com/oramasearch/lake/issues/316). |
| `audit_events` | split (decision 52) | Control-plane and identity events stay in this central hash chain. Events about a workspace's data **moved** to `lake_audit_events` in that workspace's Zetta, a chain of its own written off the request path ([lake#317](https://github.com/oramasearch/lake/issues/317), migration `0063`, [audit.md](../api/audit.md#which-log)); the central events recorded before the split are copied there by the one-shot copy and stay here, append-only. |
| `audit_retention` | multitenancy/dispatch | The space's retention of the central log, a policy row ([lake#318](https://github.com/oramasearch/lake/issues/318)). A workspace's audit is kept until the workspace is deleted (the collection's `retention_ms` is 0), never shorter ([audit.md](../api/audit.md#retention)). |
| `repository_refs` | dropped | A write-only copy of liquid's refs that nothing read (0 rows on dev). **Dropped** by migration `0064` ([lake#319](https://github.com/oramasearch/lake/issues/319), decision 54 S11): the liquid repository is the only record of refs, and `refs` reads it. |
| `identity_documents` | workspace | Identity document bodies and verification evidence. [Follow-up](https://github.com/oramasearch/lake/issues/320). |
| `devices` | workspace | Device records and authentication state. [Follow-up](https://github.com/oramasearch/lake/issues/321). |
| `credentials` | workspace | Password, passkey, token and MFA credential material. [Follow-up](https://github.com/oramasearch/lake/issues/322). |
| `sessions` | workspace | Session bearer digests and authentication state. [Follow-up](https://github.com/oramasearch/lake/issues/323). |
| `keystore` | workspace | Encrypted private key custody. [Follow-up](https://github.com/oramasearch/lake/issues/324). |
| `oauth_providers` | workspace | Provider credentials and application configuration. [Follow-up](https://github.com/oramasearch/lake/issues/325). |
| `identity_oauth_flows` | workspace | Authentication exchange state. [Follow-up](https://github.com/oramasearch/lake/issues/326). |
| `identity_webauthn_challenges` | workspace | Authentication challenge state. [Follow-up](https://github.com/oramasearch/lake/issues/327). |
| `secrets_mounts` | workspace | Secret namespace and service configuration. **Moved** (lake#328, migration `0057`): `lake_secrets_mounts` in the mount's workspace Zetta; the table is read only by the one-shot copy until a later migration drops it. |
| `secrets_entries` | workspace | Secret names and lifecycle. **Moved** (lake#329, migration `0057`): `lake_secrets_entries` in the mount's workspace Zetta; the table is read only by the one-shot copy until a later migration drops it. |
| `secrets_versions` | workspace | Encrypted secret bodies. **Moved** (lake#330, migration `0057`): `lake_secrets_versions` in the mount's workspace Zetta; the table is read only by the one-shot copy until a later migration drops it. |
| `secrets_encryption_keys` | workspace | Customer key material and versions. **Moved** (lake#331, migration `0057`): `lake_secrets_encryption_keys` in the mount's workspace Zetta; the table is read only by the one-shot copy until a later migration drops it. |
| `secrets_data_keys` | workspace | Wrapped data encryption keys. **Moved** (lake#332, migration `0057`): `lake_secrets_data_keys` in the mount's workspace Zetta; the table is read only by the one-shot copy until a later migration drops it. |
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
| `forge_records` | workspace | Reviews, issues, releases, comments and automation state. **Moved** (lake#341): each record lives in the workspace Zetta of its repository's workspace ([forge.md](../api/forge.md#where-forge-records-live)); migration `0054` records owners for the one-shot copy, and the table stays until a later migration drops it. |
| `pki_authorities` | workspace | Private root and intermediate credentials. [Follow-up](https://github.com/oramasearch/lake/issues/342). |
| `pki_certificates` | workspace | Issued certificate records. [Follow-up](https://github.com/oramasearch/lake/issues/343). |
| `pki_acme_orders` | workspace | Certificate issuance workflow. [Follow-up](https://github.com/oramasearch/lake/issues/344). |
| `pki_callers` | workspace | Service identity exchange evidence. [Follow-up](https://github.com/oramasearch/lake/issues/345). |
| `pki_recovery_ceremonies` | workspace | Root recovery workflow. [Follow-up](https://github.com/oramasearch/lake/issues/346). |
| `pki_recovery_shares` | workspace | Recovery-share evidence. [Follow-up](https://github.com/oramasearch/lake/issues/347). |
| `metering_usage` | workspace | Repository transfer, independent of billing design. **Moved** to the `lake_metering_usage` record collection of the repository's workspace Zetta by [lake#348](https://github.com/oramasearch/lake/issues/348) ([metering.md](../api/metering.md)): exact hourly counters, written off the request path. Its hourly storage gauge is retired, not moved: repository storage is a space-level measurement of `LAKE_STORE_ROOT/<customer>/store`, taken live on each read and stored nowhere until [lake#380](https://github.com/oramasearch/lake/issues/380) makes stores per workspace. The table is read only by the one-shot copy until a later migration drops it. |
| `metering_build_records` | workspace | Build compute measurements. **Moved** to the `lake_metering_build_records` record collection of the repository's workspace Zetta by [lake#349](https://github.com/oramasearch/lake/issues/349): one record per build result, written `if_absent`. The table is read only by the one-shot copy until a later migration drops it. |
| `drive_share_links` | workspace | Drive share links: token digest, folder, role, expiry, revocation. **Moved** to each workspace Zetta's `lake_drive_share_links` record collection by [lake#350](https://github.com/oramasearch/lake/issues/350) ([sharing.md](../api/sharing.md#where-links-live)); the synthetic `share-link:<id>` grant stays a grant, and `drive_share_link_routes` (multitenancy/dispatch: token digest → workspace for pre-move tokens) stays central. The table is read only by the one-shot copy until a later migration drops it. |
| `auth_applications` | split | Application identity-pool configuration. The settings (name, allowed origins, redirect URIs, theme) **moved** to the `lake_auth_applications` record collection of the application's workspace Zetta by [lake#351](https://github.com/oramasearch/lake/issues/351) ([apps.md](../api/apps.md#where-settings-live-lake351)): one record per application, the theme changed with one conditional write. The row stays as content-free routing (multitenancy/dispatch): `id`, `customer_id`, `pool_id` (named by the pool's sessions), `custom_sign_in_domain` (the host a custom-domain page is requested on, unique across lake), `workspace_id` (migration `0049`, required at registration, set once), `created_at`, `updated_at`. Migration `0059` records owners for the one-shot copy; a later migration drops the four settings columns. |
| `auth_app_signing_keys` | workspace | Application signing credentials. **Moved** to the `lake_auth_app_signing_keys` record collection of the application's workspace Zetta by [lake#352](https://github.com/oramasearch/lake/issues/352) ([apps.md](../api/apps.md#where-keys-and-webhooks-live-lake352-lake355)): one key ring per application, rotated with one conditional write, each seed kept as the secrets envelope inside the seal; the anonymous JWKS finds the workspace through `auth_applications.workspace_id`. The table is read only by the one-shot copy until a later migration drops it. |
| `auth_app_users` | workspace | Application user records. **Moved** to the `lake_auth_app_users` record collection of the application's workspace Zetta by [lake#353](https://github.com/oramasearch/lake/issues/353) ([apps.md](../api/apps.md#where-users-and-organizations-live-lake353-lake354)), with a pool user's email as a claim written `if_absent`; migration `0055` records owners for the one-shot copy, and the table stays until a later migration drops it. The person's `users` row, credentials and sessions stay with lake's identity tables (lake#320, lake#322, lake#323). |
| `auth_app_organizations` | workspace | Application organization records. **Moved** to `lake_auth_app_organizations` in the application's workspace by [lake#354](https://github.com/oramasearch/lake/issues/354), with its TAAP identity and name as claims; the table is read only by the one-shot copy until a later migration drops it. |
| `auth_app_webhooks` | workspace | Webhook configuration and secrets. **Moved** to `lake_auth_app_webhooks` in the application's workspace by [lake#355](https://github.com/oramasearch/lake/issues/355), each secret kept as the secrets envelope inside the seal. The table is read only by the one-shot copy until a later migration drops it. |
| `auth_app_webhook_events` | workspace | Application event history. **Moved** to the `lake_auth_app_webhook_events` record collection of the application's workspace Zetta by [lake#356](https://github.com/oramasearch/lake/issues/356) ([workspace-records.md](workspace-records.md)); the table is read only by the one-shot copy until a later migration drops it. |
| `auth_app_webhook_deliveries` | workspace | Delivery payloads and retry history. **Moved** to `lake_auth_app_webhook_deliveries` in the event's workspace by [lake#357](https://github.com/oramasearch/lake/issues/357); each attempt is claimed there before it is sent. No due-delivery index is kept centrally, because there is no background retry loop. The table is read only by the one-shot copy. |
| `agent_sessions` | workspace | Agent credential delegation and session state. [Follow-up](https://github.com/oramasearch/lake/issues/360). |
| `customer_storage_quotas` | multitenancy/dispatch | Tenant allocation policy; measurements move. |
| `platform_environments` | workspace | Repository environment definitions. **Moved** into the repository's workspace Zetta ([lake#358](https://github.com/oramasearch/lake/issues/358), migration `0060`, [platform.md](../api/platform.md#where-environments-live-lake358-lake359)); the environment's policy object stays here under a derived id. The table stays until the copy is verified. |
| `platform_mirror` | workspace | Deployment, log and environment state. **Moved** into the repository's workspace Zetta ([lake#359](https://github.com/oramasearch/lake/issues/359), migration `0060`). The table stays until the copy is verified. |
| `identity_taap_assertions` | workspace | Authentication replay evidence. [Follow-up](https://github.com/oramasearch/lake/issues/361). |
| `auth_app_authorization_codes` | workspace | Application authentication exchange state. **Moved** to the `lake_auth_app_authorization_codes` record collection of the application's workspace Zetta by [lake#363](https://github.com/oramasearch/lake/issues/363) ([apps.md](../api/apps.md#where-authorization-codes-live-lake363)): issued `if_absent`, redeemed once by a delete at the revision read, expired by the collection's five-minute retention; the exchange finds the workspace through `auth_applications.workspace_id`. Migration `0061` records the owners of live codes for the one-shot copy; the table is read only by that copy until a later migration drops it. |
| `secrets_dynamic_leases` | workspace | Generated credentials and lease lifecycle. **Moved** ([lake#362](https://github.com/oramasearch/lake/issues/362), migration `0058`): `lake_secrets_dynamic_leases` in the mount's workspace Zetta ([secrets.md](../api/secrets.md#where-leases-live-lake362)); the table is read only by the one-shot copy until a later migration drops it. |
| `custodial_identities` | workspace | Encrypted identity signing seeds. [Follow-up](https://github.com/oramasearch/lake/issues/364). |
| `services` | multitenancy/dispatch | Customer workspace/service identities and provisioning routing. |
| `service_status` | multitenancy/dispatch | Workspace endpoint and readiness routing. |
| `stores` | workspace | Bucket/filesystem/volume application definitions. [Follow-up](https://github.com/oramasearch/lake/issues/365). |
| `store_status` | workspace | Storage application status. [Follow-up](https://github.com/oramasearch/lake/issues/366). |
| `agent_launches` | workspace | Agent inputs/configuration; split content-free dispatch. [Follow-up](https://github.com/oramasearch/lake/issues/367). |
| `agent_launch_status` | workspace | Agent results, messages and credential digests. [Follow-up](https://github.com/oramasearch/lake/issues/368). |
| `agent_schedules` | workspace | Retired but retained application schedule records. [Follow-up](https://github.com/oramasearch/lake/issues/369). |
| `ask_messages` | workspace | Conversation bodies and answers. **Moved** to each workspace Zetta's `lake_ask_messages` record collection by [lake#370](https://github.com/oramasearch/lake/issues/370) ([workspace-records.md](workspace-records.md)); the copy was verified on dev (15 of 15 rows, `complete: true`) and migration `0050_drop_ask_messages` drops the table ([lake#483](https://github.com/oramasearch/lake/issues/483)). |
| `agent_cron_schedules` | workspace | Scheduled agent inputs and application settings. [Follow-up](https://github.com/oramasearch/lake/issues/371). |
| `agent_cron_schedule_status` | workspace | Schedule credentials and application status. [Follow-up](https://github.com/oramasearch/lake/issues/373). |
| `agent_cron_runs` | workspace | Run history and results. [Follow-up](https://github.com/oramasearch/lake/issues/372). |
| `agent_tasks` | split | Agent turns. The request body **moved** to the `lake_agent_tasks` record collection of the task's workspace Zetta by [lake#374](https://github.com/oramasearch/lake/issues/374) ([agent-bus.md](../api/agent-bus.md#where-a-task-lives-lake374-lake375)): one sealed record per task, written before the row. The row stays as content-free dispatch: `id`, `customer_id`, `workspace_id` (0038, immutable), `agent`, `kind`, `conversation_id`, `parent_task_id` (0062, the asking task), `scopes`, `deadline_seconds`, `desired`, `generation`, who asked, `published_at` and the timestamps. Migration `0062` records owners for the one-shot copy; a later migration drops `body`. |
| `agent_task_status` | split | Claim and terminal state. The agent's messages and its answer **moved** into the same record by [lake#375](https://github.com/oramasearch/lake/issues/375), written conditionally on the revision read once the row accepted the event, so the first finished answer stays. The row keeps the claim and its lease as one PostgreSQL statement with no workspace call: `phase`, `credential_sha256` (the SHA-256 of the short-lived task credential, which every task-credential call checks before any workspace is read), `started_at`, `finished_at`, `events_from`, `lease_until`, `lease_seconds`, `attempts`, `retry_notice_pending`, and `notice` (0062: lake's own retry and failure text, never a person's or an agent's). A later migration drops `message` and `result`. |
| `opportunities` | workspace | Auto-Suggest output and decisions. **Moved** to each workspace Zetta's `lake_opportunities` record collection by [lake#376](https://github.com/oramasearch/lake/issues/376) ([workspace-records.md](workspace-records.md)): saved in the workspace the scan ran in, status changes are conditional writes; the table is read only by the one-shot copy until a later migration drops it. |
| `apps` | workspace | App configuration, environment, Secret refs and token hashes; workspace ID added here. [Follow-up](https://github.com/oramasearch/lake/issues/377). |
| `builds` | workspace | Build inputs, results and token hashes; workspace ID added here. [Follow-up](https://github.com/oramasearch/lake/issues/378). |
| `stream_owners` | workspace | Moved: topic creators are sealed records in the topic's workspace Zetta (`lake_stream_owners`), so ownership follows the workspace incarnation; migration `0047_stream_owner_workspaces` records each row's workspace for the copy, and the table is dropped once it is verified ([lake#379](https://github.com/oramasearch/lake/issues/379), [streams.md](../control-plane/streams.md)). |
| `secret_mount_routes` | multitenancy/dispatch | Which workspace holds which secret mount (lake#328, migration `0057`); no name, value or key. |
| `secret_lease_routes` | multitenancy/dispatch | Which workspace holds which dynamic lease, and when the expiry loop must next look at it: the content-free due index (lake#362, migration `0058`); no role, value or ciphertext. |
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
| `LAKE_STORE_ROOT/<customer>/store` | workspace | Liquid repository objects and history. [Follow-up](https://github.com/oramasearch/lake/issues/380); [plan](workspace-storage-plan.md#lake380-the-liquid-store). |
| `LAKE_STORE_ROOT/<customer>/index` | workspace | Repository and artifact search indexes. [Follow-up](https://github.com/oramasearch/lake/issues/381); [plan](workspace-storage-plan.md#lake381-the-search-index). |
| Instance registry storage | workspace | OCI, npm, Python and debug artifacts. [Follow-up](https://github.com/oramasearch/lake/issues/383); the [plan](workspace-storage-plan.md#lake383-registry-storage) splits tenant images (workspace) from the instance's public registry. |
| Instance transparency/TUF/timestamp storage | instance (exception S7) | The instance's public commitment: one append-only Merkle tree of public, content-free statements, its signed checkpoints, the TUF metadata and the timestamp evidence that protect it. It stays at `LAKE_STORE_ROOT/<instance>/log/` on lake's store volume with no migration, an instance-level exception named in the [umbrella rule](https://github.com/oramasearch/orama-bridge/blob/main/CLAUDE.md#everything-goes-in-the-workspace-zetta-except-multitenancy-and-billing-still-to-design) (decision 54, S7; [lake#382](https://github.com/oramasearch/lake/issues/382)). The TUF and timestamp writers are safe only within one process (an in-process mutex and per-process temporary names); the store root lock (`.lake-owner.lock`) keeps a second process from writing, and that stays the protection. `lease_dynamic_secret`, `renew_dynamic_secret` and `revoke_dynamic_secret` stay in the central audit chain under decision 52 although the lease is a record of its mount's workspace, so each still appends its central event as the instance log leaf a relying party expects (the identity-exit scenario); the leaf carries the lease, mount and workspace ids and never the credential, role or mount name (`WorkspaceRecordStores.DynamicSecretsInAWorkspaceKeepTheirInstanceLogLeaf`). See the [plan](workspace-storage-plan.md#lake382-transparency-log-tuf-and-timestamp-evidence). |
| Loki application/build/platform logs | workspace | Application log history, despite external storage. [Follow-up](https://github.com/oramasearch/orama-operator/issues/215). |
| Prometheus metrics and request logs | instance (decision 54 S9) | **Decided and done** ([lake#384](https://github.com/oramasearch/lake/issues/384), [PR #521](https://github.com/oramasearch/lake/pull/521)): lake's process metrics and request logs are instance operations data, a named exception to the umbrella rule. Request log lines carry the route pattern and captured UUIDs, never the raw path, so no customer content; each workspace gets its own `lake_requests` series in its Zetta, written off the request path ([telemetry.md](../serving/telemetry.md#the-workspace-request-series), [plan](workspace-storage-plan.md#lake384-prometheus-metrics-and-request-logs)). |

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
