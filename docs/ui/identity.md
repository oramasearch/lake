# Identity pages

## What

The identity UI exposes the identity-core capabilities as server-rendered pages:

| Page | Route | Capability path |
|---|---|---|
| Sign up | `GET /sign-up`, `POST /identity/sign-up` | `sign_up_password` |
| Sign in | `GET /sign-in`, `POST /identity/sign-in`, `POST /identity/oauth` | `sign_in_password`, `start_oauth` |
| MFA challenge | `GET /mfa`, `POST /identity/mfa/verify` | `verify_mfa` with a TOTP or one-use recovery code |
| MFA management | `GET /mfa`, `POST /identity/mfa/enroll`, `POST /identity/mfa/disable` | `enroll_mfa`, `disable_mfa` |
| Passkey sign-in | `GET /passkey-sign-in` | `begin_passkey_sign_in`, `sign_in_passkey` |
| Passkey registration | `GET /passkeys` | `begin_passkey_registration`, `register_passkey` |
| Sessions and devices | `GET /sessions`, `POST /identity/session/revoke`, `POST /identity/device/revoke` | `list_sessions`, `revoke_session`, `list_devices`, `revoke_device` |
| Tokens | `GET /tokens`, `POST /identity/token/create` | `create_token`, `list_sessions`, `revoke_session` |

The `POST /identity/*` routes are form adapters. They construct the same JSON input and call the
single registry, so a page cannot perform an action the REST and MCP surfaces do not expose.
Passwords and TAAP documents are never put in a URL. A newly created token is shown once, escaped
inside the response page. MFA enrollment likewise shows the authenticator URI and recovery codes
once, with every value HTML-escaped. Re-enrollment replaces the current seed and recovery codes;
disablement requires a fresh TOTP or unused recovery code.

The sessions page resolves session and device rows by the authenticated user ID. Passkey devices
carry the credential ID and lake-signed TAAP binding on the `list_devices` API response; revoking
one invalidates both its sessions and its linked passkey credential.

## Application identity

An operator registers applications at `/applications`, choosing the workspace of a space that will
hold the application's lifecycle events (it never changes), and follows the application detail
page to pool users, application organizations, lifecycle events, and webhook registration. The
event log reads the events and their delivery attempts from that workspace's Zetta (lake#356). Those pages
call the application capabilities through the registry and use the application policy object for
authorization. Protected application mutations are recorded by the same signed audit path.

End users enter through `/apps/<application-id>/sign-up`, `/sign-in`, `/mfa`, or
`/passkey-sign-in`. A configured custom host serves the corresponding root entry paths and lake
selects the application from the request host. Each form supplies the application's pool to the
ordinary identity capability. Browser origins must match the hosted origin or an allowed origin;
OAuth redirects must match a registered redirect URI. Successful sign-up and sign-in return the
application access token as well as the HttpOnly session cookie.

## Browser sessions

Successful password, TOTP, OAuth, and passkey sign-in sets
`lake_session=<secret>; HttpOnly; SameSite=Lax; Path=/`, plus `Secure` when the request arrived
through HTTPS. The REST session-issuing capabilities set the cookie too. Every protected page
resolves it through `context_for`, so revoking its session or device takes effect on the next
request. `/login` remains the separate operator-token form, but it resolves through that same
request context. An operator credential is redirected to `/repositories` instead of entering the
user-only identity pages.

## Passkey script exception

Every page includes the shared pre-paint theme preference and theme-toggle scripts described in
[`design-system.md`](design-system.md). `/passkeys`, lake's `/passkey-sign-in`, and each application
passkey sign-in page add exactly one inline script. The passkey script converts
base64url fields to browser byte arrays, calls `navigator.credentials.create` or
`navigator.credentials.get`, and posts the result to the registered REST capabilities. It does
not write the session cookie; the REST response sets an `HttpOnly` cookie. Application themes
change validated CSS colors and escaped text only.

## How to verify

```sh
cmake --build build
ctest --test-dir build --output-on-failure -R 'WebUi|IdentityHttp|WebAuthn|Application'
```

The renderer tests cover MFA enrollment, challenge and disable forms, one-use value escaping,
the two shared theme scripts, and the additional passkey script. Application tests cover themed
hosted forms, the raw JWKS route, pool isolation, JWT audience verification, webhook retry
history, and operator user suspension.
`IdentityHttp.PagesIssueAndResolveHttpOnlySessions` submits the sign-up form, checks the
cookie attributes, loads the authenticated session row on the sessions page, and checks that a
REST sign-up response also sets an HttpOnly cookie.

## How to change

Add behavior to the registry first. Keep HTML generation in the pure `web_ui` renderers and make
the HTTP form adapter call that capability. Add its route to request metrics, renderer tests, this
table, and the route tables in `README.md` and `docs/serving/http.md`. Theme behavior belongs in
the shared shell; credential behavior belongs only on documented passkey pages.

## First account

`/sign-up` links Liquid installation and `liquid identity show --document`, describes the public
base64url document, and states that the private key stays on the client. Invalid encoding and
truncated CBOR get specific recovery instructions. Success redirects to `/repositories`, where
the new customer owner can create a private repository through the registry-backed form. See
[sign-up-walkthrough.md](sign-up-walkthrough.md) and
[../api/provisioning.md](../api/provisioning.md).
