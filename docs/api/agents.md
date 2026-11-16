# Agent session keys

Lake mints one Ed25519 key for one agent session and binds that key to a durable TAAP agent
identity and one repository. The private seed is returned once by `mint_agent_session_key`; Lake stores only the
public key, delegation, expiry, scopes, repository, and transparency-log commitment. A drip verified through
`verify_agent_session_drip` is attributed to the durable identity retained with the session.

This is the custody boundary for liquid's agent sessions. A durable identity describes the agent
across sessions. A session identity limits the useful life of secret key material held by one
running agent. The dispatcher identity records who asked for the session (`dispatcher_taap_id`);
it is kept in the stored record and the signed audit trail. Lake itself is the delegation's issuer
and its `dispatcher` (lake#1346, below).

`dispatcher_taap_id` must exactly equal the authenticated caller's acting TAAP identity.
The signing service refuses any other dispatcher, including operator calls whose acting identity
is `admin`. Customer write access alone does not authorize attribution to another dispatcher.

A customer administrator registers the durable identity through `issue_agent_certificate`.
Minting requires an unrevoked, unexpired `agent` certificate whose customer and subject match
`customer_id` and `durable_agent_taap_id`. A certificate belonging to another customer or another
profile does not qualify. Existing agents without such a certificate cannot receive new session
keys until an administrator issues one.

## Capabilities

| Capability | Authorization | Input | Result |
|---|---|---|---|
| `mint_agent_session_key` | `write` on the customer | `customer_id`, durable and dispatcher TAAP identities, `repository_id`, non-empty unique `scopes`, optional `lifetime_seconds` | Session UUID and TAAP identity, base64url public key and one-use private seed, expiry, `repository_id`, `delegation/1` attestation, log index and root |
| `verify_agent_session_drip` | `read` on the customer | Customer and session UUID, base64url canonical drip body, base64url canonical TAAP signature object | `verified: true`, durable, dispatcher and session identities, scopes, expiry, repository, delegation, and log commitment |

`repository_id` is the lake UUID of the repository the session acts for. It must be a live
repository of `customer_id`; any other value, a deleted repository or another account's is refused
`invalid_input` at `/repository_id` before anything is signed or logged.

The lifetime defaults to 900 seconds and cannot exceed 900 seconds. Every key expires: `never_expires:
true` is refused `invalid_input` at `/never_expires` (below). The server checks expiry
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

## Keys that never expire are refused (lake#1346)

lake#234 offered agent session keys with no expiry behind the customer's
`allow_non_expiring_credentials` opt-in and `LAKE_ALLOW_NON_EXPIRING_CREDENTIALS`. liquid v0.3.0
refuses a `delegation/1` without `not_after` or valid for more than 30 days (liquid#514), so a key
that never expires cannot carry a delegation liquid accepts. `mint_agent_session_key` now refuses
`never_expires: true` with `invalid_input` whatever the two switches say; they still govern
never-expiring API tokens ([identity](identity.md)). A stored key with `expires_at: "never"` no
longer verifies. `revoke_agent_session` (`write` on the customer; input `customer_id`,
`agent_session_id`) still ends any key before its expiry: `verify_agent_session_drip` refuses a
revoked key with *agent session key has been revoked*. Verify:
`AgentSessions.NeverExpiringKeysNeedTheOptInAndStayRevocable` and its Postgres twin, which now
expect the refusal.

## Delegation and transparency

The session identity fingerprint is derived from the canonical TAAP public-key object with key
label `agent-session`. Lake signs a canonical libliquid `delegation/1` attestation in the shape
liquid v0.3.0 requires (liquid#514, liquid#515; lake#1346). Lake's instance identity is its issuer:

| Field | Value |
|---|---|
| subject (actor) | `liquid::delegation_subject` of the durable agent: the BLAKE3 of its TAAP id |
| `scopes` | Requested session scopes |
| `may_publish` | `false`; Lake authorizes each operation from the retained scopes |
| `may_lease` | `false`; Lake authorizes each operation from the retained scopes |
| `depth_remaining` | `0`; the session cannot delegate again |
| `dispatcher` | Lake's instance TAAP identity, the issuer (liquid requires the two to be equal) |
| `not_after` | The key's expiry in Unix seconds, at most 900 seconds after issue (liquid's cap is 30 days) |
| `repository` | The hex id liquid gave `repository_id`: `repositories.liquid_id`, else the repository store's own id |

The person who asked is not in the statement: `dispatcher_taap_id`, the authenticated caller's
acting identity, is kept in the session row and in the mint's audit details with `repository_id`.
A repository created empty takes the identity of its first push, so a key minted before that push
names the store's id at the time.

Lake verifies the completed attestation with libliquid (`validate_delegation`,
`check_delegation_window` and the instance key's signature) before returning it. The capability then
appends it to the instance tiled transparency log and persists its entry index and resulting
Merkle root. Persistence happens after append; if persistence fails, the capability returns no
private seed. This can leave a harmless public delegation entry that no caller received a secret
for, but it cannot create an unlogged usable session.

Dispatcher and durable-agent registration checks run in `control::agents::Service::mint` before
key generation or signing. The capability supplies the acting identity from its authenticated
context, independently of the JSON request. Registration is checked at issuance against the
customer's certificate records; revoking that certificate prevents subsequent mints. Existing
session keys keep their own expiry and revocation lifecycle.

The public `transparency_identity_scan` capability finds the delegation by Lake's instance
identity, its issuer and dispatcher.
Clients combine that public statement with the durable-to-session binding retained in the session
record and signed audit trail. The pinned liquid verifier checks tiles and compact inclusion and
consistency proofs.

## Keys minted before lake#1346

A key minted before this change has a delegation liquid v0.3.0 refuses (no `not_after`, no
`repository`, the human as `dispatcher`, the session key as subject) and a row with no
`repository_id`. `verify_agent_session_drip` refuses it, `unauthorized` with *agent session key
predates repository-bound delegations; mint a new one*, as it does a stored never-expiring key.
Agents mint a new key for their repository; nothing is migrated.

## Storage and audit

Migration `0017_agent_sessions` adds `agent_sessions`, keyed by a caller-visible UUID and with a
unique session TAAP identity. Migration `0113_agent_session_repositories` (lake#1346) adds the
nullable `repository_id` (set null if the repository row goes); the operator must vendor the schema
and select `agent_session_repositories_guard` before an image with this change rolls. Rows belong to a customer and are removed with it. They retain the
public verification material after expiry so historical attribution remains possible; live drip
verification still rejects an expired key.

Both capabilities go through the registry's customer object resolver and `authorize`. Every
success, invalid signature, expired key, denial, and storage failure is recorded through
`audit::record` as `agent_session`. Audit details name identities and the transparency index but
never contain the private seed. The audit API can retrieve a session's complete event history with
the object filter `agent_session/<session UUID>`.

## Verify and change

`AgentSessions.MintsAttributesUsesAndExpiresSessionKey` covers minting, the delegation's issuer,
dispatcher, subject, repository and `not_after` checked with liquid's own validators,
transparency scanning, a valid signed drip, a modified body, expiry, and the signed audit chain.
`PostgresAgentSessions.PersistsLoggedSessionAndDurableAttribution` runs the same flow through the
production Postgres store. Keep the private seed response-only and preserve the append-before-
persistence order when changing this contract.

`AgentSessions.RefusesUnprovenDispatcherAndUnregisteredDurableAgent` and its Postgres twin refuse
another dispatcher's identity, operator attribution, missing or foreign registration, a non-agent
certificate, revocation and expiry at the issuance instant, and another account's, an unknown, a
malformed and a missing `repository_id`. They assert that refusals return no
private key and append no delegation, exercise direct service calls, unbounded lifetimes and a
missing repository id, and
allow minting after a new active certificate registers the identity. Run them with
`ctest --test-dir build --timeout 600 -R 'AgentSessions' --output-on-failure`, with
`LAKE_TEST_DB_URI` set for the Postgres cases. Keep the scoped lookup and both identity checks
inside the signing service when changing the mint entry points.
