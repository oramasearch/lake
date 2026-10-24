# Clerk sign-in

**Status: implemented 2026-09-25.** When lake is given a Clerk publishable key, people sign in and
sign up in the browser through [Clerk](https://clerk.com). lake's own identity pages (password,
TOTP, passkeys, TAAP document sign-up) stay in the binary for an install without Clerk, such as
on-prem, and leave the navigation when Clerk is on. The decisions are liquid-bridge decision 43
(Clerk is the cloud sign-in) and decision 44 (lake holds a TAAP identity for a Clerk user).

## Why

lake's own sign-up asked for a public Liquid identity document before anyone could create an
account, and offered several sign-in methods side by side. That was confusing for a person who
only wants to use lake in the browser. Clerk gives one sign-in with email codes, and lake keeps
its own session, policy and audit model behind it.

## How it works

Clerk is a **sign-in method**, the way lake's OAuth providers are. It is not a second
authentication path: after the exchange below, the browser holds an ordinary `lake_session`, and
every page and API call is resolved by `context_for` as before.

1. The web app's `/sign-in` (or `/sign-up`, `frontend/src/pages/account/ClerkSignIn.tsx`) loads
   Clerk's JavaScript from the instance's own Frontend API host, which the `viewer` read names
   (`clerk.frontend_api`), and mounts it with the publishable key. After Clerk signs the person
   in, it returns to `/sign-in`.
2. With a Clerk session present, the page asks Clerk for a token from the **`lake` JWT template**
   and for Clerk's **default session token**, and posts both once, as an ordinary form, to
   `POST /identity/clerk-session` (fields `token` and `session_token`).
