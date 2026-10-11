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
central; a repository's description (lake#315 part 2, decision 54) is the second, and the first
whose record exists only once something was written (a repository with no description has none). Repository environments and the platform mirror (lake#358, lake#359) are the first whose
policy objects stay in the control store under a derived id, so authorization never reads the
workspace. An application's authorization codes (lake#363) are the first one-use codes: issued
`if_absent`, redeemed by a delete at the revision read, and the first collection with a retention.
Agent tasks (lake#374, lake#375) are the first dispatch rows split in two: the claim, its lease
and the bus stay in PostgreSQL, and what was written for each task is a record of its workspace.
A workspace's audit (lake#317, decision 52) is the first collection that is a hash chain: each
event claims its place with one `if_absent` write keyed by that place, appended off the request
path by a bounded in-process queue.
A tenant image's manifests and tags (lake#383 phase A) are the first records whose source is not a
PostgreSQL table: the copy reads them from the instance registry's files, and a content-free central
route (`oci_repository_routes`) finds their workspace from an address that names only the account.
Transcripts (lake#561) are the first collection created in the workspace from the start, with no
PostgreSQL table behind it and nothing to copy: `lake_transcripts`, found by the account, edited at the
revision read ([transcripts.md](../api/transcripts.md)).
A project's decision (lake#577) is lake's own record about an engine object: `lake_project_decisions`,
keyed by the engine's project id and found by the account, written once when the project is created
and removed with it ([workspaces.md](../api/workspaces.md#a-projects-decision)).
Drives (lake#561) follow the same way: `lake_drives`, found by the account, and `lake_drive_entries`,
found by drive, each edited at the revision read; a workspace file's bytes are not a record but an
object in the workspace Zetta's `lake_files` bucket ([drives.md](../api/drives.md#how-file-bytes-are-kept)).
A workspace's own AI solutions (lake#597) follow too: `lake_solutions`, found by the account, edited and
deleted at the revision read; Orama's catalogue solutions stay the engine's and are never copied
([solutions.md](../api/solutions.md#where-they-live)).
A workspace's MCP connections (decision 60, lake#652) follow too: `lake_mcp_connections`, found by
the account, edited and deleted at the revision read, with each connection's credential a workspace
secret it only names; an OAuth sign-in in progress is a `lake_mcp_oauth_states` record kept ten
minutes and deleted when it is taken ([mcp-connections.md](../api/mcp-connections.md#where-things-live)).
An agent's definition revisions (lake#653) are lake's own record about an engine object too:
`lake_agent_revisions`, append-only, found by the account and the agent's id, one written after each
`create_agent` or `update_agent` the engine accepts, since the engine keeps only an agent's latest
definition ([train-agents.md](../api/train-agents.md#definition-revisions)).

The rule it implements is in [workspace-data-plane.md](workspace-data-plane.md): everything goes in
a workspace Zetta, including credentials, secrets and metrics. Lake's PostgreSQL and the lake Zetta
keep only multitenancy and content-free dispatch. Billing is the one exception, and its design is
still open.

## Status

| Part | State |
|---|---|
| `WorkspaceRecords`, `RecordSet<Codec>` | Implemented on Zetta's keyed record store ([zetta#1231](https://github.com/oramasearch/zetta/issues/1231), merged in zetta PR #1233 to `dev`, `82f617e`). Unit tests run against an engine fake; an opt-in test runs against a real Zetta node built from that commit. |
| Append-only collections | Implemented. `ask_messages` and `auth_app_webhook_events` have moved; the `ask_messages` table is dropped (migration `0050`, lake#483). The events about a workspace's data (`audit_events`, lake#317, migration `0063`) are a chain in that workspace: the n-th event is keyed by the blind index of `sequence:<n>`, written `if_absent`, so replicas never fork it; control-plane events stay in the central `audit_events` chain (decision 52, [audit.md](../api/audit.md#which-log)). |
| Mutable records and claims (`get`, conditional `put`, `remove`, `update`) | Implemented and tested. `opportunities` (lake#376) has moved: derived keys written `if_absent`, status changes conditional on the revision read. `stream_owners` (lake#379) claims a topic name with `if_absent` and releases it with `remove(id, if_revision)`. `drive_share_links` (lake#350) has moved: revocation is a conditional `put` on the revision read. `auth_app_webhook_deliveries` (lake#357) claims each attempt with `if_absent` and settles it with `if_revision`. `metering_usage` (lake#348) adds to an hourly counter with `if_revision`, and `metering_build_records` (lake#349) records each build result once with `if_absent`. `forge_records` (lake#341) has moved into the workspace of each record's repository: edits are conditional `put`s on the revision read, and the push path reads branch protection once before the push. `auth_app_users` and `auth_app_organizations` (lake#353, lake#354) have moved into the application's workspace: a status change or rename is a conditional `put` on the revision read, and a pool user's email and an organization's TAAP identity and name are claims written `if_absent` in the same collection. `auth_app_signing_keys` (lake#352) is one key ring per application, rotated with one `if_revision` write; `auth_app_webhooks` (lake#355) are written once with `if_absent`. `auth_applications` (lake#351): each application's settings are one record keyed by it, written `if_absent` at registration, and a theme change is an `if_revision` write reread on a conflict; the table keeps the content-free routing row. `platform_environments` and `platform_mirror` (lake#358, lake#359) have moved into the workspace of each repository: an environment is written `if_absent` under its derived id, and a mirror row is replaced only by a newer operator-control row, at the revision read. The five secrets tables (lake#328-#332) have moved into the workspace of each mount's route: version and key-version numbers are claimed `if_absent` and pointers moved `if_revision`, and every delete, undelete, destroy and data-key rewrap is a conditional `put`. `secrets_dynamic_leases` (lake#362) has moved into its mount's workspace: renew, revoke and the scheduled expiry each rewrite the lease at the revision read, so a renewal never revives a revoked or expired lease. `auth_app_authorization_codes` (lake#363) has moved into the application's workspace: a code is written `if_absent` under a blind key, redeemed by `remove(id, if_revision)` so only one exchange wins, and expired by the collection's `retention_ms` (five minutes). `agent_tasks` (lake#374, lake#375): each task's request, messages and answer are one record in its own workspace, written `if_absent` before the dispatch row and changed only after PostgreSQL accepted an event, conditionally on the revision read; a finished answer is never changed. The claim and its lease stay one PostgreSQL statement. `repositories` (lake#315 part 2): each repository's description is one record keyed by it, written `if_absent` before the routing row at create (and removed if the row is refused), changed `if_revision` and reread on a conflict; the table keeps the routing row. `oci_manifests`, `oci_tags` and `oci_blobs` (lake#383 phase A): a tenant image's manifests, content-addressed and written `if_absent`, its tags, moved at the revision read (never torn, still last-writer-wins between concurrent pushes), and the blobs the account pushed into the workspace, in the workspace its repository is routed to by `oci_repository_routes`. Since phase B a blob record also names the blob's object in the workspace's liquid store: written `if_absent` by an upload, or given its object at the revision read by the copy. |
| `copy_workspace_records` (the operator's one-shot copy and verify) | Implemented for `audit_events` (the central events about a workspace's data, copied as they are; the central chain stays) `oci_tags` (the instance registry's tenant tags and the manifests they name, lake#383) and `oci_blobs` (the tenant blobs the instance registry holds, streamed into each workspace's liquid store and verified by SHA-256, lake#383 phase B). Every other collection left the list with its source: `ask_messages` (migration `0050`), and `opportunities`, `stream_owners`, `drive_share_links`, `auth_app_webhook_events`, `auth_app_webhook_deliveries`, `metering_usage`, `metering_build_records`, `forge_records`, `auth_app_users`, `auth_app_organizations`, `auth_app_signing_keys`, `auth_app_webhooks`, the five `secrets_*` collections, `secrets_dynamic_leases`, `auth_applications`, `platform_mirror`, `platform_environments`, `auth_app_authorization_codes`, `agent_tasks` and `repositories` (migration `0070`, lake#311), each after its copy was verified on dev. A request for any of them is `invalid_input`. |
| Dropping the moved tables from PostgreSQL | Done. `ask_messages` is dropped by migration `0050_drop_ask_messages` (lake#483). Every other moved table, and the content columns of `auth_applications` (name, origins, redirect URIs, theme), `agent_tasks`/`agent_task_status` (body, message, result) and `repositories` (description), are dropped by migration `0070_drop_moved_tables` (lake#311), which refuses rows with no recorded workspace owner; copy and verify are a manual gate before applying it ([schema.md](../control-plane/schema.md#drop-moved-tables-migration)). The routing rows, the routes and the `workspace_assignments` record stay. `repository_refs` was dropped without a copy by migration `0064` (lake#319): nothing read it, and the liquid repository holds the refs. `audit_events` is not dropped: the split copied its workspace events and the central chain keeps every row. |

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
| Append-only log, read back by key | `ask_messages` (#370), `sso_provisioning_logs` (#333), `auth_app_webhook_events` (#356), `agent_cron_runs` (#372), `offline_recovery_notifications` (#339) | `append`: one `put_record` with `if_absent`. `list` is newest first by the lookup index, then put in order. Set `retention_ms` where a purge applies. |
| Hash-chained log written off the request path | `audit_events` of a workspace (#317, **moved**, decision 52) | Each event is signed with its predecessor's digest and written at the blind key of `sequence:<n>` with `if_absent`; a writer that loses a place reads it, searches on for the end and signs again. Calls only queue events (`AuditRecorder`); the owner loop flushes every second. `list` with a `keep` predicate walks until enough events match. `retention_ms` 0. |
| Work items sent once, with retry history | `auth_app_webhook_deliveries` (#357) | One record per attempt, keyed by `index_of(collection, "<item>\n<attempt>")`. Claim it pending with `put(if_absent)` before the side effect and settle it with `put(if_revision)` after: only the caller whose claim succeeded performs it. |
| Mutable rows found by id | `opportunities` (#376, **moved**), `drive_share_links` (#350, **moved**), `repositories` description (#315 part 2, **moved**; the routing row stays), `apps` (#377), `stores` (#365), `forge_records` (#341, **moved**), `auth_app_users` (#353, **moved**), `auth_app_organizations` (#354, **moved**), `auth_applications` settings (#351, **moved**; the routing row stays), `platform_environments` (#358, **moved**), `platform_mirror` (#359, **moved**), `oci_tags` (#383, **moved**; manifests in `oci_manifests`, content-addressed and written once), `transcripts` (#561, new: born in the workspace, never in PostgreSQL, [transcripts.md](../api/transcripts.md)), `drives` and `drive_entries` (#561, new, [drives.md](../api/drives.md)), `solutions` (#597, new, [solutions.md](../api/solutions.md)) | `get`, then `put` with `if_revision`, or `update`, which retries on `409` by rereading. `remove` with `if_revision`. A mirror row is written only when the stored row is older than the operator's. |
| One-use codes, sessions, replay evidence, claims | `sessions` (#323), `identity_oauth_flows` (#326), `identity_webauthn_challenges` (#327), `identity_taap_assertions` (#361), `auth_app_authorization_codes` (#363, **moved**), `offline_recovery_attempts` (#336) | Consume with `remove(id, if_revision)`. Only one caller succeeds; the others get `conflict` or `not_found`. Expiry is the collection's `retention_ms`. |
| Unique names | `stream_owners` (#379, done), an application user's email and an application organization's TAAP identity and name (#353, #354, done), repository and app names | A name-keyed record written with `if_absent`, using a blind key (see [Keys](#record-format)). A value that must be unique beside a record found by id is a **claim** in that record's collection: it names its holder, has its own lookup so listings never show it, is released with `remove(if_revision)` by its holder, and an abandoned one (older than its grace period, naming a holder that does not exist) is taken over with `if_revision`. |
| Secrets and credentials | `auth_app_signing_keys` (#352, **moved**), `auth_app_webhooks` (#355, **moved**), `secrets_*` (#328–#332, **moved**), `keystore` (#324), `pki_authorities` (#342), `custodial_identities` (#364), `oauth_providers` (#325) | Records whose sealed body holds the secrets subsystem's existing envelope ciphertext. Zetta's connection store is not used (lake seals its own secrets). An application's keys are one key ring record keyed by the application, so a rotation (retire the current key, add the next) is one write conditional on the revision read; each seed and webhook secret is the `keystore`-wrapped envelope (`Store::encrypt_application_credential`), which the copy moves without opening. A name that selects a record is the record's key only through `index_of`, so no name is visible. The secrets collections are routed by a content-free `secret_mount_routes` row (mount → workspace) and keep the customer KEK in `keystore` ([secrets.md](../api/secrets.md#where-secrets-live-lake328-332)). |
| Dispatch rows with content | `agent_tasks` and `agent_task_status` (#374, #375, **moved**) | The row stays in PostgreSQL as dispatch (the claim, its lease and the bus need it without a workspace call), and what was written is one record per task in its workspace, keyed by the task. The record is written `if_absent` before the row exists, and changed only after the row accepted an event, conditionally on the revision read; it is frozen once it holds a finished answer, so the first terminal write the row accepted is the one kept. |
| Records a scheduled loop settles | `secrets_dynamic_leases` (#362, **moved**) | Records in the workspace, plus a content-free central **due index** (id, workspace, due instant) that the loop claims from with `FOR UPDATE SKIP LOCKED`, so a pass reads only the workspaces that hold due records. The index is a hint, never later than the record; the loop settles each record with a conditional write, rereads on a conflict, and moves the index to what it read. |
| Exact counters that may be billed | `metering_usage` (#348, **moved**), `metering_build_records` (#349, **moved**), `metering_job_records` (lake#658, new) | Records, never metrics, since a metric point is not fsynced on its own. A counter is one record per workspace and hour, added to with `if_revision` from an in-process buffer that is flushed off the request path (`MeteringRecorder`); each counted item (a build result; a finished build, agent job or cron run whose resources were charged) is one record written `if_absent` ([metering.md](../api/metering.md#resources)). |
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
                                                             std::int64_t since_us = 0, Keep keep = {});
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
  does not page through the whole history. With `keep`, only the records it accepts are returned
  and counted toward `newest` (a filtered read of a workspace's audit). A missing collection lists
  as empty.
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
is the account (`customer:<id>`), every Opportunity of the workspace is one listing, and
`set_opportunity_status` reads, checks the account and writes with `if_revision`, rereading on a
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

- **Retention** is the collection's `retention_ms`, enforced by the engine: a record expires that
  long after its last write. `ask_messages` keeps everything, as before;
  `auth_app_authorization_codes` keeps a code five minutes, its lifetime, and lake also checks the
  code's own expiry, since a copied code is written later than it was issued. Deleting a workspace deletes its Zetta and its volume, and the records go
  with them.
- **Latency.** The page budget test holds `/ask` and the Agent Factory (`/train/agents/new`, now served at `/design/agents/new`) with an open conversation
  at one `list_records` call in one round, and Design → Opportunities at `list_records` and
  `list_methodologies` in one round.
- **Consistency.** Writes are linearizable per key, and a read after an acknowledged write sees it
  on every live node. The record store doc lists the windows where this does not hold: ownership
  changing during a membership change, and nodes that are not fully meshed.

## Migration and cutover

Every collection follows the same steps. There is no dual write, and nothing is dropped until the
copy is verified.

1. **Record owners.** A migration (`0045_ask_message_workspaces` for this one) inserts each row's
   owner into `workspace_assignments`. An account with exactly one live workspace owns its rows. Every
   other row is `needs_selection`, and rows with no account are reported. The migration adds no
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
   the table and the store methods that read it (`0050` for `ask_messages`, `0070` for every other
   collection but `audit_events` and `oci_tags`).

## Tests

| Test | Covers |
|---|---|
| `WorkspaceRecords.*` | Sealed values and blind indexes; one `put_record` per append; a duplicate id is `conflict` with `details.revision`; collection created once on first write; paged listing (1,205 records) and newest-N; a 40 KB body as one record; records that do not open in another workspace or lookup; conditional `put`, `update` retrying a raced write, `busy` retries, conditional `remove`; record capabilities outside the workspace allow-lists |
| `Stores/WorkspaceRecordStores.ConversationsStayInTheirWorkspaceAndSpace/{Memory,Postgres}` | Two accounts with two workspaces each, one conversation id in each, stored only at its own endpoint; a foreign account or workspace refused; missing, malformed and unknown `workspace_id` refused at `/workspace_id`; a workspace that is not ready; workspace deletion |
| `PostgresMovedTables.MigrationRefusesRowsWithNoRecordedWorkspaceOwner` | Migration 0070 in a PostgreSQL schema of its own: a fresh database has none of the 21 tables or 8 columns; an older database with a row in each, each with its recorded owner, and an opportunity recorded `deleted_workspace` (which does not block), keeps them all while one row is `needs_selection` (the migration raises and names the table) and drops them all once that row's workspace is deleted; two reruns of the section and two of the whole schema leave them dropped; routing rows, routes, `audit_events`, `keystore` and the `workspace_assignments` record stay |
| `PostgresAskMessages.MigrationDropsTheTableAndKeepsItsAssignments` | Migration 0050 drops a pre-0050 `ask_messages` table with its rows, keeps its `workspace_assignments` row, and two reruns of the whole schema leave it dropped (the `ask_messages` copy and its 0045 test went with the table, lake#483) |
| `Stores/WorkspaceRecordStores.OpportunitiesStayInTheWorkspaceTheirScanRanIn/{Memory,Postgres}` | Two accounts with two workspaces each, a scan saved only in its own workspace and only once, newest scan first, dismissal in the owning workspace only, foreign accounts and workspaces, missing, malformed and unknown `workspace_id` for reads and writes, a workspace that is not ready, workspace deletion |
| `WorkspaceRecords.AnOpportunityStatusChangeRereadsAfterAConcurrentWrite` | Derived keys, a second settle writing nothing, a raced conditional status write reread and retried, another account's id never opening |
| `Streams.CreatorOwnershipLivesInTheTopicsWorkspaceZetta`, `Streams.ExistingOwnershipCannotBeReassignedAfterAnExternalDrop` | Topic creators at their own workspace endpoint across two accounts and two workspaces; blind keys; the `if_absent` claim; missing, malformed, unknown and foreign `workspace_id`; a workspace that is not ready; workspace deletion; release on drop |
| `Drive.ShareLinksLiveOnlyInTheWorkspaceThatHoldsThem`, `SharedLinkScenario.*` | Drive share links (lake#350): two accounts with two workspaces each, self-routing tokens, forged and moved records, revocation conflicts, not-ready and deleted workspaces, the signed-out recipient and the sharing pages |
| `ApplicationAuth.EventsAndDeliveriesLiveInTheApplicationsWorkspace`, `ApplicationAuth.AnAttemptIsClaimedOnceAndSentOnce` | Application events in the application's workspace among two accounts and two workspaces; missing, malformed, unknown and foreign `workspace_id`; least privilege; not ready; deletion; an attempt claimed by another caller is not sent; an abandoned claim does not block retries |
| `ApplicationAuth.KeysAndWebhooksLiveInTheApplicationsWorkspace`, `ApplicationAuth.AConcurrentRotationIsRereadNotLost` | Signing keys and webhooks (lake#352, lake#355) in the application's workspace among two accounts with two workspaces each: sealed records only at their own endpoint, nothing in the control store, the anonymous JWKS by application id, missing, malformed, unknown and foreign `workspace_id`, least privilege, rotation, a raced rotation reread, a stale `if_revision` refused, a ring under another key not opening, not ready, deletion |
| `Stores/WorkspaceRecordStores.RepositoryUsageStaysInEachRepositorysWorkspace/{Memory,Postgres}` | Repository usage (lake#348, lake#349) across two accounts with two workspaces each: nothing written on the request path, one hourly record per workspace, a second replica's conditional increment, a workspace that is not ready (kept), a deleted workspace (dropped), a repository without a workspace, the bound, build records written once |
| `PostgresSync.PushAndPullUsePostgresControlPlane` | A real push and pull flushed into the repository's workspace and no metering table in PostgreSQL |
| `Stores/WorkspaceRecordStores.ForgeRecordsLiveInTheirRepositorysWorkspace/{Memory,Postgres}`, `Forge.*`, `Export.*`, `GitHubImport.*`, `PostgresSync.EnforcesBranchProtectionAtReferenceUpdates`, `SyncHttp.*ProtectedRefusal*`, `PostgresForgeScenario.*` | Forge records (lake#341) in their repository's workspace among two accounts with two workspaces each: stored sealed only at their own endpoint, another workspace's or repository's ids `not_found`, a record copied elsewhere not opening, conditional updates keeping both changes and a stale revision refused, least privilege, missing and unknown `repository_id`, a repository without a workspace (`conflict`), a workspace that is not ready and workspace deletion, for the capabilities and for branch protection read before a push; export, import and GitHub import; a real push refused and accepted by rules read from the workspace |
| `ApplicationAuth.UsersAndOrganizationsLiveInTheApplicationsWorkspace` | Application users and organizations (lake#353, lake#354) in the application's workspace among two accounts and two workspaces of one: sealed records and claims at their own endpoint only, a duplicate email, TAAP identity or name refused, fresh and abandoned claims, a sign-up completed at the next sign-in, an account that is not a user refused with its session revoked, suspension, deletion, renames, missing, malformed, unknown and foreign `workspace_id`, least privilege, not ready, workspace deletion |
| `ApplicationAuth.SettingsLiveInTheApplicationsWorkspace` | Application settings (lake#351) in the application's workspace among two accounts with two workspaces each: one sealed record at its own endpoint and none in the routing row, reads and listings from the workspace, the theme written `if_revision` with a raced change reread, missing, malformed, unknown and foreign `workspace_id`, origins checked from the record, least privilege, a record under another key not opening, not ready (listed with `settings_unavailable`), workspace deletion; a routing row with no settings columns |
| `SecretsCapabilities.*`, `Stores/WorkspaceRecordStores.SecretsLiveInTheirMountsWorkspace/{Memory,Postgres}`, `PostgresCapabilities.SecretsAndEncryptionRoundTrip`, `IdentityExit.*` | Secrets (lake#328-#332) in their mount's workspace among two accounts with two workspaces each: sealed records only at their own endpoint and none in PostgreSQL, duplicate names, missing, malformed, unknown and foreign `workspace_id` and `mount_id`, another account's person, a record copied elsewhere not opening, a workspace not ready, workspace deletion, version numbers claimed once and a stopped put completed, conditional delete/undelete/destroy, customer KEK rotation across workspaces finished after a workspace was not ready, dynamic leases |
| `SecretsCapabilities.DynamicLeases*`, `…ARenewalRacingARevocationOrAnExpiryCannotReviveTheLease`, `…TheExpiryLoopReadsOnlyTheWorkspacesOfDueLeases`, `…ARenewalThatBeatsTheExpiryLoopIsKept`, `…TheServersOwnerLoopSettlesDueLeases`, `Stores/WorkspaceRecordStores.DynamicLeasesLiveInTheirMountsWorkspace/{Memory,Postgres}` | Dynamic leases (lake#362) in their mount's workspace among two accounts with two workspaces each: sealed records and a content-free route, least privilege before any workspace call, missing, malformed and unknown `lease_id`, raced renewals refused after a revocation or expiry and kept when they win, the expiry pass reading only due workspaces, holds, not ready, abandoned routes, workspace deletion, the server tick |
| `PostgresSecrets.LeaseRoutesAreAContentFreeDueIndex` | Migration 0058 rerun, claim and hold, settled routes never due again |
| `Stores/WorkspaceRecordStores.PlatformEnvironmentsLiveInTheirRepositorysWorkspace/{Memory,Postgres}`, `PlatformFixture.*` | Repository environments and the mirror (lake#358, lake#359) among two accounts with two workspaces each: sealed records only at the repository's workspace endpoint, a quiet sync writing nothing and a newer row written once, an older row never replacing a newer one, a stale `if_revision` refused, one environment created once by two writers, a record copied elsewhere not opening, a foreign account answered without a workspace call or refused, missing, malformed and unknown `repository_id` and `environment`, a workspace that is not ready (only its repository stale), a repository without a workspace (`conflict`), workspace deletion; secret scanning, ordering, audit, logs, roles and pages |
| `ApplicationAuth.AuthorizationCodesLiveInTheApplicationsWorkspace`, `IdentityExit.PostgresProvesIdentityPkiAndDynamicSecrets` | Authorization codes (lake#363) among two accounts with two workspaces each: one sealed record under a blind key only at the application's endpoint, the collection's retention, one redemption, a replay refused, racing exchanges (another delete first, a stale revision) refused, an expired code refused and removed, `if_absent` issue, a moved record not opening, missing, malformed and unknown `application_id`, least privilege, not ready, deletion; the PKCE exchange end to end on PostgreSQL |
| `WorkspaceRecords.AnAgentTaskKeepsItsFirstAnswerAndRereadsARacedEvent`, `Stores/WorkspaceRecordStores.AgentTasksKeepWhatWasWrittenInTheirOwnWorkspace/{Memory,Postgres}`, `Stores/AgentLeases.*`, `AgentRuns.*`, `HttpResponseIsolation.*` (agent turns) | Agent tasks (lake#374, lake#375) among two accounts with two workspaces each: requests, messages and answers sealed in their own workspace and nothing in PostgreSQL or on the bus, the claim reply read from the workspace, exactly one claim, another agent's bus credential and a person refused, the first finished answer kept and a late one refused, a raced write reread, missing, malformed, unknown and foreign `workspace_id`, a workspace that is not ready (no turn recorded, the task left waiting), a task without its request never claimed, workspace deletion, the asking task routed by `parent_task_id` |
| `Stores/WorkspaceRecordStores.AuditEventsAboutAWorkspaceLiveInItsOwnChain/{Memory,Postgres}`, `AuditRouting.EveryRegisteredCapabilityIsClassified`, `PostgresSync.PushAndPullUsePostgresControlPlane`, `PostgresSync.EnforcesBranchProtectionAtReferenceUpdates`, `PlatformFixture.*` | The workspace audit (lake#317) among two accounts with two workspaces each: a call queues and writes nothing until the flush; control-plane calls, refusals for want of authority and missing, malformed and unknown `workspace_id` central; sealed records only at the workspace's endpoint; a second replica's appends never forking the chain; verify, export and both readers, a person's activity limited to their accounts; a workspace that is not ready (kept), deleted (dropped), the bound; tampering found; every capability classified; real pushes, branch protection decisions and deployment phases in the repository's workspace |
| `Stores/WorkspaceRecordStores.TheOneShotCopyMovesWorkspaceAuditEvents/{Memory,Postgres}`, `PostgresAudit.MigrationRecordsEachWorkspaceEventsOwnerForTheCopy` | The `audit_events` copy: plan, copy, rerun, verify, read back once, `workspace_deleted` not blocking, tamper detection, operator only; migration 0063 outcomes on a rerun, refusals, control-plane and unnamed events staying central, another account's workspace not trusted, late events, paging in append order |
| `Stores/WorkspaceRecordStores.RepositoryDescriptionsLiveInTheirWorkspace/{Memory,Postgres}`, `PostgresRepositories.MigrationDropsRefsAndTheRowIsRoutingOnly`, `Repositories.OwnedByACustomerNamedWithinIt`, `HttpResponseIsolation.FreshSignUpOwnsFirstRepositoryWithZeroInitialMetering/*` | Repository descriptions (lake#315 part 2) among two accounts with two workspaces each: one sealed record at its own endpoint and none in the routing row (the store refuses one), get and list (one `list_records` per workspace), the synchronous routing answer, no record without a description, missing, malformed, unknown and foreign `workspace_id`, a taken name and a not-ready workspace leaving nothing behind, least privilege, a raced change reread, a stale revision refused, a record under another key not opening, a deleted workspace, no description column in the routing row; migration 0064 (repository_refs dropped and never recreated by two whole-schema reruns, a chosen id kept) |
| `Stores/TenantImages.LiveInTheirRoutedWorkspaceAndKeepTheirAddresses/{Memory,Postgres}`, `Stores/TenantImages.TheOneShotCopyRoutesAndMovesLegacyTags/{Memory,Postgres}`, `Stores/TenantImages.RoutesAreClaimedOnlyByTheirOwnerAndUnroutedOnlyByTheOperator/{Memory,Postgres}`, `Stores/TenantImages.AMoveReroutesAnImageAndItsCacheInOneStep/{Memory,Postgres}`, `WorkspaceRecords.ImageTagsMoveAtTheRevisionReadAndManifestsAreWrittenOnce`, `PostgresImageRoutes.MigrationRoutesUnambiguousImagesOnly`, `RegistryProtocols.*` | Tenant images (lake#383 phase A) among two accounts with two workspaces each: routes created only with an explicit workspace (missing, malformed, unknown, foreign and deleted `workspace_id` refused at `/workspace_id`), never changed, a name shared across accounts, a build claiming only its own target, an app moved between workspaces and its route removed by the operator only, foreign layers refused (`lake_oci_blobs`), an app without a workspace refused, unreadable routes as `503`; push and pull through the route byte for byte; sealed records only in the routed workspace; a tag moved at the revision read and a raced move reread; a manifest written once; builds confined to their workspace and routing on their first push; app pulls; npm and one-component OCI unaffected; a workspace that is not ready refused; deletion removing the routes; the copy (no route written by plan or verify, routing by app, build cache and sole workspace, `needs_selection` and `blob_missing` blocking and `unknown_space` not, copy, rerun, verify, a tag pushed before the copy and a later push accepted, tampering found, operator only); migration 0068 outcomes on a rerun and the immutability trigger |
| `WorkspaceRecordsZetta.TheEngineStoresAndReturnsSealedRecords` | Runs with `LAKE_TEST_ZETTA_URL` and `LAKE_TEST_ZETTA_TOKEN` against a node built from Zetta dev with #1233: append, list, a 20 KB body, `if_absent` conflict with `details.revision`, `update`, a stale `if_revision`, conditional delete, a workspace audit chain appended past a taken place and verified |
| `HttpResponseIsolation.AskOrama*`, `…TheAgentFactory*`, `…TheWebApp*`, `AgentRuns.EveryAgentFlowTheSiteOffersIsACapability` | The web app's capabilities end to end over HTTP |

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
   - two accounts with two workspaces each;
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
10. Once the copy is verified on dev, file or ship the migration that drops the table, with a block
    that refuses rows with no recorded workspace owner, as `0070` does.

## Records a signed-out caller reaches: `drive_share_links`

A drive share link (lake#350) is read by someone who may not be signed in and holds only its
bearer token, so no page path or input names the workspace. The token does:
`<workspace id>.<link id>.<secret>`. Lake reads the workspace's multitenancy row (its account and
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

## Cutover runbook: agent tasks, dev cluster

**Done.** The copy was run and verified on dev, and migration `0070_drop_moved_tables` (lake#311) drops what it moved; `copy_workspace_records` no longer accepts these collections. The steps are kept as the record of the cutover.

Migration `0062_agent_task_workspaces` (lake#374, lake#375). Each task's content goes to the task's
own workspace (`agent_tasks.workspace_id`, `0038`), so there is no selection step of its own: a
task is `needs_selection` only while it names no workspace. These steps are for the coordinator
and are not run by this change.

1. Workspace Zettas run Zetta `dev` at or after `82f617e`, as for `ask_messages`.
2. Have the operator vendor `db/schema.sql` through `0062_agent_task_workspaces` and select
   `agent_task_workspaces_guard`. Apply the schema, then read the warnings
   `agent task content not copied (needs_selection|workspace_deleted)`. The operator reads no
   column of either table (decision 51; its `*_pgx.go` reads only the guard tables), so it needs no
   other change.
3. Deploy the lake image built from this change. Requests, messages and answers are read and
   written only in each task's workspace. **Until step 4 finishes, an earlier task shows no
   message or answer, a conversation's next turn does not see the earlier turn's answer, and a
   task still waiting for its agent cannot be claimed** (its request is not in the workspace yet;
   the claim answers `not_found` and the bus loop fails it at its waiting deadline). Run step 4
   right after the deploy. A task that was running across the deploy keeps working: its next event
   writes a record without a request, which the copy fills in. New turns work at once.
4. Run `POST /v1/copy_workspace_records {"collection":"agent_tasks","mode":"plan"}` with the
   operator bearer, then `{"mode":"copy"}` following `next_after` until it is null, then
   `{"mode":"verify"}` over every page. Each record keeps the task's `created_at` instant.
5. Tasks without a workspace stay `needs_selection` and keep `complete` false: choose the workspace
   in a reviewed change (set `agent_tasks.workspace_id` with the routing trigger disabled for that
   statement, and `workspace_assignments.workspace_id` for object table `agent_task_content`), or
   delete the task (dev data may be deleted); the next run copies it. Tasks recorded
   `workspace_deleted` went with their workspace and do not block. Repeat step 4.
6. Once `verify` answers `complete: true` on every page, file the migration that drops
   `agent_tasks.body`, `agent_task_status.message` and `agent_task_status.result`, with the copy
   output attached. Both tables stay: they are the dispatch rows.

## Cutover runbook: workspace audit, dev cluster

Migration `0063_audit_workspaces` (lake#317, lake#318). The steps are in
[audit.md](../api/audit.md#cutover-dev-cluster): vendor the schema through `0063` and select
`audit_workspaces_guard`, deploy, then `copy_workspace_records {"collection":"audit_events"}` plan,
copy and verify. Nothing needs selecting, and nothing is dropped: `audit_events` is append-only and
stays the central chain.

## Cutover runbook: application settings, dev cluster

**Done.** The copy was run and verified on dev, and migration `0070_drop_moved_tables` (lake#311) drops what it moved; `copy_workspace_records` no longer accepts these collections. The steps are kept as the record of the cutover.

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

## Cutover runbook: repository descriptions, dev cluster

**Done.** The copy was run and verified on dev, and migration `0070_drop_moved_tables` (lake#311) drops what it moved; `copy_workspace_records` no longer accepts these collections. The steps are kept as the record of the cutover.

Migration `0064_repository_records` (lake#315 part 2, lake#319). Each repository's description goes
to its own workspace (`repositories.workspace_id`, `0051`), so there is no selection step of its
own: a repository is `needs_selection` only while it has no workspace. On dev that is the `liquid`
repository, which stays `needs_selection` until lake#513 creates a workspace for its account and
migration `0066_assign_unrouted_repositories` assigns it (its description's row becomes
`repository_workspace`). These steps are for the coordinator and are not run by this change.

1. Workspace Zettas run Zetta `dev` at or after `82f617e`, as for `ask_messages`.
2. Have the operator vendor `db/schema.sql` through `0064_repository_records`, select
   `repository_records_guard`, and drop `CREATE TABLE IF NOT EXISTS repository_refs` from the list
   in `TestLakeSchemaMatchesVendoredSource` (`internal/controller/lake_controller_test.go`), since
   the schema no longer creates it. Apply the schema (it drops `repository_refs`, 0 rows on dev),
   then read the warnings `repository description not copied (needs_selection|workspace_deleted)`.
3. Deploy the lake image built from this change. Descriptions are read and written only in each
   repository's workspace. **Until step 4 finishes, an existing repository shows no description**
   (its pages say "No description.", and `get_repository` answers `description: ""`); nothing
   else changes. New repositories with a description work at once.
4. Run `POST /v1/copy_workspace_records {"collection":"repositories","mode":"plan"}` with the
   operator bearer, then `{"mode":"copy"}` following `next_after` until it is null, then
   `{"mode":"verify"}` over every page. Each record keeps the row's `created_at` instant.
5. A repository without a workspace stays `needs_selection` and keeps `complete` false if it holds
   a description; migration `0066` (lake#513) assigns it, and the next run, once the workspace is
   Ready, copies it. Repositories recorded
   `workspace_deleted` went with their workspace and do not block. Repeat step 4.
6. Once `verify` answers `complete: true` on every page, file the migration that drops
   `repositories.description`, with the copy output attached. The table itself stays: it is the
   routing row.

## Cutover runbook: tenant images, dev cluster

Migration `0068_oci_repository_routes` (lake#383 phase A, decision 54 S4-S6). Tenant images move
from the instance registry's `manifests/` into their routed workspace's records; blobs stay in
`blobs/` until phase B (lake#380). Dev holds one account's images (7 manifests). These steps are for
the coordinator and are not run by this change.

1. Workspace Zettas run Zetta `dev` at or after `82f617e`, as for `ask_messages`.
2. Have the operator vendor `db/schema.sql` through `0068_oci_repository_routes` and select
   `oci_repository_routes_guard`. Apply the schema and read the warnings
   `image repository needs a workspace`: names the apps and builds of several workspaces use.
3. Deploy the lake image built from this change. **Until step 4 finishes, an existing tenant image
   answers 404** to pulls (its tags are not in its workspace yet), so run step 4 at once; a running
   app keeps its pulled image, but a new pod of it cannot pull. New builds push and route as usual.
4. Run `POST /v1/copy_workspace_records {"collection":"oci_tags","mode":"plan"}` with the operator
   bearer (it routes what is unambiguous), then `{"mode":"copy"}` following `next_after` until it is
   null, then `{"mode":"verify"}` over every page.
   `plan` and `verify` write nothing; `copy` writes the route of each repository whose owner it
   found.
5. Route each `needs_selection` repository with `create_image_repository` (`customer_id`,
   `workspace_id`, `name`) after review, or leave a cache repository to be rebuilt by the next
   build (dev data). A `blob_missing` tag lost its manifest or a blob in the registry: push it
   again, or accept losing it. `unknown_space` rows (an account that no longer exists) do not block.
   Repeat step 4.
   A route that went to the wrong workspace (an app since recreated in another workspace, or a
   reviewed choice that was wrong) is moved with `POST /v1/unroute_image_repository
   {"customer_id": ..., "name": "<image>", "workspace_id": "<new workspace>"}` with the operator
   bearer: `<image>` and `cache/<image>` move together in one transaction, so a build of the old
   workspace cannot re-claim the name meanwhile. Without `workspace_id` the routes are only
   removed, and the next `create_image_repository`, build push of that target or copy run routes
   them again. Either way the old workspace's records stay there; push the image again from the
   new workspace (or rerun its build).
6. Once `verify` answers `complete: true` on every page, file the change that removes the tenant
   pointers from `manifests/`, with the copy output attached. Blobs move in phase B.

## Cutover runbook: tenant image blobs, dev cluster

lake#383 phase B, no migration. A tenant blob pushed into a switched workspace goes into its
workspace's liquid store; the blobs pushed before stay in `LAKE_STORE_ROOT/<instance>/registry/blobs/`
(on dev `/var/lib/lake/store/prod/registry/blobs/`) and are still served from there, each through
a `lake_oci_blobs` record without `object`, until this copy. These steps are for the coordinator
and are not run by this change.

1. Every workspace an image route names is switched (`move_workspace_store`,
   [workspace-store.md](workspace-store.md)), and lake runs liquid `8955519` or later. A workspace
   switched with no repository by an earlier lake has no store (`create_repository` and image
   pushes there answer `corrupt`): run `move_workspace_store` mode `provision` for it first
   ([workspace-store.md](../api/workspace-store.md#dev-cutover)); it refuses with `conflict` if the
   workspace ever held anything.
2. Deploy the lake image built from this change. Pulls keep working: unmoved blobs are read from the
   instance registry.
3. Run `POST /v1/copy_workspace_records {"collection":"oci_blobs","mode":"plan"}` with the operator
   bearer, then `{"mode":"copy"}` following `next_after` until it is null, then `{"mode":"verify"}`
   over every page. `plan` and `verify` write nothing. A rerun copies nothing twice. `copy` also
   roots, once, each blob already in a workspace store without a liquid root (`rooted`).
4. Unresolved rows: `workspace_not_switched` (switch it, then rerun), `blob_missing` and
   `blob_corrupt` (push the image again, or rerun its build; cache repositories may simply be
   rebuilt). From then on a copied blob is read only from its workspace store.
5. Once `verify` answers `complete: true` on every page, file the change that removes, with Steve's
   approval, the files listed in `instance_paths` of every verify page (null until every routed
   workspace was read and `oci_tags` is complete; advisory: check each path again against the
   records and the instance's repositories when it is removed), the tenant pointers under
   `registry/manifests/<space>/` and their manifest documents in `registry/blobs/`. Nothing is
   removed by lake.

## Cutover runbook: application authorization codes, dev cluster

**Done.** The copy was run and verified on dev, and migration `0070_drop_moved_tables` (lake#311) drops what it moved; `copy_workspace_records` no longer accepts these collections. The steps are kept as the record of the cutover.

Migration `0061_application_authorization_code_workspaces` (lake#363). A code lives five minutes
and is used once, so only codes issued in the five minutes before the deploy are worth copying; the
copy considers only live (unused, unexpired) rows and writes each into its application's workspace
(`auth_applications.workspace_id`, `0049`) at most once. These steps are for the coordinator and are
not run by this change.

1. Workspace Zettas run Zetta `dev` at or after `82f617e`, as for `ask_messages`.
2. Have the operator vendor `db/schema.sql` through `0061_application_authorization_code_workspaces`
   and select `application_authorization_code_workspaces_guard`. Apply the schema, then read the
   warnings `application authorization code not copied (needs_selection|workspace_deleted)`.
3. Deploy the lake image built from this change. Codes are issued and redeemed only in each
   application's workspace. A code issued by the previous image and not yet exchanged is refused
   until step 4 copies it; the relying party's user signs in again if it expires first. New codes
   work at once.
4. Right after the deploy, run `POST /v1/copy_workspace_records
   {"collection":"auth_app_authorization_codes","mode":"plan"}` with the operator bearer, then
   `{"mode":"copy"}` following `next_after` until it is null, then `{"mode":"verify"}` over every
   page. Each copied code keeps its issue instant and expiry. A copied code is recorded as `copied`
   in `workspace_assignments` before it is written, so a rerun never writes it again (a code
   redeemed in the workspace stays redeemed), and verify counts a copied code that has since left
   the workspace (redeemed or expired) as verified.
5. Codes of an application without a workspace are `needs_selection`: assign the application's
   workspace as in the `0049` runbook, or let them expire (dev data may be deleted). Codes recorded
   `workspace_deleted` went with their workspace and do not block. Five minutes after the deploy
   every row has expired and leaves the listing, so `plan`, `copy` and `verify` answer no rows and
   `verify` answers `complete: true`.
6. Once `verify` answers `complete: true`, file the migration that drops
   `auth_app_authorization_codes`, with the copy output attached.

## Cutover runbook: application users and organizations, dev cluster

**Done.** The copy was run and verified on dev, and migration `0070_drop_moved_tables` (lake#311) drops what it moved; `copy_workspace_records` no longer accepts these collections. The steps are kept as the record of the cutover.

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

**Done.** The copy was run and verified on dev, and migration `0070_drop_moved_tables` (lake#311) drops what it moved; `copy_workspace_records` no longer accepts these collections. The steps are kept as the record of the cutover.

Migration `0057_secret_workspaces` (lake#328, #329, #330, #331, #332). These steps are for the
coordinator and are not run by this change.

1. Workspace Zettas run Zetta `dev` at or after `82f617e`, as for `ask_messages`.
2. Have the operator vendor `db/schema.sql` through `0057_secret_workspaces` and select
   `secret_workspaces_guard`. Apply the schema, then read the warnings
   `secret mount not copied (needs_selection)`: each is a mount of an account with several live
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

**Done.** The copy was run and verified on dev, and migration `0070_drop_moved_tables` (lake#311) drops what it moved; `copy_workspace_records` no longer accepts these collections. The steps are kept as the record of the cutover.

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

**Done.** The copy was run and verified on dev, and migration `0070_drop_moved_tables` (lake#311) drops what it moved; `copy_workspace_records` no longer accepts these collections. The steps are kept as the record of the cutover.

Migration `0049_application_webhook_workspaces` (lake#356, lake#357). Steps 1 and 7 of the
`ask_messages` runbook below apply unchanged; the rest:

1. Workspace Zettas run Zetta `dev` at or after `82f617e`, as for `ask_messages`.
2. Have the operator vendor `db/schema.sql` through `0049_application_webhook_workspaces` and
   select `application_webhook_workspaces_guard`. Apply the schema, then read the warnings: each
   `table=auth_applications` row is an application whose account has several workspaces, and each
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
   account, and refuses any later change. Or delete the application (dev data may be deleted). The
   next copy resolves its events; repeat step 4.
6. Once `verify` answers `complete: true` on every page of both collections, file the migration
   that drops `auth_app_webhook_deliveries` and `auth_app_webhook_events`, with the copy output
   attached. `auth_applications.workspace_id` stays: it is the routing pointer lake#351 builds on.

## Cutover runbook: application keys and webhooks, dev cluster

**Done.** The copy was run and verified on dev, and migration `0070_drop_moved_tables` (lake#311) drops what it moved; `copy_workspace_records` no longer accepts these collections. The steps are kept as the record of the cutover.

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

## Cutover runbook: platform environments and mirror, dev cluster

**Done.** The copy was run and verified on dev, and migration `0070_drop_moved_tables` (lake#311) drops what it moved; `copy_workspace_records` no longer accepts these collections. The steps are kept as the record of the cutover.

Migration `0060_platform_workspaces` (lake#358, lake#359). Each row's owner is its repository's
workspace (`repositories.workspace_id`, migration `0051`), so there is no selection step of its
own. These steps are for the coordinator and are not run by this change.

1. Workspace Zettas run Zetta `dev` at or after `82f617e`, as for `ask_messages`.
2. Have the operator vendor `db/schema.sql` through `0060_platform_workspaces` and select
   `platform_workspaces_guard`. Apply the schema, then read the warnings
   `platform row not copied (needs_selection|workspace_deleted)` and
   `platform mirror row not copied (unknown_repository)`. The migration also renames each
   environment's policy object to the environment's derived id; grants keep pointing at the same
   object.
3. Deploy the lake image built from this change. Environments, values, domains and deployments are
   read and written only in each repository's workspace. Until step 4 finishes, a repository's
   environments page is empty and its environment pages answer `not_found`, except for what the
   first sync after the deploy mirrors again from the operator control plane; that sync records
   the phases of each deployment it mirrors before the copy once more in the audit stream. Run step
   4 right after the deploy.
4. Run `copy_workspace_records` with the operator bearer for `platform_mirror` first and then
   `platform_environments`, each `plan`, then `copy` following `next_after` until it is null, then
   `verify` over every page. A mirror row the sync has since replaced by a newer one is verified.
5. Rows of a repository without a workspace stay `needs_selection` and keep `complete` false:
   assign the repository's workspace (lake#315), or delete the rows (dev data may be deleted); the
   next run copies them. Rows recorded `workspace_deleted` or `unknown_repository` do not block.
   Repeat step 4.
6. Once `verify` answers `complete: true` on every page of both collections, file the migration
   that drops `platform_environments` and `platform_mirror`, with the copy output attached. The
   environments' policy objects stay: they carry the grants.

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
   rows and for rows with no account.
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

**Done.** The copy was run and verified on dev, and migration `0070_drop_moved_tables` (lake#311) drops what it moved; `copy_workspace_records` no longer accepts these collections. The steps are kept as the record of the cutover.

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

**Done.** The copy was run and verified on dev, and migration `0070_drop_moved_tables` (lake#311) drops what it moved; `copy_workspace_records` no longer accepts these collections. The steps are kept as the record of the cutover.

Topic creators (lake#379). Each `stream_owners` row already names its workspace, so there is no
selection step: migration `0047_stream_owner_workspaces` records `recorded_workspace` for a live
workspace of the same account, `workspace_deleted` for a deleted one (its topic went with it, and
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

**Done.** The copy was run and verified on dev, and migration `0070_drop_moved_tables` (lake#311) drops what it moved; `copy_workspace_records` no longer accepts these collections. The steps are kept as the record of the cutover.

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

**Done.** The copy was run and verified on dev, and migration `0070_drop_moved_tables` (lake#311) drops what it moved; `copy_workspace_records` no longer accepts these collections. The steps are kept as the record of the cutover.

Migration `0053_metering_workspaces` (lake#348, lake#349). Repositories must already name their
workspace (migration `0051_repository_workspaces`, lake#315 part 1). These steps are for the
coordinator and are not run by this change.

1. Workspace Zettas run Zetta `dev` at or after `82f617e`, as for `ask_messages`.
2. Have the operator vendor `db/schema.sql` through `0053_metering_workspaces` and select
   `metering_workspaces_guard`. Apply the schema, then read the warnings: each
   `table=metering_usage` or `table=metering_build_records` row belongs to an account with several
   live workspaces or none.
3. Deploy the lake image built from this change. Sync requests now count transfer in memory and
   the owner loop writes it to each repository's workspace every five seconds; usage reads write
   build records. Until step 4, the Usage page shows no transfer from before the deploy; build
   compute is unaffected, because it is read from the repositories again.
4. Run `copy_workspace_records` for `metering_usage` and then `metering_build_records`, each
   `plan`, then `copy` until `next_after` is null, then `verify` over every page.
5. Rows recorded as `needs_selection` belong to an account with several workspaces: choose the
   workspace in a reviewed change to `workspace_assignments`, or delete the rows (dev data may be
   deleted). Rows of a deleted workspace are reported and do not block. Repeat step 4.
6. Once `verify` answers `complete: true` on every page of both collections, file the migration
   that drops `metering_usage` and `metering_build_records` (and `metering_guard` stays as a guard
   table), with the copy output attached.
