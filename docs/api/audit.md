# Audit API

lake records signed, hash-linked audit events for every capability call, including authorization
and input failures. The audit stream is the source of evidence for authentication, session and
token lifecycle, denied authorization, grant changes, secrets and certificate activity, retention
refusals, and administrative activity. The canonical event contract is in
[`../design/phase-6.md`](../design/phase-6.md#audit-decision-13).

The current admin sign-in records `admin_token` as its method and the HTTP User-Agent as its
device value when one is supplied. Identity-core credential and registered-device events use the
same fields through `audit::record`.

## Capabilities

All audit capabilities except `identity` require the normal lake bearer or session authentication.

| Capability | REST route | What it returns |
|---|---|---|
| `identity` | `GET /v1/identity` | Lake's public TAAP/1 Ed25519 identity document. |
| `rotate_identity` | `POST /v1/rotate_identity` | A new public identity document after a dual-signed rotation. |
| `audit` | `GET /v1/audit` | `{items: [event]}` filtered by `principal`, `object`, `outcome`, `from`, `to`, and `limit`. |
| `export_audit` | `GET /v1/export_audit` | `{jsonl}` containing the matching event objects, one JSON line each. |
| `verify_audit` | `GET /v1/verify_audit` | `{verified: true}` after checking every stored signature, digest, and predecessor link. |
| `set_audit_retention` | `POST /v1/set_audit_retention` | The customer ID and retained days after setting a value of at least 400 days. |

`rotate_identity` is an administrative action and produces one audit event through the registry.
Identity history remains in `lake_identity`, so events signed before a rotation remain verifiable.

## Page

`GET /audit` is the server-rendered audit page. It requires the same sign-in as the other
control-plane pages and exposes the same principal, object, outcome, and time filters as `audit`.
The page links only to registry capabilities and does not offer an operation the API cannot do.

## Identity key custody

`LAKE_IDENTITY_SEED` is an optional base64-encoded, 32-byte Ed25519 seed supplied by the
operator. Lake uses that Secret directly and does not copy it into the control database. With no
configured seed, lake generates an identity at server start. The generated seed is AES-256-GCM
wrapped with the customer-independent `LAKE_INSTANCE_KEK` and stored in `lake_identity`; a seed
is never included in an API response or audit event. A Postgres-backed lake requires
`LAKE_INSTANCE_KEK` before it can create or load a generated identity. The in-memory store uses a
process-local development key because it has no persistent state.

`rotate_identity` creates a generated successor and therefore requires `LAKE_INSTANCE_KEK`, even
when the current identity was operator-supplied.

## Retention

Each customer may increase its retention through `set_audit_retention`; a setting can never be
reduced. 400 days is the enforced floor. A request below that floor or below the customer's
existing setting is refused and produces the one `retention` event for that capability call.
Pruning is intentionally absent: the database append-only trigger rejects update and delete
operations on `audit_events`.

## Verify

```sh
cmake --build build
ctest --test-dir build --output-on-failure
```

For PostgreSQL integration coverage, start Postgres 16, apply `db/schema.sql`, and provide its
URI as `LAKE_TEST_DATABASE`; the `PostgresAudit.RegistryEventsPersistAndVerify` test then runs.
The CI workflow provisions this database automatically.

## Change

Add a new event kind without changing the meaning of an existing field. A canonical payload
change requires a new event version, matching verifier support, and an update to the Phase 6
event contract. New callers must use `audit::record` rather than writing `audit_events` directly.
