# The HTTP server

**What.** lake serves HTTP/1 with Beast2 on the Capy + Corosio runtime, the stack Zetta serves
with. Port 4900 by default (`--listen HOST:PORT`); every flag and environment variable is in
[`configuration.md`](configuration.md).

**Where.** `include/lake/api/server.hpp` is the public shape (`ApiServer`, `ServerOptions`) and
names no HTTP type. `src/api/http_server.cpp` is the one translation unit that includes Beast2,
`boost::http` or Boost.JSON. Pages are pure renderers in `src/api/web_ui.cpp` (string in, string
out) and are the unit-tested surface.

**Lifecycle.** `ApiServer::start()`: build the router → construct
`beast2::http_server(io_context, workers, router, parser_cfg, serializer_cfg)` → `bind` →
`local_endpoint().port()` → `start()` → an `io_thread` runs `io_context.run()`. `stop()`:
`server->stop()`, `io_context.stop()`, join. The owner loop calls `wait_for_work()` then
`drain()` each tick; both are placeholders until lake has a node to marshal onto.

**Routes.**

| Route | Serves |
|---|---|
| `GET /` | the hello page in the server-rendered Orama application shell |
| `GET /style-<hash>.css`, `GET /fonts/<IBM Plex face>-<hash>.woff2` | content-hashed design-system assets (`public, max-age=31536000, immutable`) linked by every page |
| `GET /style.css`, `GET /fonts/<IBM Plex face>.woff2` | compatibility aliases for direct and older links (`no-cache`) |
| `GET /healthz`, `GET /readyz` | unauthenticated liveness and control-store readiness probes; see [`telemetry.md`](telemetry.md) |
| `GET /metrics` | Prometheus metrics, guarded by the same bearer or session cookie as capabilities |
| `GET /v1/server_info` | `{ok, value: {name, node, version, api, store}}`, no auth |
| `GET /sign-up`, `/sign-in`, `/mfa`, `/passkey-sign-in` | public password, TOTP, OAuth and passkey entry pages |
| `GET /settings`, `/sessions`, `/passkeys`, `/tokens` | authenticated profile, session/device, passkey and scoped-token management pages |
| `POST /repository/create` | authenticated form adapter for `create_repository`; creates a private repository and redirects to its home |
| `POST /identity/*` | form adapters that call the corresponding registry capability; see [`../ui/identity.md`](../ui/identity.md) |
| `GET /applications`, `/application`, `/application/users`, `/application/organizations`, `/application/events` | application registration and operator management pages |
| `GET /apps/<id>/sign-up`, `/sign-in`, `/mfa`, `/passkey-sign-in` | themed application identity pages; a configured custom host serves its equivalent entry pages |
| `POST /apps/<id>/identity/*` | application-pool identity form adapters |
| `GET /v1/apps/<id>/.well-known/jwks.json` | the application's public Ed25519 JSON Web Key Set |
| `GET /customers`, `/repositories`, `/repository?id=`, `/organizations`, `/organization?id=`, `/team?id=`, `/member?id=`, `/teams`, `/members`, `/invitations`, `/grants`, `/access`, `/search`, `/metering`, `/secret-mounts`, `/secrets`, `/encryption-keys`, `/audit` | the server-rendered pages behind the sign-in (`/login`, `POST /session`, `POST /logout`); see [`../ui/README.md`](../ui/README.md) |
| `GET /transparency?identity=<taap-id>` | public hosted identity monitor over checkpoint-verified bulk tiles |
| `GET /repository/tree`, `/file`, `/changes`, `/change`, `/issues`, `/issue`, `/releases`, `/webhooks`, `/protection`, `/conflicts`, `/artifacts`, `/artifact`; `POST /conflicts/resolve` | forge pages over authorized repository, review, issue, release, webhook, policy, signed-audit, and libliquid records; conflict pages explain kind, region, and reasons before posting an audited resolution |
| `GET /environments?repository_id=`, `/environment?repository_id=&name=`, `/logs?repository_id=&environment=` | platform summaries, deployment history, capability-backed environment/domain forms, and cursor-polling Loki-compatible logs; stale mirror reads show a warning |
| `GET /drive`, `/drive/preview`, `/drive/download`, `/drive/history`, `/drive/history/diff`, `/drive/conflict`, `/drive/share`, `/share/:token` | server-rendered drive list/grid browsing, registered previews with exact-byte fallback, explained conflict comparison, effective access, and scoped links; create, upload, rename, move, delete, restore, conflict resolution, and sharing mutations use POST under `/drive/` |
| `/api/models/*`, `/api/datasets/*`, model/dataset resolve and basic-LFS routes | the documented `huggingface_hub` create, upload, list, info, snapshot, and file-download subset; see [`../api/models.md`](../api/models.md) |
| `GET /models/:namespace/:name`, `/models/:id` | model revisions, files, deduplication savings, and verified training provenance |
| `GET /v1/<read>`, `POST /v1/<action>` | the capability registry as REST: identity and application authentication, short-lived agent-session keys, account recovery, offline grants, tenants, repositories, organization administration, policy, repository search, static secrets, encryption, TUF metadata, transparency monitoring, asynchronous replication, the lake identity and the signed audit stream; bearer required except documented public identity/recovery entry points, transparency reads, and application JWKS |
| `POST /v1/repositories/<repository-id>/sync/{find-missing,push,pull,chunks}` | streaming liquid push, pull, missing-object negotiation, and chunk upload with bounded streaming request and response bodies, through the same registry policy and audit path; offline push accepts a grant UUID and returns per-change reauthorization results; see [`../api/sync.md`](../api/sync.md) |
| `GET /v1/repositories/<repository-id>/sync/chunks/<digest>` | ranged liquid chunk reads with `offset` and `length` query parameters |
| `GET /v1/openapi.json` | OpenAPI 3.1, generated from the registry |
| `GET /v1/oauth/<provider>/callback` | OAuth authorization-code callback with one-use state, PKCE, and verified provider identity |
| `GET /sso?organization_id=<uuid>` | organization SSO provider, federation issuer policy, and SCIM provisioning evidence page |
| `/scim/v2/Users`, `/scim/v2/Groups` and `/:id` | provider-bearer SCIM 2.0 filtered list, get, create, PATCH and delete resources |
| `GET /acme/<customer-id>/directory`, ACME resource `POST`s, `HEAD /acme/<customer-id>/new-nonce`, `GET /.well-known/acme-challenge/<token>` | RFC 8555 account, HTTP-01 order, CSR finalize, polling, and certificate download |
| `POST /mcp` | MCP (JSON-RPC 2.0): `initialize`, `tools/list`, `tools/call`; the same registry as tools |
| anything else | `404 {"ok":false,"error":{"code":"not_found"}}` |

