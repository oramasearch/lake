# Agent session keys

Lake mints one Ed25519 key for one agent session and binds that key to a durable TAAP agent
identity. The private seed is returned once by `mint_agent_session_key`; Lake stores only the
public key, delegation, expiry, scopes, and transparency-log commitment. A drip verified through
`verify_agent_session_drip` is attributed to the durable identity retained with the session.

This is the custody boundary for liquid's agent sessions. A durable identity describes the agent
across sessions. A session identity limits the useful life of secret key material held by one
running agent. The dispatcher identity records which trusted dispatcher requested the session;
both identities remain visible in the stored record and signed audit trail.

## Capabilities

| Capability | Authorization | Input | Result |
|---|---|---|---|
| `mint_agent_session_key` | `write` on the customer | `customer_id`, durable and dispatcher TAAP identities, non-empty unique `scopes`, optional `lifetime_seconds` | Session UUID and TAAP identity, base64url public key and one-use private seed, expiry, `delegation/1` attestation, log index and root |
| `verify_agent_session_drip` | `read` on the customer | Customer and session UUID, base64url canonical drip body, base64url canonical TAAP signature object | `verified: true`, durable, dispatcher and session identities, scopes, expiry, delegation, and log commitment |

The lifetime defaults to 900 seconds and cannot exceed 900 seconds, unless the key never expires (below). The server checks expiry
before signature verification. A signature made with the session key is accepted only when its
TAAP purpose is `drip`, its signing key identifier is the session TAAP identity, and its Ed25519
signature covers the supplied canonical body under the `TAAP/1 drip` domain. Callers apply the
returned scopes to the drip operation they intend to perform.

The session key has no identity document chain: Lake minted it and stored its public half, and that
stored key is the binding. The check therefore uses `liquid::verify_key_signature` against the
stored key, not `liquid::verify`, which since liquid#383 attributes a signature only through an
identity chain and would report an unknown signer. The `delegation/1` attestation is signed by
Lake's instance key, which is not a liquid identity chain either; mint checks its profile schema and
its signature against that key the same way.

The input and output binary values use unpadded base64url. The returned private value is the
32-byte Ed25519 seed. It is deliberately absent from later reads, audit details, and the
`agent_sessions` table.

## Keys that never expire (lake#234)

`mint_agent_session_key` with `never_expires: true` mints a key with no expiry (`expires_at:
"never"`, stored as `'infinity'`). It is bound to its durable and dispatcher identities and
transparency-logged exactly like a short-lived key, and it stays **revocable**:
`revoke_agent_session` (`write` on the customer; input `customer_id`, `agent_session_id`) sets
`revoked_at`, and `verify_agent_session_drip` refuses a revoked key with *agent session key has been
revoked* from then on, expiring or not.

A never-expiring key is allowed only when both switches are on:

| Switch | Where | Default |
|---|---|---|
| The customer's opt-in | `set_customer_credential_policy` (`admin` on the customer): `{customer_id, allow_non_expiring_credentials}`; audit kind `customer.credential_policy` | off |
| The instance | `LAKE_ALLOW_NON_EXPIRING_CREDENTIALS=false` refuses every never-expiring key and token, whatever the customer chose | on |

A refusal names the switch that refused it. The mint's audit details carry `never_expires`.
Long-running agent services use these keys so they do not stop when a 900-second key ages out
(lake#231). Verify: `AgentSessions.NeverExpiringKeysNeedTheOptInAndStayRevocable` and its Postgres
twin.

## Delegation and transparency

The session identity fingerprint is derived from the canonical TAAP public-key object with key
label `agent-session`. Lake signs a canonical libliquid `delegation/1` attestation whose actor
subject is that session key. It uses liquid's current attenuation fields:

| Attribute | Value |
|---|---|
| `scopes` | Requested session scopes |
| `may_publish` | `false`; Lake authorizes each operation from the retained scopes |
| `may_lease` | `false`; Lake authorizes each operation from the retained scopes |
| `depth_remaining` | `0`; the session cannot delegate again |
| `dispatcher` | Dispatcher TAAP identity |

Lake verifies the completed attestation with libliquid before returning it. The capability then
appends it to the instance tiled transparency log and persists its entry index and resulting
Merkle root. Persistence happens after append; if persistence fails, the capability returns no
private seed. This can leave a harmless public delegation entry that no caller received a secret
for, but it cannot create an unlogged usable session.

The public `transparency_identity_scan` capability finds the delegation by dispatcher identity.
Clients combine that public statement with the durable-to-session binding retained in the session
record and signed audit trail. The pinned liquid verifier checks tiles and compact inclusion and
consistency proofs.

## Storage and audit

Migration `0017_agent_sessions` adds `agent_sessions`, keyed by a caller-visible UUID and with a
unique session TAAP identity. Rows belong to a customer and are removed with it. They retain the
public verification material after expiry so historical attribution remains possible; live drip
verification still rejects an expired key.

Both capabilities go through the registry's customer object resolver and `authorize`. Every
success, invalid signature, expired key, denial, and storage failure is recorded through
`audit::record` as `agent_session`. Audit details name identities and the transparency index but
never contain the private seed. The audit API can retrieve a session's complete event history with
the object filter `agent_session/<session UUID>`.

## Verify and change

`AgentSessions.MintsAttributesUsesAndExpiresSessionKey` covers minting, canonical delegation,
transparency scanning, a valid signed drip, a modified body, expiry, and the signed audit chain.
`PostgresAgentSessions.PersistsLoggedSessionAndDurableAttribution` runs the same flow through the
production Postgres store. Keep the private seed response-only and preserve the append-before-
persistence order when changing this contract.
