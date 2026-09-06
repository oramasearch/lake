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
| `POST /identity/*` | form adapters that call the corresponding registry capability; see [`../ui/identity.md`](../ui/identity.md) |
| `GET /applications`, `/application`, `/application/users`, `/application/organizations`, `/application/events` | application registration and operator management pages |
| `GET /apps/<id>/sign-up`, `/sign-in`, `/mfa`, `/passkey-sign-in` | themed application identity pages; a configured custom host serves its equivalent entry pages |
| `POST /apps/<id>/identity/*` | application-pool identity form adapters |
| `GET /v1/apps/<id>/.well-known/jwks.json` | the application's public Ed25519 JSON Web Key Set |
| `GET /customers`, `/repositories`, `/repository?id=`, `/organizations`, `/organization?id=`, `/team?id=`, `/member?id=`, `/teams`, `/members`, `/invitations`, `/grants`, `/access`, `/search`, `/metering`, `/secret-mounts`, `/secrets`, `/encryption-keys`, `/audit` | the server-rendered pages behind the sign-in (`/login`, `POST /session`, `POST /logout`); see [`../ui/README.md`](../ui/README.md) |
| `GET /transparency?identity=<taap-id>` | public hosted identity monitor over checkpoint-verified bulk tiles |
| `GET /repository/tree`, `/file`, `/changes`, `/change`, `/issues`, `/issue`, `/releases`, `/webhooks`, `/protection`, `/conflicts`, `/artifacts`, `/artifact` | forge pages over authorized repository, review, issue, release, webhook, policy, signed-audit, and libliquid records; file pages include highlighted line anchors and blame, and change pages include review discussion, diff views, checks, and attestations |
| `GET /environments?repository_id=`, `/environment?repository_id=&name=`, `/logs?repository_id=&environment=` | platform summaries, deployment history, capability-backed environment/domain forms, and cursor-polling Loki-compatible logs; stale mirror reads show a warning |
| `GET /drive`, `/drive/preview`, `/drive/download`, `/drive/history`, `/drive/history/diff`, `/drive/conflict`, `/drive/share`, `/share/:token` | server-rendered drive list/grid browsing, registered previews with exact-byte fallback, retained version comparison and conflict values, effective access, and scoped links; create, upload, rename, move, delete, restore, and sharing mutations use POST under `/drive/` |
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

**Known log noise.** Beast2's worker prints `read_header error: end of stream` to stderr for every
idle keep-alive connection a client closes (`modules/beast2/src/http_worker.cpp:64`, a raw
`std::cerr`, not the logger). In a pod that is one line per readiness probe. Not an error. The fix
is a private beast2 patch carrier (bridge issue #4).

**Verify.** `./build/lake --listen 127.0.0.1:4900 --node x --memory-store --admin-token t`, then
`curl -i` the hello page, `/v1/server_info`, `/healthz` and an unmatched path;
`ctest --test-dir build`.
