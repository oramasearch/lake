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

1. Fill in **Email**, **Display name** (optional), **Password** and **Device name**.
2. Press **Create account**. You land on **Data → Repositories**, signed in.

Either way, Lake gives your account a **managed key**: a temporary signing identity Lake holds for
you, so you need nothing installed to start. You can replace it with your own key later ([Your
Identity](#your-identity)).

- REST: `POST /v1/sign_up_password` with this body:

```json
{"email": "you@example.com", "password": "<password>", "display_name": "Your Name", "device_name": "laptop"}
```

  To sign up with your own identity from the start instead, prove you hold its key: `POST
  /v1/begin_identity_proof` with `{"identity_document": "<output of liquid identity show
  --document>"}`, run the `command` it answers (`liquid sign artifact <challenge>`), then add
  `"identity_document"` and `"identity_proof": {"nonce": "<nonce>", "expires_at": <expires_at>,
  "signature": "<everything liquid sign printed>"}` to the sign-up body within five minutes.
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

## Regions

Lake on Orama Cloud runs in more than one region, for example Europe (`eu.lake.live`) and the
United States (`us.lake.live`). Each region is a separate Lake with its own accounts:

- **Your account is in one region.** The sign-in and sign-up pages say which region the account is
  in or is created in, with links to the other regions. An account in Europe does not sign in to
  the United States; create one there if you need it.
- **The top bar shows your region.** Its label (for example **Europe**) opens a menu with a link to
  each other region's Lake.
- **The front door picks for you.** Opening `lake.live` shows a page that preselects the region
  closest to you, or the one you chose there before. Press **Continue** to go to that region at the
  same address; Lake remembers the choice in a cookie that holds only the region's name.
- Where Lake has no regions (an installation on your own servers, or a development Lake), none of
  this is shown.

Through the API, `server_info` answers `region` (`{name, label}`, or `null`) and `regions`
(`[{name, label, host}]`): `GET /v1/server_info`, or the MCP tool `server_info`, with no sign-in.

## Your Settings

Open the account menu in the top bar and choose **Settings** (`/settings`). Its tabs:

| Tab | What it shows |
|---|---|
| **Profile** | Your name, handle and identity, and whether multi-factor authentication is enabled |
| **Identity** | Which key your account signs with, and **Use my own identity** ([Your Identity](#your-identity)) |
| **Sessions and devices** | How many sessions and devices you have, linking to **Sessions** |
| **Passkeys** | Signing in with a passkey, linking to **Passkeys** (hidden when your Lake uses Clerk) |
| **Tokens** | Your API tokens, linking to **Tokens** |
| **Notifications** | No delivery preferences are configured |

The same menu has **Sessions**, **Passkeys**, **Tokens** and **Sign out**.

## Your Identity

Open **Settings → Identity** (`/settings?tab=identity`). It shows your **account identity** (the
identifier your access, activity and repositories belong to), which key it signs with, and what it
signs as.

- **Managed key**: Lake holds a temporary key for you. Every new account starts this way.
- **Your own key**: your account signs with the liquid identity on your computer, and Lake holds no
  key for you.

**Use my own identity** (optional, and permanent):

1. Install liquid and run `liquid identity show --document`. Paste its output into **Your identity
   document** and press **Continue**.
2. Lake shows a command, `liquid sign artifact sha256:…`. Run it within ten minutes and paste
   everything it prints into **What liquid sign printed**.
3. Press **Use my own identity**. Lake checks the signature, links your own identity to your
   account, and destroys the managed key. Your access, activity and repositories stay as they were.

Afterwards, changes you push with liquid are recorded as yours, and you can sign in with your key.
An identity can belong to one account only, and an identity you have already rotated with
`liquid identity rotate` cannot be linked yet.

- REST: `GET /v1/account_identity`; `POST /v1/begin_own_identity` with
  `{"identity_document": "<document>"}`; `POST /v1/use_own_identity` with
  `{"challenge": "sha256:…", "signature": "<what liquid sign printed>"}`.
- MCP: `account_identity`, `begin_own_identity`, `use_own_identity`.

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
