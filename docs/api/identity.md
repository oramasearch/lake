# Identity API

## What

Identity capabilities live in the single registry and are therefore available as REST, OpenAPI,
and MCP tools. `sign_up_password`, `sign_in_password`, `verify_totp`, and
`sign_in_passkey` are unauthenticated entry points. `enroll_totp`, `register_passkey`,
`create_token`, `list_sessions`, `revoke_session`, `list_devices`, and `revoke_device` require
the same row-resolved session or operator credential as every other protected capability.

`sign_up_password` and `sign_in_password` issue browser sessions. Passwords use OpenSSL 3.6.3
`EVP_KDF` `ARGON2ID` with 64 MiB memory, three iterations, one lane, and a unique 128-bit salt.
A user with an enrolled TOTP factor receives `mfa_required` from password sign-in and must use
`verify_totp`. Tokens are named sessions with explicit capability-name scopes; a static token
requires a non-empty reason.

TOTP seeds are never stored in credential JSON as plaintext. OpenSSL AES-256-GCM seals each value
with a purpose-specific random data key; `keystore` stores that key wrapped under the required
32-byte hexadecimal `LAKE_INSTANCE_KEK`. OAuth client secrets and recovery codes use the same
envelope format.

Password sign-up requires the client-held TAAP/1 document as `identity_document`. The document is
JSON with a `payload` object (`id` and base64 Ed25519 `public_key`) and a base64 `signature` over
the canonical JSON encoding of `payload`. lake verifies that self-signature, hashes the complete
document with SHA-512/256, writes `identity_documents`, and binds the resulting `taap_id` to the
new user. lake never receives a person's private key.

## Why

The random bearer secret is not the authority. Every request resolves its session and device row;
revoking either one therefore takes effect on the next request. Browser sessions receive `*` for
the user's own control-plane capabilities. API tokens receive only the scopes requested at issue.

## How to verify

```sh
cmake --build build
ctest --test-dir build --output-on-failure -R Identity
```

The `Identity.PasswordSessionsTokensAndRevocation` test covers Argon2id password sign-up, session
resolution, narrowed token scope, static-token reason validation, and next-request revocation.

## How to change

Add a capability through `src/api/capabilities.cpp`, with its input and output schema, never as a
bespoke route. Extend `Store` and both store implementations together. Keep new credential rows
pool-scoped and add the capability to `docs/api/README.md` in the same change.

OAuth registration, PKCE callback processing, JWKS verification, and persisted WebAuthn
ceremonies use the `0003_identity_core` tables. Their browser and provider adapters remain to be
completed before this API is exposed in production.
