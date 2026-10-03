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
