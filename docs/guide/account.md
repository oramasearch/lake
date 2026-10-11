# Your Account and Sign-In

Your account is who you are in Lake: how you sign in, the sessions and devices you are signed in
on, your passkeys and multi-factor authentication, and the API tokens you hold. This page covers
each, on the site and through the API.

## Concepts

- **Two ways to sign in, depending on your Lake.** A hosted Lake (such as `lake.oramadev.net`)
  signs people in through Clerk, with an email code. A Lake without Clerk, such as one installed
  on your own servers, uses Lake's own sign-in: a password with optional multi-factor
  authentication, a passkey, or a configured social provider. You see one or the other, never both.
- **Your session is a cookie.** Signing in sets an HttpOnly `lake_session` cookie that the page
  never sees. Every request checks it, so revoking a session takes effect on the next request.
- **Your own account.** Signing up makes an account that owns your own workspaces, with you as its
  administrator. Organizations you join have their own ([Organizations, Members and
  Teams](/docs/organizations)).
- **Secrets are shown once.** A new token's secret, the authenticator URI and the recovery codes
  are shown once, held only by that page, and gone when you leave it.

## Create an Account

**With Clerk.** Press **Create account** in the top bar (where your Lake allows sign-up) and follow
Clerk's steps. Some lakes create accounts only by invitation; on `lake.oramadev.net`, an
administrator adds people.

**With Lake's own sign-up** (`/sign-up`):

1. Install liquid and run `liquid identity show --document`. liquid creates your identity on first
   use (or exports your existing one) and prints its public document. The private key stays on your
   computer.
2. Fill in **Email**, **Display name** (optional), **Password**, **Device name** and **TAAP/1
   identity document** (the printed document).
3. Press **Create account**. You land on **Data → Repositories**, signed in.

- REST: `POST /v1/sign_up_password` with this body:

```json
{"email": "you@example.com", "password": "<password>", "display_name": "Your Name", "device_name": "laptop", "identity_document": "<identity document>"}
```

- MCP: `sign_up_password` with the same input.

## Sign In

**With Clerk**, press **Sign in** and follow Clerk's steps; you return to the page that sent you,
or to **Ask Orama**, Lake's home.

**With Lake's own sign-in** (`/sign-in`):

1. Enter **Email**, **Password** and **Device name**, and press **Sign in**.
2. If multi-factor authentication is on, enter a **TOTP or recovery code** and press **Verify and
   sign in**.
3. Or choose **Use a passkey**, or a configured social **Provider** and **Continue to provider**.

You return to the page that sent you to sign in, or to **Ask Orama**, Lake's home, by default.

Through the REST API, send `POST /v1/sign_in_password` with this body:

```json
{"email": "you@example.com", "password": "<password>", "device_name": "laptop"}
```

An account with multi-factor authentication answers `mfa_required`. Then send the code through
`POST /v1/verify_mfa`:

```json
{"email": "you@example.com", "password": "<password>", "code": "123456", "device_name": "laptop"}
```

Through MCP, the tools are `sign_in_password` and `verify_mfa`, with the same inputs.

Scripts and AI agents do not sign in this way; they use a token ([Tokens and Keys](/docs/tokens)).

## Your Settings

Open the account menu in the top bar and choose **Settings** (`/settings`). Its tabs:

| Tab | What it shows |
|---|---|
| **Profile** | Your name, handle and identity, and whether multi-factor authentication is enabled |
| **Sessions and devices** | How many sessions and devices you have, linking to **Sessions** |
| **Passkeys** | Signing in with a passkey, linking to **Passkeys** (hidden when your Lake uses Clerk) |
| **Tokens** | Your API tokens, linking to **Tokens** |
| **Notifications** | No delivery preferences are configured |

The same menu has **Sessions**, **Passkeys**, **Tokens** and **Sign out**.

## Multi-Factor Authentication

On a Lake with its own sign-in, open `/mfa` from **Settings → Profile**.

1. Press **Enroll or replace MFA**.
2. Add the **Authenticator URI** to your authenticator app.
3. Save the **Recovery codes** somewhere safe. Each works once, in place of a code. They are not
   shown again.

Enrolling again replaces the old authenticator and codes. To turn it off, enter a **Current TOTP or
recovery code** under **Disable MFA** and press **Disable MFA**.

- REST: `POST /v1/enroll_mfa` with an empty body, `{}`. To disable, `POST /v1/disable_mfa` with
  `{"code": "123456"}`.
- MCP: `enroll_mfa` and `disable_mfa`.

## Passkeys

On a Lake with its own sign-in, open **Passkeys** from the account menu.

1. Enter a **Passkey name** after the device or key, so you can tell it apart later.
2. Press **Register passkey** and follow your browser's prompt.

A registered passkey appears as a device on **Sessions**, where you can revoke it. To sign in with
it, choose **Use a passkey** on the sign-in page.

## Sessions and Devices

Open **Sessions** from the account menu.

- **This session** is the browser you are using. **Revoke and sign out** ends it after a
  confirmation.
- **Sessions** lists your other sessions and tokens, with their scopes, expiry and status; filter by
  **Active** or **Revoked**. **Revoke** ends one on its next request.
- **Devices** lists the devices you signed in from, including passkeys. Revoking a device ends its
  sessions and its passkey.

- REST: `GET /v1/list_sessions` and `GET /v1/list_devices` list them. `POST /v1/revoke_session` with
  `{"id": "<session id>"}` revokes a session or token. `POST /v1/revoke_device` with
  `{"id": "<device id>"}` revokes a device.
- MCP: `list_sessions`, `list_devices`, `revoke_session`, `revoke_device`.

## Tokens

**Tokens** (account menu, or **Developer → API**) makes API tokens for scripts, `liquid` and AI
agents. [Tokens and Keys](/docs/tokens) covers scopes, lifetimes and never-expiring tokens.

## Sign Out

Choose **Sign out** in the account menu. Lake revokes the session and clears the cookie; under
Clerk, you also leave Clerk's session.

## Permissions

Everything on this page acts on your own account only: your sessions, devices, passkeys,
authenticator and tokens. Nobody else can list or revoke them, except that removing you from an
organization or deactivating you through SCIM revokes your sessions there
([Single Sign-On and SCIM](/docs/sso)).

## Limits

- A session from Clerk sign-in lasts 30 days.
- A token lasts as long as the session that made it, at most 30 days, unless your account allows
  never-expiring tokens.
- Passkeys and Lake's own multi-factor authentication are not offered when your Lake uses Clerk;
  Clerk handles sign-in there.

## Troubleshooting

| You see | What to do |
|---|---|
| *Your session has ended* with **Sign in again** | Your session expired or was revoked. Sign in again; you return to the same page. |
| `unauthenticated` (401), `reason` `expired` | The session or token's lifetime passed. Sign in again or make a new token. |
| `unauthenticated` (401), `reason` `invalid` | The secret is unknown, mistyped or revoked, or its device was revoked. |
| `mfa_required` (403) | Finish multi-factor verification with a TOTP or a recovery code. |
| *This page belongs to an account* | You signed in with the operator token; account pages need a person's account. |
| A sign-in link sent you somewhere unexpected | Lake returns only to a page of its own origin, never to another site. |

## Related Pages

- [Tokens and Keys](/docs/tokens)
- [Organizations, Members and Teams](/docs/organizations)
- [Single Sign-On and SCIM](/docs/sso)
- [Activity and Audit](/docs/activity)
- [Security Model](/docs/security)
