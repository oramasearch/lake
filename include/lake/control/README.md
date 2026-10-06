# Public control types

`ServiceCondition` / `service_condition` (`zetta_workspace.hpp`) are the shared service status and
its reason; `service_json` now includes `updated_at`. `InMemoryStore::set_workspace_status` writes
the operator's workspace observation in tests (lake#452).

`WorkspaceDeletion` (`store.hpp`, lake#514) is what one workspace deletion destroyed, recorded in
`workspace_deletions` (migration `0065`): repositories, apps, builds and tenant image repositories
as `WorkspaceDeletionItem`s, with `completed_at` once lake finished the steps outside PostgreSQL.
`search::Service::drop_repository` removes a destroyed repository's segments and the blobs only it
used.

`StreamOwner` is a topic's creator; `stream_owners.hpp` keeps it as a sealed record in the
topic's workspace Zetta (lake#379); `Store` has no reads of the retired `stream_owners` table,
which migration `0070` dropped (lake#311). `list_agent_task_stream_scopes` supplies one customer's compact durable task
and agent scope for the shared Agent events and Agent tasks views.
`latest_agent_conversation_task` resolves the preceding task for a same-agent, same-kind follow-up.
`AgentBus::call` carries named
CRUD read requests over the existing service transport; unsupported custom bus implementations
fail explicitly. See `docs/control-plane/streams.md`.

`Session.parent_session_id` records derivation. `Principal.expires_at_unix` carries resolved expiry
in whole Unix seconds to the capability registry. `NewToken.parent_session_id` is supplied by
trusted capability code, and both store implementations persist and enforce its ancestry.

Verify changes with the full PostgreSQL-backed static and shared CTest suites. Keep the identity
contract in `docs/api/identity.md` updated with changes to token derivation.

`Store::list_customers(limit, user_id)` optionally selects the personal owner before limiting.
Audit filters select exact request IDs and continue after event IDs in append order. Both stores
and the telemetry wrapper preserve these filters; see `docs/build/testing.md`.

`agent_bus.hpp` is the agent bus (`docs/api/agent-bus.md`). `AgentBus` is the pub/sub interface,
with a Zetta implementation and an in-memory one for tests. `AgentTaskBus` is lake's side of it.
`Store::claim_agent_task`, `record_agent_task_event`, `fail_agent_task` and `stale_agent_tasks`
back it in both stores. Dispatch includes the task's immutable workspace for information; agent
capability inputs may omit it. `WorkspaceService::call` always receives an explicit resolved
workspace ID. `ZettaControlError` carries `code`, `message` and, when Zetta names the field at
fault, its JSON-pointer `path`.

`web_apps.hpp` defines the protected-app session claims and authorization-code records. `Store`
exposes host resolution, the one-query cookie authorization check, atomic code exchange, and
expiry cleanup; memory and PostgreSQL implementations preserve the same contract. See
`docs/api/web-apps.md`.

`builds.hpp` exposes build JSON with a null image before success and a public-host digest reference
after success. App token purposes include the hash-only, exact-repository registry pull credential.

`agent_library.hpp` registers ApplicationAgent as `agent-application` and ExperimentsAgent as
`agent-experiments`, chat-capable services used from their Ship/Train pages and Ask Orama. Each bus
identity is independent from every other library agent. A library agent's `internal` flag marks
Orama's own tooling (the eval and corpus generators); `talk_to_agents` builds Ask Orama's *Talk to*
list without them and `agent_display_name` names an agent as people call it (lake#451).

Agent claims carry a renewable lease and up to three attempts. The Store interface exposes
credential-checked renewal and acknowledgement of durable retry notices; the bus pump receives
the workspace service for publication. `parse_agent_leases` validates per-library durations.
See [the lease contract](../../../docs/api/agent-bus.md).

`store.hpp` declares `NewOrganizationSpace`, `ConvertPersonalSpace` and their `OrganizationSpace`
result for organizations a person runs, plus `list_user_memberships` and
`list_invitee_invitations`. See [organizations.md](../../../docs/api/organizations.md).

`workspace_records.hpp` declares `WorkspaceRecords` (append, list, get, conditional put,
remove and update of sealed records in one workspace Zetta's record store) and `RecordSet<Codec>`, the typed collection every table that moves into a
workspace uses; `ask_messages.hpp` is the reference codec. `Store` has no reads of the retired
`ask_messages` table, which migration `0050` dropped (lake#483). `opportunities.hpp` is the
first mutable collection (Design → Opportunities, lake#376): derived keys, conditional status
writes; the retired `opportunities` table is dropped by migration `0070` with its legacy reads. See
`docs/architecture/workspace-records.md`. `drive_share_links.hpp` is the drive share link codec
(lake#350): a mutable record per link keyed by its id and found by repository and folder, and the
bearer token format `<workspace id>.<link id>.<secret>` that routes a recipient to the workspace.
`Store` keeps only the content-free route of a pre-move token (`drive_share_link_route`); the
retired `drive_share_links` table and its legacy reads are gone (migration `0070`).
`forge_records.hpp` is the forge record codec (lake#341): a mutable record per issue, comment,
review, release, webhook, delivery, export or rule, in the workspace of its repository's workspace,
keyed by its id and found by repository and kind (`forge_record_lookup`). `ForgeRecord` carries the
engine and lake revisions; the retired `forge_records` table and its legacy reads are gone
(migration `0070`).
`secret_records.hpp` holds the secrets codecs (lake#328-#332): `lake_secrets_mounts` (a mount keyed
by its id, and its name claim), `lake_secrets_entries`, `lake_secrets_versions`,
`lake_secrets_encryption_keys` and `lake_secrets_data_keys`, every name-selected record keyed by a
blind index (`WorkspaceRecords::index_of`). `secrets.hpp` gives `SecretMount` its `workspace_id`
and declares `SecretMountRoute`, `WrappedDataKey` and `CustomerKekState`.
`Store` keeps for secrets only the content-free routes and the customer KEK
(`wrap_secret_data_key`, `unwrap_secret_data_key`, `rotate_customer_kek`, `retire_customer_kek`);
the retired secrets tables, their `LegacySecret*` rows and legacy reads are gone (migration
`0070`). Dynamic leases (lake#362) are `lake_secrets_dynamic_leases` records
(`SecretLeaseCodec`, `SecretLeaseRecord`, `to_lease`); `Store` keeps only each lease's content-free
`SecretLeaseRoute`, which is also the expiry loop's due index (`record_secret_lease_route`,
`get_secret_lease_route`, `reschedule_secret_lease`, `delete_secret_lease_route`,
`claim_due_secret_leases`).
`platform_records.hpp` holds the platform codecs (lake#358, lake#359): `lake_platform_environments`
(an environment keyed by `platform_environment_id`, written `if_absent`) and `lake_platform_mirror`
(the last operator-control row per `platform_mirror_id`, replaced only by a newer row at the
revision read), both found by the repository, and the row order (`platform_row_order`).
`PlatformEnvironment` and `PlatformMirrorRow` moved into `store.hpp`, and `platform_store.hpp` is
gone, as are the two retired tables and their legacy reads (migration `0070`). `PlatformService` (`platform.hpp`) reads
and writes through the repository's `WorkspaceRecords`, and its sync takes a
`PlatformRecordsOpener`.
`application_webhook_records.hpp` holds the application
event and delivery-attempt codecs and the claim/settle pair that sends each attempt once
(lake#356, lake#357); `apps.hpp` gives `Application` its `workspace_id`. `application_user_records.hpp` holds the codecs of
an application's users and organizations in the application's workspace (lake#353, lake#354), their
reads and conditional writes, and the claims (`write_application_claim`,
`release_application_claim`) that keep a pool user's email and an organization's TAAP identity and
name unique within the application. `NewPasswordUser::user_id` lets a sign-up
choose the account id it claimed the email for, and `IssuedSession::created_user` says the store
created the account.

`Repository::workspace_id` and `NewRepository::workspace_id` carry the workspace a repository belongs
to (migration `0051_repository_workspaces`, lake#315); `list_repositories` takes an optional
workspace, and `transfer_repository` is no longer part of `Store`.

`metering.hpp` holds repository usage in workspace Zettas (lake#348, lake#349): the
`lake_metering_usage` hourly transfer counters, added to with a conditional write, and the
`lake_metering_build_records` per-result records, written `if_absent`. `store.hpp` has no
metering store methods: the retired tables and their legacy reads are gone (migration `0070`).
`application_credential_records.hpp` holds the application's credentials in its workspace (lake#352,
lake#355): the key ring codec (one record per application, every key in version order, one
current; `rotate_application_signing_keys` writes it with `if_revision` and rereads on a conflict)
and the webhook codec (one record per webhook, written `if_absent`). `apps.hpp` gives
`ApplicationSigningKey` its `encrypted_seed` and `ApplicationWebhook` its `encrypted_secret` (the
secrets envelopes kept inside the seal) and declares `ApplicationSigningKeyRing`; `store.hpp`
declares `encrypt_application_credential`/`decrypt_application_credential`.
`application_settings_records.hpp` holds an application's settings in its workspace (lake#351):
the settings codec (one record per application, keyed by it and found by its space;
`update_application_settings` writes with `if_revision` and rereads on a conflict). `apps.hpp`
declares `ApplicationSettings` and `apply_settings`, and documents which `Application` fields are
routing; `NewApplication` carries routing only. `store.hpp` drops `update_application_theme`.
`repository_records.hpp` holds a repository's settings (its description, lake#315 part 2) in its
workspace: one record per repository, keyed by it and found by its space. `store.hpp` declares
`RepositorySettings` and `NewRepository::id`; it no longer declares `RepositoryRef`,
`set_repository_ref` or `list_repository_refs` (lake#319), nor the legacy reads of the retired
description column (migration `0070`).
`application_authorization_codes.hpp` holds an application's one-use authorization codes in its
workspace (lake#363): the codec (keyed by the blind index of the application and the code hash,
found by the application, with the five-minute lifetime as the collection's retention),
`issue_application_authorization_code` (`if_absent`) and `redeem_application_authorization_code`
(read, then `remove` at the revision read; every refusal of the code is `unauthorized`). `apps.hpp`
adds the code's instants and revision; `store.hpp` no longer declares
`create_/consume_application_authorization_code` (the retired table is dropped by `0070`).
`agent_task_records.hpp` holds what was written for an agent task in its workspace (lake#374,
lake#375): the codec (one record per task, keyed by it and found by its space) with the request,
the agent's latest message, its answer and the finished phase. `record_agent_task_content` writes
an event with `if_revision`, rereads on a conflict and leaves a finished record as it is. In
`store.hpp`, `AgentTask` gains `parent_task_id`, and
the store answers its dispatch half only (an empty `body`, a null `result`, and as `message` only
lake's notice); `create_agent_task` keeps a caller-made `id`, `record_agent_task_event` takes no
message or answer (the retired content columns are dropped by `0070`).
`workspace_audit.hpp` holds a workspace's audit chain (decision 52, lake#317): the
`lake_audit_events` collection, chained and copied events, `append_workspace_audit_event`,
`find_workspace_audit_tail`, `list_workspace_audit` and `verify_workspace_audit`.
`workspace_records.hpp`'s `list` takes an optional `keep` predicate. `store.hpp` declares
`LegacyAuditEvent`, `assign_legacy_audit_events` and `legacy_audit_events`, which serve only the
copy. `lake/audit/audit.hpp` adds `PendingEvent` (an event whose id and instant are fixed when its
call finishes and whose predecessor and signature are added when it is written), `sign_pending`
and `verify_event` (one event checked alone).


`oci_records.hpp` holds a tenant image's manifests and tags in its routed workspace (lake#383
phase A): `lake_oci_manifests`, keyed by the blind index of the digest and written `if_absent`, and
`lake_oci_tags`, keyed by repository and tag, found by the repository and moved at the revision
read, and `lake_oci_blobs`, the blobs the space pushed into the workspace, which a manifest may
reference. `store.hpp` declares `OciRepositoryRoute`, `valid_oci_route_name` and
`create_oci_repository_route`/`get_oci_repository_route`/`list_oci_repository_routes`/
`delete_oci_repository_route`; the
workspace deletion cascade deletes a workspace's routes.

`store.hpp` declares the operator's volumes (migration `0069`): `Workspace::storage_gi` and the
operator's `storage_gi_observed`, `storage_phase` and `storage_message`; `VolumeSnapshotPolicy`,
`VolumeSnapshotStatus`, `VolumeSnapshotRequest`, `valid_snapshot_schedule` (the operator's five
standard cron fields, refusing a schedule that can never fire) and the bounds
`max_workspace_storage_gi` and `max_volume_snapshots_kept`; and `set_workspace_storage`,
`request_volume_snapshot` and `volume_snapshots`.
`workspace_store.hpp` is a workspace's liquid store in its workspace Zetta (lake#380,
[workspace-store.md](../../../docs/architecture/workspace-store.md)): `ObjectTransport` (raw HTTP to
Zetta's object routes; `make_zetta_object_transport` uses Zetta's installed client), `ZettaObjectClient`
(one bucket, liquid's error codes), `WorkspaceStoreCipher` (the data key wrapped under a key derived
from `LAKE_INSTANCE_KEK`, AES-256-GCM blocks), `ZettaObjectBackend` (liquid's `store_backend`: the
encrypted block layout, buffered appends, tails, the window cache, the bucket directory, `failed()`,
and `create_key`, without which a missing data key is never created)
and `ZettaStoreOwner` (liquid's `store_owner` over the bucket lease). `zetta_workspace.hpp` adds
`WorkspaceService::endpoint`, and `store.hpp` `Workspace::store_moved_at` and
`mark_workspace_store_moved` (migration `0067`).
