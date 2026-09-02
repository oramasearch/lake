# Authentication

**Today: one admin bearer token per Lake.** The operator mints it once into the Secret
`lake-<name>-admin` (key `token`, 64 hex) and injects it as `LAKE_ADMIN_TOKEN`. lake compares
`Authorization: Bearer <token>` against it in constant time; a match is the `admin` identity.

- Every capability except `server_info` requires it, on REST and on MCP `tools/call` alike
  (`Registry::call` enforces `requires_auth`).
- Without a token configured, lake logs `admin token: NOT SET` at start and every guarded
  capability answers `403 unauthorized`. There is no anonymous write path.
- `GET /`, `/style.css`, `/v1/openapi.json` and MCP `initialize` / `tools/list` need no token.

Retrieve it for a Lake:

```sh
kubectl get secret lake-<name>-admin -n <namespace> -o jsonpath='{.data.token}' | base64 -d
```

**Next.** TAAP identities as the model (lake issue #4): per-user and per-agent keys instead of
one admin bearer, lake as OIDC issuer, scoped tokens. The registry already carries the acting
identity in `ApiContext`; only the resolution changes.
