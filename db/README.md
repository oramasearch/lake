# Database schema

`schema.sql` is the idempotent PostgreSQL source of truth. Migration `0022_session_lookup`
adds a unique public session selector and revokes legacy sessions without a selector once.
New session rows store SHA-256 of a random bearer. Apply it before the new binary. The operator
must vendor the same bytes and use `session_lookup_guard`; that handoff is a separate change.
See `docs/control-plane/schema.md` and `docs/api/auth.md` for rollout and replacement credentials.

Migrations `0034_web_apps` and `0035_web_apps_operator_contract` add the App desired/observed row,
runtime mode, optional source entrypoint and operator-written token hashes. Their rollout guard is
`web_apps_operator_contract_guard`. Apply before exposing Apps; the upgrade removes the obsolete
credential table. Lake never stores app tokens. See `docs/control-plane/web-apps.md`.

Migration `0036_builds` adds the queued/cancelled image-build row and operator observation fields.
Its guard is `builds_guard`; desired writes notify `lake_builds`, and only SHA-256 hashes of
the operator's source, push and event credentials are stored. See `docs/control-plane/builds.md`.

Migration `0037_streams` adds durable topic creator ownership keyed by customer, workspace
service ID and topic. Its guard is `streams_guard`. Vendor/apply the schema in the
operator rollout before exposing Streams; see `docs/control-plane/streams.md`.

Migration `0040_active_repository_names` replaces the repository table constraint with the
partial unique index `repository_name_unique`, which applies only while `deleted_at IS NULL`.
Deleting a repository therefore releases its customer-scoped name while preserving the tombstone.
Apps, services, stores and workspaces already use equivalent live-row predicates for their name
indexes.

Migration `0041_app_forward_auth` adds Lake-owned, short-lived authorization-code state for the
protected-app browser handoff. Only SHA-256 code hashes are stored; exchange is single-use and
expired or spent rows are swept. Its guard is `app_forward_auth_guard`. See
`docs/api/web-apps.md` and `docs/control-plane/web-apps.md`.

Verify changes with the full PostgreSQL-backed static and shared CTest suites. Keep the identity
contract in `docs/api/identity.md` updated with changes to token derivation.

Migration `0042_agent_task_leases` adds renewable leases, bounded attempts and pending retry
notices to `agent_task_status`, with partial indexes for expiry and notice delivery. Legacy
Running rows receive a lease once. The latest guard is `agent_task_leases_guard`; the operator
must vendor this schema and update its guard before rollout. The contract, idempotency checks
and deferred operator configuration are in [agent-bus.md](../docs/api/agent-bus.md).

