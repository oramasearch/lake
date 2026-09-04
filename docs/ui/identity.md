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

## Browser sessions

Successful password, TOTP, OAuth, and passkey sign-in sets
`lake_session=<secret>; HttpOnly; SameSite=Lax; Path=/`, plus `Secure` when the request arrived
through HTTPS. The REST session-issuing capabilities set the cookie too. Every protected page
resolves it through `context_for`, so revoking its session or device takes effect on the next
request. `/login` remains the separate operator-token form, but it resolves through that same
request context. An operator credential is redirected to `/repositories` instead of entering the
user-only identity pages.

## Passkey script exception

`/passkeys` and `/passkey-sign-in` each contain exactly one inline script. The script converts
base64url fields to browser byte arrays, calls `navigator.credentials.create` or
`navigator.credentials.get`, and posts the result to the registered REST capabilities. It does
not write the session cookie; the REST response sets an `HttpOnly` cookie. Every other identity
page has no script.

## How to verify

```sh
cmake --build build
ctest --test-dir build --output-on-failure -R 'WebUi|IdentityHttp|WebAuthn'
```

The renderer tests cover MFA enrollment, challenge and disable forms, one-use value escaping,
absence of scripts, and the one-script passkey exception.
`IdentityHttp.PagesIssueAndResolveHttpOnlySessions` submits the sign-up form, checks the
cookie attributes, loads the authenticated sessions page, and checks that a REST sign-up response
also sets an HttpOnly cookie.

## How to change

Add behavior to the registry first. Keep HTML generation in the pure `web_ui` renderers and make
the HTTP form adapter call that capability. Add its route to request metrics, renderer tests, this
table, and the route tables in `README.md` and `docs/serving/http.md`. Do not add another script
outside the two passkey pages.
