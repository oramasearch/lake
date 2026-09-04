# Identity API

## What

Identity capabilities live in the single registry and are therefore available as REST, OpenAPI,
and MCP tools. `sign_up_password`, `sign_in_password`, `verify_totp`,
`begin_passkey_sign_in`, `sign_in_passkey`, and `start_oauth` are unauthenticated entry points.
`enroll_totp`, `begin_passkey_registration`, `register_passkey`,
`create_token`, `list_sessions`, `revoke_session`, `list_devices`, and `revoke_device` require
the same row-resolved session or operator credential as every other protected capability.

`sign_up_password` and `sign_in_password` issue browser sessions. Passwords use OpenSSL 3.6.3
`EVP_KDF` `ARGON2ID` with 64 MiB memory, three iterations, one lane, and a unique 128-bit salt.
A user with an enrolled TOTP factor receives `mfa_required` from password sign-in and must use
`verify_totp`. Tokens are named sessions with explicit capability-name scopes. Ordinary tokens
default to a 30-day expiry; a non-expiring static token requires a non-empty reason.

TOTP seeds are never stored in credential JSON as plaintext. OpenSSL AES-256-GCM seals each value
with a purpose-specific random data key; `keystore` stores that key wrapped under the required
32-byte hexadecimal `LAKE_INSTANCE_KEK`. OAuth client secrets use the same envelope format.

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

The `Identity.PasswordSessionsTokensAndRevocation` test covers Argon2id password sign-up, session
resolution, narrowed token scope, static-token reason validation, and next-request revocation.
`OAuth.AuthorizationCodePkceOidcAndCachedJwks` runs the browser redirects against a stub OpenID
Connect provider, verifies PKCE at its token endpoint, signs an id token, serves JWKS, checks the
session cookie, proves cache reuse, and rejects state replay.
The Postgres identity test also reads back the encrypted provider secret and proves that persisted
OAuth state returns its verifier once and only once.
`WebAuthn.PersistedRegistrationAssertionAndSignCount` uses a software ES256 authenticator to
produce `clientDataJSON`, authenticator data, a CBOR attestation object and signed assertions. It
proves origin binding, first-attempt challenge consumption, session issue and sign-count replay
rejection. The Postgres identity test repeats a successful registration and assertion against the
persisted challenge and credential rows.

## How to change

Add a capability through `src/api/capabilities.cpp`, with its input and output schema. Extend
`Store` and both store implementations together. Keep new credential rows pool-scoped and add the
capability to `docs/api/README.md` in the same change. The OAuth callback is the documented
exception because the provider invokes it as a browser redirect.

OAuth registration, PKCE callback processing, JWKS verification and WebAuthn ceremonies use the
`0007_identity_core` tables. Keep the passkey browser adapter limited to the one documented inline
script that calls `navigator.credentials`.
