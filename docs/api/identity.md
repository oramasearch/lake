# Identity API

Account restoration beyond MFA sign-in is specified in [`recovery.md`](recovery.md). Recovery
restores sign-in credentials and sessions; it does not restore end-to-end encrypted repository
keys. Offline device grants are specified separately in [`offline.md`](offline.md).

## Authentication attempt limits

Password sign-in, MFA sign-in, MFA enrollment, disable and step-up, recovery-code sign-in and
recovery completion share ten failed attempts per account per fifteen minutes (lake#966,
LSR-100). Known accounts use their immutable user id across pools; email lookups use the lowercase
credential email. Unknown email budgets remain scoped to the requested pool. Unknown credentials and an account cooldown both return `unauthenticated` (REST 401)
with `invalid credentials`. Real credential verification resets account and client failures;
a password-only response requiring MFA and an accepted email-recovery start do not reset them.
Email-recovery initiation verifies no credential and does not change these failure counts;
its mail/token budgets are tracked separately in lake#968.

A refusal also takes as long for an unknown account as for a known one (lake#1101). Password
sign-in, MFA sign-in, recovery-code sign-in, recovery completion and MFA enrollment, when the email
has no account, the account has no password credential, the recovery code matches no selector or
the recovery attempt is unknown or has no token, verify the given secret against a fixed dummy
Argon2id hash before answering. The dummy is made once per process by the same function that hashes
real credentials (`identity_crypto::hash_secret`), so it always has their parameters, and it never
matches. Before this, an email with a password answered about 100 ms slower than one without, which
told a caller which emails had Lake password accounts. The failure budgets above count these
refusals exactly as before. Hosted application sign-in forms call the same `sign_in_password`
capability, so application pools get the same behaviour.

REST, hosted application forms and MCP also share 100 failed attempts per trusted client address
per fifteen minutes, across these operations and account-deletion step-up. The HTTP adapter
reuses the order/recovery address rule: ingress-nginx's rewritten `X-Real-IP`, else the final
`X-Forwarded-For` hop, else a shared direct-client bucket. It ignores `CF-Connecting-IP` and earlier
forwarded hops. Deployments must preserve ingress header sanitizing and prevent direct access
around that ingress. Capability input cannot supply transport metadata; in-process callers share
a client bucket. TAAP, passkeys and external identity-provider verification are outside these
password/MFA budgets.

There is no instance-wide ceiling: one would let anyone lock every person out of sign-in by
guessing at accounts that are not theirs. Failures against one account or from one client never
refuse another account's sign-in from another client. Client exhaustion returns `rate_limited`
(REST 429, MCP error). Refused requests do not extend a window; a successful account/client verification
starts its cleared budget's next fifteen-minute window.

Admission reserves capacity before expensive verification, including in-flight work so concurrent
guesses cannot pass the limit before failures are recorded. PostgreSQL commits admission and
completion separately from credential verification, sharing budgets across processes and
connections. Expired windows are pruned; window ids prevent late completions changing a replacement
window, and 100,000 live hashed keys bound storage. Database errors refuse authentication;
interrupted requests retain their reservation until expiry. Both stores use the same policy.

A successful TOTP records its matched 30-second step under the factor's row lock (or memory-store
mutex). Only a later step may succeed, across sign-in, step-up and disable; the previous/current/next
step tolerance remains. PostgreSQL locks the account before its factor rows so verification
and account deletion use the same lock order. Step-up also consumes recovery codes. Replacing MFA starts a fresh counter
for the new seed. PostgreSQL stores this counter in the existing encrypted-seed credential's
`public_data`, never in the client.

