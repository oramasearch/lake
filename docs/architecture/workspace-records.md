# Workspace Records

Refs [lake#370](https://github.com/oramasearch/lake/issues/370) and
[lake#311](https://github.com/oramasearch/lake/issues/311). This is the one mechanism lake uses to
keep its records in the workspace Zetta that owns them. Every follow-up that moves a table or store
out of lake's PostgreSQL uses it and applies the [checklist](#checklist-for-each-follow-up) below.
That covers lake#315, #316–#384 and the other "Move … into workspace Zettas" issues.
`ask_messages` is the first collection to move, `opportunities` (lake#376) the first mutable one, and
`stream_owners` (lake#379) the first keyed by a unique name. Application lifecycle events and their
webhook delivery attempts (lake#356, lake#357) follow, and then an application's users and
organizations (lake#353, lake#354), the first records with uniqueness claims beside them, and
its signing keys and webhooks (lake#352, lake#355), the first credentials. Secrets (lake#328-#332)
are the first collections that hold key material, and dynamic-secret leases (lake#362) the first
whose lifecycle a scheduled loop drives, from a central due index.
An application's settings (lake#351) are the first record split out of a routing row that stays
central.

The rule it implements is in [workspace-data-plane.md](workspace-data-plane.md): everything goes in
a workspace Zetta, including credentials, secrets and metrics. Lake's PostgreSQL and the lake Zetta
keep only multitenancy and content-free dispatch. Billing is the one exception, and its design is
still open.

## Status

| Part | State |
|---|---|
| `WorkspaceRecords`, `RecordSet<Codec>` | Implemented on Zetta's keyed record store ([zetta#1231](https://github.com/oramasearch/zetta/issues/1231), merged in zetta PR #1233 to `dev`, `82f617e`). Unit tests run against an engine fake; an opt-in test runs against a real Zetta node built from that commit. |
| Append-only collections | Implemented. `ask_messages` and `auth_app_webhook_events` have moved; the `ask_messages` table is dropped (migration `0050`, lake#483). |
| Mutable records and claims (`get`, conditional `put`, `remove`, `update`) | Implemented and tested. `opportunities` (lake#376) has moved: derived keys written `if_absent`, status changes conditional on the revision read. `stream_owners` (lake#379) claims a topic name with `if_absent` and releases it with `remove(id, if_revision)`. `drive_share_links` (lake#350) has moved: revocation is a conditional `put` on the revision read. `auth_app_webhook_deliveries` (lake#357) claims each attempt with `if_absent` and settles it with `if_revision`. `metering_usage` (lake#348) adds to an hourly counter with `if_revision`, and `metering_build_records` (lake#349) records each build result once with `if_absent`. `forge_records` (lake#341) has moved into the workspace of each record's repository: edits are conditional `put`s on the revision read, and the push path reads branch protection once before the push. `auth_app_users` and `auth_app_organizations` (lake#353, lake#354) have moved into the application's workspace: a status change or rename is a conditional `put` on the revision read, and a pool user's email and an organization's TAAP identity and name are claims written `if_absent` in the same collection. `auth_app_signing_keys` (lake#352) is one key ring per application, rotated with one `if_revision` write; `auth_app_webhooks` (lake#355) are written once with `if_absent`. `auth_applications` (lake#351): each application's settings are one record keyed by it, written `if_absent` at registration, and a theme change is an `if_revision` write reread on a conflict; the table keeps the content-free routing row. The five secrets tables (lake#328-#332) have moved into the workspace of each mount's route: version and key-version numbers are claimed `if_absent` and pointers moved `if_revision`, and every delete, undelete, destroy and data-key rewrap is a conditional `put`. `secrets_dynamic_leases` (lake#362) has moved into its mount's workspace: renew, revoke and the scheduled expiry each rewrite the lease at the revision read, so a renewal never revives a revoked or expired lease. |
| `copy_workspace_records` (the operator's one-shot copy and verify) | Implemented for `opportunities`, `stream_owners`, `drive_share_links`, `auth_app_webhook_events`, `auth_app_webhook_deliveries`, `metering_usage`, `metering_build_records`, `forge_records`, `auth_app_users`, `auth_app_organizations`, `auth_app_signing_keys`, `auth_app_webhooks`, `secrets_mounts`, `secrets_data_keys`, `secrets_encryption_keys`, `secrets_entries`, `secrets_versions`, `secrets_dynamic_leases` and `auth_applications` (the settings columns only). `ask_messages` left the list with its table: a request for it is `invalid_input`. |
| Dropping the moved tables from PostgreSQL | `ask_messages` is dropped by migration `0050_drop_ask_messages` (lake#483), after its copy was verified on dev; its `workspace_assignments` rows stay. Each other table is dropped by a later migration once its copy is verified on the dev cluster. `auth_applications` is not dropped: it is the routing row, and a later migration drops only its four settings columns. |

**Zetta dependency.** A workspace Zetta must run Zetta `dev` at or after zetta PR #1233
(`82f617e`). An older engine has no record store and answers `put_record` as an unknown
capability. Workspace Zettas run the image in the Lake's `serviceZettaImage`, so this dependency is
part of the [cutover](#cutover-runbook-ask_messages-dev-cluster).

## What Zetta offers

| Primitive | Capabilities | Durability and consistency | Lookup | Conditional write | Fit |
|---|---|---|---|---|---|
| **Keyed record store** (#1231) | `create_collection`, `put_record`, `get_record`, `delete_record`, `list_records` | A write is fsynced on the key's owner and installed on every live peer before the acknowledgement. Writes are linearizable per key. A read after a write returns that write on any live node. Retention is per collection. | `get_record` by key; `list_records` over one value of a declared secondary index, ordered by `order` then key, with paging and both directions | `if_absent`, `if_revision`; `409 conflict` carries `details.revision`; `503 busy` while ownership moves | **All lake records** |
| Labelled time series (#1231) | `create_metric`, `record_metric`, `query_metric` | Replicated; a point is not fsynced on its own (the log is flushed every tick) | Label filter, group-by, steps | n/a | Request metrics (#384). Not for anything that might be billed, such as metering (#348, #349), which uses records |
| SQL tables | `sql` (GET, or POST since #1233) | Rows in the replicated catalogue | Full scan, one predicate | None | Workspace data people query; not lake's records |
| Pub/sub topics | `create_topic`, `publish_event`, `browse_topic`, … | Node-local, best effort | By sequence | `update_event` only | Event streams with retention (`agent-events`, `app-events.*`, `build-events.*`) |
| Connections | `create_connection` | Credential sealed at rest since zetta#1230 | By name | None | Not used for lake secrets (lake seals its own) |

The record store's measured latency on 50,000 records is a `list_records` of 50 records at under
1 ms in the registry ([zetta `docs/data/record-store.md`](https://github.com/oramasearch/zetta/blob/dev/docs/data/record-store.md)).
A page that reads one collection therefore pays one workspace round trip.

## The primitive per record shape

| Shape | Examples (follow-up) | How |
|---|---|---|
| Append-only log, read back by key | `ask_messages` (#370), `sso_provisioning_logs` (#333), `auth_app_webhook_events` (#356), `agent_cron_runs` (#372), `offline_recovery_notifications` (#339), `audit_events` (#317) | `append`: one `put_record` with `if_absent`. `list` is newest first by the lookup index, then put in order. Set `retention_ms` where a purge applies. |
| Work items sent once, with retry history | `auth_app_webhook_deliveries` (#357) | One record per attempt, keyed by `index_of(collection, "<item>\n<attempt>")`. Claim it pending with `put(if_absent)` before the side effect and settle it with `put(if_revision)` after: only the caller whose claim succeeded performs it. |
| Mutable rows found by id | `opportunities` (#376, **moved**), `drive_share_links` (#350, **moved**), `repositories` (#315), `apps` (#377), `stores` (#365), `forge_records` (#341, **moved**), `auth_app_users` (#353, **moved**), `auth_app_organizations` (#354, **moved**), `auth_applications` settings (#351, **moved**; the routing row stays), `platform_environments` (#358) | `get`, then `put` with `if_revision`, or `update`, which retries on `409` by rereading. `remove` with `if_revision`. |
| One-use codes, sessions, replay evidence, claims | `sessions` (#323), `identity_oauth_flows` (#326), `identity_webauthn_challenges` (#327), `identity_taap_assertions` (#361), `auth_app_authorization_codes` (#363), `offline_recovery_attempts` (#336), `agent_task_status` claims (#375) | Consume with `remove(id, if_revision)`. Only one caller succeeds; the others get `conflict` or `not_found`. Expiry is the collection's `retention_ms`. |
| Unique names | `stream_owners` (#379, done), an application user's email and an application organization's TAAP identity and name (#353, #354, done), repository and app names | A name-keyed record written with `if_absent`, using a blind key (see [Keys](#record-format)). A value that must be unique beside a record found by id is a **claim** in that record's collection: it names its holder, has its own lookup so listings never show it, is released with `remove(if_revision)` by its holder, and an abandoned one (older than its grace period, naming a holder that does not exist) is taken over with `if_revision`. |
| Secrets and credentials | `auth_app_signing_keys` (#352, **moved**), `auth_app_webhooks` (#355, **moved**), `secrets_*` (#328–#332, **moved**), `keystore` (#324), `pki_authorities` (#342), `custodial_identities` (#364), `oauth_providers` (#325) | Records whose sealed body holds the secrets subsystem's existing envelope ciphertext. Zetta's connection store is not used (lake seals its own secrets). An application's keys are one key ring record keyed by the application, so a rotation (retire the current key, add the next) is one write conditional on the revision read; each seed and webhook secret is the `keystore`-wrapped envelope (`Store::encrypt_application_credential`), which the copy moves without opening. A name that selects a record is the record's key only through `index_of`, so no name is visible. The secrets collections are routed by a content-free `secret_mount_routes` row (mount → workspace) and keep the customer KEK in `keystore` ([secrets.md](../api/secrets.md#where-secrets-live-lake328-332)). |
| Records a scheduled loop settles | `secrets_dynamic_leases` (#362, **moved**) | Records in the workspace, plus a content-free central **due index** (id, workspace, due instant) that the loop claims from with `FOR UPDATE SKIP LOCKED`, so a pass reads only the workspaces that hold due records. The index is a hint, never later than the record; the loop settles each record with a conditional write, rereads on a conflict, and moves the index to what it read. |
| Exact counters that may be billed | `metering_usage` (#348, **moved**), `metering_build_records` (#349, **moved**) | Records, never metrics, since a metric point is not fsynced on its own. A counter is one record per workspace and hour, added to with `if_revision` from an in-process buffer that is flushed off the request path (`MeteringRecorder`); each counted item (a build result) is one record written `if_absent` ([metering.md](../api/metering.md)). |
| Operational metrics | Prometheus series (#384) | Zetta's labelled time series (`create_metric`/`record_metric`/`query_metric`). |
| Event streams | `agent-events`, `app-events.*`, `build-events.*` | Workspace topics with retention, as in #311. |

Lake keeps only `LAKE_INSTANCE_KEK`, the root from which the per-workspace keys are derived, which
is the same custody it already has for every encrypted column. Record bodies, lookup values and
secrets all live in the workspace.

## Record format

Each lake collection `<name>` is the Zetta collection `lake_<name>` (for example
`lake_ask_messages`), declared at first use with `indexes: ["lookup"]` and the collection's
`retention_ms`, if it has one. A record is:

| Field | Value |
|---|---|
| `key` | The record id: a lake-made UUID. A copy keeps the PostgreSQL row's id, so a rerun is a `409` instead of a duplicate. Keys are not secret. A collection whose natural key is a name or a token uses `index_of(collection, name)` as its key. A record that several writers may each create once uses a key derived from what makes it unique: an Opportunity's key is a version 8 UUID from SHA-256 of its scan's task id and its rank (`opportunity_id`), and its copy uses that key too, so a scan settled again after the copy is not saved twice. |
| `index.lookup` | The blind index of the lookup value: base64url HMAC-SHA-256 of `<collection>\0<lookup>` under the workspace's index key. |
| `order` | The creation instant in microseconds, zero-padded to 20 digits, so the index's order is time order. |
| `value` | Base64 of the envelope `{"v":1, "kv":1, "sv":<schema version>, "rev":<lake revision>, "at":<µs>, "s":<sealed body>}`. |
| `revision` | The engine's revision, which callers pass as `if_revision`. Lake keeps its own `rev` inside the seal. |

**Keys.** Two keys per workspace are derived from `LAKE_INSTANCE_KEK` with HKDF-SHA-256, under the
labels `lake/workspace-records/v1/seal/<workspace id>` and `…/index/<workspace id>`. The body is
sealed with AES-256-GCM. The workspace holds only ciphertext and HMACs, so someone who can read the
store sees no conversation text and cannot tell whose it is.

**Binding and immutable ownership.** The AEAD's associated data binds:

- the workspace;
- the collection;
- the key;
- the lookup index;
- the instant;
- the schema version;
- lake's revision.

A value copied to another workspace, collection, key or lookup does not open there, so a record
cannot change owner. A record that does not open is skipped as absent. The copy's `verify` reports
such a record as missing.

**Rollback.** Lake's revision is sealed, but an older sealed value written back by a workspace
administrator still opens. Rollback protection would need state outside the workspace, so it is
out of scope. The workspace administrator is lake's own credential (below).

## Rights

Zetta grants `records:*` only to its `admin` role. Lake's workspace credential is the workspace
Zetta's bootstrap owner, so nothing has to be granted. The operator mounts the cluster's
`<name>-zetta-admin` token as the service connection (`internal/controller/lake_services.go`), and
the same token seeds the owner through `--admin-token`. If lake ever reaches a workspace through a
non-admin service user, that user needs:

```sql
GRANT mcp:call TO ROLE lake_service;
GRANT records:read, records:write, records:manage ON COLLECTION lake_ TO ROLE lake_service;
```

The record capabilities are not in lake's `workspace_query`/`workspace_act` allow-lists, which is
correct. People and agents never reach `lake_*` collections, and lake reaches them only through
`WorkspaceRecords`. The earlier SQL design reserved a `lake_records` database and needed guards on
`workspace_sql`. That database and those guards are removed: records are no longer SQL tables.

## C++ interface

```cpp
// include/lake/control/workspace_records.hpp
class WorkspaceRecords {
    static ZettaControlResult<WorkspaceRecords> open(Call call, std::string workspace_id, std::string_view instance_kek);
    task<ZettaControlResult<StoredRecord>> append(RecordCollection, std::string lookup, json body, RecordWrite = {});
    task<ZettaControlResult<std::vector<StoredRecord>>> list(RecordCollection, std::string lookup, std::size_t newest = 0,
                                                             std::int64_t since_us = 0);
    task<ZettaControlResult<std::optional<StoredRecord>>> get(RecordCollection, std::string id);
    task<ZettaControlResult<StoredRecord>> put(RecordCollection, std::string id, std::string lookup, json body,
                                                RecordCondition, std::int64_t lake_revision = 1, std::int64_t at_us = 0);
    task<ZettaControlResult<void>> remove(RecordCollection, std::string id, std::optional<std::int64_t> if_revision = {});
    task<ZettaControlResult<std::optional<StoredRecord>>> update(RecordCollection, std::string id, std::string lookup, Change, int attempts = 5);
};
template <class Codec> class RecordSet;   // typed append(Value), list(lookup, newest)

// src/api/workspace_records_api.hpp: one workspace's records through call_workspace
ZettaControlResult<WorkspaceRecords> workspace_records(const ApiContext&, std::string customer_id, std::string workspace_id);
```

- **Append** is one `put_record` with `if_absent`. If the call returns `not_found` at
  `/collection`, `append` runs `create_collection`, which is idempotent, and puts again.
- **List** calls `list_records` with `index: "lookup"`, `direction: "desc"` and a limit, following
  `next_after` until `newest` records have opened, then returns them oldest first. With `since_us`
  the walk also stops at the first record older than that instant, so a reader of recent records
  does not page through the whole history. A missing collection lists as empty.
- **Conflicts.** A failed condition is `conflict`, and `ZettaControlError::details.revision` holds
  the live engine revision. `update` rereads the record and asks its `Change` again, up to
  `attempts` times.
- **Busy.** Every call is retried on `503 busy` (ownership moving): four attempts, backing off 50,
  100 and 200 ms.

A collection is a codec with `Value`, `collection` (name, schema version, retention),
`lookup(Value)`, `encode(Value)` and `decode(StoredRecord)`. `AskMessageCodec`
(`include/lake/control/ask_messages.hpp`) is the reference. Its lookup is the conversation plus the
person (`conversation:<id>\nuser:<id>`), so only the person who had a conversation reads it back.
`OpportunityCodec` (`include/lake/control/opportunities.hpp`) is the mutable reference: its lookup
is the space (`customer:<id>`), every Opportunity of the workspace is one listing, and
`set_opportunity_status` reads, checks the space and writes with `if_revision`, rereading on a
conflict.

`workspace_records` binds the existing workspace caller (`call_workspace` →
`WorkspaceService::call`) to one customer and workspace. Every call therefore checks the
workspace's owner and readiness, and refuses rather than falling back. Each call is also named in
`Server-Timing` (`zetta_list_records`). The caller validates `workspace_id` before opening:
`Registry::call` does it for a capability's input, and the workspace route guard for a page's
`/w/<id>/` path.

## Naming

- Zetta collection: `lake_<collection>`, where the collection is the PostgreSQL table it replaces,
  in lowercase snake_case, at most 48 characters.
- Index: `lookup` is the only index for now. A collection that is read by two different keys
  declares a second index in its own `create_collection` change and documents it here.
- Lookup values: `<field>:<value>` lines in a fixed order. Include every field the old query
  filtered on, including the person when reads are per person.
- Capabilities that read a collection take `customer_id` and `workspace_id`
  (`customer_input_schema`).

## Schema evolution

- A body change that only adds fields keeps `sv`; `decode` defaults the missing fields.
- A change that is not additive raises the codec's `schema_version`. `decode` reads every earlier
  version, and records are never rewritten in place except by a deliberate `update`.
- Changing a collection's indexes or retention: `create_collection` answers `409` for a different
  definition. Use a new collection name and copy into it.
- Key rotation introduces `kv = 2` beside `1`. Readers open both, and a rewrite retires the old
  version.
- Never put plaintext outside the sealed body, and never reuse a field name with a new meaning.

## Retention, latency and consistency

- **Retention** is the collection's `retention_ms`, enforced by the engine. `ask_messages` keeps
  everything, as before. Deleting a workspace deletes its Zetta and its volume, and the records go
  with them.
- **Latency.** The page budget test holds `/ask` and `/train/agents/new` with an open conversation
  at one `list_records` call in one round, and Design → Opportunities at `list_records` and
  `list_methodologies` in one round.
- **Consistency.** Writes are linearizable per key, and a read after an acknowledged write sees it
  on every live node. The record store doc lists the windows where this does not hold: ownership
  changing during a membership change, and nodes that are not fully meshed.

## Migration and cutover

Every collection follows the same steps. There is no dual write, and nothing is dropped until the
copy is verified.

1. **Record owners.** A migration (`0045_ask_message_workspaces` for this one) inserts each row's
   owner into `workspace_assignments`. A space with exactly one live workspace owns its rows. Every
   other row is `needs_selection`, and rows with no space are reported. The migration adds no
   column and drops nothing.
2. **Switch reads and writes.** The same lake release reads and writes only through
   `WorkspaceRecords`.
3. **Copy.** The operator runs `copy_workspace_records` (`collection`, `mode`, `after`, `limit`)
   with the operator bearer.
   - `plan` reports owners and unresolved rows.
   - `copy` puts the rows that are missing with `if_absent`; a rerun copies nothing twice.
   - `verify` reads every assigned row back and compares it. It answers `complete: true` only when
     every row on the page has one owner, is present and is unchanged.
   - Each run first reapplies step 1's assignment, which picks up rows an old replica wrote during
     a rolling update.
4. **Resolve.** Each `needs_selection` row gets an operator-reviewed
   `workspace_assignments.workspace_id`, and is copied on the next run.
5. **Drop.** Once `verify` is complete on every page of every environment, a later migration drops
   the table and the store methods that read it.

## Tests

| Test | Covers |
|---|---|
| `WorkspaceRecords.*` | Sealed values and blind indexes; one `put_record` per append; a duplicate id is `conflict` with `details.revision`; collection created once on first write; paged listing (1,205 records) and newest-N; a 40 KB body as one record; records that do not open in another workspace or lookup; conditional `put`, `update` retrying a raced write, `busy` retries, conditional `remove`; record capabilities outside the workspace allow-lists |
| `Stores/WorkspaceRecordStores.ConversationsStayInTheirWorkspaceAndSpace/{Memory,Postgres}` | Two spaces with two workspaces each, one conversation id in each, stored only at its own endpoint; a foreign space or workspace refused; missing, malformed and unknown `workspace_id` refused at `/workspace_id`; a workspace that is not ready; workspace deletion |
| `PostgresAskMessages.MigrationDropsTheTableAndKeepsItsAssignments` | Migration 0050 drops a pre-0050 `ask_messages` table with its rows, keeps its `workspace_assignments` row, and two reruns of the whole schema leave it dropped (the `ask_messages` copy and its 0045 test went with the table, lake#483) |
| `Stores/WorkspaceRecordStores.OpportunitiesStayInTheWorkspaceTheirScanRanIn/{Memory,Postgres}` | Two spaces with two workspaces each, a scan saved only in its own workspace and only once, newest scan first, dismissal in the owning workspace only, foreign spaces and workspaces, missing, malformed and unknown `workspace_id` for reads and writes, a workspace that is not ready, workspace deletion |
| `WorkspaceRecords.AnOpportunityStatusChangeRereadsAfterAConcurrentWrite` | Derived keys, a second settle writing nothing, a raced conditional status write reread and retried, another space's id never opening |
| `Stores/WorkspaceRecordStores.TheOneShotCopyMovesEachOpportunityToItsScansWorkspace/{Memory,Postgres}` | The scan's workspace owning rows in a space with two workspaces, a deleted workspace's rows reported, idempotent copy, verify accepting a later status change and reporting tampering, operator only |
| `PostgresOpportunities.MigrationRecordsEachRowsOwnerForTheCopy` | Migration 0046 outcomes (`task_workspace`, `sole_workspace`, `needs_selection`, `deleted_workspace`) on a rerun, late rows, paged legacy listing |
| `Streams.CreatorOwnershipLivesInTheTopicsWorkspaceZetta`, `Streams.ExistingOwnershipCannotBeReassignedAfterAnExternalDrop` | Topic creators at their own workspace endpoint across two spaces and two workspaces; blind keys; the `if_absent` claim; missing, malformed, unknown and foreign `workspace_id`; a workspace that is not ready; workspace deletion; release on drop |
| `Stores/WorkspaceRecordStores.TheOneShotCopyMovesEachTopicCreatorIntoItsWorkspace/{Memory,Postgres}` | The `stream_owners` copy: plan, paged copy, rerun, verify, read back per workspace, tamper detection, `unknown_workspace` blocking and `workspace_deleted` not blocking completion, operator only |
| `PostgresStreamOwners.MigrationRecordsEachRowsWorkspaceForTheCopy` | Migration 0047 outcomes on a rerun, the md5 row id lake and SQL share, late rows, paging, a workspace deleted after assignment |
| `Drive.ShareLinksLiveOnlyInTheWorkspaceThatHoldsThem`, `Stores/WorkspaceRecordStores.TheOneShotCopyMovesDriveShareLinksAndRoutesTheirTokens/{Memory,Postgres}`, `PostgresDriveShareLinks.MigrationRecordsOwnersAndContentFreeRoutes`, `SharedLinkScenario.*` | Drive share links (lake#350): two spaces with two workspaces each, self-routing tokens, forged and moved records, revocation conflicts, not-ready and deleted workspaces, the copy and its routes, migration 0048, the signed-out recipient and the sharing pages |
| `ApplicationAuth.EventsAndDeliveriesLiveInTheApplicationsWorkspace`, `ApplicationAuth.AnAttemptIsClaimedOnceAndSentOnce` | Application events in the application's workspace among two spaces and two workspaces; missing, malformed, unknown and foreign `workspace_id`; least privilege; not ready; deletion; an attempt claimed by another caller is not sent; an abandoned claim does not block retries |
| `Stores/WorkspaceRecordStores.TheOneShotCopyMovesApplicationEventsAndTheirDeliveries/{Memory,Postgres}` | Both application collections: plan, verify, copy, rerun, read back through the capability, a legacy application resolved after review, tamper detection |
| `PostgresApplicationWebhooks.MigrationRecordsEachEventsOwnerForTheCopy` | Migration 0049: application and event outcomes on a rerun, late rows, resolution, the immutable workspace |
| `ApplicationAuth.KeysAndWebhooksLiveInTheApplicationsWorkspace`, `ApplicationAuth.AConcurrentRotationIsRereadNotLost` | Signing keys and webhooks (lake#352, lake#355) in the application's workspace among two spaces with two workspaces each: sealed records only at their own endpoint, nothing in the control store, the anonymous JWKS by application id, missing, malformed, unknown and foreign `workspace_id`, least privilege, rotation, a raced rotation reread, a stale `if_revision` refused, a ring under another key not opening, not ready, deletion |
| `Stores/WorkspaceRecordStores.TheOneShotCopyMovesApplicationKeysAndWebhooks/{Memory,Postgres}`, `PostgresApplicationCredentials.MigrationRecordsEachRowsOwnerForTheCopy` | Both credential collections: plan, verify, copy, rerun, a copied key signing and a copied secret signing a delivery, a rotation after the copy accepted, `needs_selection` blocking and `workspace_deleted` not, a legacy application resolved after review, tamper detection; migration 0056 outcomes on a rerun, late rows, resolution, paged rings and webhooks |
| `Stores/WorkspaceRecordStores.RepositoryUsageStaysInEachRepositorysWorkspace/{Memory,Postgres}` | Repository usage (lake#348, lake#349) across two spaces with two workspaces each: nothing written on the request path, one hourly record per workspace, a second replica's conditional increment, a workspace that is not ready (kept), a deleted workspace (dropped), a repository without a workspace, the bound, build records written once |
| `Stores/WorkspaceRecordStores.TheOneShotCopyMovesMeteringIntoEachSpacesWorkspace/{Memory,Postgres}`, `PostgresMetering.MigrationRecordsEachRowsOwnerForTheCopy`, `PostgresSync.PushAndPullUsePostgresControlPlane` | The metering copy beside lake's own counters and build records, rows without transfer left out, `needs_selection` blocking, tamper detection, operator only; migration 0053; a real push and pull flushed into the repository's workspace and nothing in PostgreSQL |
| `Stores/WorkspaceRecordStores.ForgeRecordsLiveInTheirRepositorysWorkspace/{Memory,Postgres}`, `Forge.*`, `Export.*`, `GitHubImport.*`, `PostgresSync.EnforcesBranchProtectionAtReferenceUpdates`, `SyncHttp.*ProtectedRefusal*`, `PostgresForgeScenario.*` | Forge records (lake#341) in their repository's workspace among two spaces with two workspaces each: stored sealed only at their own endpoint, another workspace's or repository's ids `not_found`, a record copied elsewhere not opening, conditional updates keeping both changes and a stale revision refused, least privilege, missing and unknown `repository_id`, a repository without a workspace (`conflict`), a workspace that is not ready and workspace deletion, for the capabilities and for branch protection read before a push; export, import and GitHub import; a real push refused and accepted by rules read from the workspace |
| `Stores/WorkspaceRecordStores.TheOneShotCopyMovesForgeRecordsIntoTheirRepositorysWorkspace/{Memory,Postgres}`, `PostgresForgeRecords.MigrationRecordsEachRowsOwnerForTheCopy` | The `forge_records` copy: plan, verify, copy, rerun, read back through the capabilities with original ids and instants, `needs_selection` blocking and `workspace_deleted` not, a later edit accepted, tamper detection, operator only; migration 0054 outcomes on a rerun, late rows, a repository assigned after review, paged legacy listing |
| `ApplicationAuth.UsersAndOrganizationsLiveInTheApplicationsWorkspace`, `Stores/WorkspaceRecordStores.TheOneShotCopyMovesApplicationUsersAndOrganizations/{Memory,Postgres}`, `PostgresApplicationUsers.MigrationRecordsEachRowsOwnerForTheCopy` | Application users and organizations (lake#353, lake#354) in the application's workspace among two spaces and two workspaces of one: sealed records and claims at their own endpoint only, a duplicate email, TAAP identity or name refused, fresh and abandoned claims, a sign-up completed at the next sign-in, an account that is not a user refused with its session revoked, suspension, deletion, renames, missing, malformed, unknown and foreign `workspace_id`, least privilege, not ready, workspace deletion; the copy (plan, verify, copy, rerun, read back, claims holding, a later status change accepted, tamper detection, `needs_selection` resolved, operator only); migration 0055 outcomes on a rerun, late rows, resolution, paging |
| `ApplicationAuth.SettingsLiveInTheApplicationsWorkspace`, `Stores/WorkspaceRecordStores.TheOneShotCopyMovesApplicationSettings/{Memory,Postgres}`, `PostgresApplicationSettings.MigrationRecordsEachRowsOwnerForTheCopy` | Application settings (lake#351) in the application's workspace among two spaces with two workspaces each: one sealed record at its own endpoint and none in the routing row, reads and listings from the workspace, the theme written `if_revision` with a raced change reread, missing, malformed, unknown and foreign `workspace_id`, origins checked from the record, least privilege, a record under another key not opening, not ready (listed with `settings_unavailable`), workspace deletion; the copy (plan, verify, copy, rerun, read back, a later theme accepted, `needs_selection` resolved, tamper detection, operator only); migration 0059 outcomes on a rerun, routing rows holding no settings, resolution, paging |
| `SecretsCapabilities.*`, `Stores/WorkspaceRecordStores.SecretsLiveInTheirMountsWorkspace/{Memory,Postgres}`, `PostgresCapabilities.SecretsAndEncryptionRoundTrip`, `IdentityExit.*` | Secrets (lake#328-#332) in their mount's workspace among two spaces with two workspaces each: sealed records only at their own endpoint and none in PostgreSQL, duplicate names, missing, malformed, unknown and foreign `workspace_id` and `mount_id`, another space's person, a record copied elsewhere not opening, a workspace not ready, workspace deletion, version numbers claimed once and a stopped put completed, conditional delete/undelete/destroy, customer KEK rotation across workspaces finished after a workspace was not ready, dynamic leases |
| `Stores/WorkspaceRecordStores.TheOneShotCopyMovesSecretsIntoTheirMountsWorkspace/{Memory,Postgres}`, `PostgresSecrets.MigrationRecordsEachMountsOwnerAndRoute` | The five secrets collections: plan, copy, rerun, verify (data keys compared unwrapped), legacy ciphertexts read through the capabilities, later changes and a KEK rotation accepted, `needs_selection` resolved, `workspace_deleted` not blocking, tamper detection, operator only; migration 0057 outcomes and routes on a rerun, the dropped lease foreign key, immutable routes, late mounts |
| `SecretsCapabilities.DynamicLeases*`, `…ARenewalRacingARevocationOrAnExpiryCannotReviveTheLease`, `…TheExpiryLoopReadsOnlyTheWorkspacesOfDueLeases`, `…ARenewalThatBeatsTheExpiryLoopIsKept`, `…TheServersOwnerLoopSettlesDueLeases`, `Stores/WorkspaceRecordStores.DynamicLeasesLiveInTheirMountsWorkspace/{Memory,Postgres}` | Dynamic leases (lake#362) in their mount's workspace among two spaces with two workspaces each: sealed records and a content-free route, least privilege before any workspace call, missing, malformed and unknown `lease_id`, raced renewals refused after a revocation or expiry and kept when they win, the expiry pass reading only due workspaces, holds, not ready, abandoned routes, workspace deletion, the server tick |
| `Stores/WorkspaceRecordStores.TheOneShotCopyMovesDynamicLeasesIntoTheirMountsWorkspace/{Memory,Postgres}`, `PostgresSecrets.LeaseRoutesAreAContentFreeDueIndex` | The lease copy (plan, copy, rerun, verify, routes due at expiry or at once when revoked, later changes accepted, `needs_selection` resolved, tamper detection, operator only); migration 0058, claim and hold, settled routes never due again, the legacy listing |
| `WorkspaceRecordsZetta.TheEngineStoresAndReturnsSealedRecords` | Runs with `LAKE_TEST_ZETTA_URL` and `LAKE_TEST_ZETTA_TOKEN` against a node built from Zetta dev with #1233: append, list, a 20 KB body, `if_absent` conflict with `details.revision`, `update`, a stale `if_revision`, conditional delete |
| `HttpResponseIsolation.AskOrama*`, `…TheAgentFactory*`, `AgentRuns.EveryAgentFlowTheSiteOffersIsACapability`, `PagePerformance.*` | The pages and the capability end to end, and the page budgets |

`tests/record_fixture.hpp` (`RecordTables`) implements the record store as records use it,
including `409` details and injected `busy`. Every test workspace caller routes record capabilities
to it, one store per workspace endpoint.

## Checklist for each follow-up

1. Pick the shape from the [table above](#the-primitive-per-record-shape). Every shape except
   operational metrics uses `WorkspaceRecords`; operational metrics use Zetta's labelled time
   series. Anything that might be billed is a record.
2. Write a codec in `include/lake/control/<collection>.hpp`. Give it a lookup with every field the
   old query filtered on, and set `retention_ms` if rows expired before.
3. Writes: use `append` for logs, `update` for mutable rows, and `remove(id, if_revision)` for
   one-use consumption. Never use last-writer-wins where a claim, code or unique name is involved.
4. Replace the `Store` methods for the table with `assign_legacy_<collection>` and
   `legacy_<collection>(after, limit)`, in PostgreSQL, memory and the metrics wrapper.
5. Route every caller through `workspace_records(context, customer_id, workspace_id)`. Pages use
   the path's workspace, capabilities take `workspace_id`, and agents get it from their task.
6. Add the next free migration recording owners in `workspace_assignments`, and check that
   migration prefixes are unique.
7. Add the collection to `copy_workspace_records`.
8. Tests:
   - two spaces with two workspaces each;
   - missing, malformed and unknown ids;
   - a workspace that is not ready;
   - workspace deletion;
   - conflicts, for mutable collections;
   - the copy in both stores;
   - the migration;
   - the page budgets;
   - `WorkspaceRecordsZetta` against a local Zetta dev node.
9. Update the inventory row in [workspace-data-plane.md](workspace-data-plane.md), the API doc, the
   schema doc and `db/README.md`, and write the cutover steps in the pull request.
10. Once the copy is verified on dev, file or ship the migration that drops the table.

## Records a signed-out caller reaches: `drive_share_links`

A drive share link (lake#350) is read by someone who may not be signed in and holds only its
bearer token, so no page path or input names the workspace. The token does:
`<workspace id>.<link id>.<secret>`. Lake reads the workspace's multitenancy row (its space and
whether it is live), opens that workspace's records, `get`s the link by key, and compares the
token's digest with the one sealed in the record. Tokens issued before the move do not have that
shape. Migration `0048_drive_share_link_workspaces` and each copy run record a content-free route for
them in `drive_share_link_routes` (token digest → link id, workspace), which is dispatch and stays
central. Revocation is a conditional `put` on the revision read; a second revocation is `conflict`.
The `share-link:<id>` grant stays in the policy store. The full contract is
[sharing.md](../api/sharing.md#where-links-live).

The cutover is the same as for `ask_messages`, with `{"collection":"drive_share_links"}`: vendor
the schema through `0048` and select `drive_share_link_workspaces_guard`, deploy, then `plan`, `copy`
and `verify`. Until a link is copied, its old token resolves to `not_found`. A `needs_selection` row
gets its route on the next copy run after its assignment is chosen.

## Zetta follow-ups and limits

- **Fixed in Zetta dev `9798f159` (zetta#1229, zetta#1230):** every data-changing SQL statement now
  checks `sql:write`/`sql:ddl`, and connector credentials are sealed at rest and never returned.
  The connection store is still not used for lake secrets; lake seals its own.
- **Membership-change window.** During a membership change, two owners can briefly both accept a
  conditional write to the same key. Lake's claims are safe only on a fully meshed StatefulSet,
  which is what the operator runs.

## Cutover runbook: application settings, dev cluster

Migration `0059_application_settings_workspaces` (lake#351). Each application's settings go to its
own workspace (`auth_applications.workspace_id`, `0049`), so there is no selection step of its own:
an application is `needs_selection` only while it has no workspace. These steps are for the
coordinator and are not run by this change.

1. Workspace Zettas run Zetta `dev` at or after `82f617e`, as for `ask_messages`.
2. Have the operator vendor `db/schema.sql` through `0059_application_settings_workspaces` and
   select `application_settings_workspaces_guard`. Apply the schema, then read the warnings
   `application settings not copied (needs_selection|workspace_deleted)`.
3. Deploy the lake image built from this change. Settings are read and written only in each
   application's workspace. **Until step 4 finishes, an existing application's hosted and
   custom-domain pages and `get_application` answer `not_found`, its cross-origin sign-ins and
   authorization-code requests are refused, and the application lists show it by id with
   `settings_unavailable`**; run step 4 right after the deploy. Same-origin sign-ins through the
   pool's own capabilities, the JWKS and token exchange keep working. `update_application_theme`
   now requires `workspace_id`. New registrations work at once.
4. Run `POST /v1/copy_workspace_records {"collection":"auth_applications","mode":"plan"}` with the
   operator bearer, then `{"mode":"copy"}` following `next_after` until it is null, then
   `{"mode":"verify"}` over every page. Each record keeps the row's `created_at` instant.
5. Applications without a workspace stay `needs_selection` and keep `complete` false: assign the
   workspace as in step 5 of the application events runbook, or delete the application (dev data
   may be deleted); the next run copies them. Applications recorded `workspace_deleted` went with
   their workspace and do not block. Repeat step 4.
6. Once `verify` answers `complete: true` on every page, file the migration that drops the
   `name`, `allowed_origins`, `redirect_uris` and `theme` columns of `auth_applications` (and its
   `auth_applications_settings_check`), with the copy output attached. The table itself stays: it
   is the routing row.

## Cutover runbook: application users and organizations, dev cluster

Migration `0055_application_user_workspaces` (lake#353, lake#354). Each row's owner is its
application's workspace (`auth_applications.workspace_id`, migration `0049`), so there is no
selection step of its own: a row is `needs_selection` only while its application has no workspace.
These steps are for the coordinator and are not run by this change.

1. Workspace Zettas run Zetta `dev` at or after `82f617e`, as for `ask_messages`.
2. Have the operator vendor `db/schema.sql` through `0055_application_user_workspaces` and select
   `application_user_workspaces_guard`. Apply the schema, then read the warnings
   `application auth_app_users not copied (needs_selection|workspace_deleted)` and the same for
   `auth_app_organizations`.
3. Deploy the lake image built from this change. Application users and organizations are read and
   written only in the application's workspace. Until step 4 finishes, **earlier users of an
   application cannot sign in to it** (they are not users of the application in its workspace yet;
   each refused sign-in's session is revoked), its user and organization lists are empty, and a new
   sign-up with an earlier user's email fails on lake's own account table; run step 4 right after
   the deploy. The user and organization capabilities now require `workspace_id`.
4. Run `POST /v1/copy_workspace_records {"collection":"auth_app_users","mode":"plan"}` with the
   operator bearer, then `{"mode":"copy"}` following `next_after` until it is null, then
   `{"mode":"verify"}` over every page; then the same for `auth_app_organizations`. The copy writes
   each pool user's email claim and each organization's TAAP and name claims with its record; a
   claim another holder already has is reported as that row's error, never taken over.
5. Rows of an application without a workspace stay `needs_selection` and keep `complete` false:
   assign the application's workspace as in step 5 of the application events runbook below, or
   delete the application (dev data may be deleted); the next run copies them. Rows recorded
   `workspace_deleted` went with their workspace and do not block. Repeat step 4.
6. Once `verify` answers `complete: true` on every page of both collections, file the migration
   that drops `auth_app_users` and `auth_app_organizations`, with the copy output attached.

## Cutover runbook: secrets, dev cluster

Migration `0057_secret_workspaces` (lake#328, #329, #330, #331, #332). These steps are for the
coordinator and are not run by this change.

1. Workspace Zettas run Zetta `dev` at or after `82f617e`, as for `ask_messages`.
2. Have the operator vendor `db/schema.sql` through `0057_secret_workspaces` and select
   `secret_workspaces_guard`. Apply the schema, then read the warnings
   `secret mount not copied (needs_selection)`: each is a mount of a space with several live
   workspaces, or none.
3. Deploy the lake image built from this change. `create_secret_mount` and `list_secret_mounts`
   now require `workspace_id`. Until step 4 finishes, existing mounts answer `not_found` (a mount
   with a route but no copied records) and their secrets, keys and leases are unavailable; run step
   4 right after the deploy. Do not rotate a customer KEK between steps 2 and 4 unless step 4 is run
   again afterwards (the copy reads the rewrapped rows; verify compares unwrapped keys).
4. Run `copy_workspace_records` with the operator bearer for each collection in this order:
   `secrets_mounts`, `secrets_data_keys`, `secrets_encryption_keys`, `secrets_entries`,
   `secrets_versions`. For each, `plan`, then `copy` following `next_after` until it is null, then
   `verify` over every page.
5. For each `needs_selection` mount, choose its workspace in a reviewed change:
   `UPDATE workspace_assignments SET workspace_id = '<workspace>' WHERE object_table =
   'secrets_mounts' AND object_id = '<mount>'` (a live workspace of the mount's space), or delete
   the mount's rows (dev data may be deleted). The next run records its route and copies it and
   everything below it; repeat step 4. Mounts recorded `workspace_deleted` went with their
   workspace and do not block.
6. Once `verify` answers `complete: true` on every page of all five collections, file the migration
   that drops `secrets_versions`, `secrets_entries`, `secrets_encryption_keys`,
   `secrets_data_keys` and `secrets_mounts` (and the per-name `secret` policy objects 0009 created),
   with the copy output attached. `secret_mount_routes` stays: it is the routing.
   `secrets_dynamic_leases` has its own runbook below (lake#362).

## Cutover runbook: dynamic leases, dev cluster

Migration `0058_secret_lease_workspaces` (lake#362). A lease follows its mount, so the secrets
runbook above comes first: a lease is copied once its mount has a route. These steps are for the
coordinator and are not run by this change.

1. Workspace Zettas run Zetta `dev` at or after `82f617e`, as for `ask_messages`.
2. Have the operator vendor `db/schema.sql` through `0058_secret_lease_workspaces` and select
   `secret_lease_workspaces_guard`. Apply the schema, then read the warnings
   `dynamic secret lease not copied (needs_selection)`: each is a lease whose mount has no route.
3. Deploy the lake image built from this change. Leases are read and written only in their mount's
   workspace, and every replica's owner loop runs the expiry pass. Until step 4 finishes, an
   existing lease answers `not_found` to renew and revoke; run step 4 right after the deploy. New
   leases work at once.
4. Run `copy_workspace_records` with the operator bearer for `secrets_dynamic_leases`: `plan`, then
   `copy` following `next_after` until it is null, then `verify` over every page. Each copied
   lease's route is due at its expiry (at once for a revoked one), so the next expiry passes erase
   the sealed values of leases that already expired or were revoked; verify accepts that.
5. A lease of a `needs_selection` mount is resolved with its mount (step 5 of the secrets runbook);
   the next run copies it. Leases of a mount whose workspace was deleted went with it and do not
   block. Dev data may be deleted instead.
6. Once `verify` answers `complete: true` on every page, file the migration that drops
   `secrets_dynamic_leases`, with the copy output attached. `secret_lease_routes` stays: it is the
   routing and the due index.

## Cutover runbook: application events and deliveries, dev cluster

Migration `0049_application_webhook_workspaces` (lake#356, lake#357). Steps 1 and 7 of the
`ask_messages` runbook below apply unchanged; the rest:

1. Workspace Zettas run Zetta `dev` at or after `82f617e`, as for `ask_messages`.
2. Have the operator vendor `db/schema.sql` through `0049_application_webhook_workspaces` and
   select `application_webhook_workspaces_guard`. Apply the schema, then read the warnings: each
   `table=auth_applications` row is an application whose space has several workspaces, and each
   `table=auth_app_webhook_events` row is one of its events.
3. Deploy the lake image built from this change. Registering an application now requires
   `workspace_id`. An unassigned application's sign-ins and lifecycle actions answer
   `workspace_not_ready` until step 5, because its events have nowhere to go.
4. Run `copy_workspace_records` for `auth_app_webhook_events` and then
   `auth_app_webhook_deliveries`, each `plan`, then `copy` until `next_after` is null, then
   `verify` over every page. A delivery is copied into its event's workspace.
5. For each unassigned application, set its workspace once, in a reviewed change:
   `UPDATE auth_applications SET workspace_id = '<workspace>' WHERE id = '<application>' AND
   workspace_id IS NULL`. The routing trigger accepts only a live workspace of the application's
   space, and refuses any later change. Or delete the application (dev data may be deleted). The
   next copy resolves its events; repeat step 4.
6. Once `verify` answers `complete: true` on every page of both collections, file the migration
   that drops `auth_app_webhook_deliveries` and `auth_app_webhook_events`, with the copy output
   attached. `auth_applications.workspace_id` stays: it is the routing pointer lake#351 builds on.

## Cutover runbook: application keys and webhooks, dev cluster

Migration `0056_application_credential_workspaces` (lake#352, lake#355). Applications already name
their workspace (`0049`). These steps are for the coordinator and are not run by this change.

1. Workspace Zettas run Zetta `dev` at or after `82f617e`, as for `ask_messages`.
2. Have the operator vendor `db/schema.sql` through `0056_application_credential_workspaces` and
   select `application_credential_workspaces_guard`. Apply the schema, then read the warnings
   `application credential not copied (needs_selection|workspace_deleted)`.
3. Deploy the lake image built from this change. Keys and webhooks are now read and written only in
   each application's workspace. **Until step 4 finishes, an existing application cannot sign
   anyone in, its JWKS is empty, and its webhooks receive nothing**; run step 4 right after the
   deploy. Do not rotate an existing application's key before its ring is copied (a rotation
   answers `not_found` until then). New registrations work at once.
4. Run `copy_workspace_records` for `auth_app_signing_keys` and then `auth_app_webhooks`, each
   `plan`, then `copy` until `next_after` is null, then `verify` over every page. The envelopes
   are copied as the tables hold them and open with the same `keystore` data keys.
5. Rows of an application without a workspace stay `needs_selection` and keep `complete` false:
   assign the application's workspace as in the `0049` runbook, or delete the application (dev
   data may be deleted); the next run copies them. Rows recorded `workspace_deleted` went with
   their workspace and do not block. Repeat step 4.
6. Once `verify` answers `complete: true` on every page of both collections, file the migration
   that drops `auth_app_signing_keys` and `auth_app_webhooks`, with the copy output attached. The
   `keystore` rows stay until the keystore itself moves (lake#324).

## Cutover runbook: `ask_messages`, dev cluster

**Done.** On dev (lake `20261003c`, revision `5858129`; workspace Zetta `dev-82f617ea`), the copy
for `ask_messages` gave: plan 15 assigned, 0 unresolved, one workspace; copy 15 copied, 0 already
present, 0 failed; verify `complete: true`, 15 verified, 0 missing, 0 mismatched. Steve approved the
drop on 2026-10-03, and migration `0050_drop_ask_messages` (guard `drop_ask_messages_guard`,
lake#483) drops the table; `copy_workspace_records` no longer accepts the collection. Steps 1 to 7
below are kept as the pattern the later runbooks refer to. Applying `0050` on dev is the
coordinator's: the operator vendors `db/schema.sql` and selects `drop_ask_messages_guard`.

1. Roll the workspace Zettas to an image built from Zetta `dev` at or after `82f617e` (zetta PR
   #1233). Check with `GET /w/<id>/v1/openapi.json`, or a `workspace_openapi` read, that
   `put_record` is listed.
2. Have the operator vendor `db/schema.sql` through `0045_ask_message_workspaces` and select
   `ask_message_workspaces_guard`. Apply the schema, then read the warnings for `needs_selection`
   rows and for rows with no space.
3. Deploy the lake image built from this change. Ask Orama and the Agent Factory now read and
   write only workspace records, and old conversations are not shown until step 5.
4. Run `POST /v1/copy_workspace_records {"collection":"ask_messages","mode":"plan"}` with the
   operator bearer, and record `unresolved`.
5. Run `{"mode":"copy"}`, following `next_after` until it is null, then run `{"mode":"verify"}`
   over every page.
6. For each `needs_selection` row, choose the workspace in a reviewed change to
   `workspace_assignments`, or accept its loss (dev data may be deleted). Then repeat step 5.
7. Once `verify` answers `complete: true` on every page, file the migration that drops
   `ask_messages`, with the copy output attached. (Done: lake#483, migration `0050`.)

## Cutover runbook: `opportunities`, dev cluster

The same steps as for `ask_messages`, for lake#376. These steps are for the coordinator and are not
run by this change.

1. The workspace Zettas run Zetta `dev` at or after `82f617e` (step 1 above).
2. Have the operator vendor `db/schema.sql` through `0046_opportunity_workspaces` and select
   `opportunity_workspaces_guard`. Apply the schema, then read the warnings for rows without a
   workspace (`needs_selection`, `deleted_workspace`).
3. Deploy the lake image built from this change. Design → Opportunities now reads and writes only
   workspace records, and earlier Opportunities are not shown until step 5. A scan read again after
   the deploy (its page, `get_agent_task`) is saved in its workspace under the key the copy uses.
4. Run `POST /v1/copy_workspace_records {"collection":"opportunities","mode":"plan"}` with the
   operator bearer, and record `unresolved`.
5. Run `{"mode":"copy"}`, following `next_after` until it is null, then `{"mode":"verify"}` over
   every page. A status a person changed after the copy is accepted by `verify`.
6. Rows recorded as `deleted_workspace` went with their workspace: delete them from the table (dev
   data may be deleted). For each `needs_selection` row, choose the workspace in a reviewed change to
   `workspace_assignments`, or delete it. Then repeat step 5.
7. Once `verify` answers `complete: true` on every page, file the migration that drops
   `opportunities`, with the copy output attached.

## Cutover runbook: `stream_owners`, dev cluster

Topic creators (lake#379). Each `stream_owners` row already names its workspace, so there is no
selection step: migration `0047_stream_owner_workspaces` records `recorded_workspace` for a live
workspace of the same space, `workspace_deleted` for a deleted one (its topic went with it, and
the row does not hold the drop back), and `unknown_workspace` for anything else. The copy keys each
creator by the blind index of its topic in `lake_stream_owners`. These steps are for the
coordinator and are not run by this change.

1. The workspace Zettas must already run Zetta `dev` at or after `82f617e` (step 1 of the
   `ask_messages` runbook).
2. Have the operator vendor `db/schema.sql` through `0047_stream_owner_workspaces` and select
   `stream_owner_workspaces_guard`. Apply the schema and read the warnings for rows that are not
   copied.
3. Deploy the lake image built from this change. Until step 4 finishes, `get_stream` reports
   `can_manage: false` for existing topics, and `update_stream`/`delete_stream` are refused,
   because their creators are not yet in the workspace; reads, publishing and creating new topics
   work.
4. Run `POST /v1/copy_workspace_records {"collection":"stream_owners","mode":"plan"}` with the
   operator bearer, then `{"mode":"copy"}` and `{"mode":"verify"}`, following `next_after` until
   it is null.
5. For each `unknown_workspace` row, decide in a reviewed change: delete the row (dev data may be
   deleted) or correct it. Then repeat step 4.
6. Once `verify` answers `complete: true` on every page, file the migration that drops
   `stream_owners`, with the copy output attached.

## Cutover runbook: `forge_records`, dev cluster

Migration `0054_forge_record_workspaces` (lake#341). Each row's owner is its repository's workspace
(`repositories.workspace_id`, migration `0051_repository_workspaces`, lake#315 part 1), so there is
no selection step of its own: a row is `needs_selection` only while its repository has no workspace.
These steps are for the coordinator and are not run by this change.

1. Workspace Zettas run Zetta `dev` at or after `82f617e`, as for `ask_messages`.
2. Have the operator vendor `db/schema.sql` through `0054_forge_record_workspaces` and select
   `forge_record_workspaces_guard`. Apply the schema, then read the warnings
   `forge record not copied (needs_selection|workspace_deleted)`.
3. Deploy the lake image built from this change. Issues, reviews, comments, releases, webhooks,
   exports and branch protection rules are read and written only in each repository's workspace.
   Until step 4 finishes, existing records are not shown, and **existing branch protection rules
   are not enforced** (a push sees only rules set after the deploy); run step 4 right after the
   deploy. A push that updates references to a repository whose workspace is not ready, or that has
   no workspace, is refused with that reason.
4. Run `POST /v1/copy_workspace_records {"collection":"forge_records","mode":"plan"}` with the
   operator bearer, then `{"mode":"copy"}` following `next_after` until it is null, then
   `{"mode":"verify"}` over every page.
5. Rows of a repository without a workspace stay `needs_selection` and keep `complete` false: assign
   the repository's workspace in a reviewed change (lake#315), or delete the repository's rows (dev
   data may be deleted); the next run copies them. Rows recorded `workspace_deleted` went with
   their workspace and do not block. Repeat step 4.
6. Once `verify` answers `complete: true` on every page, file the migration that drops
   `forge_records`, with the copy output attached.

## Cutover runbook: metering, dev cluster

Migration `0053_metering_workspaces` (lake#348, lake#349). Repositories must already name their
workspace (migration `0051_repository_workspaces`, lake#315 part 1). These steps are for the
coordinator and are not run by this change.

1. Workspace Zettas run Zetta `dev` at or after `82f617e`, as for `ask_messages`.
2. Have the operator vendor `db/schema.sql` through `0053_metering_workspaces` and select
   `metering_workspaces_guard`. Apply the schema, then read the warnings: each
   `table=metering_usage` or `table=metering_build_records` row belongs to a space with several
   live workspaces or none.
3. Deploy the lake image built from this change. Sync requests now count transfer in memory and
   the owner loop writes it to each repository's workspace every five seconds; usage reads write
   build records. Until step 4, the Usage page shows no transfer from before the deploy; build
   compute is unaffected, because it is read from the repositories again.
4. Run `copy_workspace_records` for `metering_usage` and then `metering_build_records`, each
   `plan`, then `copy` until `next_after` is null, then `verify` over every page.
5. Rows recorded as `needs_selection` belong to a space with several workspaces: choose the
   workspace in a reviewed change to `workspace_assignments`, or delete the rows (dev data may be
   deleted). Rows of a deleted workspace are reported and do not block. Repeat step 4.
6. Once `verify` answers `complete: true` on every page of both collections, file the migration
   that drops `metering_usage` and `metering_build_records` (and `metering_guard` stays as a guard
   table), with the copy output attached.
