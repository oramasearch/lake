# Authentication

**Today: one operator bearer plus row-resolved user sessions and API tokens.** The operator mints
its bearer once into the Secret
`lake-<name>-admin` (key `token`, 64 hex) and injects it as `LAKE_ADMIN_TOKEN`. lake compares
`Authorization: Bearer <token>` against it in constant time **before consulting user sessions**; a match is the `admin` identity
and does not read or update a session row.

Password, passkey, and OAuth sign-in issue random browser-session secrets. User API tokens are
named sessions with explicit capability scopes. Both bearer values and the HttpOnly
`lake_session` cookie go through `context_for` on every request, so session or device revocation
takes effect on the next request. See [identity.md](identity.md).
Successful browser-session REST capabilities also set that secret as an `HttpOnly`, `SameSite=Lax`
cookie (`Secure` behind HTTPS). This lets the passkey page use the REST capability without exposing
the session secret to its inline script.

- Every capability except the documented identity entry points, public transparency-monitoring
  reads, and `server_info` requires a
  resolved credential, on REST and on MCP `tools/call` alike (`Registry::call` enforces
  `requires_auth`).
- `GET /metrics` uses the same resolution code: either the bearer or the UI's `lake_session`
  cookie. `GET /healthz` and `GET /readyz` are unauthenticated so Kubernetes can probe them.
- Without a token configured, lake logs `"admin_token_set":"no"` in its JSON startup event and every guarded
  capability answers `401 unauthenticated`. There is no anonymous write path.
- A guarded capability called without a credential lake accepts answers **401
  `unauthenticated`** with `reason` `missing`, `invalid` or `expired`, a `help` link to
  `/developer/mcp`, and a `WWW-Authenticate: Bearer` challenge; a valid credential that does not
  grant the call answers **403 `unauthorized`** ([errors.md](errors.md)). `context_for` records why
  a presented bearer or cookie was refused (`ApiContext::credential_rejection`); the stores tell
  `expired` apart only after the secret's hash matches, so guessing a selector reveals nothing.
- `GET /`, `/style.css`, `/v1/openapi.json` and MCP `initialize` / `tools/list` need no token.

Retrieve it for a Lake:

```sh
kubectl get secret lake-<name>-admin -n <namespace> -o jsonpath='{.data.token}' | base64 -d
```

The operator bearer remains a break-glass credential rather than a user identity. TAAP user
identities, scoped sessions, and device revocation are the default user path. Agent workloads use
the short-lived delegated keys in [agents.md](agents.md). Lake also accepts a fresh, one-use
libliquid TAAP-key assertion through `sign_in_taap` and acts as the issuer in an application OIDC
authorization-code flow; see [identity.md](identity.md) and [apps.md](apps.md).

## What the exit scenario proves

The Postgres identity-exit scenario starts with a TAAP-key session, completes a one-use code flow
with exact redirect matching and PKCE S256, and has the relying party validate the returned ID
token's EdDSA signature, issuer, audience, nonce, and `kid` against Lake's public application JWKS.
It also proves certificate issue, rotation, revocation enforcement, and dynamic-secret lease
lifecycle calls all traverse the same authorization and signed audit path.

## Indexed PostgreSQL sessions

PostgreSQL issues opaque `ls1.<lookup UUID>.<secret>` bearers. The UUID is a public selector;
the secret contains 32 bytes from OpenSSL `RAND_bytes`, encoded as 64 lowercase hexadecimal
characters. Clients retain the complete bearer unchanged, including when it is used as the
`lake_session` cookie. Session metadata responses do not expose the secret or its stored digest.

`context_for` rejects malformed versions, lengths and encodings, selects at most one row through
the unique `sessions.lookup_id` index, then compares SHA-256 of the complete bearer with that
row's `secret_hash` using `CRYPTO_memcmp`. The `ls1.` prefix binds the digest to this credential
format. Each request still checks live expiry, device revocation, application-user status and,
for derived tokens, the existing ancestor chain. Work depends on that chain's depth, never on
unrelated active sessions. There is no authentication cache and no legacy scanning fallback.

A database reader learns the public UUID and a one-way digest, neither of which authenticates.
Recovering a valid bearer requires a SHA-256 preimage over a uniformly random 256-bit secret;
there is no human password dictionary to slow down with a password KDF. Password verification
keeps the existing Argon2id policy (64 MiB, three iterations, one lane), as do existing recovery
code/password-secret uses. The memory-only demo store keeps its existing private representation;
the indexed persistence contract here applies to PostgreSQL.

## Existing sessions and deployment

Migration `0022_session_lookup` adds the lookup column and unique index and revokes all existing
rows without a lookup once. Their Argon2 hashes cannot be converted without the original bearer.
Users sign in again; callers recreate API/static tokens from fresh authority. The old rows remain
for session history and revocation evidence. Reapplying the schema leaves newly issued credentials
active. Old-format bearers are always refused, including rows written by an older binary after
migration; no request falls back to scanning them.

Before deploying this binary, the operator must vendor `db/schema.sql` byte-for-byte, select
`session_lookup_guard`, and apply the migration. Coordinate the binary rollout: an older replica
cannot authenticate the new format. The operator copy and deployment belong to the operator's
separate change. See [storage migration](../control-plane/schema.md#session-lookup-migration).

## Verify and change

Run the full PostgreSQL-backed static and shared suites. `PostgresSessionAuthentication.*`
compares 7 and 17 real sessions, measures 7,000 rows, checks actual PostgreSQL tuple counters,
rejects database-derived bearer attempts, and covers password policy, scope, expiry, cookie and
bearer revocation on the next request, operator precedence and repeatable migration. Existing
`TokenAttenuation.*` cases retain ancestor, static-token and application-pool coverage across
REST and MCP. The reproducible measurements are in
[session authentication proof](../build/session-authentication.md).

When changing this format, version the prefix and migration together, preserve the unique lookup
and constant-time digest comparison, and rerun both the scaling and lifecycle regressions.

## Scopes

A token's `scopes` (`create_token`) say what it may call. A scope is one of:

| Scope | Grants |
|---|---|
| a capability name, such as `search_index` or `workspace_query` | that capability, on the objects the person's grants reach |
| `*` | every capability (not allowed on a token that never expires) |
| `registry_pull` | pulling images from the OCI registry with `docker login` |

`GET /v1/openapi.json` lists the capability names (`operationId`). Any other string is refused
with `invalid_input` at `/scopes/<index>` (lake#450): an unknown scope would grant nothing, and
storing it would hide a typo. A token can never hold a scope its issuing credential lacks
(`unauthorized`). Agent-task credentials carry workspace action names the registry does not list
(`create_study`, `compile_intent`); lake issues those itself and `create_token` never does.