**Rules that keep it correct.**

- Every text response goes through `send_text`, which stamps headers Zetta's way: `erase(etag)`,
  `erase(content_length)`, `set_payload_size(body)`. Beast2 frames from the declared size; never
  set those by hand. Binary sync responses use the corresponding bounded byte writer.
- Routes are registered in order: `router.use(cors())`, the `router.add(...)` list, then a
  **trailing `router.use` 404 handler**. The router falls through (`route_next`) past the last
  route and nothing answers otherwise: without the trailing handler an unmatched request
  **hangs**. Keep it last.

**Idle closes.** A client closing an idle keep-alive connection is a normal end of stream and
produces no stderr output. Lake pins `oramasearch/beast2` branch `orama-main`, whose carried patch
suppresses the former `read_header error: end of stream` line. The carrier and its update procedure
are recorded in [`../build/dependencies.md`](../build/dependencies.md).

## Response isolation

Beast2 pools workers across connections. At the pinned revision, each worker retains its
`route_params::res` and resets the start line and connection policy without clearing old fields.
Lake previously overwrote only selected headers when sending a response. A sign-in response's
`Set-Cookie` and `Location` could consequently reach a different client on the next connection,
as well as a later request on a keep-alive connection. A browser accepting that leaked cookie
could access the signing user's sessions. This is a cross-user session disclosure.

Lake's first router middleware now clears the complete response, restores the current request's
HTTP version and keep-alive policy, and then runs CORS. The serializer still refers to the same
response object. Every route, including preflight, errors, redirects, static files and binary
responses, therefore begins without another request's headers or payload metadata. Handlers may
then add headers belonging to the current response. Keep this reset before CORS and dispatch;
resetting in a send helper would discard headers deliberately added by the current handler.

The four `HttpServer/HttpResponseIsolation` cases use one worker to force reuse, real password
sign-in, independent client cookies, all three public entry pages, public JSON and OpenAPI
responses, and protected session reads.
Both in-memory and Postgres variants cover the same socket and a newly accepted socket. The
tests fail against the previous response path because B receives A's cookie and can open
`/sessions`; after the reset B remains anonymous and A remains authenticated.
`HttpServer.ResponseResetPreservesCurrentProtocolAndCors` also alternates HTTP/1.0 and HTTP/1.1
over one connection, checks preflight and error responses, and requires preflight-only fields to
disappear while current CORS headers, status, protocol version and keep-alive policy remain correct.

Verify the complete native suites with Postgres, and reproduce against a local binary using
`curl -q -sS -D headers.private http://127.0.0.1:<port>/sign-up -o signup.html`. After another client
signs in, this GET must return 200 without `Set-Cookie` or `Location`. Repeat using isolated cookie
jars and both reused and fresh connections. Redact every cookie and account value before sharing
headers. A containing deployment and the same production reproduction are required to establish
that the live instance is repaired; this source change does not redeploy it.

**Verify.** `./build/lake --listen 127.0.0.1:4900 --node x --memory-store --admin-token t`, then
`curl -i` the hello page, `/v1/server_info`, `/healthz` and an unmatched path;
`ctest --test-dir build`. The focused idle-close proof is
`ctest --test-dir build -R HttpServer.IdleKeepAliveCloseWritesNothingToStderr --output-on-failure`.

## Bearer-link recipient delivery

`GET /share/:token` renders the shared root or its requested descendant folder. The sibling routes
`GET /share/:token/preview` and `GET /share/:token/download` require a file `path`; all accept `ref`
(default `main`). Each route calls its public share-link registry capability and uses `no-store`.
The attachment carries the exact authorized bytes. Scope, role, expiry, and revocation checks live
in the registry handlers and also apply to REST and MCP. See [sharing](../api/sharing.md).
