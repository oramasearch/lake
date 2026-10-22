# Control stores

Both stores reject disabled or missing users when resolving agent launch, schedule and task
credentials (lake#951). Creation, claims and renewal serialize with offboarding on the user row
(or the memory-store mutex), so a cached stream context cannot create fresh authority afterwards.
Clerk disablement and account deletion withdraw the person's workloads and invalidate task leases;
workload history remains. See [Clerk offboarding](../../docs/api/clerk.md#webhooks).
`agents.cpp` checks the authenticated dispatcher and customer registration before generating or
signing a session key. `has_active_agent_certificate` selects an unrevoked, unexpired `agent`
certificate by customer and TAAP subject in memory and PostgreSQL. See
[agent session keys](../../docs/api/agents.md).

`pki.cpp` verifies tunnel CSRs, signs only the grant URI and bounds the client certificate's expiry.
It encodes every DNS/URI SAN as a typed ASN.1 value, validates ACME DNS and X.509 SPIFFE
identities, and rejects URI SAN delimiters before signing (lake#946). See
[PKI](../../docs/api/pki.md#certificate-identities).
`network_records.cpp` supplies empty session/activity pages explicitly pending zetta#1281.
See [tunnel credentials](../../docs/api/tunnel-grants.md) and migration 0085 in [network.md](../../docs/control-plane/network.md).

`network.cpp` shares secure-networking validation and row codecs; `network_memory.cpp` and
`postgres_store.cpp` implement scoped desired writes, joined status reads and atomic signed audit.
Non-null credential scopes are restricted to app-to-service connections; rotation writes the
unchanged spec to advance its desired generation.
The memory path uses the API actor's audit identity while PostgreSQL uses its configured KEK. The
shared code also derives active tunnel grants for `network_rules`. The rollout and operator handoff
are in [network.md](../../docs/control-plane/network.md); public shapes are in
[the API contract](../../docs/api/network.md) and [scoped credentials](../../docs/api/scoped-credentials.md).

`control::service_condition` derives the one status `list_services` and the Services page show,
with the reason a service cannot progress on its own (unassigned, delete not yet observed). Both
stores leave a deleted workspace's primary Zetta out of `list_services` once the workspace is
observed `Deleted` (lake#452, [services.md](../../docs/api/services.md)).

Both stores implement `delete_workspace_cascade` (lake#514, migration `0065`): one transaction
tombstones the workspace's repositories with their retention ended, deletes its apps, cancels its
unfinished builds, sets the workspace `deleted` with its services absent, and records the deletion
in `workspace_deletions`; repeating it answers the record. `pending_workspace_deletions` and
`complete_workspace_deletion` drive the resume ([workspaces.md](../../docs/api/workspaces.md#deletion)).

Both stores implement the deletions of lake#645: `delete_organization` (with `dry_run`, refused
while the organization's account has a live workspace; marks the organization and its account
`deleted_at`, migration `0076`, and every read and list of them leaves a marked row out),
`revoke_invitation`, `get_grant` and `revoke_grant`, and `delete_secret_mount_route`, the last
step of deleting a mount ([deletion.md](../../docs/api/deletion.md)). Both implement
`delete_account` (lake#810, migration `0091`): refused while the personal space has a live workspace
or the account is an organization's only owner; removes the account's identity rows, memberships
and grants and its space's billing contact, limits and policy objects, and marks the user and its
space deleted, the email and name erased, so every read of users leaves it out; and
`verify_mfa_code`, the step-up that changes nothing ([identity.md](../../docs/api/identity.md#deleting-an-account)).

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
operator has not reported on within the timeout. `withdraw_agent_workloads` withdraws, in one
transaction, every present launch (with a `stop_reason`) and schedule of an agent in a workspace,
when the agent is deleted or found gone (lake#524). `agent_runs.cpp` holds that timeout,
`agent_launch_progress` (what a person is told about a waiting run), and `trigger_cron`, which
now says why a plain-words schedule is refused. It also holds `checked_time_zone`, which refuses
a schedule time zone that is not an IANA name (lake#586) against `iana_time_zones` in
`time_zones.cpp` (generated by `tools/tz/generate-time-zones.py`), and `agent_schedule_progress`,
which tells a person a schedule's state in lake's words instead of the operator's Kubernetes text.
`agent_launch_scopes_for` gives a launch, schedule or conversation service the scopes every run has
plus the Lake reads of its agent's skills (agent-runtime#7), from `agent_skills` in
`agent_skills.cpp` (generated by `tools/agents/vendor-agent-skills.py` from agent-runtime's
`skills.json`). See `docs/api/agent-launches.md`.
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
status changes conditional on the revision read; the retired table (migration
`0046_opportunity_workspaces`) is dropped by `0070_drop_moved_tables` with its legacy reads.
`transcripts.cpp` is the transcripts collection (lake#561): one record per transcript, found by the
space, edited and deleted at the revision read, with the paging and excerpt helpers
`list_transcripts` uses; it was never a PostgreSQL table.
`solutions.cpp` is a workspace's own AI solutions (lake#597): one record per solution in
`lake_solutions`, found by the space, edited and deleted at the revision read, with the rules a
solution keeps and the copy of one of Orama's catalogue entries; it was never a PostgreSQL table.
`methodologies.cpp` is a workspace methodology's pure rules (lake#655): the manifest it reads and
the findings Validate answers without running anything (manifest, entrypoint, lock file), drafts from
the Python templates in `methodology_templates.cpp`, the copy of one of Orama's, the image's
Dockerfile, the validation sample and the metrics a result may carry. The stores keep the content-free
`methodology_runs` dispatch rows and the operator's `methodology_run_status` (migration `0078`), and
resolve a run's `lmr.` credential by its digest.
`mcp_connections.cpp` is a workspace's MCP connections (decision 60, lake#652): one record per
connection in `lake_mcp_connections`, found by the space, unique by name and prefix, changed and
deleted at the revision read (`change_mcp_connection` rereads on a conflict), with the prefix rule
and the OAuth sign-in states in `lake_mcp_oauth_states`, taken once by a delete at the revision read.
`agent_revisions.cpp` is an agent's definition revisions (lake#653): one append-only record per
`create_agent` or `update_agent` in `lake_agent_revisions`, found by the account and the agent's id,
and `agent_definition_of`, the fields of an engine agent a person designs.
`agent_stream_triggers.cpp` is an agent's stream triggers (lake#677): one record per agent and
workspace stream in `lake_agent_stream_triggers`, found by the account, with the person its runs act
as and the last message sequence that started one, moved forward by a conditional write.
`stream_owners.cpp` is the second
(lake#379): one `if_absent` record per topic, keyed by the topic's blind index; the stores hold no
topic creator (the retired table is dropped by `0070`). `drive_share_links.cpp` is the drive share
link collection (lake#350); the stores keep only `drive_share_link_route` (the content-free route
of a pre-move token, migration `0048_drive_share_link_workspaces`). `forge_records.cpp` is the
forge record collection (lake#341); the stores no longer write or read forge records. `0070`
drops both retired tables.
`secret_records.cpp` is the secrets collections' codecs (lake#328-#332); the stores no longer hold
mounts, secrets, keys or data keys. They keep `secret_mount_routes` and the mount's policy objects
(`create_secret_mount_route`: under the organization, or under the customer for a mount the account
owns, whose route's organization is NULL, migration `0071_secret_mount_owners`), the customer KEK, which wraps and unwraps data keys without leaving
the store and keeps the replaced version during a rotation (migration `0057_secret_workspaces`).
Dynamic
leases are records of their mount's workspace (lake#362, `SecretLeaseCodec` here); the stores keep
only `secret_lease_routes`, the content-free route and due index (migration
`0058_secret_lease_workspaces`; PostgreSQL claims due rows with `FOR UPDATE SKIP LOCKED` and holds
them). `0070` drops the retired secrets tables, and a KEK rotation now rewraps only the PKI
authorities in its transaction (workspace data keys are rewrapped in their workspaces).
`platform_records.cpp` is the platform collections' codecs (lake#358, lake#359), and `platform.cpp`'s
sync writes each repository's environments and rows into that repository's workspace, creating an
environment's policy object (external id `platform_environment_id`) on first sight, skipping a
repository whose rows have not changed since its last write, and marking only that repository
stale when its workspace cannot be written. The stores no longer write or read either retired
table (migration `0060_platform_workspaces`), which `0070` drops. The sync places a row by lake's
repository id or the liquid id `liquid deploy` sends (`repositories.liquid_id`, migration
`0088_repository_liquid_ids`, lake#805), reading the ids it lacks through an injected reader, and
reports a row it cannot place (`unresolved_repositories()`) instead of skipping it.
`application_webhook_records.cpp` is the next: an application's lifecycle events and its webhook
delivery attempts in the application's workspace (lake#356, lake#357), each attempt claimed with
`if_absent` before it is sent and settled with `if_revision`. The stores no longer write or read
`auth_app_webhook_events` or `auth_app_webhook_deliveries` (migration
`0049_application_webhook_workspaces`; `0070` drops both), and `create_application` stores the
application's immutable `workspace_id`.
`application_user_records.cpp` follows: an application's users and organizations in the
application's workspace (lake#353, lake#354), with uniqueness claims beside them. The stores no
longer write or read `auth_app_users` or `auth_app_organizations`, and session resolution no longer
joins them; they keep the pool's own part (`revoke_application_user_sessions`,
`delete_application_user_credentials`; migration `0055_application_user_workspaces`, and `0070`
drops both retired tables). A sign-up
takes the caller's `user_id`, and a sign-up or first OAuth sign-in reports `created_user`.
Repositories carry the workspace they belong to (migration `0051_repository_workspaces`, lake#315).
Both stores refuse `create_repository` without a live workspace of the repository's space and
filter `list_repositories` by an optional workspace; `transfer_repository` is gone from the store
interface because the routing trigger keeps a repository's space fixed.
Repository usage lives in workspace Zettas (lake#348, lake#349, `metering.cpp`), as does the
ledger of charged app traffic hours (`record_metered_app_hour`, lake#661). Both stores read the
operator's `app_transfer` rows (migration `0081`, numbers only; `add_app_transfer` is the operator's
write, for tests). Neither store writes or reads metering rows any more; `0070` drops the retired tables. Both stores keep an
account's usage limits (`customer_usage_limits`, migration `0072`, lake#564); a limit of 0 deletes
the row.
Both stores keep billing records (migration `0073_billing`, lake#564): plans with their prices,
each account's plan by month, and invoices with their lines; one invoice an account and month
that is not void, and only a due invoice becomes paid or void.
They keep the payment provider's records too (migration `0074_payment_provider`, lake#620): the
provider's customer id for each account, kept once (`billing_accounts`), an invoice's `payment_url`
and `provider_status`, and `apply_payment_event`, which records a provider's event id and moves the
invoice it names in one transaction, only while it is due, so an event is never applied twice.
Both keep the apex order sheet's orders (migration `0089_cloud_orders`, lake#831): an order and its
first event in one transaction, every state change with its `cloud_order_events` row, a Checkout
notice applied once per event id through `billing_payment_events`, the email claim that holds an
order for a lease (`FOR UPDATE SKIP LOCKED` on PostgreSQL), and the events not yet audited.
`application_credential_records.cpp` keeps an application's signing keys and webhooks in its
workspace (lake#352, lake#355). The stores no longer write or read `auth_app_signing_keys` or
`auth_app_webhooks`: they envelope and open the credentials with the keystore
(`encrypt_application_credential`/`decrypt_application_credential`, accepted only for the
`application.jwt.*` and `application.webhook.*` purposes; migration
`0056_application_credential_workspaces`, and `0070` drops both retired tables).
`create_application` no longer writes a key.
`application_settings_records.cpp` keeps an application's settings in its workspace (lake#351).
The stores write and read only the routing columns of `auth_applications` (`create_application`
has no settings columns since `0070` dropped them, and `update_application_theme` is gone).
`repository_records.cpp` keeps a repository's description in its workspace (lake#315 part 2): the
codec, `get`/`create` (`if_absent`)/`update` (`if_revision`, reread on a conflict, `if_absent` for
a first record) and one listing per space. The stores no longer read `repositories.description`
(the Postgres columns answer `''`) and refuse one on create; `create_repository` takes an optional
`NewRepository::id` so the record can be written first; `0070` drops the retired column.
`set_repository_ref` and `list_repository_refs` are gone
with the `repository_refs` table (lake#319).
`application_authorization_codes.cpp` keeps an application's authorization codes in its workspace
(lake#363). The stores no longer write or read `auth_app_authorization_codes`, which `0070`
drops.
`agent_task_records.cpp` keeps what was written for an agent task in its workspace (lake#374,
lake#375). The stores keep only the dispatch half: `create_agent_task` inserts no body (and takes
the caller's `id` and `parent_task_id`), `record_agent_task_event` moves the phase and clears lake's
notice, and `fail_agent_task` and the expiry update write lake's text to `agent_task_status.notice`.
`0070` drops the retired content columns (`body`, `message`, `result`).
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
created only where none exists, and one listing per repository; since phase B a blob record
names its store object (`store_oci_blob`, `get_oci_blob`, `list_oci_blobs`). The stores keep
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
`workspace_store.cpp` implements `include/lake/control/workspace_store.hpp` (lake#380): Zetta's object
routes and their errors, the wrapped workspace data key, the encrypted block and tail layout, the
buffered appends sent as complete blocks at most one Zetta slice per PATCH, the refusal of a
rewrite (Zetta objects only grow), the ciphertext window cache, and the lease's renewal thread.
Since lake#535 its writes go without durability and in issue order, liquid's barrier sends nothing,
and `sync()`/`make_durable()` end an operation with one Zetta `:barrier`; the owner's release
syncs what a closed store's backend still holds. Since lake#543 the lease acquisition refuses a
workspace Zetta that does not report `object_log_prefix_durability` (an image without zetta#1237),
every write and `:barrier` carries the incarnation recorded at open as `Zetta-Incarnation`, so a
Zetta restart inside an operation fails it before a later write lands, a new incarnation is adopted
only after the lease renews, and a Zetta of several nodes is refused. See
[workspace-store.md](../../docs/architecture/workspace-store.md).
`object_http.cpp` (lake#544, its own library `lake-object-http`) is the workspace store's HTTP
client, `make_pooled_object_transport`: keep-alive connections per workspace Zetta, one name
resolution per origin while fresh, `TCP_NODELAY`, and a kept connection reused only while it is
open and silent. It never sends a request twice; `ZettaObjectClient` keeps the retries. See
[workspace-store.md](../../docs/architecture/workspace-store.md#per-request-cost-lake544).

Both stores keep `workload_resources` rows (lake#643): PostgreSQL in the operator-written table, the
memory store in a map keyed by kind and id; prices and invoice lines carry their `class`, and the
memory store's `age_build` moves a build's request time back for tests.

The memory store's `set_workspace_namespace` writes the namespace the operator reports for a
workspace, for tests (lake#659).

Both stores keep MCP connection dispatch rows (lake#684, lake#685, migration `0079`), keyed by
workspace and connection: a row that neither streams nor re-exports is removed rather than kept,
and PostgreSQL writes one only for a workspace of the row's account.
Store drives (lake#664, [storage.md](../../docs/api/storage.md#browsing-a-store)):
`store_buckets.cpp` is one `BucketContents` (folders as `<path>/` prefixes and markers) over a small
object protocol with two implementations, S3 (SigV4, `ListObjectsV2`, ranged `GET`, `PUT`, copy and
`DELETE`) and Azure Blob (Shared Key or SAS), its own XML reader and no provider SDK;
`store_kept.cpp` keeps a `zetta` store as sealed records and sealed objects in the workspace Zetta.
Both stores keep served methodology models' `methodology_deployments` rows and the operator's
`methodology_deployment_status` (lake#690, migration `0083`); `methodologies.cpp` holds
`methodology_deployment_endpoint_valid`.

`saml.cpp` is SAML's service-provider side (lake#836, `docs/api/sso.md#saml-sign-in`): a strict XML
reader (no DTDs or entities), Exclusive XML Canonicalization 1.0, XML Signature verification with
the provider's registered certificate, and the response, assertion and condition checks, with no
XML library. Both stores keep the outstanding AuthnRequests and the accepted assertion ids
(`begin_saml`, `consume_saml`; migration `0092_saml_sign_in`), each accepted once.

Ownership changes in both stores require a current organization-scope owner or the reserved
operator (LSR-099, lake#965). The checks cover membership creation, invitation creation and
acceptance, role changes and removal; PostgreSQL locks the organization before reading owner
authority and counts. Self-service joins recheck the inviter. See the
[ownership contract](../../docs/api/organizations.md#delegated-administration) and the shared
regressions in `tests/test_deletions.cpp`.
Authentication budgets are enforced in both stores against resolved user ids before hashing
(lake#969). Recovery credentials select their single Argon2id hash by a versioned SHA-256
selector in the existing unique subject index; legacy rows are never scanned. PostgreSQL uses
private authentication transactions, with row locks for one-use recovery; memory verifies
snapshots outside its shared mutex and rechecks them before committing.
See [the contract and tests](../../docs/api/identity.md#authentication-attempt-limits).
Clerk revocation (lake#948, migration `0094_clerk_revocation`) keeps user-state ordering and a
separate account-wide revocation watermark in PostgreSQL and memory. Old template JWTs cannot
re-exchange after a session event; account restrictions revoke all sessions, tokens and devices, and a newer
unban/unlock permits only new credentials. See [the contract](../../docs/api/clerk.md#webhooks).

Clerk suspension and permanent disablement use `User::may_act()` for bearer issuance,
delegation and stream authority. Suspended users' agent credentials are withdrawn just as on
offboarding; a later unban permits new work without restoring old credentials (lake#948).
