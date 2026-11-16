# The HTTP server

**What.** lake serves HTTP/1 with Beast2 on the Capy + Corosio runtime, the stack Zetta serves
with. Port 4900 by default (`--listen HOST:PORT`); every flag and environment variable is in
[`configuration.md`](configuration.md).

**Where.** `include/lake/api/server.hpp` is the public shape (`ApiServer`, `ServerOptions`) and
names no HTTP type. `src/api/http_server.cpp` is the one translation unit that includes Beast2,
`boost::http` or Boost.JSON. The console is the web app in `frontend/`, embedded in the binary and
answered for every browser page request ([`../ui/web-app.md`](../ui/web-app.md)); it calls only
`/v1`. The one surface lake still renders is an identity application's hosted sign-in pages, pure
renderers in `src/api/web_ui.cpp` (string in, string out).

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
| `GET /llms.txt`, `/llms-full.txt`, `/skills/lake/SKILL.md` or `/developer/mcp/skill` whose `Host` is `LAKE_APEX_HOST` | the same document naming every region of `LAKE_REGIONS` with its MCP server, guides and skill, instead of the picker (`text/plain` or `text/markdown`, [`../api/regions.md`](../api/regions.md#the-apexs-agent-documents-lake1110), lake#1110) |
| any other request whose `Host` is `LAKE_APEX_HOST`, apart from the stylesheet, fonts and probes | the region picker, before every other route, inside the order sheet (lake#831); `POST` with `region` sets the `lake_region` preference cookie and redirects to the region's host with the same path and query ([`../api/regions.md`](../api/regions.md#the-apex-picker), lake#757) |
| any request whose `Host` is one of `LAKE_HOST_ALIASES` | after the region picker: a browser page (what the web app would answer) is redirected (302, `no-store`) to the same path and query on `LAKE_PUBLIC_HOST`; anything else has its `Host` rewritten to `LAKE_PUBLIC_HOST` and is served as there, so the alias is never taken for an application's custom domain ([`../deployment/white-label-sign-in.md`](../deployment/white-label-sign-in.md), lake#1136) |
| on `LAKE_APEX_HOST`: `GET /order/<offer>`, `GET /order/price-list`, `POST /order`, `GET /orders/<id>?token=…` | the order sheet's forms, the published price list, an order (recorded, then Stripe Checkout or the confirmation; 32 KiB, rate limited, honeypot), and an order's status by its signed link ([`../api/orders.md`](../api/orders.md#routes), lake#831) |
| a browser's `GET` or `HEAD` of any other path (`Accept: text/html`) | the web app's `index.html` (`no-cache`, `Content-Security-Policy: frame-ancestors 'self'; img-src 'self' data:`, [images](#images)): `/`, `/repositories`, `/w/<workspace_id>/design/projects` and every other page are the app's ([`../ui/web-app.md`](../ui/web-app.md)); the same path asked for by anything else is the 404 |
| `GET /ui/<file>` | the app's content-hashed files (`public, max-age=31536000, immutable`) |
| `GET /style-<hash>.css`, `GET /fonts/<IBM Plex face>-<hash>.woff2` | content-hashed design-system assets (`public, max-age=31536000, immutable`) linked by the hosted application pages |
| `GET /style.css`, `GET /fonts/<IBM Plex face>.woff2` | compatibility aliases for direct and older links (`no-cache`) |
| `GET /healthz`, `GET /readyz` | unauthenticated liveness and control-store readiness probes; see [`telemetry.md`](telemetry.md) |
| `GET /metrics` | Prometheus metrics, guarded by the same bearer or session cookie as capabilities |
| `GET /v1/server_info` | `{ok, value: {name, node, version, revision, api, store, …, region, regions, order_sheet_url}}`, no auth; `region`, `regions` and `order_sheet_url` are [regions](../api/regions.md); `revision` is the full source commit embedded at image build time, or `unknown` for a build that did not supply it |
| `POST /session` | the operator token form: a matching token becomes the Strict `lake_session` cookie and redirects to `/repositories`; a wrong one redirects to `/login?error=…` |
| `POST /logout` | revokes the session, clears the cookie and redirects to `/` (to `/sign-in?signed_out=1` under Clerk) |
| `POST /billing/stripe/webhook` | Stripe's payment notices (lake#620, [billing.md](../api/billing.md#webhooks)): no lake credential, the `Stripe-Signature` HMAC with a five-minute window is checked against the webhook secret in lake's secret store; each event applied once by id and audited centrally; `404` where the lake does not bill or has no Stripe keys |
| `POST /identity/clerk-session` | exchanges Clerk's `lake` template token (`token`) for the `lake_session` cookie, binding it to the Clerk session named by the signed `sid` of the default session token (`session_token`, lake#1163) ([`../api/clerk.md`](../api/clerk.md)) |
| `POST /identity/clerk-webhook` | Clerk's user events through Svix (lake#765, [`../api/clerk.md`](../api/clerk.md#webhooks)): no lake credential, the `svix-signature` HMAC-SHA256 with `LAKE_CLERK_WEBHOOK_SECRET` and a five-minute `svix-timestamp` window; each `svix-id` applied once; `user.created`, `user.updated`, `user.deleted`, `session.revoked`, `session.removed`, `session.ended` audited centrally; session events revoke only the browser sessions bound to the ended Clerk session and unbound browser sessions, never API keys, static tokens or devices (lake#1163); ordered bans/locks suspend access and a newer unban/unlock allows new sign-ins (lake#948); `401` for a refused signature, `503` when the store fails (Clerk retries), `404` without the secret |
| `GET /v1/oauth/<provider>/callback` | OAuth authorization-code callback with one-use state, PKCE, and verified provider identity; sets the cookie and redirects |
| `GET /v1/saml/<provider id>/metadata`, `GET /v1/saml/<provider id>/start`, `POST /v1/saml/<provider id>/acs` | SAML sign-in ([`../api/sso.md#saml-sign-in`](../api/sso.md#saml-sign-in), lake#836), no credential: Lake's SP metadata (its URL is the entity id); a one-use AuthnRequest and a redirect to the IdP; the Assertion Consumer Service, which verifies the form's `SAMLResponse`, sets the cookie and redirects. The ACS is exempt from the cross-site check below |
| `GET /file/raw`, `/file/download`, `/drive/download`, `/exports/download`, `/share/<token>/download` | the bytes of a repository file, a Drive file, a signed export or a shared file; without a credential a protected download redirects to `/sign-in` |
| `GET /drive/files/download?customer_id=&workspace_id=&drive_id=&id=[&inline=1]` | a workspace file ([drives.md](../api/drives.md#uploads)): authorized and audited through `get_drive_entry`, then streamed from the workspace Zetta's `lake_files` bucket 1 MiB at a time (the image-pull streamer, its slots and idle timeout), `nosniff`, a sandboxing CSP, and `inline` only for media types a browser shows as data; always `Content-Disposition` with the file's name |
| (both drive downloads) | described in `GET /v1/openapi.json` as `drive_download` and `drive_files_download` (lake#587; `openapi_document` in `src/api/rest.cpp`), although they answer bytes rather than a capability's JSON. `/drive/download` sends a file's own media type (from its extension) when a browser shows that type as data, else `application/octet-stream` (`displayable_media_type`) |
| `GET /tunnel-clients/<platform>?expires=&signature=` | a tunnel client binary for Developer Access ([tunnel-clients.md](../api/tunnel-clients.md), lake#745): no session, the [signed link](#signed-links) `tunnel_client_downloads` answered is the credential; `<platform>` is allow-listed (`darwin-arm64`, `linux-amd64`) before anything is read; `application/octet-stream` attachment with `Content-Length`, `no-store` and an `ETag` of its SHA-256; an invalid or expired link is `401 unauthenticated` (`reason`), an unknown platform or a Lake without clients `404 not_found`. In OpenAPI as `tunnel_client_download` |
| `GET /llms.txt`, `/llms-full.txt`, `/developer/mcp/skill`, `/skills/lake/SKILL.md` | the agents' guide and the Claude skill, public text |
| `GET /apps/<id>/sign-up`, `/sign-in`, `/mfa`, `/passkey-sign-in`; the same four paths on an application's custom sign-in host | themed application identity pages, server-rendered ([`../api/apps.md`](../api/apps.md)); on lake's own host those paths are the web app's |
| `POST /apps/<id>/identity/*` | application-pool identity form adapters |
| `GET /v1/apps/<id>/.well-known/jwks.json` | the application's public Ed25519 JSON Web Key Set |
| `GET /apps/authorize?app=<id>&rd=<URL>` | Lake-session handoff for a protected Ship app; validates exact HTTPS app host and redirects with a 60-second one-use code |
| `GET /v1/apps/auth`, `/w/<workspace_id>/v1/apps/auth` | nginx forward-auth for protected Ship apps; returns app-scoped identity headers and exchanges one-use codes for a host-only cookie |
| `/api/models/*`, `/api/datasets/*`, model/dataset resolve and basic-LFS routes | the documented `huggingface_hub` create, upload, list, info, snapshot, and file-download subset; see [`../api/models.md`](../api/models.md) |
| `/v2/*`, native npm and Python package paths, `/buildid/*` | OCI Distribution and package/debug registries; OCI blob upload patches and final puts use the streamed registry limit below; manifests are stored and served only as OCI or Docker schema 2 manifest and index types, and byte responses carry `nosniff` and a sandboxing CSP ([media types and isolation](../api/registry.md#media-types-and-isolation)); see [`../api/registry.md`](../api/registry.md) |
| `GET /v1/<read>`, `POST /v1/<action>`; the same below `/w/<workspace_id>` | the capability registry as REST: identity and application authentication, short-lived agent-session keys, account recovery, offline grants, tenants, repositories, organization administration, policy, repository search, static secrets, encryption, TUF metadata, transparency monitoring, asynchronous replication, the lake identity and the signed audit stream; bearer required except documented public identity/recovery entry points, transparency reads, and application JWKS |
| `POST /v1/repositories/<repository-id>/sync/{find-missing,push,pull,chunks}` | streaming liquid push, pull, missing-object negotiation, and chunk upload with bounded streaming request and response bodies, through the same registry policy and audit path; offline push accepts a grant UUID and returns per-change reauthorization results; see [`../api/sync.md`](../api/sync.md) |
| `GET /v1/repositories/<repository-id>/sync/chunks/<digest>` | ranged liquid chunk reads with `offset` and `length` query parameters |
| `GET /v1/openapi.json`, `/w/<workspace_id>/v1/openapi.json` | the same OpenAPI 3.1 document, generated from the registry |
| `GET`/`POST /workspaces/<customer_id>/zetta/v1/<capability>`, and below `/w/<workspace_id>` | a workspace's Zetta in Zetta's own REST shape for agents ([`../api/workspaces.md`](../api/workspaces.md)) |
| `/scim/v2/Users`, `/scim/v2/Groups` and `/:id` | provider-bearer SCIM 2.0 filtered list, get, create, PATCH and delete resources |
| `GET /acme/<customer-id>/directory`, ACME resource `POST`s, `HEAD /acme/<customer-id>/new-nonce`, `GET /.well-known/acme-challenge/<token>` | RFC 8555 account, HTTP-01 order, CSR finalize, polling, and certificate download |
| `POST /mcp`, `/w/<workspace_id>/mcp` | MCP (JSON-RPC 2.0): `initialize`, `tools/list`, `tools/call`; the same registry as tools. `initialize` declares `tools.listChanged` and answers an `Mcp-Session-Id`; a session from an older tool list gets `notifications/tools/list_changed` ([mcp.md](../api/mcp.md)). `GET`/`DELETE` answer 405. |
| anything else | `404 {"ok":false,"error":{"code":"not_found"}}` |

The server-rendered console pages, their deferred panels and their HTML form handlers
(`/identity/sign-in`, `/repository/create`, `/drive/upload`, `/ship/app/*`, `/design/*` and the
rest) were retired in lake#561: each did what a `/v1` capability does, and the web app calls the
capability. `WebAppHttp.BrowsersGetTheAppAndCustomDomainsTheirHostedSignIn` checks that a browser
gets the app for former page paths and that anything else gets the 404, that a retired form is
gone, that `/w/<workspace_id>/` is checked only where the API is reached, and that a custom sign-in
host still answers its hosted pages.

## Connections and workers

Lake used to serve with Beast2's `http_server`: a fixed pool of 32 workers, each owning one
connection for its whole life. An idle keep-alive connection kept its worker, so about 32 clients
holding connections open denied service to every tenant (lake#534). Lake now serves the way Zetta
does (`zetta/src/core/api/http_server.cpp`, 4096 connections by default): it accepts every
connection up to a cap and serves each with its own session coroutine
(`src/api/http_connections.hpp`). A connection is not a worker.

| Limit | Default | Environment | What happens |
|---|---|---|---|
| Connections | 4096 | `LAKE_HTTP_MAX_CONNECTIONS` | A connection above the cap is accepted, answered `503` with `Connection: close`, `Retry-After: 1` and the JSON envelope `busy`, and closed lingering (below). Lake keeps accepting meanwhile. At start lake raises its soft open-file limit to the hard limit and lowers the cap to fit: the limit less 512 descriptors (half the limit when that is smaller) for PostgreSQL, store files and outbound connections; it says so on stderr, and the startup line reports `open_files_limit` and the effective `http_max_connections`. |
| Request workers | 32 | `LAKE_HTTP_WORKERS` | A request takes a worker only while its route runs. Reading a request header, waiting for the next request, and waiting for the store root (below) hold none. A request that parks or streams leaves its worker (below). `GET /healthz` and `/readyz` need no worker, so the probes answer while every worker is busy. |
| Wait for a worker | 10 s | `LAKE_HTTP_WORKER_WAIT_SECONDS` | A request that finds every worker taken waits, first come first served; past the wait it is answered `503` `busy` (with `Access-Control-Allow-Origin: *`, no body for `HEAD`) and its connection closed. The refusal is logged as a request line with route `workers_busy` and counted in `lake_http_requests_total`. A client that has left by the time its request gets a worker is not dispatched. |
| Idle keep-alive | 75 s | `LAKE_HTTP_IDLE_TIMEOUT_SECONDS` | A connection that waits this long for its next request is closed. **It must exceed the idle timeout of any proxy's pooled upstream connections** (nginx keeps them 60 s): a proxy that reuses a connection lake is closing answers the client `502`, and does not retry a `POST`. |
| Request header | 10 s | `LAKE_HTTP_HEADER_TIMEOUT_SECONDS` | A request header must arrive whole within this of its first byte (of the accept, for a connection's first request); otherwise the connection is closed without an answer. A client trickling a header a byte at a time (slowloris) is cut off however steadily it sends. |
| Body and response I/O | 60 s | `LAKE_HTTP_IO_TIMEOUT_SECONDS` | Each read of a request body and each write of a response must make progress within this, or the connection is closed: a client that stops sending or stops reading. It matches the registry's `registry_stream_idle_timeout`, so a pull is not cut off sooner than the registry allows. This bounds only the gap between two reads or writes; a client that keeps that gap short but moves a byte at a time is the transfer rate's (next row). |
| Transfer rate | 1024 B/s after 10 s | `LAKE_HTTP_MIN_TRANSFER_RATE` (0: off) | While a request holds a worker its transfer is metered as a bucket of waiting time: it starts with 10 s, each byte moved adds 1/1024 of a second (never past 10 s), and the time spent waiting on the client is taken out. A read or write may wait only as long as the bucket holds, so a body trickled a byte every few seconds, or a response read a few bytes at a time, empties it and the connection is closed, however steadily the client keeps it alive; a fast transfer keeps it full. Time the route spends on its own work is not counted. A request that left its worker (a long poll, a pull) is not metered. |
| Long requests | 512 in all, 64 per caller | `LAKE_HTTP_MAX_LONG_REQUESTS`, `LAKE_HTTP_MAX_LONG_REQUESTS_PER_CALLER` | Requests that left their worker (below). The caller is the authenticated identity, or the client address when there is none. Past either cap the request is answered `429` (`rate_limited` over REST, `TOOMANYREQUESTS` for a pull) with `Retry-After: 1`, and its connection closed. 512 long requests at the 25 KiB a connection costs, plus their buffers, fit the dev pod's 512 MiB limit. |

Every limit must be positive; lake refuses to start otherwise. The startup line reports the
workers, the worker wait, the cap, the idle, header and I/O timeouts, the transfer rate and the
long-request caps.

**Waiting for the store root.** While a starting lake waits for its predecessor's store root
(lake#479, [store volume](../deployment/store-volume.md#the-hand-off)), a request that needs it waits
*before* it takes a worker, polling up to `LAKE_STORE_OWNER_WAIT_SECONDS` plus two minutes, then is
dispatched; so however many requests a rollout sends, none is refused for want of a worker. A
request still not admitted when that wait runs out is answered `503 store_unavailable` there,
never dispatched to wait again while holding a worker. A waiting request, like one in a route,
counts in the drain's in-flight count, so stopping lake answers it rather than closing it unanswered.

**Long polls and streams leave their worker.** A capability declared a long poll
(`Capability::long_poll`: `receive_agent_tasks`, `receive_agent_events`, `receive_build_events`,
`receive_app_events`, `browse_stream` and `tail_experiment_events`, which the UI's pollers call)
gives its worker back before it parks when it is called with `wait_ms` > 0: `Registry::call_async`
calls `ApiContext::leave_worker`; `tail_experiment_events`, which waits 10 s when no `wait_ms`
is sent (`Capability::default_wait_ms`), leaves it for that wait too. A caller's `wait_ms` alone
never decides it; every other
capability keeps its worker whatever its input, and repository writes refuse fields they do not
read (the Hub preupload adapter passes `prepare_repository_upload` only `files`, `id` and
`revision`, never the client's own fields such as `huggingface_hub`'s `gitIgnore`). An image pull
gives its worker back once it starts streaming; its own stream slots bound it
too (8 per workspace, 24 per process, [registry](../api/registry.md)). Such requests count in
`lake_http_requests_long` and `lake_http_long_requests_total`, and are capped (the table above),
so parked agents and open UI tabs never fill the short-request workers. Repository-store work runs
on a 32-thread blocking pool whose queue is bounded at 256 tasks; past it the call is refused at
once (`busy`, 503) instead of queueing.

**Closing a connection.** A response with status `503` or `429` closes its connection
(`Connection: close`), whichever handler sent it, so a refused client gives the connection back
while it waits to retry; the registry's own refusals did since #528. So does every response during
the drain (lake#536), and a response whose request body its route left unread unless that body is
at most 64 KiB with a `Content-Length`: lake then reads and discards it (within a second) and keeps
the connection, because unread body bytes would otherwise be parsed as the next request.
`detail::RefusalClosingSink` decides this in the sink every response body goes through, before the
header is written. Every close lingers: lake shuts its sending side, then reads and discards what
the client still sends (at most 64 KiB, at most a second) before it closes, so a `503`, `413` or
`401` answered while the request body is still arriving reaches the client instead of being lost to
a reset. During the drain a connection waiting for its next request is closed at once; a request
in flight finishes, and a connection that has not sent its first request yet is still answered.

**Request headers (lake#1108).** All of a request's headers together may be up to
`LAKE_HTTP_MAX_HEADER_BYTES` (default 1 MiB, settable from 8 KiB to 16 MiB). boost::http's defaults
(8 KiB in all, 4 KiB a field) refused browsers signed in through Clerk, whose cookies for the apex
domain grow past them after an OAuth round trip (`header_too_large` at `/identity/clerk-session`).
Its parser compares the per-field limit with how far into the whole header block it has parsed, so
lake sets the field limit to the total; one line is still capped by the ingress (nginx, 8 KiB header
buffers). `HttpConnections.ABrowsersClerkCookiesFitLakesHeaderLimits` holds it.

**Before any route.** A request target that is not a valid URL, or a request that cannot be parsed,
is answered `400`; request headers over 1 MiB in all (`LAKE_HTTP_MAX_HEADER_BYTES`, lake#1108) are `431`; both close the connection and are logged like the
busy refusal. A request with `Expect: 100-continue` is answered `100 Continue` at its route's first
read of the body, so a route that refuses without reading never invites the body (and its
connection closes). A client that reset its connection, or closed it before sending the whole
request, while its request waited is not dispatched; one that half-closed after a complete request
is answered. The worker is given back before an unread body is discarded. An accept that fails for
want of descriptors (`EMFILE`, `ENFILE`) backs off 10 ms; other accept errors are retried at once.
A connection refused at the cap while 64 refusals are already lingering gets one non-blocking
`503` write and is closed, so a flood of refusals never slows the accept loop.

**Memory.** An idle connection costs about 49 KiB of lake's resident memory (the parser's header
area, filled only as far as a request's headers reach, its buffers, the serializer, the session),
measured with lake's 1 MiB header limit by `HttpConnections.AnIdleConnectionCostsLittleMemory` on
macOS (about 25 KiB with boost::http's 8 KiB default), plus the kernel's socket buffers,
which the container's memory cgroup also charges. At the default cap, 4096 idle connections are
about 200 MiB of a 512 MiB pod limit (lake.live's) (`Lake/prod`, `requests 128Mi`, `limits 512Mi`); lower
`LAKE_HTTP_MAX_CONNECTIONS` or raise the limit for a pod that must hold more. On Linux, `ApiServer::start` fixes glibc's
mmap threshold at 128 KiB (unless `MALLOC_MMAP_THRESHOLD_` is set): glibc otherwise raises it to
32 MiB as large blocks are freed, after which request bodies and transfer buffers come from the
heap and stay resident. Measured on the Linux test image, a 304 MiB pull after a 304 MiB push grew
the resident set by 230 MiB without it and by 15 MiB with it
(`RegistryStore.ALargeLayerIsStreamedNeverHeldWhole`).

The worker, connection and timeout counts are metrics (`lake_http_connections`,
`lake_http_requests_active`, `lake_http_requests_waiting`, `lake_http_requests_long`,
`lake_http_long_requests_total`, `lake_http_refusals_total{reason}`,
`lake_http_connections_closed_total{reason}`;
[telemetry](telemetry.md)).

`tests/test_http_connections.cpp` proves it:
- two hundred idle connections (half kept alive after a request, half silent) leave two workers
  free and the next request answers in under 500 ms, also through `ApiServer`;
- forty long polls leave the two short-request workers free; the registry gives the worker back
  only for a capability declared a long poll and `wait_ms` > 0 (a `wait_ms` on any other capability
  keeps it), the six long polls are the declared ones, and repository writes are strict; a full
  blocking pool refuses at once; long requests are capped in all and per caller (by address when
  unauthenticated) with `429`;
- an idle connection is closed at its timeout, and the default idle timeout outlasts nginx's;
- a trickled header, a silent connection and a partial header on a kept-alive connection are each
  cut off at the header timeout; a trickled body and a client that never reads a 64 MiB response are
  cut off at the I/O timeout; a body trickled a byte every 300 ms and a response read 64 bytes
  every 100 ms are cut off by the transfer rate, well inside the I/O timeout;
- the cap answers 503 and closes, and a closed connection makes room; 503 and 429 close their
  connection; a 503 answered while an 8 MiB body is still arriving reaches the client;
- a body the route did not read is never parsed as a request: a small one is discarded and the
  connection kept, a large one closes it;
- a request waits for a worker and is refused past the wait (with CORS, no body for HEAD, reported);
  the probe needs none; a client gone before its worker came is not dispatched;
- a bad target is 400, a 9 KiB header 431, `Expect: 100-continue` is answered; Content-Length and
  chunked uploads, HEAD and a client's `Connection: close` work over one connection;
- a 4 MiB streamed response completes beside a hundred idle connections; the drain closes idle
  connections, lets a request in flight finish and answers a connection that had not sent yet;
- the cap fits the open-file limit; an idle connection costs under 256 KiB;
- a request not admitted in time is answered 503 without running its route; a half-close after a
  complete request is answered; a refusal sends no `100 Continue`; the worker is free while
  another request's unread body is discarded.

`StoreOwner.EveryRequestDuringTheHandOffIsAnsweredAfterIt` (`tests/test_shared_store.cpp`) sends
forty requests to a successor with two workers while it waits for the root; all are answered `200`
after the hand-off, and `StoreOwner.StoppingAWaitingSuccessorAnswersItsWaitingRequests` stops a
successor with eight requests waiting for the root: each is answered `503`.
`HttpServer/HttpResponseIsolation.ALongPollOverRestGivesItsWorkerBack` calls
`receive_agent_tasks` over REST through `ApiServer` with and without `wait_ms` and reads
`lake_http_long_requests_total`. `RegistryStore.AStreamingPullGivesItsWorkerBack` holds four
stalled pulls on two workers and still answers a HEAD. The registry's other streaming pulls are
`RegistryStore.*` in `tests/test_registry_store.cpp`.

```sh
./build/tests/lake-tests --gtest_filter='HttpConnections.*:RegistryStore.*:HttpServer.*:StoreOwner.*'
```

## Request body limits

Beast2 exposes one parser limit for the whole server, not a parser handle to individual route
handlers. Lake therefore sets that parser ceiling to the larger of 64 MiB and
`LAKE_MAX_REGISTRY_BLOB_BYTES`, then enforces the effective limit in each body consumer.

| Route class | Effective limit and handling |
|---|---|
| OCI blob upload `PATCH` and final `PUT`, for every supported repository-name shape | `LAKE_MAX_REGISTRY_BLOB_BYTES` for the completed upload, 4 GiB by default. Incoming buffers append directly to the upload file. `Content-Range` must begin at the stored offset and cover the bytes received. |
| REST capability JSON | `Capability::max_body_bytes`, 1 MiB by default; registered bulk inputs allow up to 64 MiB only with an accepted credential. The registry's credential gate runs before the body is read. |
| MCP JSON-RPC | Anonymous or rejected credentials: 256 KiB for the entire envelope. Accepted credentials: the registered tool's body limit, at most 64 MiB. Discovery and public identity tools remain anonymous within the small budget. |
| Forms and other string bodies | 64 MiB. The common string reader counts bytes while receiving them. |
| Liquid sync, model and dataset publication, drive uploads, packages, debug artifacts, SCIM and other specialized adapters | 64 MiB or a smaller route-specific bound. Their bounded buffers and parsers remain in force. |

JSON admission (lake#944, LSR-083) resolves the bearer or browser session before reading input.
Credentials are resolved again after transfer so a revocation or expiry during the upload is
still enforced by the final registry gate.
Protected REST calls without an accepted credential answer 401 and `WWW-Authenticate` before
reading a body, including malformed input and a declared 2 MiB body. Application-pool sessions
retain their existing restrictions. Public identity capabilities such as `sign_in_password`
allow at most 1 MiB, even with an operator credential. A declared oversized body answers 413
from its headers; chunked bodies count every received byte and stop at the same limit.

The shared JSON reader builds one nlohmann document directly, with at most 64 nested containers
and 65,536 elements (containers, object keys and scalar values, including duplicate keys). It
aborts parsing when either structure limit is reached. Byte and structure failures answer 413
on REST and MCP; malformed JSON answers 400. REST capability refusals are audited without body
contents.
The hosted passkey and SCIM JSON adapters use this reader with the 1 MiB default as well.

Larger registered inputs are audio/transcripts, repository and drive file uploads, model gateway
requests, generic workspace calls, and the engine's dataset, model, workflow and agent-source
imports. The workspace Zetta proxy and model gateway also apply the credential gate before
reading input. All URL-prefixed forms use the same handlers and limits.

MCP must read its envelope to learn the tool name. Lake resolves credentials first and reads
anonymous envelopes incrementally within 256 KiB, then applies the registry's capability gate.
Authenticated envelopes are bounded by the largest registered input while receiving, then by
the selected tool's limit before dispatch. The limit counts the whole wire envelope, including
whitespace and unknown fields; exported tools and other MCP methods keep the 1 MiB default.
The depth and element caps apply during the single parse, before dispatch on every path.

To change an allowance, set `max_body_bytes` beside the capability registration, retain the
anonymous ceilings and update its API contract. `HttpServer.CapabilityAdmissionPrecedesTheBody`
proves 413/401 responses with a declared 2 MiB body and no body bytes sent, for both URL forms,
missing/rejected credentials, MCP and the alternate authenticated adapters.
`JsonStructureLimitsApplyToRestAndMcp`, `ChunkedJsonCannotBypassBodyBudgets` and
`McpAnonymousBudgetPreservesDiscoveryAndBulkInputsRequireCredentials` cover structural limits,
chunked transfer, duplicate keys, anonymous discovery, the allowed nesting boundary and bulk
allowances. Run these through `ctest --test-dir build -R 'HttpServer.*(Admission|Json|Budget)'
--timeout 600 --output-on-failure`, then run the full suite.

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
OpenAPI response is byte-for-byte the same registry document as `/v1/openapi.json`. A workspace's
Zetta below `/w/<workspace_id>/workspaces/...` is checked the same way. Every other path below
`/w/<workspace_id>/` is a page of the web app, which a browser gets as `index.html`; the server
neither redirects it nor remembers a workspace in a cookie (lake#561).

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

## Statuses Behind Cloudflare

lake on `lake.oramadev.net` is reached through Cloudflare. Cloudflare answers an origin's `502` or
`504` with its own HTML page (*"Error 502: Bad gateway … The origin web server returned an invalid
or incomplete response"*) and drops lake's body; it passes `500`, `501` and `503` and every 4xx
through unchanged (Cloudflare's *Custom Errors* and *Error 502/504* pages). Until lake#572 lake
answered `engine_error` with `502`, so a REST caller who mistyped a SQL table, or uploaded audio
OpenAI could not read, got Cloudflare's page telling them to retry in 60 seconds, while MCP (always
`200` with the error inside) showed the real message.

The rule, enforced by `ApiErrors.NoCodeAnswersAStatusCloudflareReplaces`:

- **lake never answers `502` or `504` itself.** `engine_error` (the workspace engine or an outside
  service refused the call) is `422`; an unreachable dependency is `503` (`store_unavailable`);
  lake's own faults are `500`. The OAuth callback's provider failures are `503`.
- A `502` or `504` a caller sees therefore came from a proxy, not from lake: lake was unreachable,
  closed the connection (see [Handler exceptions](#handler-exceptions)), or a long call (a long
  transcription) passed Cloudflare's 100-second origin timeout. lake's request log has no `502`.

How to verify: `curl -s -w '%{http_code}\n' -H 'Authorization: Bearer <key>'
'https://<host>/v1/workspace_sql?customer_id=<c>&workspace_id=<w>&statement=SELECT%20*%20FROM%20datasets.no_such_table%20LIMIT%201'`
answers `422` with `{"ok":false,"error":{"code":"engine_error","message":"table \"no_such_table\" does not exist",…}}`.

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
with `Cache-Control: no-store` and `X-Request-Id`: a standalone HTML error page
(`web_ui_error`) when `Accept` names `text/html`, otherwise the JSON envelope with code `internal_error`. The response starts from a
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

The four `HttpServer/HttpResponseIsolation.SignInCannotAuthenticateAnotherClient` cases use one
worker to force reuse, real password sign-in through `/v1/sign_in_password`, independent client
cookies, the public `viewer`, `server_info` and OpenAPI reads, and protected session reads.
Both in-memory and Postgres variants cover the same socket and a newly accepted socket. The
tests fail against the previous response path because B receives A's cookie and can read
`list_sessions`; after the reset B remains anonymous and A remains authenticated.
`HttpServer.ResponseResetPreservesCurrentProtocolAndCors` also alternates HTTP/1.0 and HTTP/1.1
over one connection, checks preflight and error responses, and requires preflight-only fields to
disappear while current CORS headers, status, protocol version and keep-alive policy remain correct.

Verify the complete native suites with Postgres, and reproduce against a local binary using
`curl -q -sS -D headers.private http://127.0.0.1:<port>/v1/viewer -o viewer.json`. After another
client signs in, this GET must return 200 without `Set-Cookie` or `Location`. Repeat using isolated cookie
jars and both reused and fresh connections. Redact every cookie and account value before sharing
headers. A containing deployment and the same production reproduction are required to establish
that the live instance is repaired; this source change does not redeploy it.

**Verify.** `./build/lake --listen 127.0.0.1:4900 --node x --memory-store --admin-token t`, then
`curl -i -H 'Accept: text/html' /` (the web app), `/v1/server_info`, `/healthz` and an unmatched
path;
`ctest --test-dir build`. The focused idle-close proof is
`ctest --test-dir build -R HttpServer.IdleKeepAliveCloseWritesNothingToStderr --output-on-failure`.

## Images

Every HTML page lake answers (the web app, the hosted application pages, the region picker) carries
`Content-Security-Policy: frame-ancestors 'self'; img-src 'self' data:` (`send_text`). Only lake
frames its pages, and the browser loads images only from lake's own origin and `data:` URLs (the
Drive image previews). With Clerk on, the web app's pages add `https://img.clerk.com`, the host
Clerk's sign-in form shows its logos and provider icons from.

Why (lake#943, LSR-082): an agent's reply is Markdown, and an agent may have read untrusted content
(a document, a dataset row, an MCP tool's result) that tells it to name an image at an address
carrying workspace data. A browser that loaded that image would send the data to the address's
server without anyone clicking. The console's Markdown never loads an image at all; it shows
`Image: <alt>` with the address in the tooltip ([web app](../ui/web-app.md#markdown)). The policy is
the second line: no page lake serves can load an image from another origin, whatever renders it.

Verify: `ctest --test-dir build -R 'WebAppHttp.BrowsersGetTheApp|HttpServer.ClerkExchange'`, which read
the header on the web app's pages, the hosted application pages and the Clerk variant; and
`npm test` in `frontend/` (`tests/markdown-images.test.ts`). By hand:
`curl -sI -H 'Accept: text/html' http://127.0.0.1:4900/ | grep -i content-security-policy`.

Change it here and in the tests together. An image host is added only for a page that needs it,
never for every page; a host that fetches arbitrary addresses for its callers (an image proxy)
reopens the leak, so any such host is a known weakness to record here. `img.clerk.com` is Clerk's
image service and is allowed only where Clerk's sign-in is mounted.

## Cross-site requests

**What.** An unsafe request (`POST`, `PUT`, `PATCH`, `DELETE`) that the browser session
authenticates must come from lake's own origin (lake#561). "The browser session authenticates it"
means what `context_for` resolves as `authentication_method: session`: no `Authorization: Bearer`,
and a `lake_session` cookie. Such a request is refused with `403`
`{"ok": false, "error": {"code": "unauthorized", "message": "Cross-site requests cannot use the
browser session."}}` when:

- `Sec-Fetch-Site` is present and is anything but `same-origin` (`same-site`, `cross-site`,
  `none`); or
- `Origin` is present and is not lake's origin: the configured `LAKE_EXTERNAL_ORIGIN`,
  `https://<LAKE_PUBLIC_HOST>`, or the `Host` the request was sent to (under `http` or `https`, as a
  proxy in front of lake terminates TLS). `Origin: null` is refused; or
- it is a `POST` to a JSON capability (`/v1/<capability>`, under `/w/<id>/` too) or to `/mcp` and
  its `Content-Type` is not `application/json` (parameters such as `charset` are allowed). The
  binary repository sync routes under `/v1/repositories/` are not JSON and are not checked for it.

**Routes that issue a session are checked with or without one** (LSR-110, lake#977). A cross-site
`POST` carries no `SameSite=Lax` cookie, so the rule above alone would never see a request that
*creates* a session: a page elsewhere could post the attacker's own credential and sign the
visitor's browser in as the attacker (login CSRF). `POST /session`, `POST /identity/clerk-session`,
the App sign-in forms (`/apps/<id>/identity/...`) and the sign-in capabilities (`/v1/sign_up_password`,
`sign_in_password`, `verify_mfa`, `sign_in_passkey`, `recover_with_code`, `complete_recovery`) are
refused with `403` `"Cross-site requests cannot sign in."` on the same origin rules whenever no bearer
is sent, and the `/v1` ones also when their `Content-Type` is not `application/json`.

A missing `Origin` and a missing `Sec-Fetch-Site` pass: a non-browser client sends neither, and a
browser sends `Origin` on every cross-origin unsafe request. Bearer-authenticated calls are not
affected, from any origin, with one exception: a **browser key** (`lbk.…`, lake#1152,
[`../api/browser-keys.md`](../api/browser-keys.md)) is refused (`403 unauthorized`) unless `Origin`
names one of the origins it lists, and the answer to a call made with one carries
`Access-Control-Allow-Origin: <that origin>` and `Vary: Origin`, or no `Access-Control-Allow-Origin`
at all, instead of CORS's `*` (`restrict_cors_to_browser_key`). A preflight carries no credential and
is answered as every preflight is. The check is a router middleware that runs right after the response
reset and CORS, before any route, so it covers `/v1`, `/mcp` and the remaining form routes
(`/logout`, `/session`, `/identity/clerk-session`) alike. The one exception is the SAML Assertion
Consumer Service, `POST /v1/saml/<provider id>/acs`: the identity provider's form post is cross-site
by design, and the route never reads the session a cookie carries; it verifies the signed response
and replaces the session (lake#836).

**Why.** Ship apps are hosted one label below the platform's zone (`<name>.oramadev.net` beside
`lake.oramadev.net`), so customer app code is *same-site* with lake and a browser sends
`lake_session` with its requests whatever the cookie's `SameSite`. Before this, an app page could
make a signed-in person's browser run any action as a "simple" `text/plain` request or a form post
(inventory §4.4). Requiring JSON forces a CORS preflight, which lake's wildcard CORS cannot satisfy
with credentials; the `Origin` and `Sec-Fetch-Site` check refuses the request itself.

**Lake's own web app** keeps working: it posts JSON (`application/json`) to `/v1` from lake's
origin, and a same-origin form post carries a same-origin `Origin`.

**Verify.** `HttpServer/HttpResponseIsolation.TheWebAppTalksToAskOramaThroughCapabilities` refuses a
foreign `Origin`, `Origin: null`, `Sec-Fetch-Site: same-site` and `cross-site`, `text/plain` and
form bodies to `/v1` and `/mcp`, and a foreign form post to `/logout`, and accepts the same calls
from lake's origin and a bearer call from a foreign origin. By hand:
`curl -i -X POST localhost:4900/v1/enroll_mfa -H 'Cookie: lake_session=<secret>' -H 'Origin: https://other.example' -H 'Content-Type: application/json' -d '{}'`
answers 403.

**Change.** The middleware is `refuse_cross_site` in `src/api/http_server.cpp`. A new route that a
browser must post to from another origin is a bearer route, never an exemption here.

## Signed links

Two routes take a link instead of a credential, for clients that cannot hold a session: the Hub's
basic-LFS upload `href` (`PUT /api/lfs/<repository>/<oid>`, [models](../api/models.md)) and a
tunnel client download (`GET /tunnel-clients/<platform>`, lake#745). Both sign their fields with
HMAC-SHA-256 under a key derived from `LAKE_INSTANCE_KEK` for this purpose alone (HMAC-SHA-256 of `lake signed link v1`; the instance key itself never signs a link, LSR-266), base64url without padding, carry `expires` (Unix seconds,
15 minutes ahead) and `signature` in the query, and are checked in constant time
(`signed_link_signature` and `signed_link_matches`, `src/api/tunnel_clients.cpp`). No new key and no
new primitive: a new kind of link names itself on its payload's first line, as the tunnel client's
`lake-tunnel-client` does, so one kind's signature never verifies as another's. The signature and
expiry are never logged: the request log names the route pattern only.

## Bearer-link recipient delivery

A recipient opens `/share/:token` and `/share/:token/preview` in the web app, which reads them
through `resolve_drive_share_link` and `preview_drive_share_link`. `GET /share/:token/download`
stays a server route: it requires a file `path`, accepts `ref` (default `main`), calls
`download_drive_share_link` and answers the exact authorized bytes as an attachment with
`no-store`. Scope, role, expiry, and revocation checks live in the registry handlers and also apply
to REST and MCP. See [sharing](../api/sharing.md).

A protected download without a credential redirects to `/sign-in`. The explicit operator entry
stays at `/login` (a web app page) with `POST /session` retaining its existing operator credential
contract. See [the front-door contract](../ui/front-door.md).

Password, MFA and recovery calls share [authentication attempt limits](../api/identity.md#authentication-attempt-limits) across REST, hosted forms and MCP (lake#966). Client admission reuses the address from `order_client_address`: the one the proxy reported on the proxy listener (`LAKE_PROXY_PORT`) or from a trusted proxy, else the peer's own ([client addresses](../deployment/client-addresses.md), lake#1083, lake#1132); account admission reserves capacity in the store before credential work.
