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

- Every capability except the documented identity entry points and `server_info` requires a
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
identities, scoped sessions, and device revocation are the default user path; workload identities
and lake acting as an OIDC issuer remain later work.
