# Control-plane contracts

[streams.md](streams.md) describes creator ownership bound to a workspace incarnation (kept in
the topic's workspace Zetta since lake#379), server-side shared-bus isolation, migration
`0037_streams` with `streams_guard`, and `0047_stream_owner_workspaces`.

[schema.md](schema.md) describes persisted tables, migrations and the operator handoff, including
active-only repository-name uniqueness, token ancestry and the one-use invalidation of legacy
API/static tokens. See
[postgres.md](postgres.md) for the backing store and deployment contract.
[web-apps.md](web-apps.md) defines the `apps` desired row, protected-app authorization-code state,
operator status fields, notification, quotas, namespaces, Secret-reference boundary and the four
purpose-hash credentials including exact-repository image pulls.
[builds.md](builds.md) defines the `builds` desired row, configured Ship App runtime base, operator
status loop, cancellation, notifications and purpose-hash credential boundary.

Verify changes with the full PostgreSQL-backed static and shared CTest suites. Keep the identity
contract in `docs/api/identity.md` updated with changes to token derivation.

[schema.md](schema.md#session-lookup-migration) documents the indexed session selector, stored digest,
legacy-session invalidation and required operator handoff for `0022_session_lookup`.

[schema.md](schema.md#repository-workspaces-migration) documents `0051_repository_workspaces`, the
immutable workspace of each repository (lake#315).

[schema.md](schema.md#forge-record-workspaces-migration) documents
`0054_forge_record_workspaces`, which retires `forge_records` for the workspace Zettas (lake#341).

[schema.md](schema.md#secret-lease-workspaces-migration-0058_secret_lease_workspaces) documents
`0058_secret_lease_workspaces`, which retires `secrets_dynamic_leases` for the workspace Zettas and
keeps `secret_lease_routes`, the leases' content-free route and due index (lake#362).
[schema.md](schema.md#platform-workspaces-migration-0060_platform_workspaces) documents
`0060_platform_workspaces`, which retires `platform_environments` and `platform_mirror` for the
workspace Zettas (lake#358, lake#359).
[schema.md](schema.md#audit-workspaces-migration-0063_audit_workspaces) documents
`0063_audit_workspaces`, which records the workspace of each central audit event about a
workspace's data for the copy into that workspace's own audit chain (decision 52, lake#317).
[schema.md](schema.md#repository-records-migration-0064_repository_records) documents
`0064_repository_records`, which drops `repository_refs` (lake#319) and records each repository
description's workspace for the copy into its `lake_repositories` record (lake#315 part 2).
[schema.md](schema.md#workspace-deletions-migration-0065_workspace_deletions) documents
`0065_workspace_deletions`, the record of what each cascading workspace deletion destroyed
(decision 54 S12, lake#514). [schema.md](schema.md#unrouted-repositories-migration) documents
`0066_assign_unrouted_repositories`, the reviewed assignment of repositories left without a
workspace (decision 54 S14, lake#513).

[schema.md](schema.md#tenant-image-routes-migration-0068_oci_repository_routes) documents
`0068_oci_repository_routes`, the content-free route from a tenant image repository to the workspace
whose Zetta holds its manifests and tags (lake#383 phase A).
