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

lake holds **no Clerk secret**. Verification needs only the public JWKS.

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

## Configuration

| Setting | Where | Value |
|---|---|---|
| `LAKE_CLERK_PUBLISHABLE_KEY` / `--clerk-publishable-key` | lake runtime | The instance's `pk_test_…` or `pk_live_…`. Public. Empty or `xxx` turns Clerk off |

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

Clerk application `lake` (`app_3JpNNpVUwgRgnkItIPbzzRWa2BP`) in the `steve@orama.com` workspace,
development instance `ins_3JpNNvZd4jnehvW6XsKWcMq6MKx` (`above-seahorse-7535.clerk.accounts.dev`),
created 2026-09-25. Template `jtmp_3JpNeutaiIPwfTcdSz8BXC1SEYW`; sign-up `restricted`; email-code
sign-in, password optional. Add a person with `clerk users create --email <address>` from a
directory linked to that application (`clerk link --app app_3JpNNpVUwgRgnkItIPbzzRWa2BP`). Before
any Clerk command, run `clerk whoami` and confirm the linked application; the same email holds
several workspaces.

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