Migration `0043_agent_launch_workspaces` adds `workspace_id` to `agent_launches` and
`agent_cron_schedules` (the column the operator's launch and schedule queries already join on)
and `stop_reason` to `agent_launches`. Existing rows are assigned to the space's only live
workspace and recorded in `workspace_assignments`. Launches the operator never reported and that
are older than five minutes are withdrawn with a reason. New rows go through
`enforce_workspace_routing`. The latest guard is `agent_launch_workspaces_guard`; the operator
must vendor this schema and update its guard before rollout. See
[agent-launches.md](../docs/api/agent-launches.md).

Migration `0044_store_workspaces` adds `workspace_id` to `stores`, the column the operator's
`listStoresSQL` has joined on since orama-operator ef005a4 (lake#465). Existing stores are
assigned to the space's only live workspace and recorded in `workspace_assignments`; a space with
several or none is recorded as `needs_selection`. New stores go through
`enforce_workspace_routing` (trigger `store_workspace_routing`). The latest guard is
`store_workspaces_guard`; the operator must vendor this schema and update its guard before
rollout. `tests/test_operator_contract.cpp` runs the operator's statements against this schema.
See [storage.md](../docs/control-plane/storage.md).

Migration `0045_ask_message_workspaces` records the owner of each retired `ask_messages` row in
`workspace_assignments` (a space's only live workspace; otherwise `needs_selection`; rows without a
space are reported). Ask Orama conversations now live in the owning workspace Zetta (lake#370), so
lake no longer writes or reads the table except through the operator's `copy_workspace_records`.
It adds no column and drops nothing; `0050_drop_ask_messages` drops the table. The guard is
`ask_message_workspaces_guard`. See
[workspace-records.md](../docs/architecture/workspace-records.md).

Migration `0046_opportunity_workspaces` records the owner of each retired `opportunities` row in
`workspace_assignments`: the workspace the scan's agent task ran in (`task_workspace`); for a task
that names none, a space's only live workspace (`sole_workspace`); otherwise `needs_selection`, or
`deleted_workspace` when the scan's workspace is deleted. Design → Opportunities now live in the
owning workspace Zetta (lake#376), so lake no longer writes or reads the table except through the
operator's `copy_workspace_records`. It adds no column and drops nothing. The latest guard is
`opportunity_workspaces_guard`; the operator must vendor this schema and update its guard before
rollout.
The latest guard is `ask_message_workspaces_guard`; the operator must vendor this schema and update
its guard before rollout. See [workspace-records.md](../docs/architecture/workspace-records.md).

Migration `0047_stream_owner_workspaces` records the workspace of each retired `stream_owners` row
in `workspace_assignments`, keyed by `md5(customer_id, workspace_id, topic)` as a UUID because the
table has no id. A row whose workspace is live in the same space is `recorded_workspace`; a deleted
workspace is `workspace_deleted` (its topic went with it); anything else is `unknown_workspace` and
reported. Topic creators now live in the topic's workspace Zetta (lake#379), so lake no longer
writes or reads the table except through `copy_workspace_records`. It adds no column and drops
nothing. The latest guard is `stream_owner_workspaces_guard`; the operator must vendor this schema
and update its guard before rollout. See [streams.md](../docs/control-plane/streams.md).
Migration `0048_drive_share_link_workspaces` records the owner of each retired `drive_share_links`
row in `workspace_assignments` (the only live workspace of the repository's space; otherwise
`needs_selection`) and creates `drive_share_link_routes`: for each assigned row, the content-free
route (token digest → link id, workspace) that lets a pre-move bearer token find its workspace.
Drive share links now live in the workspace Zetta that holds them (lake#350); a new token names its
workspace itself. Lake no longer writes or reads `drive_share_links` except through
`copy_workspace_records`, which reapplies the assignment and routes before each run. Nothing is
dropped. The latest guard is `drive_share_link_workspaces_guard`; the operator must vendor this
schema and update its guard before rollout. See [sharing.md](../docs/api/sharing.md).
Migration `0049_application_webhook_workspaces` adds `auth_applications.workspace_id` (required for
new applications by the `application_workspace_routing` trigger, assignable once for a legacy
application, then immutable), assigns it where the space has one live workspace, and records the
owner of each retired `auth_app_webhook_events` row in `workspace_assignments`. Application events
and delivery attempts now live in the application's workspace Zetta (lake#356, lake#357); lake
reads the two tables only through `copy_workspace_records`. Nothing is dropped. The latest guard is
`application_webhook_workspaces_guard`; the operator must vendor this schema and update its guard
before rollout.

Migration `0050_drop_ask_messages` drops the retired `ask_messages` table (lake#483), after the
operator's copy into the workspace Zettas was verified on dev (15 of 15 rows, `complete: true`). The
`workspace_assignments` rows 0045 recorded for it stay. The table's original `CREATE` (from `0030`)
is removed from this file and 0045's block runs only while the table exists, so rerunning the file
never recreates it. The latest guard is `drop_ask_messages_guard`; the operator must vendor this
schema and update its guard before rollout.
Migration `0051_repository_workspaces` adds `repositories.workspace_id`, the workspace a repository
belongs to (lake#315, part 1: routing only). Earlier repositories, tombstoned ones included, get the
space's only live workspace and are recorded in `workspace_assignments`; a space with several or
none is recorded as `needs_selection` and reported. New repositories go through
`enforce_workspace_routing` (trigger `repository_workspace_routing`), which also keeps the space
fixed, so repositories are no longer transferred. The latest guard is `repository_workspaces_guard`;
the operator must vendor this schema and update its guard before rollout. See
[repositories.md](../docs/api/repositories.md#the-owning-workspace).
Migration `0053_metering_workspaces` retires `metering_usage` and `metering_build_records`
(lake#348, lake#349): repository transfer and build records live in the repository's workspace
Zetta, and repository storage is measured live and not recorded. It records each row's owner in
`workspace_assignments` for `copy_workspace_records` (the space's only live workspace, otherwise
`needs_selection`); usage rows without transfer are left out. Nothing is dropped. The latest guard
is `metering_workspaces_guard`; the operator must vendor this schema and update its guard before
rollout. See [metering.md](../docs/api/metering.md#cutover-lake348-lake349).
Migration `0054_forge_record_workspaces` retires `forge_records` (lake#341): issues, reviews,
comments, releases, webhooks, exports and branch protection rules live in the workspace Zetta of
their repository's workspace. It records each row's owner in `workspace_assignments` for
`copy_workspace_records`: `repository_workspace`, `needs_selection` while the repository has no
workspace (resolved by the copy once it has one), or `workspace_deleted`. Nothing is dropped. The
latest guard is `forge_record_workspaces_guard`; the operator must vendor this schema and update its
guard before rollout. See [forge.md](../docs/api/forge.md#migration-and-cutover-lake341).
Migration `0055_application_user_workspaces` retires `auth_app_users` and `auth_app_organizations`
(lake#353, lake#354): an application's users and organizations live in the workspace Zetta of the
application's workspace, with a pool user's email and an organization's TAAP identity and name as
claims there. It records each row's owner in `workspace_assignments` for `copy_workspace_records`:
`application_workspace`, `needs_selection` while the application has no workspace (resolved by the
copy once it has one), or `workspace_deleted`. A user row's id is md5(application id, newline, user
id) as a UUID. Nothing is dropped; `users`, `credentials` and `sessions` stay. The latest guard is
`application_user_workspaces_guard`; the operator must vendor this schema and update its guard
before rollout. See [apps.md](../docs/api/apps.md#where-users-and-organizations-live-lake353-lake354).
Migration `0056_application_credential_workspaces` retires `auth_app_signing_keys` and
`auth_app_webhooks` (lake#352, lake#355): an application's signing keys (one key ring per
application) and its webhooks live in the workspace Zetta of the application's workspace, each
seed and secret kept as the secrets envelope the table held, inside the sealed record. It records
each key ring's (by application) and webhook's owner in `workspace_assignments` for
`copy_workspace_records`: `application_workspace`, `needs_selection` while the application has no
workspace (resolved by the copy once it has one), or `workspace_deleted`. Nothing is dropped. The
latest guard is `application_credential_workspaces_guard`; the operator must vendor this schema and
update its guard before rollout. See
[apps.md](../docs/api/apps.md#where-keys-and-webhooks-live-lake352-lake355).
Migration `0057_secret_workspaces` retires the five 0009 secrets tables (lake#328-#332): mounts,
secrets, versions, named keys and data keys live in the workspace Zetta of each mount. It adds
`secret_mount_routes` (mount → workspace, content-free, immutable), gives each mount of a space with
exactly one live workspace that workspace and its route (others are `needs_selection`), keeps a
replaced customer KEK in `keystore` until rotation finishes, and drops `secrets_dynamic_leases`'
foreign key to `secrets_mounts`. Nothing is dropped. The latest guard is
`secret_workspaces_guard`; the operator must vendor this schema and update its guard before
rollout. See [secrets.md](../docs/api/secrets.md#migration-and-cutover-lake328-332).
Migration `0058_secret_lease_workspaces` retires `secrets_dynamic_leases` (lake#362): leases live in
their mount's workspace Zetta. It adds `secret_lease_routes` (lease → mount, workspace, content-free,
immutable), whose `due_at` is the expiry loop's due index, and reports leases whose mount has no
route. Nothing is dropped. The latest guard is `secret_lease_workspaces_guard`; the operator must
vendor this schema and update its guard before rollout. See
[secrets.md](../docs/api/secrets.md#migration-and-cutover-lake362).
Migration `0059_application_settings_workspaces` moves an application's settings out of
`auth_applications` (lake#351): its name, allowed origins, redirect URIs and theme are one sealed
record in the workspace Zetta of the application's workspace. The row stays as the content-free
routing that finds the application before its workspace is read (`id`, `customer_id`, `pool_id`,
`custom_sign_in_domain`, `workspace_id`, `created_at`, `updated_at`); the four settings columns
lose NOT NULL and their defaults, are NULL on every new row, and a row without a name may hold none
of them (`auth_applications_settings_check`). Each application that still holds settings has its
owner recorded in `workspace_assignments` under `object_table = 'auth_application_settings'` for
`copy_workspace_records`: `application_workspace`, `needs_selection` while the application has no
workspace (resolved by the copy once it has one), or `workspace_deleted`. Nothing is dropped. The
latest guard is `application_settings_workspaces_guard`; the operator must vendor this schema and
update its guard before rollout. See [apps.md](../docs/api/apps.md#where-settings-live-lake351).
Migration `0060_platform_workspaces` retires `platform_environments` and `platform_mirror`
(lake#358, lake#359): a repository's environments and the operator rows lake mirrors for it live in
the workspace Zetta of the repository's workspace. It gives each environment's policy object the
environment's derived id, `md5(repository id, newline, name)` as a UUID, as its external id (grants
keep pointing at the same object), and records each row's owner in `workspace_assignments` for
`copy_workspace_records`: `repository_workspace`, `needs_selection` while the repository has no
workspace (resolved by the copy once it has one), or `workspace_deleted`; a mirror row naming no
repository is reported. Nothing is dropped. The latest guard is `platform_workspaces_guard`; the
operator must vendor this schema and update its guard before rollout. See
[platform.md](../docs/api/platform.md#where-environments-live-lake358-lake359).
Migration `0061_application_authorization_code_workspaces` moves an application's one-use OIDC
authorization codes (lake#363) into the workspace Zetta of the application's workspace, where they
are issued `if_absent`, redeemed once by a delete at the revision read, and expired by the
collection's five-minute retention. Each live (unused, unexpired) code of
`auth_app_authorization_codes` has its owner recorded in `workspace_assignments` under
`object_table = 'auth_app_authorization_codes'` and `object_id = md5(code_hash)::uuid` for
`copy_workspace_records`: `application_workspace`, `needs_selection` while the application has no
workspace, or `workspace_deleted`; the copy marks a code `copied` before writing it. Nothing is
dropped. `app_authorization_codes` (protected-app forward auth) is unrelated and unchanged. The
latest guard is `application_authorization_code_workspaces_guard`; the operator must vendor this
schema and update its guard before rollout. See
[apps.md](../docs/api/apps.md#where-authorization-codes-live-lake363).
Migration `0062_agent_task_workspaces` moves what was written for an agent task out of
`agent_tasks` and `agent_task_status` (lake#374, lake#375): its request body, the agent's message
and its answer are one sealed record in the workspace Zetta of the task's workspace. Both tables
stay as the content-free dispatch rows the bus, the claim and its lease use; new rows leave `body`,
`message` and `result` empty. It adds `agent_tasks.parent_task_id` (the asking task of a delegated
turn) and `agent_task_status.notice` (lake's own retry and failure text), both filled from existing
rows, and records the owner of each task with content in `workspace_assignments` under
`object_table = 'agent_task_content'` for `copy_workspace_records`: `task_workspace`,
`needs_selection` while the task names no workspace, or `workspace_deleted`. Nothing is dropped.
The latest guard is `agent_task_workspaces_guard`; the operator must vendor this schema and update
its guard before rollout. See [agent-bus.md](../docs/api/agent-bus.md#where-a-task-lives-lake374-lake375).
Migration `0063_audit_workspaces` splits the audit by subject (decision 52, lake#317, lake#318):
`audit_events` keeps control-plane and identity events, and events about a workspace's data go to
that workspace's own chain in its Zetta. It records, in `workspace_assignments`, the workspace of
each central event about a workspace's data (`event_workspace`, or `workspace_deleted`, which stays
central) for `copy_workspace_records`; every other event stays central and is counted. Nothing is
dropped; `audit_events` stays append-only and `audit_retention` governs it. The latest guard is
`audit_workspaces_guard`; the operator must vendor this schema and update its guard before rollout.
See [audit.md](../docs/api/audit.md#moving-the-central-events-of-workspaces-migration-0063).
Migration `0064_repository_records` makes the repository row routing only (decision 54). It drops
`repository_refs` (lake#319), a copy of liquid's refs that nothing read; its `CREATE` (from `0006`)
is removed from this file, so rerunning the file never recreates it. It also records, in
`workspace_assignments` under `object_table = 'repository_descriptions'`, the owner of each
repository whose row holds a description (lake#315 part 2): `repository_workspace`,
`needs_selection` while the repository has no workspace, or `workspace_deleted`. The description
lives in the repository's workspace Zetta (`lake_repositories`), lake no longer writes or reads the
column, and `copy_workspace_records` (collection `repositories`) copies it; a later migration drops
the column. The latest guard is `repository_records_guard`; the operator must vendor this schema,
update its guard, and drop `repository_refs` from its vendored-schema test before rollout. See
[repositories.md](../docs/api/repositories.md#where-the-description-lives-lake315-part-2).
Migration `0065_workspace_deletions` makes deleting a workspace cascade (decision 54 S12,
lake#514). The transaction that sets a workspace's desired state to `deleted` also tombstones its
repositories still live or inside retention with their retention ended (which frees their names),
deletes its apps, cancels its unfinished builds, and records what it destroyed in
`workspace_deletions`. Lake then removes the tenant image tags and search segments and records the
central audit events, and sets `completed_at`; its owner loop resumes a row without it. The
operator reads only `workspaces.desired`, as before. The latest guard is
`workspace_deletions_guard`; the operator must vendor this schema and update its guard before
rollout. See [workspaces.md](../docs/api/workspaces.md#deletion).
Migration `0066_assign_unrouted_repositories` is the reviewed assignment `0051` left open (decision
54 S14, lake#513). Once, each repository whose `workspace_id` is still NULL is assigned its space's
only live workspace when the space now has exactly one, and recorded in `workspace_assignments`
(`repositories`: `sole_workspace`; `repository_descriptions`: `repository_workspace`); any other is
left alone and reported by a warning. The `repository_workspace_routing` trigger stays:
`enforce_workspace_routing` allows that one change (NULL to a live workspace of the same space, on
`repositories`) only while the transaction-local setting `lake.repository_assignment` is
`0066_assign_unrouted_repositories`, which the block sets and clears around its updates. The latest
guard is `assign_unrouted_repositories_guard`; the operator must vendor this schema and update its
guard before rollout. See [repositories.md](../docs/api/repositories.md#the-owning-workspace).
Migration `0068_oci_repository_routes` splits the registry (decision 54 S4-S6, lake#383 phase A).
`oci_repository_routes` is the content-free route from a tenant image repository (the space's
customer id and the name below it, `<image>` or `cache/<image>`) to the workspace whose Zetta holds
its manifests and tags (`lake_oci_manifests`, `lake_oci_tags`); it holds no manifest, digest or tag.
A build's first push writes it from the build's workspace and `create_image_repository` from an
explicit `workspace_id`; there is no default. A name is `<image>` or `cache/<image>` only; a build routes only its own target, and the operator's `unroute_image_repository` deletes a wrong route, or moves it to another workspace
in one transaction (`cache/<image>` with `<image>`). `enforce_oci_repository_route` refuses any change of a
route and any route to a workspace that is not live in the space, and the workspace deletion cascade
deletes a workspace's routes. Once, each image name the apps and builds of exactly one live
workspace use is routed there (`<name>` and `cache/<name>`); a name several workspaces use is
reported by a warning. npm, Python, debuginfod and one-component OCI stay with the instance, and
blobs stay in the instance registry until phase B. `0067` is held by the open lake#380 change. The
latest guard is `oci_repository_routes_guard`; the operator must vendor this schema and update its
guard before rollout (it never reads the table). See
[registry.md](../docs/api/registry.md#where-a-tenant-image-lives).
Migration `0069_workspace_volumes` gives the operator its volumes (decision 54 S13, operator#259,
operator#260). `workspaces.storage_gi` (default 5, 1 to 32767) is the desired size of each of the
workspace Zetta's volumes in GiB; `enforce_workspace_storage_growth` refuses a decrease, and the
operator reports `storage_gi_observed`, `storage_phase` and `storage_message` on the same row.
`volume_snapshot_policies` is content-free (target `store` or a workspace id, a five-field cron
`schedule` in UTC defaulting to daily at 03:00, `keep` defaulting to 7, `requested_at`); a trigger
gives every new workspace the default policy, another notifies `lake_volume_snapshots`, and the
backfill adds `store` and every live workspace once. `volume_snapshot_status` is written only by the
operator. Neither table refers to `workspaces`, so a deleted workspace's rows stay with its retained
snapshots. `0067` is lake#380's, below. The latest guard is
`workspace_volumes_guard`; the operator must vendor this schema and update its guard before rollout.
See [schema.md](../docs/control-plane/schema.md#workspace-volumes-migration-0069_workspace_volumes).
Migration `0067_workspace_store` (it sits before `0068` in `schema.sql`) adds
`workspaces.store_moved_at` (lake#380): NULL while a workspace's repositories are served from its
space's store on lake's volume, set by `move_workspace_store` `switch` once they are served from the
workspace's own encrypted store in its workspace Zetta. Nothing else moves into PostgreSQL. Its
guard is `workspace_store_guard`; the latest guard stays `workspace_volumes_guard`, and the operator
must vendor this schema before rollout. See
[workspace-store.md](../docs/architecture/workspace-store.md).

Migration `0070_drop_moved_tables` (lake#311) drops the retired copies of workspace data whose
one-shot copy was reported run and verified on dev: the tables `opportunities`, `stream_owners`,
`drive_share_links`, the six `auth_app_*` tables (signing keys, users, organizations, webhooks,
webhook events and deliveries), `auth_app_authorization_codes`, `metering_usage`,
`metering_build_records`, `forge_records`, the five `secrets_*` tables of `0009` and
`secrets_dynamic_leases`, `platform_environments` and `platform_mirror`; and the columns
`auth_applications.name`, `allowed_origins`, `redirect_uris` and `theme`, `agent_tasks.body`,
`agent_task_status.message` and `result`, and `repositories.description`. A first block refuses rows
with no recorded workspace owner (`RAISE EXCEPTION`, nothing dropped); it does not prove a copy ran,
so `copy_workspace_records` `plan`, `copy` and `verify` are a manual gate before applying. When it refuses, nothing is dropped. Either run `copy_workspace_records` `plan`, `copy` and `verify` for each table named with the previous lake image (`oramasearch/lake:20261005a`, the image on dev when this was written, or any image built before `0070`), resolve its unresolved rows and apply the schema again; or, on a development environment and only with the owner's approval (it destroys data), delete the named rows and apply again. Their `CREATE`s and columns
are removed from the earlier sections and the earlier migration blocks run only while they exist, so
a rerun never recreates them. Routing rows, routes, policy objects, `keystore`, `audit_events` and
the `workspace_assignments` record stay. Its guard is `drop_moved_tables_guard`; the operator
must vendor this schema, drop the four `CREATE TABLE IF NOT EXISTS secrets_*` entries from
`TestLakeSchemaMatchesVendoredSource` and add the guard to its rehearsal query before rollout. See
[schema.md](../docs/control-plane/schema.md#drop-moved-tables-migration).

Migration `0071_secret_mount_owners` (lake#561) drops `NOT NULL` from
`secret_mount_routes.organization_id`: a secret mount created without an organization belongs to
the account that owns its workspace, and its route stores NULL there. Nothing else changes and
nothing is dropped; idempotent. Its guard is `secret_mount_owners_guard`. See
[schema.md](../docs/control-plane/schema.md#secret-mount-owners-migration).

Migration `0072_usage_limits` (lake#564) adds `customer_usage_limits`: an account's limit on one
dimension of usage (customer, dimension, limit in the dimension's unit, warning percentage), the
content-free configuration `usage_limits` reads and `set_usage_limit` writes. The usage it is
compared with stays in each workspace Zetta; no price, plan or bill is stored. Nothing is dropped;
idempotent. Its guard is `usage_limits_guard`. See
[schema.md](../docs/control-plane/schema.md#usage-limits-migration).

Migration `0073_billing` (lake#564, decision 56) adds billing records as multitenancy:
`billing_plans` and `billing_plan_prices` (the price list, a price per usage dimension; since
lake#635 one row per publication of the usage-only list), `billing_subscriptions` (an account's
plan from the first of a month; unused since lake#635), `billing_invoices` (one
account and month, a total and its payment state; at most one that is not void) and
`billing_invoice_lines` (the quantity billed per dimension, frozen). Usage stays in each workspace
Zetta; no workspace content is kept. Nothing is dropped; idempotent. The latest guard is
`billing_guard`; the operator must vendor this schema and select that guard before rollout. See
[schema.md](../docs/control-plane/schema.md#billing-migration).

Migration `0074_payment_provider` (lake#620, decision 57) adds Stripe's records:
`billing_accounts` (an account's customer id at a payment provider, kept once),
`billing_invoices.payment_url` and `provider_status`, an index on the provider's invoice id, and
`billing_payment_events` (each provider event applied, by event id, so none is applied twice). Ids,
states and URLs only: no key and no card. Nothing is dropped; idempotent. The latest guard is
`payment_provider_guard`; the operator must vendor this schema and select that guard before
rollout. See [schema.md](../docs/control-plane/schema.md#payment-provider-migration).

Migration `0075_workload_resources` (lake#643) adds `workload_resources`, the resources the Orama
operator schedules each workload with (CPU millicores, memory, GPUs, GPU class and GPU memory,
written only by the operator), and a `class` on `billing_plan_prices` (now keyed by plan,
dimension and class) and `billing_invoice_lines`, so GPU time can carry a rate per class. Numbers
only; usage stays in each workspace Zetta. Nothing is dropped; idempotent. The latest guard is
`workload_resources_guard`; the operator must vendor this schema, select that guard and report the
resources before rollout. See [schema.md](../docs/control-plane/schema.md#workload-resources-migration).

Migration `0076_organization_deletion` (lake#645) adds `deleted_at` to `organizations` and
`customers`, set by `delete_organization` once the organization's space has no workspace and
everything it held is removed; the rows stay because the central audit chain names the space, and
lake never reads a marked one again. Nothing is dropped; idempotent. The latest guard is
`organization_deletion_guard`; the operator must vendor this schema and select that guard before
rollout. See [schema.md](../docs/control-plane/schema.md#organization-deletion-migration).