Apply migration `0098_authentication_attempts` before the new binary. The operator must vendor
`db/schema.sql` and select `authentication_attempts_guard`; no operator files change here.
To verify, run `ctest --test-dir <build> --timeout 600 -R 'AuthenticationLimits|MfaVerificationLocks' --output-on-failure`
with `LAKE_TEST_DB_URI` configured. The tests alternate REST and MCP over fresh connections, check
unrelated accounts, separate PostgreSQL connections, cooldown expiry, and TOTP/recovery replay.
Keep admission before credential verification and counter consumption inside the successful
verification transaction when changing these paths; update both stores and these tests together.

## What

Identity capabilities live in the single registry and are therefore available as REST, OpenAPI,
and MCP tools. `sign_up_password`, `sign_in_password`, `verify_mfa`,
`begin_passkey_sign_in`, `sign_in_passkey`, `begin_identity_proof`, `start_oauth`, and
`sign_in_taap` are unauthenticated entry points.
`enroll_mfa`, `disable_mfa`, `begin_passkey_registration`, `register_passkey`,
`create_token`, `list_sessions`, `revoke_session`, `list_devices`, `revoke_device` and
`sign_out_everywhere` require the same row-resolved session or operator credential as every other
protected capability. `sign_out_everywhere` (lake#1163) revokes every session, API key, static
token and device of the caller's own account, the calling credential included; an operator
credential is refused, since it is no person. It is the person's account-wide sign-out: signing out
of one browser, or a Clerk session event, ends browser sessions only and never a key or a device
([clerk.md](clerk.md#webhooks)).
The session and device lists authorize the authenticated principal through the registry's batched
authorization path before returning any row.

**Under Clerk the `lake` pool's password entries are off** (lake#1098). With a Clerk publishable
key configured, `sign_up_password`, `sign_in_password`, `verify_mfa` and `recover_with_code` in the
`lake` pool answer `403 forbidden` and issue no session; `viewer` reports the same rule as
`password_sign_up: false` ([clerk.md](clerk.md)). Application pools are unaffected.

`sign_up_password` and `sign_in_password` issue browser sessions. Passwords use OpenSSL 3.6.3
`EVP_KDF` `ARGON2ID` with 64 MiB memory, three iterations, one lane, and a unique 128-bit salt.
A user with an enrolled TOTP factor receives `mfa_required` from password sign-in and must use
`verify_mfa` with a current TOTP or an unused recovery code. `enroll_mfa` returns an `otpauth` URI
whose `secret` is the key in RFC 4648 base32 without padding, as the Key Uri Format requires and
authenticator apps decode (lake#773; it was hexadecimal before, so every code an authenticator
showed was refused; the stored seed is unchanged, so earlier enrollments verify the same), and
eight recovery codes once; replacing the enrollment invalidates the previous seed and codes.
Enrollment and replacement require `password`, verified against the account's current password
on every call, and an active full-authority interactive session (`browser` or `taap`) belonging
to that user and pool. Tokens (including wildcard and static tokens), device credentials,
operator credentials and callers without a password credential cannot enroll. A session's age
does not substitute for the password. Both stores check the proof before changing the seed or
recovery codes, so a refusal preserves the existing enrollment (lake#959, LSR-093).
Run `ctest --test-dir <build> --timeout 600 -R MfaEnrollment --output-on-failure` with
`LAKE_TEST_DB_URI` set to exercise first enrollment, replacement and refusals through REST/MCP
against memory and PostgreSQL. Keep password and session verification in the store transaction
when changing enrollment; a caller-side check alone can be bypassed by another store caller.
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
carries it through the short-lived flow. It takes the document only with `identity_proof`, a fresh
signature by the document's key ([Proving you hold the key](#proving-you-hold-the-key)); without
one it answers `identity_proof_required`. The Postgres store verifies and records that document
before creating the user and OAuth credential. A returning linked identity does not need to send
the document again.

OAuth starts bind each flow to the initiating browser (lake#963, LSR-097). The REST start
response and the hosted `/apps/<id>/identity/oauth` redirect set a separate, unpredictable HttpOnly cookie;
its name is `__Host-lake_oauth_<SHA-256(state)>` over HTTPS, with `Secure`, `SameSite=Lax`,
`Path=/`, no Domain, and a five-minute lifetime. Loopback HTTP development uses `lake_oauth_`
without `Secure`. Different flows use different cookies, so starting another login does not
replace an earlier flow's binding. The cookie secret is independent of state, PKCE, and the OIDC
nonce, and never appears in the authorization URL or JSON result. Start the flow on the host that
will receive the callback; HTTP clients must retain its cookie. An authorization URL obtained
through MCP alone cannot complete a browser login.

Both stores require the cookie secret as well as provider and state before consuming a flow.
Postgres hashes state together with the binding in the existing `state_hash` column; no migration
or operator schema update is required. Missing or foreign cookies answer 403 before a code exchange
or session issuance and leave the legitimate flow usable. A successful consumption is atomic and
one-use. The cookie expires after five minutes. Flows started before this change must be restarted.
Verify with `OAuth.AuthorizationCodePkceOidcAndCachedJwks`,
`OAuth.EnterpriseOidcAuthorizationCodeCallback`, and
`PostgresIdentity.SessionsTokensAndDeviceRevocation`: missing and foreign cookies fail, the
initiating cookie completes once, and replay fails. Changes to either start handler must preserve
the cookie output and the store's required binding argument.

The callback consumes bound state before exchanging the code. OpenID Connect id tokens must use RS256
and pass signature, issuer, audience, expiry, not-before, nonce, subject, and key-id checks. JWKS
documents are fetched through Corosio with TLS peer and hostname verification on the blocking pool.
Malformed tokens and failures with a known key never trigger a fetch; unknown key IDs and valid
tokens using keys older than an hour share a single-flight refresh with a 60-second cooldown,
including failures ([bounded key refresh](clerk.md#bounded-key-refresh-lake956-lsr-090), lake#956). GitHub instead verifies
the returned access token through `https://api.github.com/user`. Google and GitHub use their fixed
public endpoints; every other provider's endpoints come from its OpenID Connect discovery document
(below). Provider registration and each OAuth callback create one signed registry-compatible audit
event.

### OpenID Connect discovery

A generic Lake OAuth provider (`register_oauth_provider` with a `provider` other than `google` or
`github`) and every enterprise OIDC provider (`register_sso_provider`, `protocol: oidc`) are read
from OpenID Connect Discovery 1.0 (lake#837):

- Lake fetches `<issuer>/.well-known/openid-configuration` (the issuer's trailing `/` removed first)
  when a sign-in starts (`start_oauth`, `start_sso_oidc`) and again in the callback, and uses its
  `authorization_endpoint`, `token_endpoint` and `jwks_uri`. So Microsoft Entra
  (`https://login.microsoftonline.com/<tenant>/v2.0`), Okta (`https://<org>.okta.com` or
  `https://<org>.okta.com/oauth2/<server>`) and Google Workspace (`https://accounts.google.com`)
  work, although their endpoints are not under `<issuer>/`.
- **The document's `issuer` must equal the configured issuer exactly**, trailing slash included
  (Discovery §4.3); a document naming another issuer is refused (`invalid_input`). Entra's
  multi-tenant `common` and `organizations` documents name a `{tenantid}` template, so they are
  refused: register the tenant's own v2.0 issuer.
- All three endpoints must be present and HTTPS. When the document lists
  `id_token_signing_alg_values_supported`, it must include `RS256`, the one algorithm Lake
  verifies.
- Documents are cached per issuer for one hour, in memory; a failed fetch is not cached. A document
  that cannot be fetched answers `engine_error`.
- **Outbound protections.** The discovery document, the token exchange and the keys are fetched
  through the server's outbound client within 10 seconds, HTTPS to public addresses only: every
  address the host resolves to is checked at connect time, as webhooks and MCP connections are
  (lake#876). `LAKE_OIDC_ALLOW_PRIVATE_ADDRESSES=1` lifts that, and allows plain HTTP to loopback,
  for tests, local development, or an install whose identity provider is on its own network. The
  discovery read runs on the blocking pool, so a slow issuer holds up only its own request.
- ID tokens are verified against the discovered `jwks_uri` as above: RS256 only, `iss` equal to the
  configured issuer, `aud` containing the client id (and `azp` when there are several audiences),
  `exp`, `nbf`, `iat`, `sub` and the flow's `nonce`.

The tests are `OidcDiscovery.*` and `OidcSignIn*` in `tests/test_oidc_discovery.cpp`. Local providers
shaped like Entra and Okta serve their documents, endpoints and keys at their own paths. Against
them, valid tokens sign in once through the discovered endpoints; tampered, wrong-audience, expired
and wrong-nonce tokens and replayed callbacks are refused; a document naming another issuer is
refused; and private addresses are never contacted without the setting.

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
`decode_identity_document`, `encode_identity_document`, and `verify_document` APIs, requires an
initial document (its identifier must be derived from its own key, liquid#383), hashes the complete
canonical record with SHA-512/256, writes `identity_documents`, and binds the resulting `taap_id` to
the new user. A document is public, so sign-up takes one only with `identity_proof`, a fresh
signature by its key ([Proving you hold the key](#proving-you-hold-the-key)). A rotated successor
document is refused at sign-up, because without its chain it does not prove which identity it
belongs to. lake never receives a person's private key.

`sign_in_taap` lets that registered user return without a password. The client signs
`pool_id + "\n" + nonce + "\n" + issued_at` with the key in its liquid identity document. Lake
verifies the canonical document, then the `lake_sign_in` signature against that document as a
carried identity chain (`liquid::verify(actor, chain, ...)`), accepts a five minute freshness window,
and records the assertion digest before issuing a one-day session. Assertion digests are one-use,
so replay fails while the timestamp remains fresh.

The chain check is what binds the presented `taap_id` to the signing key (liquid#383). Before it,
Lake checked only the document's self-signature and then the signature with the document's own key,
so a self-signed document carrying a registered person's identifier with someone else's key signed
in as that person. `OwnIdentity.ASignInDocumentNamingAnotherIdentityIsRefused` builds that forged
document and is refused; the honest initial document signs in.

### Signing in with a TAAP key: the key must be the identity's

Lake issue [#917](https://github.com/oramasearch/lake/issues/917) (security review finding
LSR-006): liquid's `verify_document` checks only that a document is signed by the key it carries
and that its identifier ends with the fingerprint it claims. A document carrying a registered
person's identifier with the caller's own key passed, its assertion verified with that key, and
Lake issued a session for the person's account. Anyone who knew a TAAP id could sign in as its
owner.

Now the assertion must be signed with the key **bound** to the identifier
(`identity_crypto::bound_taap_key`):

- **The identity's first document.** An initial document (no predecessor) whose identifier is the
  one its own key derives, `taap:1:<algorithm>:<key fingerprint>` (its fingerprint equals its key
  fingerprint). Only that key can produce it, so it proves the identifier, the rule own-identity
  linking already applied.
- **A rotation the replaced key signed.** After `liquid identity rotate`, the client sends
  `identity_chain`: every document from the identity's first, oldest first, ending with
  `identity_document` (at most 64). Lake requires the first to be bound as above, the chain to pass
  liquid's `verify_chain`, and **every** rotation in it to carry the replaced key's signature. liquid
  also accepts a successor that declares the previous key compromised and is signed by the new key
  alone; Lake refuses that, because anyone could have made it. The assertion is verified with the
  chain's current key, and its `signing_key_identifier` must name that key.
- **Anything else is refused** with `unauthorized`: a document naming another identifier with a
  different key, a rotated successor sent without its chain, a chain that does not start at a
  bound document, an unbound rotation, or an assertion signed by any other key. No session is
  issued. A person who has lost their key returns through account recovery
  ([`recovery.md`](recovery.md)), which does not register a new TAAP key.
- **Each refusal is audited**: the `sign_in_taap` event has outcome `denied` and details
  `claimed_taap_id` (the identifier the document named), `reason`, and `identity_chain_documents`.

The same rule binds the document registered at sign-up. PostgreSQL password and OAuth sign-up with
an `identity_document` take only an identity's first document, bound as above, so nobody can
register an account under another person's identifier. The in-memory store (demos and
tests) now gives an account the identifier of a bound document, as PostgreSQL does; any other
document still gets the account an identifier of its own, never the one it claims.

Related: liquid's own `verify()` fell back to whatever key the caller supplied when no chain was
stored locally ([liquid#383](https://github.com/oramasearch/liquid/issues/383), fixed in
[liquid#448](https://github.com/oramasearch/liquid/pull/448)). This check does not depend on it:
Lake binds the key itself before calling `verify`. Lake's companion change for liquid#448,
[lake#1021](https://github.com/oramasearch/lake/pull/1021), moves this call to liquid's carried-chain
`verify(actor, chain, ...)`, passing the same documents.

To verify: `TaapSignIn.MemoryStoreBindsTheKeyToTheRegisteredIdentity` and
`TaapSignIn.PostgresStoreBindsTheKeyToTheRegisteredIdentity` register a real liquid identity, sign
in with it, and then show that a forged document with the victim's identifier and the attacker's
key (which liquid's `verify_document` accepts) is refused and audited, that registering it is
refused, that the rotated key signs in with its chain and not without it, and that a compromise
rotation signed only by the new key (which liquid's `verify_chain` accepts) is refused.

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
  every request; revoking it, `sign_out_everywhere`, `revoke_user_sessions`, or removing the person
  stops it. The tokens
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
Connect provider (its endpoints from its discovery document), verifies PKCE at its token endpoint,
signs an id token, serves JWKS, checks the session cookie, proves cache reuse, and rejects state
replay.
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

## Proving you hold the key

**Status: implemented 2026-10-21 (lake#931), not deployed. No migration.**

### What

An identity document is public: `liquid identity show --document` prints it to be shared. Holding
one proves nothing, so `sign_up_password` with an `identity_document` and `start_oauth` with one
(a first OAuth sign-in) bind it to a new account only with a fresh signature by the document's key
over a challenge Lake issued for that document and pool. Without the proof they answer
`identity_proof_required` (403) and create nothing. Before lake#931 anyone holding a copy of a
person's document could sign up as that identity in any pool where it was not yet taken.

| Capability | Kind | Input | Output |
|---|---|---|---|
| `begin_identity_proof` | action, unauthenticated | `{pool_id, identity_document}` (`pool_id` defaults to `lake`) | `{challenge, nonce, expires_at, command}` |
| `sign_up_password`, `start_oauth` | action, unauthenticated | `identity_proof: {nonce, expires_at, signature}` beside `identity_document` | as before |

`signature` is everything `liquid sign artifact <challenge>` printed (its `signature=` line, or the
hex alone); `nonce` and `expires_at` are returned unchanged from `begin_identity_proof`.

### How

1. `begin_identity_proof` verifies the document as `begin_own_identity` does
   (`own_identity::verify_document`: canonical CBOR, a valid self-signature, an initial document
   whose identifier is its own key's fingerprint) and answers a challenge valid for five minutes.
   Lake stores nothing: the challenge is `sha256:<hex>` of an HMAC-SHA256, under a key derived from
   `LAKE_INSTANCE_KEK`, over the canonical CBOR of `{purpose: "lake_enroll_identity", pool,
   taap_id, key_fingerprint, document_hash, nonce, expires_at}`
   (`own_identity::enrollment_challenge`). Only this Lake can make one, so the nonce and expiry a
   client returns are ones it issued for that pool and document.
2. The person runs `liquid sign artifact <challenge>` with their identity.
3. The Postgres store's `sign_up_password` and `begin_oauth` recompute the challenge from the
   request's pool, the document, the nonce and the expiry, refuse an expired one, and verify the
   signature with the document's key (`own_identity::verify_challenge_signature`). A signature by
   another key, over a challenge for another pool or document, or after the expiry is
   `unauthorized`.
4. A proof is used once: sign-up records `identity_proof:<challenge>` in `identity_taap_assertions`
   with the new user; an OAuth start records it as a spent row in `identity_oauth_flows` (state hash
   `sha256(identity_proof:<challenge>)`, already used, so it can never be consumed), since there is no
   user yet. Each path checks both before accepting a proof, so a second use on either is
   `unauthorized`.

Enterprise OIDC flows (`start_sso_oidc`, providers `sso-<id>`) carry a `taap_id`, not a document, to
`sign_in_sso`; caller-chosen TAAP ids there are tracked in
[lake#872](https://github.com/oramasearch/lake/issues/872). The in-memory store binds no document (it
gives each new user a random identity), so it needs no proof.

The hosted application pages and the console run the same two steps: a sign-up or OAuth form that
brings a document and is answered `identity_proof_required` shows the challenge, the command and a
field for its output, then sends the request again with `identity_proof`
([hosted pages](apps.md), [Lake's sign-in](../ui/web-app.md)).

### Limits

- A rotated identity cannot enroll with its current document alone (as for
  [your own identity](#your-own-identity)): its key does not prove its identifier.
- Clients that sent `identity_document` without a proof are refused until they add the second
  step.

### Verify

```sh
LAKE_TEST_DB_URI=... ctest --test-dir build -R 'IdentityProof'
```

`PostgresIdentityProof.*` signs up and starts OAuth with another identity's genuine document and no
proof, a proof by another key, a replayed proof, an expired challenge and a challenge minted for
another pool (all refused, no user created), and with a genuine proof (accepted).

### Change it

The challenge is `own_identity::enrollment_challenge` (`src/control/own_identity.cpp`); the checks
are `verify_identity_proof` and `identity_proof_used` in `src/control/postgres_store.cpp`; the
capability is `begin_identity_proof` in `src/api/capabilities.cpp`. A change to the statement
invalidates the challenges outstanding, which last five minutes.

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
- Rotating the own key afterwards (`liquid identity rotate`) is not recorded by Lake. `sign_in_taap`
  accepts the rotated key only with the identity chain from the first document, every rotation
  signed by the key it replaced ([above](#signing-in-with-a-taap-key-the-key-must-be-the-identitys));
  the first key also still signs in, because Lake keeps no record that it was replaced.
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

## Deleting an account

Before lake#810 Lake could create an account but not remove one: every end-to-end run left its
sign-up accounts behind. `delete_account` deletes one, for the person themselves or for the
operator. It is in the one registry (REST `POST /v1/delete_account`, OpenAPI, MCP) and audited in
the central log, as every identity event is (decision 52).

### Who can delete an account

| Caller | What it needs |
|---|---|
| The account itself | Its own session (omit `user_id`, or give its own), and proof that it is the person now: a sign-in (`browser` or `taap` session) started in the last 15 minutes, or `code`, a fresh TOTP code or unused recovery code when MFA is enrolled. A token, however new, is not a sign-in. |
| The operator (principal `admin`) | `user_id`. No step-up: the operator credential is the instance's own. |

Anyone else naming another account is refused `401 unauthorized` before the handler runs, and so is
an agent, app or build credential. `confirm` must be the account's handle (its user id when it has
no personal account); `dry_run: true` deletes nothing and answers the plan below.

### What goes, in order

1. **The workspaces of its personal account**, each through `delete_workspace`, so each is one
   transaction that marks the workspace `deleted` (the Orama operator removes its Zetta from that
   desired state, decision 51), tombstones its repositories and deletes its apps, with its own
   central audit events ([workspaces.md](workspaces.md#deletion)).
2. **The organizations it alone owns** (it is their only organization-wide `owner`): first their
   workspaces, then each organization through `delete_organization`
   ([organizations.md](organizations.md#deleting-an-organization)). One with nobody else in it goes
   with the account. One that other people belong to is deleted only with
   `delete_organizations: true`; otherwise the deletion is **refused** (`409`, naming it, path
   `/delete_organizations`) before anything is deleted, so an organization is never left without an
   owner. An organization with another owner is left to them (`action: "leave"`); the account's
   membership is removed.
3. **The account**, in one store transaction that is refused if a workspace of its personal account is still
   live or it is still an organization's only owner (whoever asks the store):
   - every session and token, device, credential (password, TOTP, recovery codes, passkeys, OAuth,
     SSO and Clerk links), identity document, TAAP assertion, WebAuthn challenge, recovery attempt
     and offline grant, identity link, SCIM mapping and app hand-off code;
   - the recovery custody it holds for other people's repositories;
   - every membership and every grant naming its TAAP identity;
   - the seed of an identity Lake minted for it (a Clerk sign-in's, decision 44) is destroyed; the
     public key stays so the signatures it made still verify, as after lake#762's rotation;
   - of its personal account: the billing contact, usage limits, storage quota override, secret mount
     routes and its policy objects (with every grant on them);
   - the user row and its personal account are marked deleted (migration `0091_account_deletion`), the email
     erased to null and the display name to `''`.

### What is kept, and why

- **The central audit log.** `audit_events` is append-only and hash-chained, and its rows name the
  account by its TAAP identity (`principal`) and its personal account (`customer_id`, `ON DELETE
  RESTRICT`). So the user and account rows stay, marked deleted and never listed or read again, and
  the handle and TAAP identity stay retired; the email and name are erased, and principal names no
  longer resolve the identity to a person. Events already written keep what they recorded; see
  *Open questions* below. Each workspace's own audit chain goes with its workspace Zetta.
- **Invoices, their lines and payment records, the Stripe customer link, and orders.** They are
  billing records Lake issued (decision 56; orders by `0089`), kept for as long as the invoice. An
  unpaid invoice stays collectable; the plan says how many are kept (`kept.invoices`) and how many
  are still due (`kept.open_invoices`).

### The answer

```json
{
  "user_id": "…", "handle": "brave-otter-7",
  "workspaces": [{"id": "…", "name": "dev", "environment": "dev"}],
  "organizations": [{"organization_id": "…", "display_name": "Acme", "handle": "acme",
                     "role": "owner", "other_owners": 0, "other_members": 3,
                     "workspaces": [], "action": "delete | leave | blocked"}],
  "counts": {"sessions": 2, "credentials": 1, "memberships": 2, "grants": 4, "billing_contacts": 1, "…": 0},
  "kept": {"invoices": 0, "open_invoices": 0, "audit": "…"},
  "dry_run": false, "deleted": true, "deleted_at": "…",
  "deleted_workspaces": [{"id": "…", "name": "dev"}],
  "deleted_organizations": [{"organization_id": "…", "display_name": "Acme"}]
}
```

It also carries `customer_id` (the personal account) and each workspace's `customer_id`. The central
audit event is `account.delete` (a dry run, `account.delete.plan`) on `user/<id>`, with
the handle, the counts and the deleted workspaces and organizations; never the email. If a step
fails, the call answers that step's error with what it was deleting; the steps already done stay
done, and calling again resumes (a deleted workspace or organization is no longer in the plan).

### End-to-end cleanup

An end-to-end run signs up, works, then deletes its own accounts:

```sh
curl -sS -X POST "$LAKE/v1/delete_account" -H "Authorization: Bearer $SESSION" \
  -H 'Content-Type: application/json' \
  -d '{"confirm": "'"$HANDLE"'", "delete_organizations": true}'
```

`$SESSION` is the session its sign-up or sign-in answered (fresh, so no MFA code is needed) and
`$HANDLE` is `viewer.handle`. With the operator credential, `user_id` names the account instead.

### Open questions

The decisions (50–56) say where identity, audit and billing live; none says how long a deleted
person's identity may stay in events already written, or what GDPR erasure requires of them. Lake
pseudonymises going forward (the TAAP identity no longer resolves to an email or name) but does not
rewrite the signed, append-only chain, whose `details` can carry an email recorded at the time (an
invitation, a sign-up). Redacting those needs a decision first.

### Verify

```sh
LAKE_TEST_DB_URI=… ./build/tests/lake-tests --gtest_filter='*Deletions*'
```

`Stores/Deletions.OneAccountCannotDeleteAnother` (another account, a token and a wrong MFA code are
refused), `AnAccountDeletionRemovesExactlyWhatItShould` (workspaces, the sole organization, sessions,
memberships, grants and billing contact gone; the shared organization, its workspace and every other
account untouched; one central event without the email),
`AnOrganizationIsNeverLeftWithoutAnOwner` (refused before anything is deleted, by the capability and
by the store) and `TheOperatorDeletesAnAccountAndItsSoleOrganizationsWhenAsked`, each over the
in-memory store and PostgreSQL. The confirmation in Lake Console is
[`../ui/settings.md`](../ui/settings.md#deleting-your-account).

## Bounded authentication work and recovery selectors

Password sign-in, `verify_mfa`, `recover_with_code`, MFA enrollment/disablement, sign-up and
email/token recovery run their store work on the bounded blocking pool. At most two authentication
jobs may be queued or running across the process; a third receives `rate_limited` (HTTP 429). Admission stays reserved
until the worker finishes, including if its client disconnects. PostgreSQL authentication uses
private transactions, so an Argon2 calculation or a locked recovery row never holds the control
store's shared connection mutex. In-memory verification uses an owned snapshot and rechecks it
under the lock before issuing or consuming a credential.

The shared [failure budgets](#authentication-attempt-limits) apply before credential work.
Email-recovery initiation keeps its uniform acknowledgement without clearing verification
failures. Registration also reserves client capacity; a successful registration does not clear
failures from earlier authentication attempts.

New recovery codes have the form `rc1_` followed by 32 lowercase hexadecimal characters
(128 random bits). The SHA-256 selector in `credentials.subject`, indexed by the existing
unique `(pool_id, kind, subject)` constraint, selects one Argon2id hash. Unknown selectors and
malformed codes never scan stored hashes; `recover_with_code` verifies them against the dummy hash
instead, so its refusals all cost one derivation (lake#1101), while MFA verification and
disablement, whose caller is already signed in, refuse them without one. Password-plus-recovery sign-in performs
one password derivation and at most one recovery derivation. Every recovery verifier, including
fresh verification and MFA disablement, uses the same selector rule. Every successful use, including step-up, consumes the selected recovery row under lock.

Earlier 12-character recovery codes are refused: their salted hashes cannot be backfilled with
a selector. Regenerate codes by enrolling MFA again with the current password from an interactive
session. The MFA page displays this instruction. Existing TOTP seeds, passwords and passkeys
continue to work. Selector storage uses existing credential columns; the shared failure budgets require migration `0098_authentication_attempts`.

With `LAKE_TEST_DB_URI` configured, run:

```sh
ctest --test-dir <build> --timeout 600 --output-on-failure \
  -R 'AuthenticationLimits|AuthenticationStores|AuthenticationClientBudget|RecoveryWorkCannotBlock|MfaEnrollment'
```

The tests cover shared budgets, selectors, one-use factors, bounded workers, and `/healthz` plus
unrelated store reads while a recovery row is locked. Keep hash verification outside shared
connection/store locks and never add a legacy multi-hash fallback.
