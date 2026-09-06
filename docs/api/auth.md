# Authentication

**Today: one operator bearer plus row-resolved user sessions and API tokens.** The operator mints
its bearer once into the Secret
`lake-<name>-admin` (key `token`, 64 hex) and injects it as `LAKE_ADMIN_TOKEN`. lake compares
`Authorization: Bearer <token>` against it in constant time; a match is the `admin` identity.

Password, passkey, and OAuth sign-in issue random browser-session secrets. User API tokens are
named sessions with explicit capability scopes. Both bearer values and the HttpOnly
`lake_session` cookie go through `context_for` on every request, so session or device revocation
takes effect on the next request. See [identity.md](identity.md).
Successful browser-session REST capabilities also set that secret as an `HttpOnly`, `SameSite=Lax`
cookie (`Secure` behind HTTPS). This lets the passkey page use the REST capability without exposing
the session secret to its inline script.

- Every capability except the documented identity entry points, public transparency-monitoring
  reads, and `server_info` requires a
  resolved credential, on REST and on MCP `tools/call` alike (`Registry::call` enforces
  `requires_auth`).
- `GET /metrics` uses the same resolution code: either the bearer or the UI's `lake_session`
  cookie. `GET /healthz` and `GET /readyz` are unauthenticated so Kubernetes can probe them.
- Without a token configured, lake logs `"admin_token_set":"no"` in its JSON startup event and every guarded
  capability answers `403 unauthorized`. There is no anonymous write path.
- `GET /`, `/style.css`, `/v1/openapi.json` and MCP `initialize` / `tools/list` need no token.

Retrieve it for a Lake:

```sh
kubectl get secret lake-<name>-admin -n <namespace> -o jsonpath='{.data.token}' | base64 -d
```

The operator bearer remains a break-glass credential rather than a user identity. TAAP user
identities, scoped sessions, and device revocation are the default user path. Agent workloads use
the short-lived delegated keys in [agents.md](agents.md). Lake also accepts a fresh, one-use
libliquid TAAP-key assertion through `sign_in_taap` and acts as the issuer in an application OIDC
authorization-code flow; see [identity.md](identity.md) and [apps.md](apps.md).

## What the exit scenario proves

The Postgres identity-exit scenario starts with a TAAP-key session, completes a one-use code flow
with exact redirect matching and PKCE S256, and has the relying party validate the returned ID
token's EdDSA signature, issuer, audience, nonce, and `kid` against Lake's public application JWKS.
It also proves certificate issue, rotation, revocation enforcement, and dynamic-secret lease
lifecycle calls all traverse the same authorization and signed audit path.
