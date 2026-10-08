# The HTTP server

**What.** lake serves HTTP/1 with Beast2 on the Capy + Corosio runtime, the stack Zetta serves
with. Port 4900 by default (`--listen HOST:PORT`); every flag and environment variable is in
[`configuration.md`](configuration.md).

**Where.** `include/lake/api/server.hpp` is the public shape (`ApiServer`, `ServerOptions`) and
names no HTTP type. `src/api/http_server.cpp` is the one translation unit that includes Beast2,
`boost::http` or Boost.JSON. Pages are pure renderers in `src/api/web_ui.cpp` (string in, string
out) and are the unit-tested surface.

**Lifecycle.** `ApiServer::start()`: build the router → construct `HttpConnections(io_context,
router, parser_cfg, serializer_cfg, limits)` (`src/api/http_connections.hpp`) → `bind` (open,
`SO_REUSEADDR`, bind, listen) → `port()` → `start()` → an `io_thread` runs `io_context.run()`.
`begin_drain()` closes the connections waiting for their next request. `stop()`:
`server->stop()` closes the listener and every connection and waits up to 2 s for the sessions to
unwind, then `io_context.stop()`, join. The owner loop calls `wait_for_work()` then `drain()` each
tick.

**Routes.**

| Route | Serves |
|---|---|
| `GET /` | the account → repository → push landing page with viewer-specific navigation |
| `GET /style-<hash>.css`, `GET /fonts/<IBM Plex face>-<hash>.woff2` | content-hashed design-system assets (`public, max-age=31536000, immutable`) linked by every page |
| `GET /style.css`, `GET /fonts/<IBM Plex face>.woff2` | compatibility aliases for direct and older links (`no-cache`) |
| `GET /healthz`, `GET /readyz` | unauthenticated liveness and control-store readiness probes; see [`telemetry.md`](telemetry.md) |
| `GET /metrics` | Prometheus metrics, guarded by the same bearer or session cookie as capabilities |
| `GET /v1/server_info` | `{ok, value: {name, node, version, revision, api, store}}`, no auth; `revision` is the full source commit embedded at image build time, or `unknown` for a build that did not supply it |
| `GET /sign-up`, `/sign-in`, `/mfa`, `/passkey-sign-in` | public password, TOTP, OAuth and passkey entry pages |
| `GET /settings`, `/sessions`, `/passkeys`, `/tokens` | authenticated profile, session/device, passkey and scoped-token management pages |
| `POST /repository/create` | authenticated form adapter for `create_repository`; takes `workspace_id` (the workspace names its space when `customer_id` is absent), creates a private repository there and redirects to its home |
| `POST /identity/*` | form adapters that call the corresponding registry capability; see [`../ui/identity.md`](../ui/identity.md) |
| `GET /applications`, `/application`, `/application/users`, `/application/organizations`, `/application/events` | application registration and operator management pages |
| `GET /apps/<id>/sign-up`, `/sign-in`, `/mfa`, `/passkey-sign-in` | themed application identity pages; a configured custom host serves its equivalent entry pages |
| `GET /apps/authorize?app=<id>&rd=<URL>` | Lake-session handoff for a protected Ship app; validates exact HTTPS app host and redirects with a 60-second one-use code |
| `GET /v1/apps/auth`, `/w/<workspace_id>/v1/apps/auth` | nginx forward-auth for protected Ship apps; returns app-scoped identity headers and exchanges one-use codes for a host-only cookie |
| `GET /ship/apps`, `/ship/app?customer_id=<id>&app_id=<id>` | Ship app list and operating page, including retained and live app/build events |
| `POST /ship/apps/create`, `/ship/apps/agent`, `/ship/app/*` | capability-backed app creation, agent start, deploy, lifecycle, environment, binding, auth, build, and delete form adapters |
| `GET /train/experiments`, `/train/experiment`, `/train/experiment/run`, `/train/experiments/compare` | space-scoped study, trial, metric, artifact and comparison pages |
| `POST /train/experiments/agent`, `/train/experiments/agent/confirm` | `agent_turn` form adapters for an ExperimentsAgent goal and the exact edited study proposal |
| `POST /apps/<id>/identity/*` | application-pool identity form adapters |
| `GET /v1/apps/<id>/.well-known/jwks.json` | the application's public Ed25519 JSON Web Key Set |
| `GET /customers`, `/repositories`, `/repository?id=`, `/organizations`, `/organization?id=`, `/team?id=`, `/member?id=`, `/teams`, `/members`, `/invitations`, `/grants`, `/access`, `/search`, `/metering`, `/secret-mounts`, `/secrets`, `/encryption-keys`, `/audit` | server-rendered pages with ordinary `/sign-in` for protected reads and public repository browsing; operator entry remains `/login` and `POST /session`; see [`../ui/README.md`](../ui/README.md) |
| `GET /transparency?identity=<taap-id>` | public hosted identity monitor over checkpoint-verified bulk tiles |
| `GET /repository/tree`, `/file`, `/changes`, `/change`, `/issues`, `/issue`, `/releases`, `/webhooks`, `/protection`, `/conflicts`, `/artifacts`, `/artifact`; `POST /conflicts/resolve` | forge pages over authorized repository, review, issue, release, webhook, policy, signed-audit, and libliquid records; conflict pages explain kind, region, and reasons before posting an audited resolution |
| `GET /environments?repository_id=`, `/environment?repository_id=&name=`, `/logs?repository_id=&environment=` | platform summaries, deployment history, capability-backed environment/domain forms, and cursor-polling Loki-compatible logs; stale mirror reads show a warning |
| `GET /drive`, `/drive/preview`, `/drive/download`, `/drive/history`, `/drive/history/diff`, `/drive/conflict`, `/drive/share`, `/share/:token` | server-rendered drive list/grid browsing, registered previews with exact-byte fallback, explained conflict comparison, effective access, and scoped links; create, upload, rename, move, delete, restore, conflict resolution, and sharing mutations use POST under `/drive/` |
| `/api/models/*`, `/api/datasets/*`, model/dataset resolve and basic-LFS routes | the documented `huggingface_hub` create, upload, list, info, snapshot, and file-download subset; see [`../api/models.md`](../api/models.md) |
| `GET /models/:namespace/:name`, `/models/:id` | model revisions, files, deduplication savings, and verified training provenance |
| `/v2/*`, native npm and Python package paths, `/buildid/*` | OCI Distribution and package/debug registries; OCI blob upload patches and final puts use the streamed registry limit below; see [`../api/registry.md`](../api/registry.md) |
| `GET /v1/<read>`, `POST /v1/<action>`; the same below `/w/<workspace_id>` | the capability registry as REST: identity and application authentication, short-lived agent-session keys, account recovery, offline grants, tenants, repositories, organization administration, policy, repository search, static secrets, encryption, TUF metadata, transparency monitoring, asynchronous replication, the lake identity and the signed audit stream; bearer required except documented public identity/recovery entry points, transparency reads, and application JWKS |
| `POST /v1/repositories/<repository-id>/sync/{find-missing,push,pull,chunks}` | streaming liquid push, pull, missing-object negotiation, and chunk upload with bounded streaming request and response bodies, through the same registry policy and audit path; offline push accepts a grant UUID and returns per-change reauthorization results; see [`../api/sync.md`](../api/sync.md) |
| `GET /v1/repositories/<repository-id>/sync/chunks/<digest>` | ranged liquid chunk reads with `offset` and `length` query parameters |
| `GET /v1/openapi.json`, `/w/<workspace_id>/v1/openapi.json` | the same OpenAPI 3.1 document, generated from the registry |
| `GET /v1/oauth/<provider>/callback` | OAuth authorization-code callback with one-use state, PKCE, and verified provider identity |
| `GET /sso?organization_id=<uuid>` | organization SSO provider, federation issuer policy, and SCIM provisioning evidence page |
| `/scim/v2/Users`, `/scim/v2/Groups` and `/:id` | provider-bearer SCIM 2.0 filtered list, get, create, PATCH and delete resources |
| `GET /acme/<customer-id>/directory`, ACME resource `POST`s, `HEAD /acme/<customer-id>/new-nonce`, `GET /.well-known/acme-challenge/<token>` | RFC 8555 account, HTTP-01 order, CSR finalize, polling, and certificate download |
| `POST /mcp`, `/w/<workspace_id>/mcp` | MCP (JSON-RPC 2.0): `initialize`, `tools/list`, `tools/call`; the same registry as tools. `initialize` declares `tools.listChanged` and answers an `Mcp-Session-Id`; a session from an older tool list gets `notifications/tools/list_changed` ([mcp.md](../api/mcp.md)). `GET`/`DELETE` answer 405. |
| anything else | `404 {"ok":false,"error":{"code":"not_found"}}` |

