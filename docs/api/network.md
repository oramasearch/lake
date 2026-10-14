# Network Capabilities

Lake issue [#728](https://github.com/oramasearch/lake/issues/728) exposes the secure-networking
control plane through the capability registry. The same definitions produce REST operations in
OpenAPI and MCP tools, so validation, authorization, audit and response shapes do not vary by
transport. The desired and observed rows are defined by
[migration `0084_network`](../control-plane/network.md).

This API exists so a client declares intent to Lake rather than addressing a network provider.
The operator turns that intent into `Connection`, `EgressRule`, `Tunnel` and `PeerConnection`
resources. Provider endpoints, Kubernetes objects and credentials are observations, never API
inputs.

## Capabilities

| Capability | Kind | Result |
|---|---|---|
| `create_network_connection` | action | Declares a source-to-target path and returns its desired row. |
| `list_network_connections` | read | Returns present and withdrawn connection history. |
| `remove_network_connection` | action | Sets one connection to `desired: absent`. |
| `create_network_egress_rule` | action | Declares CIDR or DNS egress for one workload or the workspace. |
| `list_network_egress_rules` | read | Returns present and withdrawn egress history. |
| `grant_network_tunnel` | action | Grants one account member access to one target port until `expires_at`. |
| `list_network_tunnel_grants` | read | Returns active, expired and revoked grant history. |
| `revoke_network_tunnel` | action | Sets `revoked_at` on an active grant. |
| `list_network_tunnel_sessions` | read | Reads current and past sessions once the workspace-record provider is available. |
| `list_network_peers` | read | Returns present and withdrawn peer connections. |
| `network_rules` | read | Returns only rules currently effective from Lake's desired state. |
| `network_activity` | read | Reads recent network decisions once the workspace-record provider is available. |

Reads use `GET /v1/<capability>` and actions use `POST /v1/<capability>`. Every one is also an
MCP tool of the same name and schema. `remove_network_connection` and
`revoke_network_tunnel` carry MCP's destructive hint. All inputs are closed objects and require
`customer_id` and `workspace_id`; the registry also verifies that the workspace belongs to that
account and agrees with a workspace route when one is present. Agent Docs puts all twelve tools in
the deliberate Network group.

## Authorization and Audit

The account policy object is the authorization boundary. Lists and effective-rule reads require
`read`; declarations, removal, grants and revocation require `write`. A tunnel's `user_id` must
name a persisted user whose TAAP identity can read the same account. This prevents an authorized
writer from granting an unrelated user access.

The five `list_*` entries declare that they authorize their candidates, as every list capability
does; their handlers check account `read` before asking the network store for rows. The registry
performs the same account check for the other reads and every action. This also puts the permission
check before the two unavailable workspace-record reads. Denials and failures use the registry's
ordinary outcome audit.
Successful desired-state mutations also retain the signed, atomic store audit described in the
[control-plane contract](../control-plane/network.md#audit), with the same request ID and actor.

## Desired Rows and Operator Status

A connection names a source kind and ID, a target kind and ID, optional ports and an optional
credential scope. Empty ports mean the target's service ports. `credential_scope` is null or
`{"access":"read"|"write","schema":"optional name"}`. It contains no credential bytes; an
operator observation may name the workspace secret in `status.credential_secret`.

An egress rule has exactly one of `cidr` and `dns_name`. `source_kind` and `source_id` are either
both absent for every workload in the workspace or both present. Tunnel expiry is a future UTC
RFC 3339 instant, and `active` is derived from expiry and revocation rather than an operator phase.
Peer rows are read-only here because the peer creation exchange belongs to operator integration.

Every returned desired row carries `generation`, creator and timestamps. `status` is null until
the operator observes it; afterward it carries phase, message, observed generation and update
time, plus the connection secret name or tunnel endpoint where applicable. History lists retain
withdrawn, revoked and expired rows. `network_rules` filters that history to present connections,
egress rules and peers, plus grants whose expiry is still in the future and which have not been
revoked.

## Sessions and Activity

Decision D1 keeps tunnel sessions and network activity in the workspace Zetta, not in the
`0084_network` tables. The producing and reading interface is tracked by
[zetta#1281](https://github.com/oramasearch/zetta/issues/1281), and the live grant/connect/revoke
flow by [lake#730](https://github.com/oramasearch/lake/issues/730). Until that provider is
available, `list_network_tunnel_sessions` and `network_activity` authorize normally and answer
`not_supported`. Registering them now fixes their REST, OpenAPI and MCP contract without inventing
a second source of network history. Their advertised value objects name the eventual `sessions`
and `events` arrays without requiring those fields on a successful response until that provider
exists; their current response is the standard error envelope.

## Verify

Build with at most six jobs, then run the focused registry and store cases and both complete
native suites against a fresh local PostgreSQL administrative database:

```sh
cmake --build build -j 6
LAKE_TEST_SKIP_STALE_SWEEP=1 LAKE_INSTANCE_KEK="$LAKE_TEST_KEK" \
  LAKE_TEST_DB_URI="$NETWORK_TEST_DB_URI" \
  ctest --test-dir build -R 'NetworkCapabilities|NetworkStore|PostgresNetwork' \
  --output-on-failure
LAKE_TEST_SKIP_STALE_SWEEP=1 LAKE_INSTANCE_KEK="$LAKE_TEST_KEK" \
  LAKE_TEST_DB_URI="$NETWORK_TEST_DB_URI" \
  ctest --test-dir build --output-on-failure
```

Repeat the build and full suite in `build-shared` with `BUILD_SHARED_LIBS=ON`.
`tests/test_network_capabilities.cpp` exercises desired-state lifecycle, effective filtering,
account permissions, the unavailable-record boundary, OpenAPI operations, MCP tool schemas and an
MCP call. The complete schema-conformance case also makes every available network capability
succeed over MCP so each required output is independently validated. The store cases continue to
prove the PostgreSQL/operator contract.

## Change and Handoff

Add or rename a capability only in the registry, then update this page and every consumer in the
same change. A desired-row change must first update the operator contract and `0084_network`
coverage. A session or activity shape must follow zetta#1281 rather than adding those records to
Lake's control database. Deployment remains a separate observation in
[`design/status.md`](../design/status.md).
