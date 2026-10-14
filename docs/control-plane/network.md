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
