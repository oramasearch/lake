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
   and posts it once, as an ordinary form, to `POST /identity/clerk-session`.
3. lake verifies the token with `identity_crypto::verify_oidc_id_token` against the instance JWKS
   (`<issuer>/.well-known/jwks.json`, cached for an hour and refreshed once on a miss): RS256,
   `iss` equal to the instance issuer, `aud` equal to `lake`, `exp` in the future, and `nbf` and
   `iat` not more than 60 seconds ahead. Clerk's default session token has no `aud` and is
   refused; only the template token is accepted.
4. `Store::sign_in_clerk` finds the lake user for the Clerk subject, links or creates them (below),
   and issues a 30-day session. The handler records an `authentication` audit event with object
   `clerk` and method `clerk`, success or denied, then redirects with the `lake_session` cookie
   (`HttpOnly`, `SameSite=Lax`, `Secure` over HTTPS) to the posted `next` field when it is a
   same-origin path (one leading `/`, never `//` or `/\`) or a Ship app return
   (`/apps/authorize?app=…&rd=…`), else to the `lake_sign_in_return` app return, else to
   the home, Ask Orama (`/`; lake#600, `account_landing` in `http_server.cpp`; it was `/repositories`)
   (lake#561: the web app posts the page that sent the person to sign in). An OAuth or OIDC sign-in
   in lake's own pool lands there too. A
   failure redirects to `/sign-in?error=<message>`.
5. `POST /logout` clears `lake_session` and, with Clerk on, redirects to `/sign-in?signed_out=1`,
   which signs the browser out of Clerk as well. Without that, the next visit would sign the person
   straight back in.

lake holds **no Clerk secret key**. Verification of sign-in needs only the public JWKS; the one
Clerk secret Lake may hold is the webhook signing secret ([Webhooks](#webhooks)).

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

**Status: implemented 2026-10-17 (lake#765, migration `0087_clerk_webhooks`), not deployed.**

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
- **`user.updated`** sets the verified primary email and the display name (first and last name, else
  the username) of the account linked to the subject. It never links or relinks an account. An
  email another account has is refused.
- **`user.deleted`** disables the linked account (`users.disabled_at`) and revokes every session,
  token and device of it in one transaction. **Nothing is erased**: the account, its workspaces and its
  audit stay; deleting data is a separate, confirmed step. A disabled account is issued no session
  (`issue_session`) and its credentials no longer resolve (`context_for`), so Clerk sign-in, TAAP
  sign-in and tokens all stop.
- Other event types are acknowledged and ignored. `session.revoked` is not acted on: a Lake session
  is Lake's own, not tied to a Clerk session id.
- **Audit:** every delivery is one central event, object `clerk`, method `clerk_webhook`, action
  `webhook.<type>` (`webhook` for a refused or replayed delivery), outcome `success`, `failure` or
  `denied`, with the `svix_id`, the Clerk user and the Lake user in its details.
- **Answers:** `200` when applied, acknowledged or refused for good (no account linked, an email
  conflict), so Clerk stops retrying; `503` when the store fails, so Clerk retries; `400` for a body
  that is not a Clerk event.

**Configure the Clerk instance:** add an endpoint `https://<lake host>/identity/clerk-webhook`
subscribed to `user.created`, `user.updated` and `user.deleted`, and give its signing secret to the
Lake as `LAKE_CLERK_WEBHOOK_SECRET` (orama-operator#541 does both per instance).

**Verify:** `ctest --test-dir build -R 'ClerkWebhook'` (with `LAKE_TEST_DB_URI` set). It covers
Svix's documented signature vector, a wrong secret, a tampered body, a changed id, old and future
timestamps and a replayed `svix-id`; on both stores, `user.created` giving the same account as a
first sign-in (and a later sign-in reaching it), linking by verified email only, `user.updated`,
and `user.deleted` disabling the account with its session no longer working; and the route's `404`
without the secret, `401` to a forged delivery and `200` creating the account.

## Configuration

Setting up an instance and its social connections is the runbook [`../deployment/sign-in.md`](../deployment/sign-in.md).

| Setting | Where | Value |
|---|---|---|
| `LAKE_CLERK_PUBLISHABLE_KEY` / `--clerk-publishable-key` | lake runtime | The instance's `pk_test_…` or `pk_live_…`. Public. Empty or `xxx` turns Clerk off |
| `LAKE_CLERK_WEBHOOK_SECRET` / `--clerk-webhook-secret` | lake runtime, an Orama platform key | The webhook endpoint's `whsec_…` signing secret. Empty: no webhook route ([Webhooks](#webhooks)) |

Everything else is derived from the key: the publishable key is `pk_test_` or `pk_live_` followed
by base64 of the Frontend API host and a trailing `$`, so the issuer is `https://<host>` and the
JWKS is `https://<host>/.well-known/jwks.json` (`include/lake/api/clerk.hpp`).

The Clerk instance needs:

- **The `lake` JWT template**, with `aud` set to `lake` and the email and name claims:
  `{"aud": "lake", "email": "{{user.primary_email_address}}", "name": "{{user.full_name}}"}`,
  lifetime 300 seconds.
- **Email sign-in** with verification at sign-up, so the email a token carries is verified.
- **Sign-up mode** `restricted` while lake is in development: people are added in Clerk. The
  sign-up page and the visitor homepage say the same sentence, `INVITATION_NOTE` in
  `frontend/src/pages/account/signIn.ts`, and no page offers a visitor *Create account* (lake#448):
  the `viewer` read says `password_sign_up: false` under Clerk.

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
a refused token, logout, and the exchange being absent without Clerk. The browser flow itself
needs a real Clerk instance: sign in at `/sign-in` with an invited address and confirm you land on
`/repositories` with a `lake_session` cookie.

## Change it

- A different instance: change `LAKE_CLERK_PUBLISHABLE_KEY` and create the `lake` template there.
- Public sign-up: set the instance's `auth_access_control.sign_up_mode` to `public`, then change
  `INVITATION_NOTE` and the Clerk branches of the web app's homepage and account menu to offer
  *Create account* again.
- Keep the verifier strict: any change to the claims lake reads belongs in both the template and
  `serve_clerk_session`, with a test in `tests/test_clerk.cpp`.
