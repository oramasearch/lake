# Identity pages

## What

The identity pages of the web app call the identity-core capabilities directly, as `POST /v1/<name>`
with a JSON body. A page cannot do anything the REST and MCP surfaces cannot. Identity calls
belong to no workspace and always go unprefixed. Files are under `frontend/src/pages/account/`.

| Page | Route | File | Capabilities |
|---|---|---|---|
| Sign up | `/sign-up` | `SignUpPage.tsx` | `sign_up_password` (no identity document: the account gets a managed key, lake#762) |
| Sign in | `/sign-in` | `SignInPage.tsx` | `sign_in_password` (then `verify_mfa` when it answers `mfa_required`), `start_oauth` |
| Sign in under Clerk | `/sign-in`, `/sign-up` | `ClerkSignIn.tsx` | none; Clerk's token is posted to `POST /identity/clerk-session` |
| MFA challenge (signed out) | `/mfa` | `MfaPage.tsx` | `verify_mfa` with a TOTP or one-use recovery code |
| MFA management (signed in) | `/mfa` | `MfaPage.tsx` | `enroll_mfa`, `disable_mfa` |
| Passkey sign-in | `/passkey-sign-in` | `PasskeySignInPage.tsx`, `webauthn.ts` | `begin_passkey_sign_in`, `sign_in_passkey` |
| Passkey registration | `/passkeys` | `PasskeysPage.tsx`, `webauthn.ts` | `begin_passkey_registration`, `register_passkey` |
| Sessions and devices | `/sessions` | `SessionsPage.tsx` | `list_sessions`, `revoke_session`, `list_devices`, `revoke_device` |
| Identity | `/settings?tab=identity` | `IdentityPanel.tsx` | `account_identity`, `begin_own_identity`, `use_own_identity` |
| Tokens | `/tokens` | `TokensPage.tsx` | `list_sessions`, `create_token`, `revoke_session` |
| Operator sign in | `/login` | `LoginPage.tsx` | none; an ordinary form posted to `POST /session` |

- **Secrets are shown once and never stored.** A new token's secret, the MFA authenticator URI and
  the recovery codes live only in that page's memory and are gone when it is left. Re-enrollment
  requires the current password and replaces the current seed and recovery codes; disabling
  requires a fresh TOTP or an unused recovery code.
- **Passwords and identity documents never go in a URL.** They travel in the JSON body.
- **The MFA step keeps its input in memory.** When `sign_in_password` answers `mfa_required`, the
  page asks for the code and calls `verify_mfa` with the email and password it still holds.
- **Sessions mark the current one.** `viewer.session_id` names the session this browser uses;
  revoking it signs this browser out and opens `/sign-in`. Revoking a device invalidates both its sessions and its
  linked passkey credential. Passkey devices carry the credential ID and lake-signed TAAP binding
  on the `list_devices` response.
- **Passkeys are hidden under Clerk.** The account menu omits Passkeys when `viewer.clerk.enabled`.

## Where sign-in goes

Sign-in pages leave with a full page load (`leaveFor` in `signIn.ts`), so the whole app and its
`viewer` read start again as the new session. The destination (`returnTarget`):

1. `?return_to=` when it is a Ship app's `/apps/authorize?…` (set by that route, lake#561;
   [web-apps.md](../api/web-apps.md#lake-protected-app-authentication));
2. else `?next=` when it is a same-origin path (one leading `/`, never `//host` or `/\host`) — a
   protected page sends a visitor to `/sign-in?next=<that page>`;
3. else the page's default: the home, Ask Orama (`/`), after an account signs in (lake#600;
   `landingAfterSignIn` in `pages/ask/home.ts`), `/repositories` for the operator and after sign-up.

Links between sign-in pages carry `next` and `return_to` along. Under Clerk, the pending return is
posted with Clerk's token as `next` ([clerk.md](../api/clerk.md#how-it-works)). OAuth sends the
browser to the provider's `authorization_url`; lake's callback sets the cookie. An OAuth first
sign-in that brings an identity document first calls `begin_identity_proof`, shows the challenge and
the `liquid sign artifact` command, and sends what it printed as `start_oauth`'s `identity_proof`
([identity.md](../api/identity.md#proving-you-hold-the-key)).

## Browser sessions

Successful password, TOTP, OAuth, Clerk and passkey sign-in sets
`lake_session=<secret>; HttpOnly; SameSite=Lax; Path=/`, plus `Secure` when the request arrived
through HTTPS. The REST session-issuing capabilities set the cookie themselves, so the page never
sees or stores the secret. Every request resolves the cookie through `context_for`, the same code
that resolves an API bearer, so revoking a session or device takes effect on the next call. A
cookie-authenticated write must come from lake's own origin and be `application/json`
([cross-site requests](../serving/http.md#cross-site-requests)).

**Sign out** is a form posted to `POST /logout` (account menu, `signOut` in `components/Shell.tsx`):
it revokes the session and clears the cookie, then follows the server's redirect (under Clerk,
its sign-out page).

**The operator** signs in at `/login` with the operator token. `POST /session` compares it in
constant time and sets the same cookie; the token never passes through the page's script. An
operator credential has no account pages: they show *This page belongs to an account*.

## Application identity

An operator registers identity applications at `/applications` (`pages/admin/ApplicationsPage.tsx`),
choosing the workspace of an account that will hold the application's records (it never changes). The
application page (`/application?id=`) links its users (`/application/users`), organizations
(`/application/organizations`), lifecycle events and webhook registration
(`/application/events`). Its settings, users, organizations, events with their delivery attempts,
and webhooks with their sealed secrets live in that workspace's Zetta (lake#351–lake#356); each
page reads the application first (`get_application`) and then its list in that workspace. A new
webhook's secret is shown once. Every call is an application capability, authorized on the
application's policy object and recorded by the signed audit path.

**End users never see the web app.** They enter through `/apps/<application-id>/sign-up`,
`/sign-in`, `/mfa` or `/passkey-sign-in`, or a configured custom host's root entry paths, where lake
picks the application from the request host. Lake still renders these pages itself, in the
application's colours, and they work without JavaScript: their data cannot be read before sign-in,
and they need the request host for the passkey relying party, the issuer and the OAuth callback.
Each form supplies the application's pool to the ordinary identity capability. Browser origins
must match the hosted origin or an allowed origin; OAuth redirects must match a registered redirect
URI. Successful sign-up and sign-in return the application access token as well as the HttpOnly
session cookie. See [../api/apps.md](../api/apps.md).

## First account

`/sign-up` links Liquid installation and `liquid identity show --document`, describes the public
base64url document, and states that the private key stays on the client. Invalid encoding and
truncated CBOR get the capability's specific recovery message. Success opens `/repositories`, where
the new owner creates a workspace and then a private repository. See
[sign-up-walkthrough.md](sign-up-walkthrough.md) and [../api/provisioning.md](../api/provisioning.md).

## How to verify

```sh
cmake --build build
ctest --test-dir build --output-on-failure -R 'IdentityHttp|WebAuthn|Application|WebApp'
```

`HttpServer/HttpResponseIsolation.SignInReturnsWhereTheUrlSaysAndOnlyThere`
checks `return_to` and `next` and refuses foreign returns. Application tests cover themed hosted
forms, the raw JWKS route, pool isolation, JWT audience verification, webhook retry history, and
operator user suspension.

In a browser: sign up, enroll MFA on `/mfa`, sign out, sign in with the password and a code,
register a passkey and sign in with it, create and revoke a token on `/tokens`, and revoke the
current session on `/sessions` (the page goes to `/sign-in`). Revoke the session from another
browser instead, and this one's next call shows *Your session has ended*.

## How to change

Add behaviour to the registry first, then call it from the page with `act`/`useRead` and
`NO_WORKSPACE`. Never add a sign-in path that sets the cookie outside a capability or the server's
sign-in routes ([README.md](README.md#what-lake-serves)). Update this table and the page table in [README.md](README.md#pages).

The MFA page warns that legacy 12-character recovery codes are retired and asks the person to
enter their current password and re-enroll for new `rc1_` codes (lake#969). Passwords, TOTP and
passkeys remain available. The recovery-code list wraps long codes inside each grid cell.
Authentication throttles use the existing error notice; retry after one minute. See [attempt limits](../api/identity.md#authentication-attempt-limits).
