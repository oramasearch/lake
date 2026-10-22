# Public control types

`User::disabled_at` retains offboarding state for administrative reads and the stream workers.
Person-owned agent creation, claims, renewal and credential resolution require an active account;
offboarding withdraws its launches/schedules and invalidates its tasks in the same store operation
([Clerk offboarding](../../../docs/api/clerk.md#webhooks), lake#951).

`network_records.hpp` defines the scoped session/activity provider and its explicit pending
default. `pki::Service` can ensure a persisted customer authority and sign a tunnel CSR without
handling the recipient's private key. `network.hpp` carries public client/relay CA fields and
the shared grant-expiry parser. See [tunnel credentials](../../../docs/api/tunnel-grants.md).

`network.hpp` defines the four network specifications, desired records, operator observations,
workspace scope and write actor, including the API audit identity used by the memory store. `Store`
exposes create, replacement update, withdrawal/revocation, get and list; both stores implement the
same contract. It also derives whether a tunnel grant is active from expiry and revocation for the
[public rules API](../../../docs/api/network.md). See
[network.md](../../../docs/control-plane/network.md).

`ServiceCondition` / `service_condition` (`zetta_workspace.hpp`) are the shared service status and
its reason; `service_json` now includes `updated_at`. `InMemoryStore::set_workspace_status` writes
the operator's workspace observation in tests (lake#452).

`WorkspaceDeletion` (`store.hpp`, lake#514) is what one workspace deletion destroyed, recorded in
`workspace_deletions` (migration `0065`): repositories, apps, builds and tenant image repositories
as `WorkspaceDeletionItem`s, with `completed_at` once lake finished the steps outside PostgreSQL.
`search::Service::drop_repository` removes a destroyed repository's segments and the blobs only it
used from its space's index; `search::Service::drop_workspace` removes a deleted workspace's own
search index (lake#381).

`OrganizationDeletion` (`store.hpp`, lake#645) is what `Store::delete_organization` removes or,
with `dry_run`, would remove: the organization, its account's id and handle, the live workspaces that
refuse it, and counts per kind. `Store` also gains `revoke_invitation`, `get_grant`,
`revoke_grant` and `delete_secret_mount_route` ([deletion.md](../../../docs/api/deletion.md)).
`AccountDeletion` and `AccountOrganization` (lake#810) are what `Store::delete_account` removes or
would remove: the user, its personal space and live workspaces, each organization it is in with its
role and how many other owners and members it has, counts per kind, and the invoices kept.
`Store::verify_mfa_code` checks a TOTP or recovery code without changing anything
([identity.md](../../../docs/api/identity.md#deleting-an-account)).

`StreamOwner` is a topic's creator; `stream_owners.hpp` keeps it as a sealed record in the
topic's workspace Zetta (lake#379); `Store` has no reads of the retired `stream_owners` table,
which migration `0070` dropped (lake#311). `list_agent_task_stream_scopes` supplies one customer's compact durable task
and agent scope for the shared Agent events and Agent tasks views.
`latest_agent_conversation_task` resolves the preceding task for a same-agent, same-kind follow-up.
`list_conversation_agent_tasks` answers the tasks one person's Ask Orama or Agent Factory
conversation started, with the tasks they asked for (`parent_task_id`), and `delete_agent_tasks`
removes their dispatch rows; `delete_ask_conversation` uses both (lake#673).
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

`agents::Service::mint` takes the authenticated acting identity separately from `MintRequest`.
It requires the dispatcher to match and calls `Store::has_active_agent_certificate` to verify
customer registration before signing. The lookup matches customer, TAAP subject, `agent` profile,
revocation and expiry at issuance in both stores and through telemetry. See
[agent session keys](../../../docs/api/agents.md).

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

`agent_library.hpp` registers the Application Agent as `agent-application` and the Experiments Agent as
`agent-experiments`, chat-capable services used from their Ship/Train pages and Ask Orama. Each bus
identity is independent from every other library agent. No library agent is internal (lake#788):
`talk_to_agents` builds Ask Orama's *Talk to* list from every chat agent of the catalogue, the Eval
Generator Agent and the two corpus agents included, and `agent_display_name` names an agent as
people call it (lake#451).

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
writes; the retired `opportunities` table is dropped by migration `0070` with its legacy reads.
`transcripts.hpp` (lake#561) is the transcripts codec: one record per transcript in its
workspace, found by the space, never in PostgreSQL (`docs/api/transcripts.md`).
`mcp_connections.hpp` (lake#652) is the MCP connection codec (a stdio server's `app_id` and `stdio`,
lake#659): the connection, the person's decision
for each tool, the auto-approve grants and the streams, with its credential only named
(`lake-secret://`), and the OAuth state codec (`docs/api/mcp-connections.md`).
`agent_stream_triggers.hpp` (lake#677) is the stream trigger codec: an agent, a workspace stream, the
person its runs act as and its cursor (`docs/api/agent-launches.md#stream-triggers`). See
`docs/architecture/workspace-records.md`. `drive_share_links.hpp` is the drive share link codec
(lake#350): a mutable record per link keyed by its id and found by repository and folder, and the
bearer token format `<workspace id>.<link id>.<secret>` that routes a recipient to the workspace.
`Store` keeps only the content-free route of a pre-move token (`drive_share_link_route`); the
retired `drive_share_links` table and its legacy reads are gone (migration `0070`).
`store_contents.hpp` is the interface a store drive browses (lake#656) and `StoreOpening`, what Lake
hands a `StoreBrowser` (the store, its connection, the workspace secret's fields; lake#664);
`store_buckets.hpp` is Lake's own S3 (SigV4) and Azure Blob (Shared Key or SAS) clients over an
injected `StoreHttp`, and `cloud_store_browser`; `store_kept.hpp` a `zetta` store's sealed
`lake_store_items` records and `lake_st_<hex>` bucket (`docs/api/storage.md#browsing-a-store`).
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
`lake_metering_build_records` per-result records, written `if_absent`, and (lake#658) the
`lake_metering_job_records` that a finished build, agent job or cron run was charged, written
`if_absent` before its resources are added to the usage series, and (lake#661) the
`lake_metering_app_transfer_records` that an app's closed hour of ingress traffic was charged.
`store.hpp`'s `AppTransfer`, `app_transfer` and `add_app_transfer` read the operator's
`app_transfer` rows (migration `0081_app_transfer`): numbers only. `store.hpp` has no other
metering store methods: the retired tables and their legacy reads are gone (migration `0070`).
`store.hpp`'s `UsageLimit`, `usage_limits` and `set_usage_limit` are an account's per-dimension
usage limits (migration `0072_usage_limits`, lake#564): content-free configuration; the usage stays
in workspace Zettas.
`store.hpp`'s `BillingPlan`, `BillingPrice`, `Invoice` and `InvoiceLine`, with `billing_plans`,
`put_billing_plan`, `create_invoice`, `list_invoices`, `get_invoice` and `set_invoice_state`, are
billing records (migration `0073_billing`, lake#564, decision 56; since lake#635 each publication
of the usage-only price list is one `BillingPlan` row, and the subscription methods are gone because
there is no plan to choose): control-plane multitenancy; usage stays in
workspace Zettas and an invoice line keeps only the quantity billed. `PaymentEvent`,
`PaymentEventResult`, `billing_provider_customer`, `set_billing_provider_customer`,
`replace_billing_provider_customer` and `apply_payment_event` are the payment provider's records
(migration `0074_payment_provider`, lake#620, decision 57): the provider's customer per account,
replaced only from the id read when the provider no longer has it (lake#856), and its notices,
applied once by event id. `CloudOrder`, `NewCloudOrder`, `CloudOrderTransition`,
`CloudOrderPayment`, `CloudOrderEvent` and `CloudOrderEmailOutcome`, with
`create_cloud_order`, `get_cloud_order`,
`PaymentEventResult`, `billing_provider_customer`, `set_billing_provider_customer` and
`apply_payment_event` are the payment provider's records (migration `0074_payment_provider`,
lake#620, decision 57): the provider's customer per account and its notices, applied once by event
id. `BillingContact`, `billing_contact`, `set_billing_contact` and `billing_contact_problem` are each
account's billing email, name and address (migration `0090_billing_contacts`, lake#857), a billing
record replaced whole on every save; `set_invoice_provider_status` records whether an invoice's
email was sent (`send_failed`), moving a due invoice only from the status its caller read. `CloudOrder`, `NewCloudOrder`, `CloudOrderTransition`, `CloudOrderPayment`, `CloudOrderEvent`
and `CloudOrderEmailOutcome`, with `create_cloud_order`, `get_cloud_order`,
`transition_cloud_order`, `apply_cloud_order_payment`, `claim_cloud_order_emails`,
`record_cloud_order_email`, `unaudited_cloud_order_events` and `mark_cloud_order_events_audited`,
are the apex order sheet's orders (migration `0089_cloud_orders`, lake#831,
[orders](../../../docs/api/orders.md)), implemented in both stores and the instrumented wrapper.
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
reference. Since phase B a blob record also names the blob's object in the workspace's liquid
store (`object`, the liquid digest), whether lake rooted it (`rooted`) and the rooted objects a
repair replaced (`superseded`): `get_oci_blob`, `store_oci_blob` (written `if_absent`, or the
object added or replaced at the revision read) and `list_oci_blobs`. `store.hpp` declares `OciRepositoryRoute`, `valid_oci_route_name` and
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
Zetta's object routes; `make_zetta_object_transport` is `make_pooled_object_transport`, lake's
keep-alive client, lake#544, with `ObjectConnectionStats`), `ZettaObjectClient`
(one bucket, liquid's error codes), `WorkspaceStoreCipher` (the data key wrapped under a key derived
from `LAKE_INSTANCE_KEK`, AES-256-GCM blocks), `ZettaObjectBackend` (liquid's `store_backend`: the
encrypted block layout, buffered appends, tails, the window cache, the bucket directory, `failed()`,
and `create_key`, without which a missing data key is never created; since lake#527 every write
is sent in liquid's issue order, one at a time, small puts and appends held until a later write
needs them sent, removals at once; since lake#535 every write goes without durability, liquid's
`barrier()` sends nothing, and `sync()`/`make_durable()` send what is held and one Zetta
`:barrier`, which lake runs at the end of each store operation before it answers; since lake#543
`ZettaObjectClient` refuses at `:acquire` a Zetta without `workspace_store_prefix_durability`
(an image without zetta#1237) and a Zetta of several nodes, `workspace_zetta_unsupported`
recognizing the refusal; it sends the incarnation it recorded as `Zetta-Incarnation` on every write
and `:barrier`, renews the lease before adopting a new one, and reports `restarted()`)
and `ZettaStoreOwner` (liquid's `store_owner` over the bucket lease, which settles the backend
before it releases). `ObjectRequestKind`, `ObjectRequestTally` and `ObjectRequestScope` count the
requests by kind for the process metrics and a request's `Server-Timing`, and
`run_object_requests` runs a sync's `:barrier` and stale-tail removals (lake#527). `zetta_workspace.hpp` adds
`WorkspaceService::endpoint`, and `store.hpp` `Workspace::store_moved_at` and
`mark_workspace_store_moved` (migration `0067`).

`store.hpp`'s `WorkloadResources` (lake#643, migration `0075_workload_resources`) is what the Orama
operator reports a workload is scheduled with; `Store::workload_resources` lists every row and
`put_workload_resources` is the operator's write, for tests. `BillingPrice::class_name` and
`InvoiceLine::class_name` carry a GPU class rate and the class a line bills; `AgentLaunch::status_updated_at`
is when the operator last wrote a launch's status (a finished job's end). See
[metering](../../../docs/api/metering.md#resources).

`store.hpp`'s `McpConnectionDispatch` (lake#684, lake#685, migration `0079_mcp_connection_dispatch`)
is where a workspace's MCP connection needs Lake outside a request: whether it streams and the digest
of the tools it re-exports. `put_mcp_connection_dispatch` writes or removes it,
`mcp_stream_dispatch` lists the streaming rows for the stream worker and `mcp_exported_dispatch` a
workspace's re-exporting rows for its MCP endpoint. See
[mcp-connections.md](../../../docs/api/mcp-connections.md).

`methodologies.hpp` (lake#655) is a workspace methodology's manifest, the findings Validate answers
without running anything, drafts and copies of Orama's, the image's Dockerfile and the validation
sample. `store.hpp`'s `MethodologyRun`, `NewMethodologyRun` and `MethodologyRunStatus` are the
dispatch rows of its runs (migration `0078_methodology_runs`), and `methodology_run_principal` the
principal a run's `lmr.` credential resolves to: no person, only the runner's two capabilities.

`store.hpp` also holds a served methodology model's dispatch row, `MethodologyDeployment` (lake#690,
migration `0083_methodology_deployments`), its operator status, `methodology_deployment_principal`
(the `lmd.` credential, which reaches only `methodology_deployment_input`) and
`methodology_deployment_endpoint_valid` (only a serve Service's in-cluster URL). `methodologies.hpp`
adds the chunked artifact bounds (8 MiB chunks, 1 GiB in all) and the predict row limit.

`authentication_limits.hpp` provides the bounded fixed-window attempt table used by both stores
and HTTP client admission (lake#969). Full tables fail closed without evicting active budgets.
The store's protected account admission uses resolved user ids across all password/MFA/recovery
verification paths; see [the contract](../../../docs/api/identity.md#authentication-attempt-limits).
