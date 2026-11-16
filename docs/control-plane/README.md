# Control-plane contracts

[network.md](network.md) defines migration `0084_network`, secure-networking desired and status
rows, generation and revocation rules, scoped stores, atomic audit, notifications, rollout and the
public capability handoff in [the API contract](../api/network.md).
It also documents migration `0085_network_tunnel_ca`, the Lake/operator-owned public trust fields
used by [tunnel credentials](../api/tunnel-grants.md), leaving 0084 unchanged.

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
[schema.md](schema.md#app-transfer-migration) documents `0081_app_transfer`, each app's ingress
bytes by hour, written by the operator and charged once as `transfer` (lake#661).
[services.md](services.md) defines the managed-services catalog, what the Orama operator writes per
kind and each kind's `<slug>.json` credential. TiDB's carries `database` and Cassandra's
`keyspace`, both `app`, and both name a tenant user rather than the store's administrator
(orama-operator#771); the fields are additive.
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

[schema.md](schema.md#workspace-volumes-migration-0069_workspace_volumes) documents
`0069_workspace_volumes`: each workspace Zetta's desired volume size, which never decreases, and the
content-free volume snapshot policies and status the operator reads and writes (operator#259,
operator#260).

[schema.md](schema.md) also describes `0094_clerk_revocation` (lake#948): ordered Clerk
suspension and a persistent account-wide revocation watermark, with `clerk_revocation_guard`; and
`0103_clerk_session_binding` (lake#1163): each Clerk browser session's Clerk session id, so a
session event ends only that browser, with `clerk_session_binding_guard`; and `0104_app_restarts`
(lake#1228): the generation an app's last restart wrote, which the operator puts on the pod template
([web apps](web-apps.md#restart)), with `app_restarts_guard`; and
`0105_agent_model_prices` (lake#1230): the gpt-6.x and long-context model token rates, with
`agent_model_prices_guard`; `0107_order_email_confirmation` (lake#1000): when an order form's
contact email was confirmed, with `order_email_confirmation_guard`; `0108_retained_usage`
(lake#988): a deleted workspace's unbilled usage, kept as quantities so its bill still counts it,
with `retained_usage_guard`; and `0109_principal_ids` (lake#872): the database checks every user's
and organization's `taap_id` is a TAAP id, with `principal_ids_guard`.
`agent_model_prices_guard`; and `0111_service_workspace_routing` (lake#1301): the triggers that
refuse a service routed to another customer's workspace, with `service_workspace_routing_guard`
([services](services.md#workspace-routing)).
[Schema](schema.md#authentication-attempts-migration-0098_authentication_attempts) describes shared password/MFA attempt budgets (lake#966).
