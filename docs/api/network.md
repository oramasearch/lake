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
| `rotate_network_connection_credential` | action | Advances a scoped connection's generation so the operator replaces the credential in its workspace secret. |
| `remove_network_connection` | action | Sets one connection to `desired: absent`. |
| `create_network_egress_rule` | action | Declares CIDR or DNS egress for one workload or the workspace. |
| `list_network_egress_rules` | read | Returns present and withdrawn egress history. |
| `grant_network_tunnel` | action | Grants one account member access to one target port until `expires_at`; `quota_exceeded` past the active grant caps ([tunnel-grants.md](tunnel-grants.md#how-many-grants-lake983-lsr-116)). |
| `issue_network_tunnel_certificate` | action | Signs the recipient's CSR for a live grant; returns client certificate, chain, relay endpoint and relay CA. |
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
account and agrees with a workspace route when one is present. Agent Docs puts all fourteen tools in
the deliberate Network group, with `tunnel_client_downloads` beside them.

The tunnel client a grant's recipient installs is offered by `tunnel_client_downloads`, a
separate read any signed-in person may call: signed 15-minute download links per platform
([tunnel-clients.md](tunnel-clients.md), lake#745). It takes no account or workspace and is not one
of the fourteen.

## Authorization and Audit

The account policy object is the authorization boundary. Lists and effective-rule reads require
`read`; connection/egress declarations, credential rotation and removal require `write`; tunnel
grants and revocation require `admin`. Certificate issuance requires account read plus the grant's
recipient identity, never an operator or another admin. A tunnel's `user_id` must
name a persisted user whose TAAP identity can read the same account. This prevents an authorized
admin from granting an unrelated user access. [Tunnel credentials](tunnel-grants.md) describes
CSR validation, bounded certificates, public trust material and certificate audit.

The five `list_*` entries declare that they authorize their candidates, as every list capability
does; their handlers check account `read` before asking the network store for rows. The registry
performs the same account check for the other reads and every action. This also puts the permission
check before the two unavailable workspace-record reads. Denials and failures use the registry's
ordinary outcome audit.
Successful desired-state mutations also retain the signed, atomic store audit described in the
[control-plane contract](../control-plane/network.md#audit), with the same request ID and actor.

## Desired Rows and Operator Status

A connection names a source kind and ID, a target kind and ID, optional ports and an optional
credential scope. Empty ports mean the target's service ports. A non-null scope is valid only from
an app to a service. `credential_scope` is null or
`{"access":"read"|"write","schema":"optional name"}`. It contains no credential bytes; an
operator observation may name the workspace secret in `status.credential_secret`.

`rotate_network_connection_credential` keeps the scope and every endpoint field unchanged and
advances `generation`. The operator reconciles that new generation by replacing the per-connection
role credential and rewriting the same workspace secret. Until then, `status` continues to show
the prior `observed_generation` and secret reference, which makes the pending rotation explicit.
The action refuses a connection whose scope is null. Removing the connection advances generation
again with `desired: absent`; the operator drops the role, revokes its delivered credential and
reports `Deleted`. [Scoped connection credentials](scoped-credentials.md) carries the complete
lifecycle and the operator boundary.

An egress rule has exactly one of `cidr` and `dns_name`. `source_kind` and `source_id` are either
both absent for every workload in the workspace or both present. Tunnel expiry is a future UTC
RFC 3339 instant, and `active` is derived from expiry and revocation rather than an operator phase.
Peer rows are read-only here because the peer creation exchange belongs to operator integration.

Every returned desired row carries `generation`, creator and timestamps. `status` is null until
the operator observes it; afterward it carries phase, message, observed generation and update
time, plus the connection secret name or tunnel endpoint and relay CA where applicable. Grants
also carry the public `client_ca_pem` pinned at creation (`0085_network_tunnel_ca`). History lists retain
withdrawn, revoked and expired rows. `network_rules` filters that history to present connections,
egress rules and peers, plus grants whose expiry is still in the future and which have not been
revoked.

## Sessions and Activity

Decision D1 keeps tunnel sessions and network activity in the workspace Zetta, not in the
`0084_network` tables. The producing and reading interface is tracked by
[zetta#1281](https://github.com/oramasearch/zetta/issues/1281), and the live grant/connect/revoke
flow by [lake#730](https://github.com/oramasearch/lake/issues/730). Until that provider is
available, `list_network_tunnel_sessions` and `network_activity` authorize normally and return
empty `sessions`/`events` arrays with `available: false` and `pending: "zetta#1281"`. Their
scoped provider interface does not invent a second source of network history. See
[the provider contract](tunnel-grants.md#audit-and-session-history).

## Verify

Build with at most six jobs, then run the focused registry and store cases and both complete
native suites against a fresh local PostgreSQL administrative database:

```sh
cmake --build build -j 6
LAKE_TEST_SKIP_STALE_SWEEP=1 LAKE_INSTANCE_KEK="$LAKE_TEST_KEK" \
  LAKE_TEST_DB_URI="$NETWORK_TEST_DB_URI" \
  ctest --test-dir build -R 'NetworkCapabilities|NetworkStore|PostgresNetwork|TunnelCredentials' \
  --output-on-failure
LAKE_TEST_SKIP_STALE_SWEEP=1 LAKE_INSTANCE_KEK="$LAKE_TEST_KEK" \
  LAKE_TEST_DB_URI="$NETWORK_TEST_DB_URI" \
  ctest --test-dir build --output-on-failure
```

Repeat the build and full suite in `build-shared` with `BUILD_SHARED_LIBS=ON`.
`tests/test_network_capabilities.cpp` exercises desired-state and scoped-credential lifecycle,
effective filtering, account permissions, the unavailable-record boundary, OpenAPI operations,
MCP tool schemas and an MCP call. The complete schema-conformance case also makes every available
network capability succeed over MCP so each required output is independently validated. The store
cases continue to prove the PostgreSQL/operator contract.

## Change and Handoff

Add or rename a capability only in the registry, then update this page and every consumer in the
same change. A desired-row change must first update the operator contract and migration coverage
(0085 adds trust without rewriting 0084). A session or activity shape must follow zetta#1281 rather than adding those records to
Lake's control database. Deployment remains a separate observation in
[`design/status.md`](../design/status.md).
