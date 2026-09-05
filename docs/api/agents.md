# Agent session keys

Lake mints one Ed25519 key for one agent session and delegates that key to a durable TAAP agent
identity. The private seed is returned once by `mint_agent_session_key`; Lake stores only the
public key, delegation, expiry, scopes, and transparency-log commitment. A drip verified through
`verify_agent_session_drip` is attributed to the durable identity carried by the delegation.

This is the custody boundary for liquid's agent sessions. A durable identity describes the agent
across sessions. A session identity limits the useful life of secret key material held by one
running agent. The dispatcher identity records which trusted dispatcher requested the session;
both identities remain visible in the stored record and signed audit trail.

## Capabilities

| Capability | Authorization | Input | Result |
|---|---|---|---|
| `mint_agent_session_key` | `write` on the customer | `customer_id`, durable and dispatcher TAAP identities, non-empty unique `scopes`, optional `lifetime_seconds` | Session UUID and TAAP identity, base64url public key and one-use private seed, expiry, `delegation/1` attestation, log index and root |
| `verify_agent_session_drip` | `read` on the customer | Customer and session UUID, base64url canonical drip body, base64url canonical TAAP signature object | `verified: true`, durable, dispatcher and session identities, scopes, expiry, delegation, and log commitment |

The lifetime defaults to 900 seconds and cannot exceed 900 seconds. The server checks expiry
before signature verification. A signature made with the session key is accepted only when its
TAAP purpose is `drip`, its signing key identifier is the session TAAP identity, and its Ed25519
signature covers the supplied canonical body under the `TAAP/1 drip` domain. Callers apply the
returned scopes to the drip operation they intend to perform.

The input and output binary values use unpadded base64url. The returned private value is the
32-byte Ed25519 seed. It is deliberately absent from later reads, audit details, and the
`agent_sessions` table.

## Delegation and transparency

The session identity fingerprint is derived from the canonical TAAP public-key object with key
label `agent-session`. Lake signs a canonical libliquid `delegation/1` attestation whose actor
subject is that session key and whose attributes are:

| Attribute | Value |
|---|---|
| `principal` | Durable agent TAAP identity |
| `scopes` | Requested session scopes |
| `not_after` | Session expiry as Unix seconds |

Lake verifies the completed attestation with libliquid before returning it. The capability then
appends it to the instance tiled transparency log and persists its entry index and resulting
Merkle root. Persistence happens after append; if persistence fails, the capability returns no
private seed. This can leave a harmless public delegation entry that no caller received a secret
for, but it cannot create an unlogged usable session.

The public `transparency_identity_scan` capability finds the delegation by durable identity.
Clients can therefore bind a session signature to the durable agent and independently verify the
tile and checkpoint chain. Full client-side inclusion and consistency verification is tracked in
[liquid #207](https://github.com/oramasearch/liquid/issues/207).

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