## Connections and workers

Lake used to serve with Beast2's `http_server`: a fixed pool of 32 workers, each owning one
connection for its whole life. An idle keep-alive connection kept its worker, so about 32 clients
holding connections open denied service to every tenant (lake#534). Lake now serves the way Zetta
does (`zetta/src/core/api/http_server.cpp`, 4096 connections by default): it accepts every
connection up to a cap and serves each with its own session coroutine. A connection is not a
worker.

| Limit | Default | Environment | What happens |
|---|---|---|---|
| Connections | 4096 | `LAKE_HTTP_MAX_CONNECTIONS` | A connection above the cap is accepted, answered `503` with `Connection: close`, `Retry-After: 1` and the JSON envelope `busy`, and closed. Lake keeps accepting meanwhile. |
| Request workers | 32 | `LAKE_HTTP_WORKERS` | A request takes a worker only while its route runs, from dispatch to the end of its response, so a streaming response (an image pull, an event stream) holds its worker until it ends. Reading a request header and waiting for the next request hold none. `GET /healthz` and `/readyz` need no worker, so the probes answer while every worker is busy. |
| Wait for a worker | 10 s | `LAKE_HTTP_WORKER_WAIT_SECONDS` | A request that finds every worker taken waits, first come first served; past the wait it is answered `503` `busy` and its connection closed. |
| Idle keep-alive | 30 s | `LAKE_HTTP_IDLE_TIMEOUT_SECONDS` | A connection that waits this long for its next request is closed. |
| Request header | 10 s | `LAKE_HTTP_HEADER_TIMEOUT_SECONDS` | A request header must arrive whole within this of its first byte (of the accept, for a connection's first request); otherwise the connection is closed without an answer. A client trickling a header a byte at a time (slowloris) is cut off however steadily it sends. |

Every limit must be positive; lake refuses to start otherwise. A response with status `503` or
`429` closes its connection (`Connection: close`), whichever handler sent it, so a refused client
gives the connection back while it waits to retry; the registry's own refusals did since #528.
`detail::RefusalClosingSink` does this in the sink every response body goes through, before the
header is written. During the drain (lake#536) a connection waiting for its next request is closed
at once, and every response closes its connection.

The request body and the response are not bounded by these timeouts; their routes bound them (the
64 MiB body limit below; `registry_stream_idle_timeout` for a pull). The worker, connection and
timeout counts are metrics (`lake_http_connections`, `lake_http_requests_active`,
`lake_http_requests_waiting`, `lake_http_refusals_total{reason}`,
`lake_http_connections_closed_total{reason}`; [telemetry](telemetry.md)).

`tests/test_http_connections.cpp` proves it: two hundred idle connections (half kept alive after a
request, half silent) leave two workers free and the next request answers in under 500 ms, also
through `ApiServer`; an idle connection is closed at its timeout; a trickled header, a silent
connection and a partial header on a kept-alive connection are each cut off at the header timeout;
the cap answers 503 and closes, and a closed connection makes room; 503 and 429 close their
connection; a request waits for a worker and is refused past the wait, the probe needs none; a
4 MiB streamed response completes beside a hundred idle connections; and the drain closes idle
connections. The registry's streaming pulls are `RegistryStore.*` in
`tests/test_registry_store.cpp`.

```sh
./build/tests/lake-tests --gtest_filter='HttpConnections.*:RegistryStore.*:HttpServer.*'
```

## Request body limits

Beast2 exposes one parser limit for the whole server, not a parser handle to individual route
handlers. Lake therefore sets that parser ceiling to the larger of 64 MiB and
`LAKE_MAX_REGISTRY_BLOB_BYTES`, then enforces the effective limit in each body consumer.

| Route class | Effective limit and handling |
|---|---|
| OCI blob upload `PATCH` and final `PUT`, for every supported repository-name shape | `LAKE_MAX_REGISTRY_BLOB_BYTES` for the completed upload, 4 GiB by default. Incoming buffers append directly to the upload file. `Content-Range` must begin at the stored offset and cover the bytes received. |
| REST capability JSON and MCP JSON-RPC | 64 MiB. JSON sinks count bytes before parsing; oversized capability input returns HTTP 413 with `size_invalid` and the byte limit. |
| Forms and other string bodies | 64 MiB. The common string reader counts bytes while receiving them. |
| Liquid sync, model and dataset publication, drive uploads, packages, debug artifacts, SCIM and other specialized adapters | 64 MiB or a smaller route-specific bound. Their bounded buffers and parsers remain in force. |

OCI limit failures return HTTP 413 with `BLOB_UPLOAD_INVALID` and the configured byte count.
Transport and storage failures distinguish reading the request from appending the upload. A failed
chunk is truncated back to its size before that request, so retry starts at a stable offset.
Digest verification reads the completed file incrementally before the upload is moved into blob
storage.

## Workspace API prefix

Workspace clients receive a Lake base URL ending in `/w/<workspace_id>`. REST capability calls,
OpenAPI, and MCP therefore accept that prefix without changing their unprefixed forms. For a
capability whose schema includes `workspace_id`, the path supplies the field when it is absent.
A query, JSON body, or MCP tool argument that also supplies the field must match the path or the
call returns `invalid_input`.

The router resolves the workspace before dispatch. A malformed, absent, or deleted workspace
returns HTTP 404 with `not_found`. App-source, app-event, build, and task credentials retain their
existing capability restrictions and must belong to the selected workspace. Other credentials
continue through the same registry policy authorization used by `/v1` and `/mcp`. The prefixed
OpenAPI response is byte-for-byte the same registry document as `/v1/openapi.json`; page routes
under `/w/<workspace_id>/...` retain their existing handling.

The prefixed `GET /w/<workspace_id>/v1/apps/auth` route is the one exception to the generic
workspace resolver. It authenticates the host and checks that the stored app workspace equals the
path workspace in one indexed control-store query. A valid app cookie therefore makes no Zetta
call and performs at most one PostgreSQL read. Its `Server-Timing` data is included in the route
performance regression. The complete handoff, ingress annotations, cookie boundary, and logout
behavior are in [`../api/web-apps.md`](../api/web-apps.md).

`HttpServer.WorkspacePrefixServesRestOpenApiMcpAndAppSourceReads` compares prefixed and
unprefixed REST results, rejects a conflicting field, compares both OpenAPI documents, reads an
actual libliquid repository tree with an app-source token over REST and MCP, refuses that token on
another workspace, and checks the unknown-workspace 404. Run it with:

```sh
./build/tests/lake-tests \
  --gtest_filter=HttpServer.WorkspacePrefixServesRestOpenApiMcpAndAppSourceReads
```

**Rules that keep it correct.**

- Every text response goes through `send_text`, which stamps headers Zetta's way: `erase(etag)`,
  `erase(content_length)`, `set_payload_size(body)`. Beast2 frames from the declared size; never
  set those by hand. Binary sync responses use the corresponding bounded byte writer.
- Routes are registered in order: `router.use(cors())`, the `router.add(...)` list, then a
  **trailing `router.use` 404 handler**. The router falls through (`route_next`) past the last
  route and nothing answers otherwise: without the trailing handler an unmatched request
  **hangs**. Keep it last.

**Idle closes.** A client closing an idle keep-alive connection is a normal end of stream and
produces no stderr output; lake's session loop (`HttpConnections`, [below](#connections-and-workers))
writes nothing for it. Lake pins `oramasearch/beast2` branch `orama-main`, whose carried patch
suppresses the former `read_header error: end of stream` line. The carrier and its update procedure
are recorded in [`../build/dependencies.md`](../build/dependencies.md).

## Handler exceptions

Beast2 closes the connection when a route handler's task ends in an exception: the client sees a
reset (a `502` behind nginx and Cloudflare) and Lake writes no request log line. Lake registers
every route, and the workspace and trailing 404 middleware, through `guard_route`
(`src/api/route_guard.hpp`). An exception thrown while the handler is called or while its task
runs is logged as one JSON line on stdout:

```json
{"ts":"…","level":"error","event":"handler_exception","method":"GET","route":"/ship/packages","request_id":"…","error":"…"}
```

Like the request log line, it names the route pattern and never the raw path (decision 54 S9,
lake#384, [telemetry.md](telemetry.md#request-logs)).

The request id is the caller's `X-Request-Id`, or a new UUID. The request is then answered `500`
with `Cache-Control: no-store` and `X-Request-Id`: the HTML error page when `Accept` names
`text/html`, otherwise the JSON envelope with code `internal_error`. The response starts from a
cleared header set, so fields the handler set before it threw do not leak, and the exception text
stays in the log, never in the response. The usual request log line and metrics follow with status
`500`, and the connection stays open for the next request.

`HttpServer.AHandlerThatThrowsIsAnswered500AndLogged` throws from a handler before and inside its
task over one keep-alive connection and checks both answers, the log fields and a following `200`.

## Response isolation

Beast2's server pooled workers across connections (lake now gives each connection its own,
[above](#connections-and-workers), but a keep-alive connection still reuses its response across
requests). At the pinned revision, a worker retains its `route_params::res` and resets the start
line and connection policy without clearing old fields.
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
`curl -i` the landing page, `/v1/server_info`, `/healthz` and an unmatched path;
`ctest --test-dir build`. The focused idle-close proof is
`ctest --test-dir build -R HttpServer.IdleKeepAliveCloseWritesNothingToStderr --output-on-failure`.

## Bearer-link recipient delivery

`GET /share/:token` renders the shared root or its requested descendant folder. The sibling routes
`GET /share/:token/preview` and `GET /share/:token/download` require a file `path`; all accept `ref`
(default `main`). Each route calls its public share-link registry capability and uses `no-store`.
The attachment carries the exact authorized bytes. Scope, role, expiry, and revocation checks live
in the registry handlers and also apply to REST and MCP. See [sharing](../api/sharing.md).

Customer-page authentication redirects go to `/sign-in`. The explicit operator entry stays at
`/login`, with `POST /session` retaining its existing operator credential contract. Shared page
renderers receive `WebUiViewer` from `context_for`; navigation never grants authority. See
[the front-door contract](../ui/front-door.md).
