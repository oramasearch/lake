# Control stores

`control::service_condition` derives the one status `list_services` and the Services page show,
with the reason a service cannot progress on its own (unassigned, delete not yet observed). Both
stores leave a deleted workspace's primary Zetta out of `list_services` once the workspace is
observed `Deleted` (lake#452, [services.md](../../docs/api/services.md)).

Both stores implement `delete_workspace_cascade` (lake#514, migration `0065`): one transaction
tombstones the workspace's repositories with their retention ended, deletes its apps, cancels its
unfinished builds, sets the workspace `deleted` with its services absent, and records the deletion
in `workspace_deletions`; repeating it answers the record. `pending_workspace_deletions` and
`complete_workspace_deletion` drive the resume ([workspaces.md](../../docs/api/workspaces.md#deletion)).

`WorkspaceService` caches deployment catalogues and project/pipeline definitions in memory for
five seconds, scoped to the workspace and connection generation. Successful writes explicitly
invalidate entries and prevent in-flight stale refills. Ownership, readiness and credentials
are checked even on hits; mutable datasets, connections, runs and usage remain uncached. Every
call takes a workspace ID; agent-task callers derive it in the API registry before reaching this
service, so omission cannot select a default workspace.
See [page performance](../../docs/ui/performance.md) for limits and correctness tests.
A workspace refusal keeps Zetta's `error.path` in `ZettaControlError::path` (lake#463).

Stream ownership is persisted by both stores and bound to customer/workspace/topic; agent-task
enumeration is customer scoped and returns the task and agent data needed by shared Streams views
in one query. `AgentBus::call` uses the existing Zetta transport for Streams reads, while
`MemoryAgentBus` provides non-consuming fake browse and named call counts. Space `write` grants
permit stream publication/editing without granting deletion. See `docs/control-plane/streams.md`.

Memory and PostgreSQL stores implement the public Store contract together. Token issuance retains
its parent and caps expiry; authentication checks the complete session/device ancestry. PostgreSQL
DDL lives in `db/schema.sql` and must be applied before the containing binary.

Verify changes with the full PostgreSQL-backed static and shared CTest suites. Keep the identity
contract in `docs/api/identity.md` updated with changes to token derivation.

Password sign-up provisions a personal customer and ownership grant in the same PostgreSQL
transaction as the account and session. The memory store stages fallible credential derivation
before publishing account rows. See `docs/api/provisioning.md`.

Customer lists optionally filter by user ID before the limit. Audit lists apply exact request
filters and event-ID cursors before the limit in both stores. The test regressions and fixture
rules are in `docs/build/testing.md`; the public query contract is in `docs/api/audit.md`.
`audit::Filter` also selects newest-first order and leaves out the successful events of named
actions, and `Store::principal_names` names TAAP identities, in both stores (lake#445).

PostgreSQL session authentication uses a unique public lookup UUID and constant-time SHA-256
verification of one random bearer. Legacy sessions are invalidated by migration `0022_session_lookup`.
Password Argon2 policy remains unchanged. See `docs/api/auth.md` and `docs/control-plane/schema.md`.

Agent tasks (lake#269, #414) use a conditional upsert into `agent_task_status`, so one initial or
retry claim wins. Activity renews its lease; an indexed expiry update reclaims at most twice,
then fails the third attempt. A finished task keeps its phase and loses its credential digest. `agent_bus.cpp` publishes
tasks with their informational workspace ID on Zetta pub/sub and applies what agents publish;
`stale_agent_tasks` is what its loop re-publishes or fails. Memory and PostgreSQL stores can also
select the latest same-kind task in a conversation so a follow-up carries its prior turn. See
`docs/api/agent-bus.md`.

Pending retry notices survive publication failures and are published into the task workspace
before dispatch. No HTTP request runs the sweep. Shared bus credentials permit progress from a
hung old claimant after re-claim; terminal writes are atomic and first-write-wins. The lease
contract and memory/PostgreSQL proof are in [agent-bus.md](../../docs/api/agent-bus.md).

`web_apps.cpp` defines the Ship App JSON and deterministic namespace contract. Memory and
PostgreSQL stores enforce live name/host uniqueness and app/replica quotas; desired writes advance
generation and PostgreSQL notifies `lake_apps`. The operator writes token hashes only; Lake checks
purpose, current binding and target, including the exact app repository for pull tokens, and purges
rows after Deleted acknowledgement. See `docs/control-plane/web-apps.md`.

Protected-app forward authentication derives its signing key with `lake/app-session/v1` and
keeps one-use authorization-code hashes in control-plane storage. Both stores check the app host,
workspace, membership, expiry, and use state; PostgreSQL consumes a code atomically. See
`docs/api/web-apps.md` and `docs/control-plane/web-apps.md`.

`builds.cpp` defines build JSON, its successful public digest reference, plus source, push and event
token purposes. Memory and PostgreSQL stores insert queued rows, retain operator observations and
notify `lake_builds` on create/cancel. See `docs/control-plane/builds.md`.

Both stores implement `create_organization_space` and `convert_personal_space` as one transaction
each, list a user's memberships and the invitations addressed to an email or handles, and keep an
organization-scope membership's grant on the organization's own space in step with the membership.
See `docs/api/organizations.md`.
Agent launches and cron schedules carry the workspace that holds their agent (migration
`0043_agent_launch_workspaces`, lake#442). Both stores filter launch, schedule and scheduled-run
lists by it. `expire_unstarted_agent_launches` withdraws, with a `stop_reason`, a launch the
operator has not reported on within the timeout. `agent_runs.cpp` holds that timeout,
`agent_launch_progress` (what a person is told about a waiting run), and `trigger_cron`, which
now says why a plain-words schedule is refused. See `docs/api/agent-launches.md`.
Stores carry the workspace they belong to (migration `0044_store_workspaces`, lake#465). Both
stores record it on `create_store` and filter `list_stores` by it; `WorkspaceService::store` and
`delete_store` treat another workspace's store as `not_found`. See
`docs/control-plane/storage.md`.
`workspace_records.cpp` is lake's one mechanism for records in a workspace Zetta (lake#370,
[workspace-records.md](../../docs/architecture/workspace-records.md)): sealed, blind-indexed
records in `lake_*` collections of the workspace's keyed record store (zetta#1231) through the
workspace caller, with engine-decided conditional writes, `busy` retries and a read-modify-write
`update`. `ask_messages.cpp` is its first collection. The stores hold no Ask Orama conversations:
the retired `ask_messages` table was copied and then dropped by migration `0050_drop_ask_messages`
(lake#483), and its legacy reads went with it. `opportunities.cpp` is the first mutable
collection (lake#376): a scan's Opportunities keyed by scan and rank and written only if absent,
status changes conditional on the revision read; `assign_legacy_opportunities` and
`legacy_opportunities` serve only the copy (migration `0046_opportunity_workspaces`).
`stream_owners.cpp` is the second
(lake#379): one `if_absent` record per topic, keyed by the topic's blind index; the stores keep only
`assign_legacy_stream_owners` and `legacy_stream_owners` for the copy (migration
`0047_stream_owner_workspaces`). `drive_share_links.cpp` is the drive share
link collection (lake#350); the stores keep only `drive_share_link_route` (the content-free route
of a pre-move token) and `assign_legacy_drive_share_links`/`legacy_drive_share_links` for the copy
(migration `0048_drive_share_link_workspaces`). `forge_records.cpp` is the forge record collection
(lake#341); the stores keep only `assign_legacy_forge_records`/`legacy_forge_records` for the copy
(migration `0054_forge_record_workspaces`), and no longer write or read `forge_records`.
`secret_records.cpp` is the secrets collections' codecs (lake#328-#332); the stores no longer hold
mounts, secrets, keys or data keys. They keep `secret_mount_routes` and the mount's policy objects
(`create_secret_mount_route`), the customer KEK, which wraps and unwraps data keys without leaving
the store and keeps the replaced version during a rotation, and
`assign_legacy_secrets`/`legacy_secret_*` for the copy (migration `0057_secret_workspaces`). Dynamic
leases are records of their mount's workspace (lake#362, `SecretLeaseCodec` here); the stores keep
only `secret_lease_routes`, the content-free route and due index (migration
`0058_secret_lease_workspaces`; PostgreSQL claims due rows with `FOR UPDATE SKIP LOCKED` and holds
them), and `legacy_dynamic_secret_leases` for the copy. A KEK
rotation still rewraps the retired `secrets_data_keys` rows and the PKI authorities in its
transaction.
`platform_records.cpp` is the platform collections' codecs (lake#358, lake#359), and `platform.cpp`'s
sync writes each repository's environments and rows into that repository's workspace, creating an
environment's policy object (external id `platform_environment_id`) on first sight, skipping a
repository whose rows have not changed since its last write, and marking only that repository
stale when its workspace cannot be written. The stores keep only
`assign_legacy_platform_environments`/`legacy_platform_environments` and
`assign_legacy_platform_mirror`/`legacy_platform_mirror` for the copy (migration
`0060_platform_workspaces`), and no longer write or read either table.
`application_webhook_records.cpp` is the next: an application's lifecycle events and its webhook
delivery attempts in the application's workspace (lake#356, lake#357), each attempt claimed with
`if_absent` before it is sent and settled with `if_revision`. The stores no longer write or read
`auth_app_webhook_events` or `auth_app_webhook_deliveries`; `assign_legacy_application_webhook_events`
and the two `legacy_application_webhook_*` reads serve only the copy (migration
`0049_application_webhook_workspaces`), and `create_application` stores the application's
immutable `workspace_id`.
`application_user_records.cpp` follows: an application's users and organizations in the
application's workspace (lake#353, lake#354), with uniqueness claims beside them. The stores no
longer write or read `auth_app_users` or `auth_app_organizations`, and session resolution no longer
joins them; they keep the pool's own part (`revoke_application_user_sessions`,
`delete_application_user_credentials`), `assign_legacy_application_directory` and the two
`legacy_application_*` reads for the copy (migration `0055_application_user_workspaces`). A sign-up
takes the caller's `user_id`, and a sign-up or first OAuth sign-in reports `created_user`.
Repositories carry the workspace they belong to (migration `0051_repository_workspaces`, lake#315).
Both stores refuse `create_repository` without a live workspace of the repository's space and
filter `list_repositories` by an optional workspace; `transfer_repository` is gone from the store
interface because the routing trigger keeps a repository's space fixed.
Repository usage lives in workspace Zettas (lake#348, lake#349, `metering.cpp`). Neither store
writes or reads metering rows any more; `assign_legacy_metering` reruns migration `0053`'s owner
assignment and `legacy_metering_usage`/`legacy_metering_build_records` page the retired rows for
the copy (usage rows without transfer are left out).
`application_credential_records.cpp` keeps an application's signing keys and webhooks in its
workspace (lake#352, lake#355). The stores no longer write or read `auth_app_signing_keys` or
`auth_app_webhooks`: they envelope and open the credentials with the keystore
(`encrypt_application_credential`/`decrypt_application_credential`, accepted only for the
`application.jwt.*` and `application.webhook.*` purposes), and `assign_legacy_application_credentials`
with the two `legacy_application_*` reads serve only the copy (migration
`0056_application_credential_workspaces`). `create_application` no longer writes a key.
`application_settings_records.cpp` keeps an application's settings in its workspace (lake#351).
The stores write and read only the routing columns of `auth_applications` (`create_application`
leaves the settings columns NULL, and `update_application_theme` is gone);
`assign_legacy_application_settings` reruns migration `0059`'s owner assignment and
`legacy_application_settings` pages the rows that still hold settings, for the copy only.
`repository_records.cpp` keeps a repository's description in its workspace (lake#315 part 2): the
codec, `get`/`create` (`if_absent`)/`update` (`if_revision`, reread on a conflict, `if_absent` for
a first record) and one listing per space. The stores no longer read `repositories.description`
(the Postgres columns answer `''`) and refuse one on create; `create_repository` takes an optional
`NewRepository::id` so the record can be written first. `assign_legacy_repository_descriptions` reruns
migration `0064`'s owner assignment and `legacy_repository_descriptions` pages the rows that still
hold a description, for the copy only. `set_repository_ref` and `list_repository_refs` are gone
with the `repository_refs` table (lake#319).
`application_authorization_codes.cpp` keeps an application's authorization codes in its workspace
(lake#363). The stores no longer write or read `auth_app_authorization_codes` except for the copy:
`assign_legacy_application_authorization_codes` reruns migration `0061`'s owner assignment of live
codes, `legacy_application_authorization_codes` pages the live codes by hash, and
`record_legacy_application_authorization_code_copied` marks a code `copied` before it is written.
`agent_task_records.cpp` keeps what was written for an agent task in its workspace (lake#374,
lake#375). The stores keep only the dispatch half: `create_agent_task` inserts no body (and takes
the caller's `id` and `parent_task_id`), `record_agent_task_event` moves the phase and clears lake's
notice, and `fail_agent_task` and the expiry update write lake's text to `agent_task_status.notice`.
`assign_legacy_agent_tasks` reruns migration `0062`'s owner assignment and `legacy_agent_tasks`
pages the tasks whose rows still hold content, for the copy only; the memory store's
`seed_legacy_agent_task` writes such a task for the copy's tests.
`workspace_audit.cpp` keeps a workspace's audit (decision 52, lake#317): its chain in
`lake_audit_events`, the n-th event at the blind key of `sequence:<n>` written `if_absent`
(`append_workspace_audit_event` signs it after the chain's end and passes a place another writer
took), the tail search, copied central events under their id, the filtered newest-first listing,
and `verify_workspace_audit`. `workspace_records.cpp`'s `list` takes a `keep` predicate for that
filtered walk. `platform.cpp` appends each deployment phase to the repository's workspace audit.
The stores' `assign_legacy_audit_events` reruns migration `0063`'s assignment with the current
workspace actions and `legacy_audit_events` pages the assigned central events in append order, for
the copy only.

`oci_records.cpp` keeps a tenant image's manifests and tags in its routed workspace (lake#383
phase A): the codecs (manifests, tags marked `copied` when the copy wrote them, and the blobs a
workspace received), a manifest put `if_absent` (a conflict answers the stored bytes, which a
digest fixes), a tag set `if_absent` or at the revision read (reread on a conflict), a copied tag
created only where none exists, and one listing per repository. The stores keep
`oci_repository_routes` (migration `0068`): a route is created only for a live workspace of the
space, answered again for the same workspace, a `conflict` for another, and deleted in
`delete_workspace_cascade`, which lists each as an image in state `routed`, or by
`delete_oci_repository_route` (the operator's `unroute_image_repository`), which removes `<image>`
with `cache/<image>` and, given a workspace, writes them again for it in the same transaction.

Both stores keep the operator's volumes (migration `0069`): `set_workspace_storage` refuses a smaller
size (PostgreSQL's `enforce_workspace_storage_growth` trigger as well) and advances the generation;
`request_volume_snapshot` upserts the target's policy with `requested_at` = now and checks that a
workspace target is live; `volume_snapshots` joins each policy with the operator's
`volume_snapshot_status`. The memory store starts with the `store` policy and adds one per workspace
it creates, as the migration and its trigger do; `set_workspace_storage_status` and
`set_volume_snapshot_status` stand in for the operator in tests.