3. lake verifies the token with `identity_crypto::verify_oidc_id_token` against the instance JWKS
   (`<issuer>/.well-known/jwks.json`, with the bounded refresh policy below): RS256,
   `iss` equal to the instance issuer, `aud` equal to `lake`, `exp` in the future, and `nbf` and
   `iat` not more than 60 seconds ahead. Clerk's default session token has no `aud` and is
   refused; only the template token is accepted. `email` and `name` are strings or `null`: Clerk
   renders an unset template value as `null`, so a person with no name in Clerk carries
   `"name": null` and is accepted with an empty display name; any other type is refused
   (lake#1115, which locked every nameless person out of lake.live through v0.5.2).
   **Which Clerk session this browser is** (lake#1163). A JWT template carries no session id
   (Clerk: *"session-bound claims like `sid` ... cannot be included in custom JWTs"*,
   [JWT templates](https://clerk.com/docs/guides/sessions/jwt-templates)), but Clerk's default
   session token carries a signed `sid` ([session tokens](https://clerk.com/docs/guides/sessions/session-tokens)).
   Lake verifies `session_token` with the same JWKS and issuer, RS256, `exp`, `nbf` and `iat` as
   above, and requires that it carry **no `aud`** (so the template can never stand in for it), name
   the template's `sub`, and carry a string `sid` of at most 256 characters. The Lake browser
   session is then bound to that `sid` (`sessions.clerk_session_id`, migration
   `0103_clerk_session_binding`), and a Clerk session event ends only the browser it names
   ([Webhooks](#webhooks)). A present but unverifiable session token refuses the sign-in. An absent
   one (a page loaded before lake#1163) signs in unbound; any session event of the account ends an
   unbound browser session. No Clerk configuration changes: the default session token exists on
   every instance.
   A refused token is logged as one `clerk_sign_in_refused` line: the reason (for example
   `aud does not name lake`, `expired (exp …, now …)`, `key … is not in the JWKS`), the issuer,
   audience and JWKS URL lake expected, and the token's `alg`, `kid`, `iss`, `aud`, `azp`, `iat`,
   `nbf`, `exp` and each claim's JSON type. Never the token, its signature, `sub`, `email` or
   `name`.
4. Lake checks `GET https://api.clerk.com/v1/users/<sub>` with `LAKE_CLERK_SECRET_KEY`,
   without caching. A missing key, transport failure, non-200 reply, mismatched user or incomplete
   status refuses exchange. A banned, locked or Directory Sync deprovisioned account is suspended
   locally and refused. The
   request uses public HTTPS only and a ten-second timeout; its response and key are never logged.
   `Store::sign_in_clerk` finds the lake user for the Clerk subject, links or creates them (below),
   and issues a 30-day session. It refuses a template whose signed `iat` is at or before the
   account's last upstream revocation, including other Clerk subjects linked to that account.
   The handler records an `authentication` audit event with object
   `clerk` and method `clerk`, success or denied, then redirects with the `lake_session` cookie
   (`HttpOnly`, `SameSite=Lax`, `Secure` over HTTPS) to the posted `next` field when it is a
   same-origin path (one leading `/`, never `//` or `/\`) or a Ship app return
   (`/apps/authorize?app=…&rd=…`), else to the `lake_sign_in_return` app return, else to
   the home, Ask Orama (`/`; lake#600, `account_landing` in `http_server.cpp`; it was `/repositories`)
   (lake#561: the web app posts the page that sent the person to sign in). An OAuth or OIDC sign-in
   in lake's own pool lands there too. A
   failure redirects to `/sign-in?error=<message>`. With `?error=` the page does not post again by
   itself: Clerk's session is still live, so it would be refused again, once a second, into the
   rate limit (lake#1115). It shows the error with **Try again** (one more exchange) and **Use
   another account** (signs out of Clerk and shows Clerk's form).
5. `POST /logout` clears `lake_session` and, with Clerk on, redirects to `/sign-in?signed_out=1`,
   which signs the browser out of Clerk as well. Without that, the next visit would sign the person
   straight back in.

Lake verifies the template signature with the public JWKS and checks account status with the
instance's secret key. `LAKE_CLERK_SECRET_KEY` is an Orama platform credential, injected from the
same instance as the publishable key. The webhook secret is separate ([Webhooks](#webhooks)).

## Who a Clerk sign-in becomes

The credential is `credentials(kind = 'oauth', pool_id = 'lake', subject = 'clerk:<Clerk user id>')`.

| Situation | Result |
|---|---|
| The Clerk subject is already linked | Signs in that user |
| Not linked, and a lake-pool user has the same email | Links the subject to that user. Clerk verified the address, so it is the same person |
| Neither | Creates the user with a **managed key** (a TAAP identity Lake holds), provisions their personal customer with an `admin` grant (as password sign-up does, decision 40), and links the subject |

Policy is keyed on TAAP identities, and a Clerk user arrives without one. So on first sign-in lake
mints one: `audit::mint_custodial_identity` generates an Ed25519 seed, derives
`taap:1:ed25519:<base32 fingerprint>` the way lake's own identity is derived, and wraps the seed
under `LAKE_INSTANCE_KEK`. The seed, public key and identifier are stored in
`custodial_identities` (migration `0023_custodial_identities`, see
[`../control-plane/schema.md`](../control-plane/schema.md)). Work done in the browser does not need
the seed; lake commits drive changes itself. It is held so the managed key can sign its own
replacement: in Account → Identity, *Use my own identity* rotates the account to the person's own
liquid identity as a linked successor and destroys the seed ([identity.md](identity.md#your-own-identity),
lake#762, decision 65). Lake's own sign-up without Clerk gives the same managed key when no
identity document is sent.

## Webhooks

**Status: implemented, not deployed.** Account provisioning uses `0087_clerk_webhooks` (lake#765);
ordered suspension and the revocation watermark use `0094_clerk_revocation` (lake#948, LSR-085);
session events end only browser sessions, bound per Clerk session by `0103_clerk_session_binding`
(lake#1163).

Without webhooks a Lake account exists only from a person's first sign-in. With them, Clerk's user
events reach Lake as they happen: a user made in Clerk (an invitation, a waitlist approval, the
dashboard) has a Lake account at once, a change of email or name follows, and a deleted user's
account is disabled.

**`POST /identity/clerk-webhook`**, as Clerk (through Svix) sends it:

- **Verified with Lake's own HMAC, no SDK** (`src/api/clerk_webhooks.cpp`). The signed content is
  `<svix-id>.<svix-timestamp>.<raw body>`; the key is the base64 after `whsec_` in
  `LAKE_CLERK_WEBHOOK_SECRET`; the signature is HMAC-SHA256 in base64, and `svix-signature` lists
  one or more `v1,<signature>` (any one matching, compared in constant time, is enough, so a rotated
  secret works during the overlap). `svix-timestamp` must be within five minutes of Lake's clock.
  A refusal answers `401`.
- **Each delivery applies once.** A `svix-id` already applied is acknowledged (`200`,
  `{"applied": false, "duplicate": true}`) and not applied again; its ids are kept in
  `clerk_webhook_deliveries`. Every event is also idempotent on its own.
- **`user.created`** does what a first sign-in does, without a session: the account with a
  **managed key** (decision 65), its personal customer with an `admin` grant (decision 40), and the
  Clerk subject linked (`Store::provision_clerk_user`, the same code `sign_in_clerk` runs). An
  account that already has the subject is found again; one with the same **verified** primary email
  is linked. An unverified primary email is never used: the account is created without an email,
  and a later `user.updated` sets it once Clerk has verified it.
- **`user.updated`** applies `banned`, `locked` or `deprovisioned` as account suspension,
  revoking every Lake
  session, API/static token and device for the linked account, including token descendants.
  Suspension also withdraws agent launches and schedules, invalidates task credentials and
  leases, and blocks new delegated work and agent/MCP streams through the same account-state
  predicate as deletion. Stream cleanup follows the durable offboarding process below; an unban
  permits new work and never restores withdrawn credentials or stream settings.
  Each Clerk subject's state follows its own millisecond `data.updated_at` (event `timestamp`
  when absent). Older updates are ignored; at equal timestamps a ban, lock or deletion wins and a permissive
  update is ignored. A strictly newer unban/unlock for that subject permits new sign-ins;
  revoked credentials stay revoked. Any suspended Clerk subject linked to the account keeps it
  suspended; a deletion stays permanent. State is kept before provisioning,
  and the security update commits before a profile conflict can refuse an email change. An
  identical current delivery can retry unfinished provisioning/profile work. Linking a suspended
  subject to an existing account revokes that account's credentials before any later unban.
  `deprovisioned` is optional for older payloads; a present value must be boolean.
  It also sets the verified primary email and the display name (first and last name, else
  the username) of the account linked to the subject. It never links or relinks an account. An
  email another account has is refused.
- **`user.deleted`** disables the linked account (`users.disabled_at`) and revokes every session,
  token and device of it in one transaction. It also withdraws its agent launches and schedules,
  removes task credentials and leases, and prevents queued tasks from being claimed (lake#951,
  LSR-086). Agent credential resolution and new person-owned launches, schedules and tasks require
  an active account, including accounts disabled before this change. Creation, claims and lease
  renewals lock that account against concurrent offboarding. **Nothing is erased**: the account, its workspaces and its
  audit stay; deleting data is a separate, confirmed step. A disabled account is issued no session
  (`issue_session`) and its credentials no longer resolve (`context_for`), so Clerk sign-in, TAAP
  sign-in and tokens all stop.
  Agent stream triggers and MCP streams refuse that person's authority immediately. Their sealed
  workspace records retain pending cleanup across restarts: on the next reachable worker pass,
  agent triggers are removed and MCP streaming settings, subscriptions and pending results are
  cleared. A storage outage leaves those records for retry and grants no authority; other workspace
  data and the MCP connection remain. The same agent-workload withdrawal runs on account deletion.
- **`session.revoked`, `session.removed`, `session.ended`** end **browser sessions only**
  (lake#1163; Steve, 2026-10-24: *"don't revoke api keys on signout"*). `data.id` is the Clerk
  session that ended and `data.user_id` its user. Lake revokes, for the account linked to that user:
  - every Lake browser session bound to that Clerk session (`sessions.clerk_session_id`, set at
    exchange from the session token's signed `sid`), whenever the event arrives;
  - every **unbound** Lake browser session of the account (exchanged before lake#1163, or by a
    password or passkey sign-in), unless the event is older than the account's revocation
    watermark. Lake cannot tell which Clerk session those belong to.

  It **never** revokes API keys, static tokens, MCP keys, TAAP sessions or devices, nor anything
  derived from them: they end when the person revokes them (`revoke_session`, `revoke_device`,
  `sign_out_everywhere`), when an operator runs `revoke_user_sessions`, or when the account is
  banned, locked, deprovisioned or deleted (above). Another browser bound to another Clerk session
  stays signed in. A token **derived from the browser session that ended** (an ordinary key made
  in that browser without *Never expires*, whose `parent_session_id` is that session) ends with it,
  exactly as it does when that browser posts `/logout` ([identity.md](identity.md#token-derivation-and-revocation),
  lake#234); a key that must outlive the browser is made with *Never expires*.

  The event's millisecond `timestamp` is still a monotonic revocation watermark: it only moves
  forward, and a template minted in the revocation's second or earlier cannot be exchanged again,
  from any browser of the account. This watermark is independent of profile ordering, so a newer
  profile cannot hide a delayed revocation. Revocation is retained before the first exchange, and
  account row locks serialize it with session issuance. Replaying the ended browser's Lake bearer
  fails through the common `context_for` path.

  **Signing out everywhere.** Clerk's `signOutOfOtherSessions` (password recovery) and a sign-out
  of every device in Clerk reach Lake only as these session events, so they now end the account's
  browser sessions and **not** its API keys, MCP keys or devices. To end those too, the person calls
  **`sign_out_everywhere`** (REST `POST /v1/sign_out_everywhere`, MCP tool, any of their own
  credentials): it revokes every session, API key, static token and device of their own account,
  including the credential making the call, and answers the counts. An operator's
  `revoke_user_sessions` does the same for any user's sessions and tokens.
  Independently delegated agent work and stream settings retain their own lifecycle; bans, locks,
  deprovisioning and deletion withdraw and gate that authority as described above. Other event
  types are acknowledged and ignored.
- **Audit:** every delivery is one central event, object `clerk`, method `clerk_webhook`, action
  `webhook.<type>` (`webhook` for a refused or replayed delivery), outcome `success`, `failure` or
  `denied`, with the `svix_id`, the Clerk user and the Lake user in its details.
- **Answers:** `200` when applied, acknowledged or refused for good (no account linked, an email
  conflict), so Clerk stops retrying; `503` when the store fails, so Clerk retries; `400` for a body
  that is not a Clerk event.

**Configure the Clerk instance:** add an endpoint `https://<lake host>/identity/clerk-webhook`
subscribed to `user.created`, `user.updated`, `user.deleted`, `session.revoked`, `session.removed`
and `session.ended`, and give its signing secret to Lake as `LAKE_CLERK_WEBHOOK_SECRET`. Before rollout, the operator must vendor `db/schema.sql`,
select `clerk_revocation_guard`, add any missing event subscriptions, and inject the instance's
secret key as `LAKE_CLERK_SECRET_KEY`. No operator files are changed by lake#948.
Migration `0103_clerk_session_binding` (lake#1163) adds the nullable `sessions.clerk_session_id`, its
index and the guard `clerk_session_binding_guard`. Additive; the operator vendors the schema and
selects the guard before the image that needs it rolls. Lake sessions issued before it are unbound.
Migration 0094 revokes every existing session, token and device of Clerk-linked accounts once,
and sets their token cutoff to migration time. This requires a fresh Clerk sign-in: old ignored
webhooks were already acknowledged and cannot reliably be replayed. Reapplying the schema never
revokes credentials issued after its first application. Drain and stop old replicas before applying
this migration, then start the fixed binary: old code must not issue credentials or acknowledge
webhooks after the migration cutoff.

**Verify:** `ctest --test-dir build -R 'ClerkWebhook'` (with `LAKE_TEST_DB_URI` set). It covers
Svix's documented signature vector, a wrong secret, a tampered body, a changed id, old and future
timestamps and a replayed `svix-id`; on both stores, `user.created` giving the same account as a
first sign-in (and a later sign-in reaching it), linking by verified email only, `user.updated`,
and `user.deleted` disabling the account with its session no longer working; and the route's `404`
without the secret, `401` to a forged delivery and `200` creating the account.

`ctest --test-dir build --timeout 600 -R 'DisablingAUserRevokesAgentDelegation|DisabledUsersCannotDriveAgentOrMcpStreams'`
checks retained launch, schedule and running-task credentials, queued claims, cached stream
contexts, direct disabled-account state, repeat offboarding and another person's unaffected launch
on memory and PostgreSQL. Keep the store checks and the shared stream-person resolver together
when changing offboarding; workload cleanup alone does not protect previously disabled accounts.

## Configuration

Setting up an instance and its social connections is the runbook [`../deployment/sign-in.md`](../deployment/sign-in.md).

| Setting | Where | Value |
|---|---|---|
| `LAKE_CLERK_PUBLISHABLE_KEY` / `--clerk-publishable-key` | lake runtime | The instance's `pk_test_…` or `pk_live_…`. Public. Empty or `xxx` turns Clerk off |
| `LAKE_CLERK_SECRET_KEY` | lake runtime, an Orama platform key | The instance's server key. Required for exchange; Backend API errors fail closed. Never sent to the browser |
| `LAKE_CLERK_WEBHOOK_SECRET` / `--clerk-webhook-secret` | lake runtime, an Orama platform key | The webhook endpoint's `whsec_…` signing secret. Empty: no webhook route ([Webhooks](#webhooks)) |

Everything else is derived from the key: the publishable key is `pk_test_` or `pk_live_` followed
by base64 of the Frontend API host and a trailing `$`, so the issuer is `https://<host>` and the
JWKS is `https://<host>/.well-known/jwks.json` (`include/lake/api/clerk.hpp`).

The Clerk instance needs:

- **The `lake` JWT template**, with `aud` set to `lake` and the email and name claims:
  `{"aud": "lake", "email": "{{user.primary_email_address}}", "name": "{{user.full_name}}"}`,
  lifetime 300 seconds.
- **Email sign-in** with verification at sign-up, so the email a token carries is verified.
- **Sign-up mode**, chosen per instance in Clerk; lake follows it (lake#1116). The web app reads
  the mode clerk-js fetched from the instance (`userSettings.signUp.mode`,
  `frontend/src/lib/clerkSignIn.ts`), never a setting of its own. Under `restricted` or `waitlist`
  the sign-in and sign-up pages and the visitor homepage say the invitation sentence,
  `INVITATION_NOTE`, and the homepage offers a visitor Sign in only. Under `public` (lake.live since
  2026-10-24) no page mentions invitations and the homepage offers **Create an account**. Lake's own
  password sign-up stays off under Clerk either way (`password_sign_up: false`); people sign up
  through Clerk. The API enforces it (lake#1098): in the `lake` pool `sign_up_password`,
  `sign_in_password`, `verify_mfa` and `recover_with_code` answer `403 forbidden` under Clerk, by
  the same rule `viewer` reports (`lake_pool_password_sign_in`). A password account could otherwise
  be registered under someone's email before their first Clerk sign-in links to it (LSR-011,
  lake#920). The per-environment table is in
  [`../deployment/users.md`](../deployment/users.md#what-the-pages-say-about-sign-up).

### The instance lake.oramadev.net uses

Clerk application `lake-testing` (`app_3JpNNpVUwgRgnkItIPbzzRWa2BP`, first named `lake`) in the
`steve@orama.com` workspace, development instance `ins_3JpNNvZd4jnehvW6XsKWcMq6MKx`
(`above-seahorse-7535.clerk.accounts.dev`), created 2026-09-25. Template
`jtmp_3JpNeutaiIPwfTcdSz8BXC1SEYW`; sign-up `restricted`; email-code sign-in, password optional. Add
a person with `clerk users create --email <address>` from a directory linked to that application
(`clerk link --app app_3JpNNpVUwgRgnkItIPbzzRWa2BP`). Before any Clerk command, run `clerk whoami`
and confirm the linked application; the same email holds several workspaces. Every environment's
instance is listed in [`../deployment/sign-in.md`](../deployment/sign-in.md).

## Verify

```sh
ctest --test-dir build -R 'Clerk|HttpServer'   # with LAKE_TEST_DB_URI set, as in docs/build/testing.md
curl -s https://<host>/sign-in | grep data-clerk-publishable-key
```

`Clerk.*` covers the key parsing, template-token verification (wrong audience, issuer, signer,
expiry and tampering are refused), managed-key minting, and the sign-in contract on both stores
(create, repeat, link by email, refuse without an identity). `HttpServer.Clerk*` covers the pages,
a refused token, logout, and the exchange being absent without Clerk.
`Clerk.ATemplateTokenWithANullNameOrEmailVerifies`, `Clerk.ARefusedTokenSaysWhyAndSummarizesWithoutPersonalClaims`
and `HttpServer.ClerkExchangeAcceptsANamelessPersonAndLogsWhyATokenWasRefused` pin lake#1115: the
token shape lake.live issued for a nameless person, each refusal's reason, and a log line without
the token. `frontend/tests/clerk-sign-in.test.ts` pins the page holding after a refused exchange
and the sign-up wording per mode (lake#1116). The browser flow itself
needs a real Clerk instance: sign in at `/sign-in` with an invited address and confirm you land on
`/repositories` with a `lake_session` cookie.

`Stores/ClerkRevocation.*` runs signed session events, bans, locks, Directory Sync deprovisioning, ordered unban/unlock,
pre-exchange delivery, old-template replay, token descendants, provisioning retries and suspended
account linking, a suspended account's email recovery queueing no mail, and a delayed ban after
another subject's newer profile on memory and PostgreSQL stores.
`Stores/ClerkRevocation.SessionEventsEndOnlyBrowserSessions` pins lake#1163: each of the three
session events ends the browser bound to its Clerk session (and unbound browsers) while another
browser, API keys and their descendants, MCP keys and their descendants, and every device keep
working; a delayed event still ends its own browser but not a newer unbound sign-in; the watermark
still refuses old templates. `Stores/ClerkRevocation.SignOutKeepsKeysAndBansStillEndEverything`
checks a ban, a lock and a deletion still end keys, derived keys and devices.
`Clerk.TheSessionTokenBindsTheBrowserToItsClerkSession` runs the exchange route with a session token
(refusing the template in its place, another user's token, a missing `sid` and any `aud`), and
`Clerk.SignOutEverywhereEndsEveryKeyAndDeviceOfOnlyThatPerson` the person's own revocation.
`Clerk.BackendStatusFailsClosedAndUsesOnlyTheFixedApiOrigin` checks malformed and failed Backend
API responses; `ClerkWebhooks.MigrationRevokesLegacyCredentialsOnceAndRejectsOldTemplates`
checks the one-time upgrade, native-account isolation, old-template rejection and repeatability.
`Clerk.TemplateExchangeChecksLiveAccountStateWithoutCaching` exercises the real
exchange route with signed template tokens and an isolated HTTP double. Run
`ctest --test-dir build -R 'Clerk' --timeout 600 --output-on-failure` with PostgreSQL configured.

## Change it

- A different instance: change `LAKE_CLERK_PUBLISHABLE_KEY` and create the `lake` template there.
- Public or restricted sign-up: set the instance's `auth_access_control.sign_up_mode`. The pages
  follow on the next load; nothing in lake changes.
- Keep the verifier strict: any change to the claims lake reads belongs in both the template and
  `serve_clerk_session`, with a test in `tests/test_clerk.cpp`.

## Bounded key refresh (lake#956, LSR-090)

Clerk and generic OIDC share `OidcJwks`. Before any network request, it rejects tokens over
16 KiB, malformed JWTs, absent key IDs, unsupported algorithms and invalid issuer, audience,
nonce or time claims, including Clerk's required positive `iat`. Present time claims must be
integers within the verifier's signed range; token JSON nesting is limited to 16 levels.
A failed signature with a cached key never refreshes the JWKS. An unknown
key ID can trigger one fetch per URL and address policy per 60 seconds, including failed fetches.
Only one fetch can run at a time; other requests needing that fetch fail closed and can retry.
A valid token using keys older than an hour also renews through this same gate. Key rotation can
therefore require a retry after the cooldown. A failed refresh never authenticates an unknown key.

Verification, JWKS fetching and the uncached Backend API account-status check run on the bounded
blocking pool, so a slow Clerk endpoint does not
hold the inbound I/O thread. An offloaded task owns its cache and transport through shutdown,
even if its response is abandoned. All JWKS HTTP requests have the OIDC ten-second deadline.
`ServerOptions.oidc_jwks_fetch` substitutes the transport in tests.

`POST /identity/clerk-session` accepts 60 attempts per minute per client address, counting
successes and failures before reading the body. Further attempts answer 429 with `Retry-After: 60`
and do not add an authentication audit row. The address comes from the trusted ingress hop, as the
order sheet's does ([orders](orders.md#abuse-protection), lake#838): `X-Real-IP`, which
ingress-nginx always sets itself, else the last `X-Forwarded-For` hop. The socket peer is the
ingress pod, so keying on it would put every user in one bucket. A client that reaches Lake
without either header shares one `direct` bucket, which is still limited. Lake trusts these
headers only because the ingress is its one way in; a deployment that exposes Lake's port
directly lets a sender choose its bucket. The address map holds at most 4,096 active buckets,
refuses new addresses when full, and releases buckets after a minute without evicting active
limits.

Verify with `ctest --test-dir build --timeout 600 -R 'OidcJwks|HttpServer.Clerk'`.
The fake fetch tests send 100 invalid tokens per case, advance a monotonic clock across the
cooldown, hold one refresh open during concurrent attempts, and exercise key rotation and failed
fetches. The HTTP tests hold JWKS and Backend API requests open while health requests complete,
and send 61 exchanges from each of two ingress-reported addresses and a direct client over one
socket peer. Change the limits in `oidc_jwks.cpp` and `http_server.cpp`, updating these tests and
this contract together.
