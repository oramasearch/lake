# Workspace Records

Refs [lake#370](https://github.com/oramasearch/lake/issues/370) and
[lake#311](https://github.com/oramasearch/lake/issues/311). This is the one mechanism lake uses to
keep its records in the workspace Zetta that owns them. Every follow-up that moves a table or store
out of lake's PostgreSQL uses it and applies the [checklist](#checklist-for-each-follow-up) below.
That covers lake#315, #316–#384 and the other "Move … into workspace Zettas" issues.
`ask_messages` is the first collection to move, and `opportunities` (lake#376) the first mutable one.

The rule it implements is in [workspace-data-plane.md](workspace-data-plane.md): everything goes in
a workspace Zetta, including credentials, secrets and metrics. Lake's PostgreSQL and the lake Zetta
keep only multitenancy and content-free dispatch. Billing is the one exception, and its design is
still open.

## Status

| Part | State |
|---|---|
| `WorkspaceRecords`, `RecordSet<Codec>` | Implemented on Zetta's keyed record store ([zetta#1231](https://github.com/oramasearch/zetta/issues/1231), merged in zetta PR #1233 to `dev`, `82f617e`). Unit tests run against an engine fake; an opt-in test runs against a real Zetta node built from that commit. |
| Append-only collections | Implemented. `ask_messages` has moved. |
| Mutable records and claims (`get`, conditional `put`, `remove`, `update`) | Implemented and tested. `opportunities` (lake#376) has moved: derived keys written `if_absent`, status changes conditional on the revision read. |
| `copy_workspace_records` (the operator's one-shot copy and verify) | Implemented for `ask_messages` and `opportunities`. |
| Dropping `ask_messages` and `opportunities` from PostgreSQL | Not part of these changes. A later migration drops each once its copy is verified on the dev cluster. |

**Zetta dependency.** A workspace Zetta must run Zetta `dev` at or after zetta PR #1233
(`82f617e`). An older engine has no record store and answers `put_record` as an unknown
capability. Workspace Zettas run the image in the Lake's `serviceZettaImage`, so this dependency is
part of the [cutover](#cutover-runbook-ask_messages-dev-cluster).

## What Zetta offers

| Primitive | Capabilities | Durability and consistency | Lookup | Conditional write | Fit |
|---|---|---|---|---|---|
| **Keyed record store** (#1231) | `create_collection`, `put_record`, `get_record`, `delete_record`, `list_records` | A write is fsynced on the key's owner and installed on every live peer before the acknowledgement. Writes are linearizable per key. A read after a write returns that write on any live node. Retention is per collection. | `get_record` by key; `list_records` over one value of a declared secondary index, ordered by `order` then key, with paging and both directions | `if_absent`, `if_revision`; `409 conflict` carries `details.revision`; `503 busy` while ownership moves | **All lake records** |
| Labelled time series (#1231) | `create_metric`, `record_metric`, `query_metric` | Replicated | Label filter, group-by, steps | n/a | Metering and request metrics (#348, #349, #384) |
| SQL tables | `sql` (GET, or POST since #1233) | Rows in the replicated catalogue | Full scan, one predicate | None | Workspace data people query; not lake's records |
| Pub/sub topics | `create_topic`, `publish_event`, `browse_topic`, … | Node-local, best effort | By sequence | `update_event` only | Event streams with retention (`agent-events`, `app-events.*`, `build-events.*`) |
| Connections | `create_connection` | Credential sealed at rest since zetta#1230 | By name | None | Not used for lake secrets (lake seals its own) |

The record store's measured latency on 50,000 records is a `list_records` of 50 records at under
1 ms in the registry ([zetta `docs/data/record-store.md`](https://github.com/oramasearch/zetta/blob/dev/docs/data/record-store.md)).
A page that reads one collection therefore pays one workspace round trip.

## The primitive per record shape

| Shape | Examples (follow-up) | How |
|---|---|---|
| Append-only log, read back by key | `ask_messages` (#370), `sso_provisioning_logs` (#333), `auth_app_webhook_events` (#356), `auth_app_webhook_deliveries` (#357), `agent_cron_runs` (#372), `offline_recovery_notifications` (#339), `audit_events` (#317) | `append`: one `put_record` with `if_absent`. `list` is newest first by the lookup index, then put in order. Set `retention_ms` where a purge applies. |
| Mutable rows found by id | `opportunities` (#376, **moved**), `repositories` (#315), `apps` (#377), `stores` (#365), `forge_records` (#341), `platform_environments` (#358) | `get`, then `put` with `if_revision`, or `update`, which retries on `409` by rereading. `remove` with `if_revision`. |
| One-use codes, sessions, replay evidence, claims | `sessions` (#323), `identity_oauth_flows` (#326), `identity_webauthn_challenges` (#327), `identity_taap_assertions` (#361), `auth_app_authorization_codes` (#363), `offline_recovery_attempts` (#336), `agent_task_status` claims (#375) | Consume with `remove(id, if_revision)`. Only one caller succeeds; the others get `conflict` or `not_found`. Expiry is the collection's `retention_ms`. |
| Unique names | repository and app names | A name-keyed record written with `if_absent`, using a blind key (see [Keys](#record-format)). |
| Secrets and credentials | `secrets_*` (#328–#332), `keystore` (#324), `pki_authorities` (#342), `custodial_identities` (#364), `auth_app_signing_keys` (#352), `oauth_providers` (#325) | Records whose sealed body holds the secrets subsystem's existing envelope ciphertext. Zetta's connection store is not used (lake seals its own secrets). |
| Metrics and usage series | `metering_usage` (#348), `metering_build_records` (#349), Prometheus series (#384) | Zetta's labelled time series (`create_metric`/`record_metric`/`query_metric`). The first metering follow-up adds a typed wrapper beside `WorkspaceRecords`. |
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
    task<ZettaControlResult<std::vector<StoredRecord>>> list(RecordCollection, std::string lookup, std::size_t newest = 0);
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
  `next_after` until `newest` records have opened, then returns them oldest first. A missing
  collection lists as empty.
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
| `Stores/WorkspaceRecordStores.TheOneShotCopyMovesEachOwnedRowOnceAndVerifiesIt/{Memory,Postgres}` | Plan, verify before copy, copy, idempotent rerun, verify, read back in order with original ids, tamper detection, operator only |
| `PostgresAskMessages.MigrationRecordsEachRowsOwnerForTheCopy` | Migration 0045 outcomes on a rerun, late rows, paged legacy listing |
| `Stores/WorkspaceRecordStores.OpportunitiesStayInTheWorkspaceTheirScanRanIn/{Memory,Postgres}` | Two spaces with two workspaces each, a scan saved only in its own workspace and only once, newest scan first, dismissal in the owning workspace only, foreign spaces and workspaces, missing, malformed and unknown `workspace_id` for reads and writes, a workspace that is not ready, workspace deletion |
| `WorkspaceRecords.AnOpportunityStatusChangeRereadsAfterAConcurrentWrite` | Derived keys, a second settle writing nothing, a raced conditional status write reread and retried, another space's id never opening |
| `Stores/WorkspaceRecordStores.TheOneShotCopyMovesEachOpportunityToItsScansWorkspace/{Memory,Postgres}` | The scan's workspace owning rows in a space with two workspaces, a deleted workspace's rows reported, idempotent copy, verify accepting a later status change and reporting tampering, operator only |
| `PostgresOpportunities.MigrationRecordsEachRowsOwnerForTheCopy` | Migration 0046 outcomes (`task_workspace`, `sole_workspace`, `needs_selection`, `deleted_workspace`) on a rerun, late rows, paged legacy listing |
| `WorkspaceRecordsZetta.TheEngineStoresAndReturnsSealedRecords` | Runs with `LAKE_TEST_ZETTA_URL` and `LAKE_TEST_ZETTA_TOKEN` against a node built from Zetta dev with #1233: append, list, a 20 KB body, `if_absent` conflict with `details.revision`, `update`, a stale `if_revision`, conditional delete |
| `HttpResponseIsolation.AskOrama*`, `…TheAgentFactory*`, `AgentRuns.EveryAgentFlowTheSiteOffersIsACapability`, `PagePerformance.*` | The pages and the capability end to end, and the page budgets |

`tests/record_fixture.hpp` (`RecordTables`) implements the record store as records use it,
including `409` details and injected `busy`. Every test workspace caller routes record capabilities
to it, one store per workspace endpoint.

## Checklist for each follow-up

1. Pick the shape from the [table above](#the-primitive-per-record-shape). Every shape except
   metrics uses `WorkspaceRecords`; metrics use Zetta's labelled time series.
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

## Zetta follow-ups and limits

- **Fixed in Zetta dev `9798f159` (zetta#1229, zetta#1230):** every data-changing SQL statement now
  checks `sql:write`/`sql:ddl`, and connector credentials are sealed at rest and never returned.
  The connection store is still not used for lake secrets; lake seals its own.
- **Membership-change window.** During a membership change, two owners can briefly both accept a
  conditional write to the same key. Lake's claims are safe only on a fully meshed StatefulSet,
  which is what the operator runs.

## Cutover runbook: `ask_messages`, dev cluster

The cluster is development (`orama-production-cluster-01`). These steps are for the coordinator and
are not run by this change.

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
   `ask_messages`, with the copy output attached.

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
