# Scoped Connection Credentials

Lake issue [#731](https://github.com/oramasearch/lake/issues/731) gives one app-to-service
connection its own revocable credential scope. It completes Lake's side of decision D4 in the
operator's [secure-networking design](https://github.com/oramasearch/orama-operator/blob/dev/docs/infrastructure/secure-networking.md#credentials-on-connections-decision-d4).
The operator owns native datastore enforcement; Lake owns the customer's desired scope, shows the
workspace-secret reference and requests rotation or removal.

## Scope Chosen at Connection Creation

`create_network_connection` accepts `credential_scope` only when `source_kind` is `app` and
`target_kind` is `service`:

```json
{
  "credential_scope": {
    "access": "read",
    "schema": "analytics"
  }
}
```

`access` is exactly `read` or `write`; `schema` is an optional nonempty name. A null scope declares
network reachability without asking for a credential. Unknown fields and credential bytes are
rejected. The per-connection role and its grants are the operator's work, beginning with Postgres in
[orama-operator#469](https://github.com/oramasearch/orama-operator/issues/469). The service's owner
credential is not part of Lake's desired or observed row.

## What Lake Shows

`list_network_connections` returns the chosen scope and the operator's latest status. Once the
operator has issued the role, `status.credential_secret` is the name of the workspace secret that
holds it. Lake answers the reference, never the credential value. The value stays sealed in the
workspace Zetta and becomes a Kubernetes Secret only as a projection mounted into that connection's
source app.

`generation` is Lake's desired version and `status.observed_generation` is the version the operator
has reconciled. Equality means the shown status describes the current scope. A lower observed value
means issuance, scope application, rotation or removal is still pending.

## Rotation

`rotate_network_connection_credential {customer_id, workspace_id, connection_id}` requires `write`
on the account and a non-null credential scope. It writes the connection's unchanged spec through
the control-plane store, advancing its generation and retaining the prior operator status. The
operator treats the new generation as a rotation request, replaces the role credential and rewrites
the same workspace secret. The app keeps one secret reference and receives the replacement through
that projection.

A credentialless connection answers `conflict`; an ID outside the account and workspace answers
`not_found`. Concurrent removal wins through the desired-row lock: a withdrawn connection cannot be
rotated.

## Removal and Revocation

`remove_network_connection` advances generation and sets `desired: absent`. The operator drops the
per-connection role, revokes the credential and reports `Deleted` at that generation. Lake retains
the desired row and its status as history; `network_rules` excludes it immediately.

The Lake tests prove that a read-only scope reaches the exact operator contract, rotation preserves
that scope while advancing generation, the stable secret reference is shown before and after the
operator observation catches up, credentialless rotation is refused, and removal produces absent
desired state. Native role grants, write refusal and credential invalidation require the operator's
#469 implementation and integration proof because Lake neither connects as nor issues that
datastore role. This Lake proof does not establish those end-to-end acceptance items.

## Change and Verify

The capability is registered once in `src/api/network_capabilities.cpp`; REST, OpenAPI and MCP are
renderings of that registry entry. Scope validity belongs to `src/control/network.cpp`, shared by
the memory and PostgreSQL stores. A behavior change updates this page, `network.md`, the control-plane
contract and the operator issue together.

Build with at most six jobs and run the focused and complete static and shared suites against a
fresh local PostgreSQL database. The exact commands and counts for this change are recorded in
[the proof record](../build/scoped-credentials-proof.md).
