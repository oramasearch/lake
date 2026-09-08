# Identity API

Account restoration beyond MFA sign-in is specified in [`recovery.md`](recovery.md). Recovery
restores sign-in credentials and sessions; it does not restore end-to-end encrypted repository
keys. Offline device grants are specified separately in [`offline.md`](offline.md).

## What

Identity capabilities live in the single registry and are therefore available as REST, OpenAPI,
and MCP tools. `sign_up_password`, `sign_in_password`, `verify_mfa`,
`begin_passkey_sign_in`, `sign_in_passkey`, `start_oauth`, and `sign_in_taap` are unauthenticated
entry points.
`enroll_mfa`, `disable_mfa`, `begin_passkey_registration`, `register_passkey`,
`create_token`, `list_sessions`, `revoke_session`, `list_devices`, and `revoke_device` require
the same row-resolved session or operator credential as every other protected capability.
The session and device lists authorize the authenticated principal through the registry's batched
authorization path before returning any row.

`sign_up_password` and `sign_in_password` issue browser sessions. Passwords use OpenSSL 3.6.3
`EVP_KDF` `ARGON2ID` with 64 MiB memory, three iterations, one lane, and a unique 128-bit salt.
A user with an enrolled TOTP factor receives `mfa_required` from password sign-in and must use
`verify_mfa` with a current TOTP or an unused recovery code. `enroll_mfa` returns an `otpauth` URI
and eight recovery codes once; replacing the enrollment invalidates the previous seed and codes.
Recovery codes are single-use. `disable_mfa` requires a fresh TOTP or unused recovery code and
revokes the seed and every remaining recovery code. Tokens are named sessions with explicit
capability-name scopes. Ordinary tokens
default to a 30-day expiry; a non-expiring static token requires a non-empty reason.

