# Network Control Plane

Lake issue [#727](https://github.com/oramasearch/lake/issues/727) implements the desired state in
section 11 of the operator's [secure-networking contract](https://github.com/oramasearch/orama-operator/blob/dev/docs/infrastructure/secure-networking.md#11-the-contract-between-lake-and-the-operator).
Migration `0084_network` creates the eight contract tables and `network_guard`, column for column.
This is source-tree support; operator adoption and deployment are separate.

## Ownership and Rollout

Lake writes `network_connections`, `network_egress_rules`, `network_tunnel_grants` and
`network_peers`. The operator reads them only after `network_guard` exists and writes only
`network_connection_status`, `network_egress_rule_status`, `network_tunnel_grant_status` and
`network_peer_status`. Lake's PostgreSQL store exposes no status writer. The memory store has
explicit status setters for tests and demos.

On first application, the migration creates a connection from every existing nondeleted app to
every present service in its workspace, including the primary Zetta. Stopped apps and workspaces
are included because they can resume. Deleted apps/workspaces and absent services are excluded.
The ports are empty (the target's service ports) and the credential scope is NULL, preserving the
existing path without issuing new credentials. The migration creates no egress rules:
`internetEgress` exists only in the Lake CR's `spec.workspaces.overrides`, and the operator retains
its existing public TCP 443 allowance as an implicit rule.

The backfill is conditional on the absence of `0084_network` in `schema_migrations`. Reapplying
the idempotent schema never restores a withdrawn connection or creates blanket paths for apps
added after rollout. The guard and migration marker follow the backfill in the schema transaction.
Migration-created rows have NULL `created_by`; they represent compatibility state, not a person.

Every insert, update or delete of a desired table emits `NOTIFY lake_network, '<row UUID>'` in
the same transaction. Rollbacks emit nothing. Status writes emit nothing. Polling remains the
operator's fallback if a listener misses a notification. Customer/workspace/creation/id indexes
support scoped ordered reads.

## Store Contract

`include/lake/control/network.hpp` defines the four typed records and their mutable specs.
`Store`, `InMemoryStore` and the telemetry wrapper expose:

| Record | Operations |
|---|---|
| Connection | `create_network_connection`, `get_network_connection`, `list_network_connections`, `update_network_connection`, `withdraw_network_connection` |
| Egress rule | `create_network_egress_rule`, `get_network_egress_rule`, `list_network_egress_rules`, `update_network_egress_rule`, `withdraw_network_egress_rule` |
| Tunnel grant | `create_network_tunnel_grant`, `get_network_tunnel_grant`, `list_network_tunnel_grants`, `update_network_tunnel_grant`, `revoke_network_tunnel_grant` |
| Peer | `create_network_peer`, `get_network_peer`, `list_network_peers`, `update_network_peer`, `withdraw_network_peer` |

Every operation takes a `NetworkScope` containing both customer and workspace IDs. A read or
mutation of an ID from another scope returns `not_found`. Lists include history, ordered by
`created_at, id`, and return an empty list for a scope with no records. The optional `status`
distinguishes no operator observation from an observed empty phase. Status stays intact across
desired updates, so a caller can compare `observed_generation` with `generation`.

Creates begin at generation 1. Updates replace the entire mutable spec and advance generation;
the ID, owner, creator and creation time stay fixed. Withdrawals set `desired = 'absent'`.
Revocations set `revoked_at`; tunnel grants have no desired column. Both advance generation and
update the timestamp. An ended record cannot be updated or ended again (`conflict`); an expired
grant cannot be renewed or used as a new connection source. Its last operator status can lag its
expiry, so consumers must also examine `expires_at` and `revoked_at`.

Writes require a live workspace of the named customer. Reads of a deleted workspace's retained
rows remain available. Targets must be live apps/services in that workspace. Sources can also name
a live agent launch (`agent_launches.id`, the UUID of the workload) or an unexpired, unrevoked
tunnel grant. An egress source is either an app, service or agent launch, or both source fields
are absent for workspace-wide scope.

Validation rejects invalid UUIDs, foreign or missing workloads, ports outside 1–65535, empty
egress port lists, a CIDR with host bits, both/neither CIDR and DNS, and partially specified
sources. Credential scopes are NULL or `{"access":"read"|"write","schema":"optional name"}`;
credential bytes are never accepted as scope fields. Peer exports name local app/service targets
and ports; imports name remote services and ports. Tunnel expiration input is UTC RFC 3339 with
`Z`, optionally up to six fractional digits, and must be in the future. The grant user must exist.

The store enforces ownership and input validity. Authorization is the caller's responsibility,
as with other control-store operations; REST/MCP policy checks belong to lake#728.

## Audit

Every successful store mutation requires `NetworkActor`: the principal, request ID, and, for a
person, their persisted user ID. A person's principal must match that user's TAAP ID. System or
operator callers leave `user_id` empty and supply their principal and request ID.

The store uses Lake's existing identity, signing and central `audit_events` chain. The event
identifies the customer, workspace, actor, row, operation and new generation. `created_by` keeps
the initial user even when another person updates or withdraws the row. The operation names are
the store method names, with objects such as `network_connection/<UUID>`.
Successful mutations use the existing audit outcome `success`. Failed store mutations leave no
desired change or success event; the capability layer records `failure` or `denied` for rejected
requests through its ordinary outcome audit.

PostgreSQL locks the desired row for an update and the existing audit-chain advisory lock for its
append. Signing, audit insertion and the desired mutation succeed in one transaction; a signing
or audit-insert failure rolls the desired state and its notification back. Concurrent writers
retain every generation and a verifiable audit chain. Memory applies both changes under one lock.
The telemetry wrapper forwards the actor and scope and counts each operation normally.

The future capability layer should use these already-audited writes and account for its ordinary
registry outcome event. Workspace network activity and tunnel sessions remain workspace-Zetta
records in zetta#1281; they are not stored in these tables.

## Verify

Use the native configurations and local PostgreSQL fixture described in
[local build](../build/local.md) and [test isolation](../build/testing.md). Builds use at most six
jobs and suites run one at a time:

```sh
cmake --build build -j 6
LAKE_TEST_SKIP_STALE_SWEEP=1 LAKE_TEST_DB_URI="$NETWORK_TEST_DB_URI" \
  ctest --test-dir build -R 'NetworkStore|PostgresNetwork' --output-on-failure
LAKE_TEST_SKIP_STALE_SWEEP=1 LAKE_TEST_DB_URI="$NETWORK_TEST_DB_URI" \
  ctest --test-dir build --output-on-failure
cmake --build build-shared -j 6
LAKE_TEST_SKIP_STALE_SWEEP=1 LAKE_TEST_DB_URI="$NETWORK_TEST_DB_URI" \
  ctest --test-dir build-shared --output-on-failure -j 6
```

Set `LAKE_INSTANCE_KEK` for the full suite and point `NETWORK_TEST_DB_URI` at a freshly created
local administrative database. The fixture creates an isolated database for each PostgreSQL
case; the skip-sweep flag leaves earlier runs' databases alone.

`tests/test_network.cpp` runs the same CRUD, scope, validation, status and signed-audit cases
against both stores through the telemetry wrapper. PostgreSQL cases also check all 75 columns'
names/types/nullability, foreign keys and default values, invalid table constraints, backfill
selection and reruns, notifications versus observations/rollback, audit failure atomicity,
expiration and concurrent writers.

### Native Proof — 2026-10-14

Both native builds completed with Clang 22.1.8, `BUILD_SHARED_LIBS=OFF` in `build` and `ON` in
`build-shared`, using the authorized Zetta checkout and sibling Liquid checkout. The commands
above used a fresh local PostgreSQL administrative database, `LAKE_INSTANCE_KEK`, and disabled
the stale-database sweep. No container was used.

```text
ctest --test-dir build -R 'NetworkStore|PostgresNetwork' --output-on-failure
100% tests passed, 0 tests failed out of 20

ctest --test-dir build --output-on-failure
100% tests passed, 0 tests failed out of 1042

ctest --test-dir build-shared --output-on-failure -j 6
100% tests passed, 0 tests failed out of 1042
```

Each full configuration registers 1,043 tests: 1,023 passed, 19 skipped and one disabled.
The focused run passed all 20 cases, including 13 PostgreSQL cases; no network test skipped.
The 19 skips are eleven opt-in integration cases (Liquid CLI, Python Hugging Face client, MCP
runner and real Zetta) and eight PostgreSQL variants whose existing fixtures explicitly require
memory-only operator status setters. `SearchBenchmark.BuildAndQueryTenThousandFiles` remains
disabled by the repository. Neither full run failed or required an isolated rerun.

The built binary printed `lake 0.1.0`; a memory-store process answered `/v1/server_info` and
`/readyz` with HTTP 200, then shut down cleanly. Formatting and `git diff --check` passed.
Linux proof deferred: the work order's `linux-proof.sh` helper is absent on this host.

## Change and Handoff

Any column change must first agree with section 11 of the operator contract. Keep this schema
the source of truth, reserve a unique migration number against both the integration branch and
open PRs, update the contract test and this page, and have the operator vendor the resulting
schema and select the guard after Lake merges. Do not run migrations in Lake pods.

Deferred by scope: REST/MCP capabilities (#728), Network UI (#729), developer tunnel grant flow
(#730), credential issuance (#731), workspace records (zetta#1281), and operator reconciliation
(operator#468, #470, #471, #472). Native tests prove the persistence and operator SQL contract;
they do not claim deployed network enforcement.
