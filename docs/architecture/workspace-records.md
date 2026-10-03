# Workspace Records

Refs [lake#370](https://github.com/oramasearch/lake/issues/370) and
[lake#311](https://github.com/oramasearch/lake/issues/311). This is the one mechanism lake uses to
keep its records in the workspace Zetta that owns them. Every follow-up that moves a table or
store out of lake's PostgreSQL (lake#315, #316–#384 and the other "Move … into workspace Zettas"
issues) uses it and applies the [checklist](#checklist-for-each-follow-up) below. `ask_messages`
is the first collection on it.

The rule it implements is in [workspace-data-plane.md](workspace-data-plane.md). Every kind of data
goes in a workspace Zetta, including credentials, secrets and metrics. Lake's PostgreSQL and the
lake Zetta keep only multitenancy and content-free dispatch. Billing is the one undesigned
exception.

## Status

| Part | State |
|---|---|
| `WorkspaceRecords`, `RecordSet<Codec>` and the reserved `lake_records` database | Implemented. Unit tests run against an engine fake. An opt-in test runs against a real Zetta node. |
| Append-only collections | Implemented; `ask_messages` is moved. |
| Mutable collections that need conditional writes | Designed below. Blocked on the [Zetta capability](#zetta-capability-required). |
| `copy_workspace_records` (operator one-shot copy and verify) | Implemented for `ask_messages`. |
| Dropping `ask_messages` from PostgreSQL | Not in this change. A later migration drops it once the copy is verified on the dev cluster. |

## What Zetta offers today

These were surveyed read-only in the authorized checkout (`modules/zetta-bridge/modules/zetta`,
2026-10-02). The checkout has no key/value, document, secret, blob or general time-series
capability.

| Primitive | Capabilities | Durability and consistency | Lookup | Conditional write | Fit |
|---|---|---|---|---|---|
| SQL tables | `sql` (a **read**, so `GET`): CREATE DATABASE/TABLE, INSERT, UPDATE, DELETE, SELECT … WHERE one predicate … ORDER BY … LIMIT | One JSON line appended per row, and the row is a key in the replicated Automerge document. Read-your-writes on one node; eventual across nodes (about 500 ms gossip). | Full in-memory scan; one equality predicate; no indexes, no AND, no bind parameters | None | **Records (interim)** |
| Datasets | `upload_dataset`, `create_dataset_from_query`, `promote_dataset` | Partitioned tables | Not keyed | None | Bulk import only. Every call creates a new table. |
| Pub/sub topics | `create_topic`, `publish_event`, `browse_topic`, `update_event` (`expected_revision`), `delete_events` | Node-local, not replicated, best effort ([zetta `docs/pubsub/restart.md`](https://github.com/oramasearch/zetta/blob/main/docs/pubsub/restart.md): no durability guarantee) | By sequence; no key index or compaction | Only `update_event` | Event streams with retention (already used for `agent-events`, `app-events.*`, `build-events.*`). Not records. |
| Replicated catalogue | Typed entities (`create_plan`, profiles, runs) | Replicated | By entity | None | Fixed schemas; not general storage |
| Large objects | Internal `LargeObjectStore` only | Replicated metadata | By id | None | No capability exposes it |
| Connections | `create_connection` | Replicated table, credential **in plaintext** | By name | None | Never for secrets |
| Run metrics | `log_run_metrics`, `get_run_metrics` | Stored in the run, replicated | By run and series | None | ML runs only; no labels; 10,000 points, then downsampled |

There are two engine limits every design below accounts for:

- **Requests are GETs with a 4 KiB start line.** `sql` is registered as a read, so lake sends the
  statement in the query string. Zetta's HTTP parser refuses a request line of about 3.9 KiB:
  measured locally, a 3,800-character statement passed and a 3,900-character one was dropped with
  `field size limit`.
- **There is no atomic compare-and-set.** A compound `WHERE id = … AND rev = …` cannot be written,
  and concurrent writes from two nodes merge as CRDT changes rather than linearizing.

## The primitive per record shape

| Shape | Examples (follow-up) | Primitive | Notes |
|---|---|---|---|
| Append-only log, read back by key | `ask_messages` (#370), `sso_provisioning_logs` (#333), `auth_app_webhook_events` (#356), `auth_app_webhook_deliveries` (#357), `agent_cron_runs` (#372), `offline_recovery_notifications` (#339), `audit_events` (#317) | A `WorkspaceRecords` collection (sealed SQL rows) | Implemented. Ordered by instant, then id. A record is written once and never changes. |
| Event streams consumers replay | `agent-events`, `app-events.*`, `build-events.*` | Workspace pub/sub topics with `retention_ms`/`max_events` | Already done in lake#311. Not copied to records. |
| Mutable rows found by id | `repositories` (#315), `apps` (#377), `stores` (#365), `forge_records` (#341), `platform_environments` (#358), `opportunities` (#376) | Interim: a collection of **revisions** (below). Target: the Zetta keyed record store. | Last writer wins until Zetta offers conditional writes. That is acceptable only where two concurrent edits of one row are harmless. |
| Rows that need atomic conditional writes | `sessions` (#323), `identity_oauth_flows` (#326), `identity_webauthn_challenges` (#327), `identity_taap_assertions` (#361), `auth_app_authorization_codes` (#363), `offline_recovery_attempts` (#336), `agent_task_status` claims (#375), unique names | **Blocked** on the [Zetta capability](#zetta-capability-required) | One-use codes, replay evidence, claims and uniqueness are not safe with last-writer-wins. Do not move them on the interim. |
| Secrets and credentials | `secrets_*` (#328–#332), `keystore` (#324), `pki_authorities` (#342), `custodial_identities` (#364), `auth_app_signing_keys` (#352), `oauth_providers` (#325) | A `WorkspaceRecords` collection. The secrets subsystem's existing envelope ciphertext goes in the sealed body. | Every record is already sealed with a per-workspace key. A secret keeps its own envelope inside that seal. Zetta's connection store is plaintext and is never used for this. |
| Metrics and usage series | `metering_usage` (#348), `metering_build_records` (#349), Prometheus series (#384) | Hourly gauge and counter records keyed by `(scope, hour)`, summed in lake | Run metrics are tied to ML runs and cannot hold usage. A workspace-queryable form, and #384's request series, need the time-series part of the Zetta capability. |

Lake writes only what the contract allows. The record body, the lookup values and any secret all
live in the workspace. Lake keeps only `LAKE_INSTANCE_KEK`, the root the per-workspace keys are
derived from. That is the same custody it has today for every encrypted column.

### Revisions (designed for the mutable follow-ups)

A mutable collection stores each change as a new revision of the record. That revision is written
in full: `rev = n + 1`, the same `rid`, a body or a tombstone. A read keeps each `rid`'s highest
complete revision. Older revisions are removed with `DELETE … WHERE vk = '<rid>:<rev>'`, which
needs a single predicate. Without compare-and-set, two concurrent writers can both write revision
`n + 1`. The reader breaks the tie deterministically, on the higher `at`, then the body digest, so
every replica agrees, but one write is lost. The first mutable follow-up implements revisions in
`WorkspaceRecords` together with its tests. `ask_messages` does not need them, so they are not
built yet.

## Record format

Every collection is a table in the workspace's reserved `lake_records` database. The table name
is the collection name, which is the name of the PostgreSQL table it replaces (`ask_messages`).
Lake creates the database and the table on the first write. A read of a missing table returns no
records.

| Column | Meaning |
|---|---|
| `rid` | Record id (UUID). A copy keeps the PostgreSQL row's id, which makes a rerun idempotent. |
| `rev` | Revision; `1` for append-only collections. |
| `part`, `parts` | This row's part index and the total. A record is visible only when all its parts are present. |
| `at` | Creation instant in microseconds. Strictly increasing within one lake process, so the two messages of one request keep their order. |
| `sv` | Body schema version. |
| `kv` | Key version (`1`). |
| `k` | Blind index of the lookup value: base64url HMAC-SHA-256 of `collection \0 lookup`. |
| `body` | One part of the sealed body (base64url AES-256-GCM). |

**Keys.** For each workspace, lake derives two keys from `LAKE_INSTANCE_KEK` with HKDF-SHA-256:
`lake/workspace-records/v1/seal/<workspace id>` and `…/index/<workspace id>`. The workspace holds
only ciphertext and HMACs. A person who can query the workspace sees no conversation text, and
cannot tell whose it is.

**Integrity and immutable ownership.** The AEAD's associated data binds the workspace, collection,
record id, revision, lookup index, instant and schema version. A row copied into another
workspace's table does not open there. Neither does a row re-keyed to another lookup or re-dated.
A record cannot change owner, so a workspace can never be reassigned for an existing record.

**Parts.** The sealed body is split into parts of 2,800 characters, each written by one INSERT. The
encoded request line then stays under 3.3 KiB, below Zetta's start-line limit. A failed write
leaves an incomplete record, which readers ignore. A part written twice, by a retried insert or a
rerun copy, is the same ciphertext and is deduplicated.

**Statements.** Statements contain only values lake made itself: UUIDs, integers, and base64url
HMACs and ciphertext. None of them contains a quote, and `literal_safe` refuses anything else, so
no caller text reaches SQL.

**Visibility.** The database is lake's. `workspace_sql` refuses a statement or `database` that names
`lake_records`. `workspace_query` and `workspace_act` refuse an `input` that names it.
`call_workspace` removes its rows from `list_datasets` and from `system.*` answers. Sealing is what
protects the data. The guards keep the table out of Train → Data and out of agents' views.

## C++ interface

```cpp
// include/lake/control/workspace_records.hpp
class WorkspaceRecords {
    using Call = std::function<task<ZettaControlResult<json>>(std::string capability, bool read, json input)>;
    static ZettaControlResult<WorkspaceRecords> open(Call call, std::string workspace_id, std::string_view instance_kek);
    task<ZettaControlResult<StoredRecord>> append(RecordCollection, std::string lookup, json body, RecordWrite = {});
    task<ZettaControlResult<std::vector<StoredRecord>>> list(RecordCollection, std::string lookup, std::size_t newest = 0);
};
template <class Codec> class RecordSet;   // typed: append(Value), list(lookup, newest)

// src/api/workspace_records_api.hpp: the records of one workspace through call_workspace
ZettaControlResult<WorkspaceRecords> workspace_records(const ApiContext&, std::string customer_id, std::string workspace_id);
```

A collection is a codec: `Value`, `collection` (name and schema version), `lookup(Value)`,
`encode(Value)` and `decode(StoredRecord)`. `AskMessageCodec` in
`include/lake/control/ask_messages.hpp` is the reference. Its lookup is the conversation and the
person (`conversation:<id>\nuser:<id>`), so only the person who had a conversation reads it back.

`workspace_records` binds the existing workspace caller (`call_workspace`) to one customer and
workspace. Every call therefore goes through `WorkspaceService::call`. That call checks the
workspace's owner and readiness and refuses rather than falling back. Each call is also counted
and named in `Server-Timing` (`zetta_sql`). The caller validates `workspace_id` before opening:
`Registry::call` does it for a capability's input, and the workspace route guard does it for a
page's `/w/<id>/` path.

## Naming

- Database: `lake_records`, reserved in every workspace Zetta.
- Table: the collection name, which is the PostgreSQL table it replaces, in lowercase snake_case of
  at most 48 characters.
- Lookup values: `<field>:<value>` lines in a fixed order. A collection with several lookups writes
  one record per lookup it is read by, or defines a combined value; Zetta has one predicate.
- Capabilities that read a collection take `customer_id` and `workspace_id` (`customer_input_schema`).

## Schema evolution

- A body change that only adds fields keeps `sv` and is handled by `decode`, which defaults the
  missing fields.
- A change that is not additive raises the codec's `schema_version`. `decode` reads every earlier
  version, so old records are never rewritten in place.
- A change to the lookup or the collection name is a new collection, copied with
  `copy_workspace_records` like a table.
- Key rotation introduces `kv = 2` beside `1`. Readers open both versions, and a background rewrite
  retires the first.
- Never reuse a field name with a new meaning, and never put plaintext outside `body`.

## Retention, latency and consistency

- **Retention.** A collection's retention is lake's choice, as it was in PostgreSQL. `ask_messages`
  keeps everything, as before. A purge is a `DELETE … WHERE at < …` in the collection's own job.
  Deleting a workspace deletes its Zetta and its volume, and its records go with them.
- **Latency.** A list is one `GET` and one in-memory scan of the collection's table on the
  workspace node. That is one sequential round within a page's 100 ms budget
  ([performance.md](../ui/performance.md)). The page budget test holds `/ask` and
  `/train/agents/new` with an open conversation at one `sql` call in one round. The scan is linear
  in the table's size, so large collections need the index in the Zetta capability.
- **Consistency.** On a single-node workspace Zetta, a read sees every completed write. On a
  multi-node Zetta, a read on another node can lag by about one gossip interval.
- **Audit volume.** Zetta audits every call into its replicated document (zetta#1154, #1173). A
  record write costs one audited call per part.

## Migration and cutover

Each collection follows the same sequence. There is no dual write, and nothing is dropped until the
copy is verified.

1. **Record owners.** A migration (`0045_ask_message_workspaces` for this one) inserts each row's
   owner into `workspace_assignments`. A space with exactly one live workspace owns its rows. Every
   other row is `needs_selection`, and rows without a space are reported. It adds no column and
   drops nothing.
2. **Switch reads and writes.** The same lake release reads and writes only through
   `WorkspaceRecords` and stops touching the table.
3. **Copy.** The operator runs `copy_workspace_records` (`collection`, `mode`, `after`, `limit`),
   operator bearer only. `plan` reports owners and unresolved rows. `copy` writes the rows that are
   missing; a rerun copies nothing twice. `verify` reads every assigned row back and compares it,
   and answers `complete: true` only when every row on the page has one owner, is present and is
   unchanged. Each run first reapplies step 1's assignment, so it picks up rows an old replica
   wrote during a rolling update.
4. **Resolve.** For each `needs_selection` row, an operator-reviewed change sets
   `workspace_assignments.workspace_id`. The next `copy` moves it.
5. **Drop.** Once `verify` is complete on every page in every environment, a later migration drops
   the table and the store methods that read it.

The [Cutover runbook](#cutover-runbook-ask_messages-dev-cluster) below applies these steps to
`ask_messages` on the dev cluster.

## Tests

| Test | Covers |
|---|---|
| `WorkspaceRecords.*` | Sealed rows and blind indexes, missing table, first-write creation, parts under the request-line limit, incomplete and duplicate parts, rows that do not open in another workspace or lookup, refused KEK and workspace ids, the reserved-name guard |
| `Stores/WorkspaceRecordStores.ConversationsStayInTheirWorkspaceAndSpace/{Memory,Postgres}` | Two spaces with two workspaces each; one conversation id in each, stored only at its own endpoint; foreign space or workspace refused; missing, malformed and unknown `workspace_id` refused at `/workspace_id`; not-ready workspace; workspace deletion |
| `Stores/WorkspaceRecordStores.TheOneShotCopyMovesEachOwnedRowOnceAndVerifiesIt/{Memory,Postgres}` | Plan, verify before copy, copy, idempotent rerun, verify, read back in order with original ids, tamper detection, operator-only |
| `PostgresAskMessages.MigrationRecordsEachRowsOwnerForTheCopy` | Migration 0045 outcomes on rerun, late rows, paged legacy listing |
| `WorkspaceRecordsZetta.TheEngineStoresAndReturnsSealedRecords` | With `LAKE_TEST_ZETTA_URL` and `LAKE_TEST_ZETTA_TOKEN`: the statements against a real Zetta node, including a 20 KB multi-part body |
| `HttpResponseIsolation.AskOrama*`, `AgentRuns.EveryAgentFlowTheSiteOffersIsACapability`, `PagePerformance.*` | The pages and the capability end to end, and the page budgets |

`tests/record_fixture.hpp` (`RecordTables`) implements the subset of `sql` that records use. Every
test workspace caller routes `lake_records` statements to it, one store per workspace endpoint.

## Checklist for each follow-up

1. Classify the table's shape from the [table above](#the-primitive-per-record-shape). If it needs
   conditional writes, stop and link the Zetta capability issue.
2. Write a codec in `include/lake/control/<collection>.hpp`. Give it a lookup that includes every
   field the old query filtered on, including the person when reads are per person.
3. Replace the `Store` methods for the table with `assign_legacy_<collection>` and
   `legacy_<collection>(after, limit)`, in PostgreSQL, memory and the metrics wrapper.
4. Route every caller through `workspace_records(context, customer_id, workspace_id)`: pages use the
   path's workspace, capabilities take `workspace_id`. Agents receive it from their task.
5. Add the next free migration recording owners in `workspace_assignments`. Check that prefixes are
   unique.
6. Add the collection to `copy_workspace_records`.
7. Test: two spaces with two workspaces each; missing, malformed and unknown ids; not ready;
   deletion; the copy in both stores; the migration; the page budgets.
8. Update the inventory row in [workspace-data-plane.md](workspace-data-plane.md), the API doc, the
   schema doc and `db/README.md`. Write the cutover steps in the pull request.
9. After verification on dev, file or ship the migration that drops the table.

## Zetta capability required

The interim does not violate the contract, but it is not the target. Zetta needs the following
before lake moves rows that require conditional writes, or collections large enough that a full
scan breaks the page budget. This is specified here for a Zetta issue, and Zetta is not edited by
lake.

1. **`sql` accepts a JSON body.** Declare `post_alias` on `sql` (as pub/sub reads do) and audit
   DML/DDL statements as actions. This removes the 4 KiB request-line limit and the part protocol
   for new writes.
2. **Per-statement authorization.** UPDATE, DELETE, TRUNCATE and DDL are executed without the
   `sql:write`/`sql:ddl` check that INSERT receives (`src/core/query/executor.cpp`). Enforce it so
   that a reader role cannot modify `lake_records`. Also make the `lake_records` database grantable
   so that only lake's service user can write it.
3. **A keyed record store**, replicated and durable before acknowledgement:
   - `put_record {collection, key, value, if_absent?, if_revision?}` → `{revision}`, or `409 conflict`;
   - `get_record {collection, key}` → `{value, revision}`;
   - `delete_record {collection, key, if_revision?}`;
   - `list_records {collection, index, value, after?, limit, direction}` over a declared secondary
     index with an ordering column;
   - `create_collection {collection, indexes, retention_ms?}`.

   Writes to one key are linearizable, routed to the key's owner. A read after a successful write
   to the same key returns it on every node. Retention is enforced by the engine.
4. **Time series.** Counters and gauges with labels, appended and summed over a time range, for
   metering (#348) and request metrics (#384).
5. **Connections.** `zetta_connect.connections` stores credentials in plaintext and may be readable
   through `sql`. It should seal them, and refuse that table to SQL readers.

## Cutover runbook: `ask_messages`, dev cluster

The cluster is development (`orama-production-cluster-01`). These steps are for the coordinator and
are not run by this change.

1. Have the operator vendor `db/schema.sql` through `0045_ask_message_workspaces` and select
   `ask_message_workspaces_guard`. Apply the schema. Read the warnings for `needs_selection` and
   for rows without a space.
2. Deploy the lake image built from this change. Ask Orama and the Agent Factory now read and write
   only the workspace records. Old conversations are not shown until step 4.
3. Run `POST /v1/copy_workspace_records {"collection":"ask_messages","mode":"plan"}` with the
   operator bearer, and record `unresolved`.
4. Run `{"mode":"copy"}`, following `next_after` until it is null, then `{"mode":"verify"}` over
   every page.
5. For each `needs_selection` row, choose the workspace in a reviewed change to
   `workspace_assignments`, or accept its loss: dev data may be deleted (bridge `CLAUDE.md`). Then
   repeat step 4.
6. Once `verify` answers `complete: true` on every page, file the migration that drops
   `ask_messages`, with the copy output attached.