Every HTTP response starts with empty headers before CORS or a route handler runs. A session
cookie belongs only to the request that successfully completed its authentication ceremony;
ordinary anonymous GETs of `/`, `/sign-up`, and `/sign-in` issue no `lake_session` cookie and
inherit no `Location` header. This holds on keep-alive connections and when a worker accepts a
new connection. Separate clients retain separate cookie jars: signing in one cannot authenticate
another visiting public pages. The shared construction rule is in
[`../serving/http.md`](../serving/http.md#response-isolation).

TOTP seeds are never stored in credential JSON as plaintext. OpenSSL AES-256-GCM seals each value
with a purpose-specific random data key; `keystore` stores that key wrapped under the required
32-byte hexadecimal `LAKE_INSTANCE_KEK`. Recovery codes use independent Argon2id hashes and are
revoked as they are consumed. OAuth client secrets use the same envelope format.

The server passes its configured instance key to the Postgres store when it is created. The store
uses that fixed configuration for every encrypted credential operation and never re-reads mutable
process environment state after startup.

`register_oauth_provider` records the issuer, client id, encrypted client secret, and requested
scopes for a provider in a user pool. `start_oauth` creates a five-minute, one-use state record,
stores its RFC 7636 verifier encrypted, and returns the provider authorization URL with an S256
challenge. The callback is `GET /v1/oauth/<provider>/callback`, as OAuth providers require a
browser redirect rather than a JSON capability call.

For a first OAuth sign-in, `start_oauth` also accepts the client-held TAAP/1 identity document and
carries it through the short-lived flow. The Postgres store verifies and records that document
before creating the user and OAuth credential. A returning linked identity does not need to send
the document again.

The callback consumes state before exchanging the code. OpenID Connect id tokens must use RS256
and pass signature, issuer, audience, expiry, not-before, nonce, subject, and key-id checks. JWKS
documents are fetched through Corosio with TLS peer and hostname verification, cached for one hour,
and refreshed once when verification fails so key rotation is picked up. GitHub instead verifies
the returned access token through `https://api.github.com/user`. Google and GitHub use their fixed
public endpoints; another registered provider uses `<issuer>/authorize`, `<issuer>/token`, and
`<issuer>/jwks`. Plain HTTP provider URLs are rejected except on loopback for integration tests.
Provider registration and each OAuth callback create one signed registry-compatible audit event.

Passkeys use a two-capability WebAuthn ceremony. `begin_passkey_registration` binds a random
challenge to the authenticated user and pool, then `register_passkey` accepts only attestation
format `none`, verifies `clientDataJSON` type, challenge and origin, verifies the RP ID hash and
user-presence flag in authenticator data, and stores the ES256 public key and initial sign count.
Registration also creates a named device and a TAAP/1 `passkey_binding/1` attestation signed by
lake's current instance identity. The attestation binds the credential ID and pool to the user's
TAAP identity. It is stored with the credential and returned by `list_devices` together with the
credential ID. Verification resolves the signing identity from `lake_identity`, so bindings made
before an instance-identity rotation remain verifiable. Revoking the linked device also revokes
the passkey credential before the next assertion.
`begin_passkey_sign_in` creates an assertion challenge without revealing whether a credential
exists. `sign_in_passkey` verifies the same pool-specific origin and RP ID, the ES256 assertion
over authenticator data plus the client-data hash, and advancement of a nonzero sign count before
issuing a session. Registration and assertion challenges persist for five minutes and are
consumed on their first completion attempt.

`LAKE_PUBLIC_HOST` is the RP ID. The current `lake` pool allows
`https://<LAKE_PUBLIC_HOST>`; each persisted challenge records that origin together with its pool,
so an assertion cannot cross pool or origin. Additional pool-origin configuration belongs to the
user-pools work in #51.

Password sign-up requires the client-held TAAP/1 document as a base64url-encoded canonical-CBOR
`identity_document`. lake decodes and verifies its self-signature through libliquid's public
`decode_identity_document`, `encode_identity_document`, and `verify_document` APIs, hashes the
complete canonical record with SHA-512/256, writes `identity_documents`, and binds the resulting
`taap_id` to the new user. lake never receives a person's private key.

`sign_in_taap` lets that registered user return without a password. The client signs
`pool_id + "\n" + nonce + "\n" + issued_at` with the key in its liquid identity document. Lake
verifies the canonical document and `lake_sign_in` signature through libliquid, accepts a five
minute freshness window, and records the assertion digest before issuing a one-day session.
Assertion digests are one-use, so replay fails while the timestamp remains fresh.

## What the exit scenario proves

`IdentityExit.PostgresProvesIdentityPkiAndDynamicSecrets` creates a real identity with the pinned
libliquid, registers its public document, signs a fresh challenge with the private TAAP key, signs
in to the lake pool, resolves the resulting session, and rejects reuse of the same assertion. The
same resolved user then authorizes a relying party, proving that TAAP sign-in feeds the ordinary
authorization and application-identity paths without giving Lake the private key.

## Why

The random bearer secret is not the authority. Every request resolves its session and device row;
revoking either one therefore takes effect on the next request. Browser sessions receive `*` for
the user's own control-plane capabilities. API tokens receive only the scopes requested at issue.
OAuth proves control of the provider account, while the verified TAAP document remains the user's
durable identity.

## How to verify

```sh
cmake --build build
ctest --test-dir build --output-on-failure -R 'Identity|OAuth|WebAuthn'
```

The `Identity.PasswordSessionsTokensAndRevocation` test covers Argon2id password sign-up, MFA
enrollment, recovery-code sign-in and one-use enforcement, fresh-verification disablement, session
resolution, narrowed token scope, static-token reason validation, and next-request revocation.
`OAuth.AuthorizationCodePkceOidcAndCachedJwks` runs the browser redirects against a stub OpenID
Connect provider, verifies PKCE at its token endpoint, signs an id token, serves JWKS, checks the
session cookie, proves cache reuse, and rejects state replay.
The Postgres identity test repeats the MFA recovery and disable flow, reads back the encrypted
provider secret, and proves that persisted OAuth state returns its verifier once and only once.
`WebAuthn.PersistedRegistrationAssertionAndSignCount` uses a software ES256 authenticator to
produce `clientDataJSON`, authenticator data, a CBOR attestation object and signed assertions. It
proves origin binding, first-attempt challenge consumption, lake-signed TAAP binding verification
before and after identity rotation, session issue and sign-count replay rejection. The Postgres
identity test repeats a successful registration, binding verification and assertion against the
persisted challenge and credential rows.
The identity-exit Postgres scenario covers fresh liquid-key sign-in, persisted replay protection,
and use of that session in the relying-party authorization-code flow.

`HttpServer/HttpResponseIsolation.SignInCannotAuthenticateAnotherClient` runs with both stores
and both reused and fresh connections. It signs in A, visits each public page without credentials
as B, also checks `/v1/server_info` and `/v1/openapi.json`, adopts any received cookie as a browser
would, and requires B's `/sessions` request to
redirect to `/sign-in`. A's own cookie must still open its sessions page. Assertions never print
credential values. Run the complete Postgres-backed suite, including these four cases, with
`LAKE_TEST_DB_URI` and `LAKE_INSTANCE_KEK` configured as in [`../build/local.md`](../build/local.md).

## How to change

Add a capability through `src/api/capabilities.cpp`, with its input and output schema. Extend
`Store` and both store implementations together. Keep new credential rows pool-scoped and add the
capability to `docs/api/README.md` in the same change. The OAuth callback is the documented
exception because the provider invokes it as a browser redirect.

OAuth registration, PKCE callback processing, JWKS verification and WebAuthn ceremonies use the
`0007_identity_core` tables. Keep the passkey browser adapter limited to the one documented inline
script that calls `navigator.credentials`.
