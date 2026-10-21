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
whose `secret` is the key in RFC 4648 base32 without padding, as the Key Uri Format requires and
authenticator apps decode (lake#773; it was hexadecimal before, so every code an authenticator
showed was refused; the stored seed is unchanged, so earlier enrollments verify the same), and
eight recovery codes once; replacing the enrollment invalidates the previous seed and codes.
Recovery codes are single-use. `disable_mfa` requires a fresh TOTP or unused recovery code and
revokes the seed and every remaining recovery code. Tokens are named sessions with explicit
capability-name scopes. Ordinary tokens
default to the lesser of 30 days and the issuer’s remaining lifetime. Non-expiring static
tokens require static authority and a non-empty reason; see the derivation policy below.

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

Lake-pool password sign-up does not require an identity document (lake#762, liquid-bridge
decision 65): without one, Lake mints a **managed key** for the account (as for a first Clerk
sign-in) and the person can switch to their own identity later ([Your own
identity](#your-own-identity)). With one, and always in an application pool, password sign-up takes
the client-held TAAP/1 document as a base64url-encoded canonical-CBOR `identity_document`. lake decodes and verifies its self-signature through libliquid's public
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

## Token derivation and revocation

`create_token` derives authority from the credential that authenticated the request. Its handler in
`src/api/capabilities.cpp` applies these rules once for REST, OpenAPI, MCP, and hosted pages:

- Every requested capability must be in the issuer’s scopes. An issuer with `*` may issue named
  scopes or `*`; a named-scope issuer cannot issue `*`. `create_token` itself must be granted.
- An explicit ordinary-token lifetime must be positive, fit a signed 32-bit seconds value, and
  fit within the issuer’s remaining lifetime. A longer lifetime is refused with `unauthorized`.
  When omitted, the lifetime is the lesser of 30 days and that remaining lifetime. The store caps
  the persisted expiry at the parent’s exact timestamp, including time spent handling issuance.
- **Tokens that never expire** (lake#234). `create_token` with `never_expires: true` from a
  signed-in person issues a **standing** token: static, no expiry, and **no parent**, so signing out
  or the browser session expiring does not end it. It needs a non-empty `reason` and named scopes
  (never `*`) within the issuer's scopes, the person's personal account must have
  `allow_non_expiring_credentials` on (`set_customer_credential_policy`), and the instance must not
  set `LAKE_ALLOW_NON_EXPIRING_CREDENTIALS=false`. Its authority is still the person's live grants on
  every request; revoking it, `revoke_user_sessions`, or removing the person stops it. The tokens
  page offers it as *Never expires* and lists such tokens as *Never expires* with their reason.
  Verify: `TokenAttenuation.NeverExpiringTokensNeedTheOptInAndSurviveSignOut`.
- Browser and TAAP sessions are expiring issuers and cannot create static tokens. Static authority
  is derivable only from an already static credential, with the same scope restrictions and a
  non-empty reason. The trusted operator context retains its explicit root authority; the public
  token capability adds no static-authority bootstrap flow.
- Every session-derived token retains `parent_session_id`, the issuer’s immutable row ID. The
  store verifies that issuer belongs to the same user and pool and is still active before issue.
  Every subsequent authentication verifies the entire ancestor chain, including each ancestor’s
  expiry, revocation and linked device. Revoking a parent, any ancestor, or its device makes all
  descendants unusable on their next request. Children do **not** survive their parent.
  Parent deletion cascades to descendants. Ancestry revocation is evaluated at authentication;
  a child’s own `revoked_at` need not change when an ancestor is revoked.
- Application-pool API-token issuance returns only the derived session bearer, with its usual
  session lifecycle event. It does not also issue an independent application JWT.
- Refused amplification creates no session. Each refusal and successful issuance passes through
  `Registry::call` and produces exactly one signed audit event. Audit records contain outcome,
  principal and request metadata, never the supplied or issued bearer values.

Migration `0021_token_attenuation` adds the parent foreign key and its index. Existing API and
static tokens have no trustworthy derivation history, so the migration revokes those tokens once.
Users must reissue tokens from an active sign-in session. Reapplying the schema preserves tokens
issued after the migration. Deploy the schema before the new binary; the operator handoff and
new guard are in [`../control-plane/schema.md`](../control-plane/schema.md).

## How to verify

```sh
cmake --build build -j 6
ctest --test-dir build --output-on-failure
```

Set `LAKE_TEST_DB_URI` to a fresh schema-loaded PostgreSQL database and `LAKE_INSTANCE_KEK` to a
random 32-byte hex key as in [`../build/local.md`](../build/local.md). Run the full CTest suite in
both the static build and a `-DBUILD_SHARED_LIBS=ON` build; filtered runs are diagnostic only.

`Transports/TokenAttenuation` repeats the scope, lifetime, static-authority, recursive parent and
device-revocation cases through real REST and MCP requests against both stores. PostgreSQL cases
also prove expired-ancestor refusal, ancestry across store reopen, cascading parent deletion, and
one-time legacy-token invalidation with safe schema reapplication. Each test checks that denied
issuance adds no session, that each call has exactly one audit event, that the full audit chain verifies, and that none of its
bearer values appears anywhere in the audit export. Each PostgreSQL case creates and removes its
own schema inside the supplied test database to isolate credential and audit fixtures.

The `Identity.PasswordSessionsTokensAndRevocation` test covers Argon2id password sign-up, MFA
enrollment, recovery-code sign-in and one-use enforcement, fresh-verification disablement, session
resolution, narrowed token scope, static-token reason validation and refusal from an expiring
issuer, and next-request parent revocation.
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

Keep attenuation policy in `src/api/capabilities.cpp` and ancestry validity/expiry persistence in
both stores. Carry issuer metadata through the common authenticated context, never through
client-supplied token fields. Add a capability with its input and output schema. Extend
`Store` and both store implementations together. Keep new credential rows pool-scoped and add the
capability to `docs/api/README.md` in the same change. The OAuth callback is the documented
exception because the provider invokes it as a browser redirect.

OAuth registration, PKCE callback processing, JWKS verification and WebAuthn ceremonies use the
`0007_identity_core` tables. Keep the passkey browser adapter limited to the one documented inline
script that calls `navigator.credentials`.

## Your own identity

**Status: implemented 2026-10-17 (lake#762, migration `0086_identity_links`), not deployed.**
Decision 65 in liquid-bridge's `docs/decisions.md`.

### What

Every account that starts without an identity document (Clerk sign-in, or Lake's own sign-up
without one) signs with a **managed key**: an Ed25519 TAAP identity Lake mints and holds, its seed
wrapped under `LAKE_INSTANCE_KEK` in `custodial_identities`. The account's identifier, the one
grants, audit and repositories are keyed on, is that key's `taap:1:ed25519:…`.

The person may later **use their own identity**: the liquid identity on their computer. The
account keeps its identifier, so its grants, audit history and repositories stay exactly as they
are; what changes is the key it signs with. The own identity becomes the managed key's **linked
successor**, recorded once with both keys' signatures, and the managed seed is destroyed.

| Capability | Kind | Input | Output |
|---|---|---|---|
| `account_identity` | read | — | `{taap_id, key: managed\|own, signs_as, managed_key: {public_key, held, destroyed_at} \| null, linked: {own_taap_id, own_document_hash, linked_at, link_document} \| null}` |
| `begin_own_identity` | action | `{identity_document}` (the output of `liquid identity show --document`) | `{challenge, command, own_taap_id, expires_at, statement}` |
| `use_own_identity` | action | `{challenge, signature}` (everything `liquid sign artifact <challenge>` printed) | the account identity, as `account_identity` |

REST: `GET /v1/account_identity`, `POST /v1/begin_own_identity`, `POST /v1/use_own_identity`. MCP:
the same names. The two actions are refused (`unauthorized`) for anything but the signed-in person
in Lake's own pool: an API token, an agent, the operator credential or an application user cannot
change which key an account signs with. They are `conflict` when the account already signs with its
own identity, and when that identity already belongs to another Lake account.

### How a person proves they hold their own key

Lake does not take a successor document produced on the person's computer: that would need the
managed key's signature, which only Lake holds. Instead the two keys sign the same transition:

1. `begin_own_identity` decodes the document with libliquid's public codec
   (`decode_identity_document`), re-encodes it and compares the bytes (canonical CBOR), verifies its
   self-signature (`verify_document`), and accepts only an **initial document**: one without a
   predecessor hash whose identifier is its own key's fingerprint, so the key proves the identifier.
   A rotated successor names an identifier its key alone does not prove and is refused (link before
   `liquid identity rotate`, or see *Limits*).
2. Lake writes a **statement** `{version: "taap_identity_link/1", purpose: "lake_use_own_identity",
   account, previous (the managed identifier), next (the own identifier), next_document_hash,
   nonce, expires_at}`, and the challenge `sha256:<hex>` of its CBOR. The challenge, bound to the
   account, is stored in `identity_link_challenges` for ten minutes and can be used once.
3. The person runs `liquid sign artifact <challenge>` with their identity. liquid signs the
   challenge text (a canonical CBOR text string) as a TAAP/1 `artifact` and prints
   `signature=<hex of the canonical signature object>` and `signer=<identity>`.
4. `use_own_identity` consumes the challenge (a refused attempt spends it too), checks a `signer=`
   line names the identity being linked, decodes the signature with `decode_signature`, and
   verifies it with `liquid::verify` against the key in the stored document. A signature by any
   other key, or over any other challenge, is `unauthorized`.
5. The managed key then signs the **link body** `{version, account, previous, next,
   next_document_hash, challenge, statement, linked_at}` as `TAAP/1 identity_link`. The link
   document is `{body, previous_signature, previous_public_key, next_signature}`:
   `previous_signature` is the managed key's Ed25519 signature, `next_signature` the person's
   signature over the challenge the body commits to. `own_identity::verify_link` re-checks both.
6. In one transaction the link is written to `identity_links` and the managed seed is wiped
   (`wrapped_seed` set to null, `seed_destroyed_at` set). The seed is never destroyed without a
   committed link and never kept after one. Its public half stays, so the link still verifies.

Each step is one audit event in the central log (`begin_own_identity`, `use_own_identity`), whose
principal is the account and whose details name `previous` and `next`;
`use_own_identity` adds `managed_seed_destroyed` and `own_document_hash`. Refusals are recorded too.

### What the own identity can do afterwards

- `sign_in_taap` with the own identity's document signs in to the account.
- Repository pushes name each change's signer: the `push_change` audit event carries `author` (the
  signing identity), `author_account` (the account that identity acts for, through
  `Store::account_for_identity`) and `author_is_caller`. A push signed by the linked key is
  attributed to the account.
- Grants, memberships, workspace records and the audit history keep the account identifier.

### Limits

- A person whose liquid identity has already been rotated cannot link it yet: the current document
  alone does not prove its identifier. Accepting the whole chain (`verify_chain`) is the follow-up.
- Rotating the own key afterwards (`liquid identity rotate`) is not tracked by Lake: `sign_in_taap`
  verifies the document presented, as before.
- The link is one-way. An account that uses its own identity cannot return to a managed key.
- Postgres keeps old row versions until vacuum; the wiped seed was wrapped under the instance key
  throughout.

### Verify

```sh
ctest --test-dir build -R 'OwnIdentity|Clerk'   # with LAKE_TEST_DB_URI set
```

`OwnIdentity.*` covers sign-up without a document (memory and Postgres), a Clerk sign-in starting
with a managed key, the whole rotation on both stores with a real libliquid identity (wrong signer,
wrong challenge, replay, token credential, second link and a taken identity refused; grants,
repositories and identifier kept; managed seed gone; link verifies and a tampered one does not;
`sign_in_taap` with the own key; audit), a rotated successor document refused, and a liquid push
from the linked key attributed to the account.

### Change it

The statement, the link body and the verification are in `src/control/own_identity.cpp`; the
capabilities in `register_own_identity_capabilities` (`src/api/capabilities.cpp`); the managed
signature in `audit::sign_with_managed_key`. A change to the statement or the body changes every
future challenge; keep `verify_link` able to read the links already written.

## Personal customer on sign-up

Lake-pool password sign-up atomically provisions the user-owned customer, policy object, and
`admin` grant before issuing its session. Application pools remain isolated. Naming, default
quotas, zero-byte metering, transaction failure, and retry behavior are specified in
[provisioning.md](provisioning.md). The public identity export command is
`liquid identity show --document`; the private signing key stays with the client.
