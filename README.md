# lake

lake hosts Liquid repositories, collaboration, file workspaces, search, package artifacts, and
identity services through one API and server-rendered web application. Start with the account and
repository journey, then use the same service to store, review, find, and share work.

## Build and get a first result

Install CMake 3.28+, Ninja, Homebrew LLVM, OpenSSL 3, libpq, Node, and initialise submodules.
Set `LAKE_ZETTA_SOURCE_DIR` to the authorized checkout named in [`docs/build/local.md`](docs/build/local.md).

```sh
cmake -S . -B build -G Ninja \
  -DLAKE_ZETTA_SOURCE_DIR=/path/to/zetta-bridge/modules/zetta
cmake --build build -j 6
ctest --test-dir build --output-on-failure
```

| Route | Serves |
|---|---|
| `GET /` | the account → repository → push journey, with navigation matched to the viewer |
| `GET /style-<hash>.css`, `GET /fonts/<IBM Plex face>-<hash>.woff2` | content-hashed design-system assets (`public, max-age=31536000, immutable`) linked by every page |
| `GET /style.css`, `GET /fonts/<IBM Plex face>.woff2` | compatibility aliases for direct and older links (`no-cache`) |
| `GET /healthz`, `GET /readyz` | unauthenticated liveness and control-store readiness probes |
| `GET /metrics` | Prometheus metrics, guarded by the same bearer or session cookie as capabilities |
| `GET /v1/server_info` | `{ok, value: {name, node, version, api, store}}`, no auth |
| `GET /sign-up`, `/sign-in`, `/mfa`, `/passkey-sign-in` | public password, TOTP, OAuth and passkey entry pages; with `LAKE_CLERK_PUBLISHABLE_KEY` set, `/sign-in` and `/sign-up` are Clerk's forms ([`docs/api/clerk.md`](docs/api/clerk.md)) |
| `GET /design/projects`, `/design/project`, `/train/data`, `/train/runs`, `/train/run`, `/ship/models`, `/ship/indexes`; `POST /workspace/start` and the forms under them | Design, Train and Ship over the customer's own Zetta workspace ([`docs/api/workspaces.md`](docs/api/workspaces.md)) |
| `GET /ship/apps`, `/ship/app`; `POST /ship/apps/*`, `/ship/app/*` | create and operate customer apps, or review and confirm an ApplicationAgent proposal, with masked secrets and live app/build events ([`docs/ui/apps.md`](docs/ui/apps.md)) |
| `GET /ship/apps`, `/ship/app`; `POST /ship/apps/*`, `/ship/app/*` | create and operate customer apps, with masked secrets and live app/build events ([`docs/ui/apps.md`](docs/ui/apps.md)) |
| `GET /train/streams`, `/train/stream` | paged space-scoped topic/message CRUD, shared agent views and non-consuming live browse on workspace and bus Zettas ([`docs/ui/streams.md`](docs/ui/streams.md), [`docs/api/streams.md`](docs/api/streams.md)) |
| `GET /train/streams`, `/train/stream` | space-scoped topic/message CRUD and non-consuming live browse on workspace and bus Zettas ([`docs/ui/streams.md`](docs/ui/streams.md), [`docs/api/streams.md`](docs/api/streams.md)) |
| `GET /train/experiments`, `/train/experiment`, `/train/experiment/run`, `/train/experiments/compare` | space-scoped studies, live run metrics, comparison and confirmed promotion ([`docs/ui/experiments.md`](docs/ui/experiments.md), [`docs/api/experiments.md`](docs/api/experiments.md)) |
| `GET /design/projects`, `/design/project`, `/design/solutions`, `/design/solution`, `/train/data`, `/train/runs`, `/train/run`, `/ship/models`, `/ship/indexes`; `POST /workspace/start` and the forms under them | Design, solution projects, Train and Ship over the customer's own Zetta workspace ([`docs/api/workspaces.md`](docs/api/workspaces.md)) |
| `POST /identity/clerk-session` | exchanges a verified Clerk `lake` template token for a `lake_session`; absent without Clerk |
| `GET /settings`, `/sessions`, `/passkeys`, `/tokens` | authenticated profile, session/device, passkey and scoped-token management pages |
| `GET /applications`, `/application`, `/application/users`, `/application/organizations`, `/application/events` | application registration and operator management pages |
| `GET /apps/<id>/sign-up`, `/sign-in`, `/mfa`, `/passkey-sign-in` | themed application identity pages; a configured custom host serves its equivalent entry pages |
| `GET /v1/apps/<id>/.well-known/jwks.json` | the application's public Ed25519 JSON Web Key Set |
| `GET /customers`, `/repositories`, `/repository?id=`, `/organizations`, `/organization?id=`, `/team?id=`, `/member?id=`, `/teams`, `/members`, `/invitations`, `/grants`, `/access`, `/search`, `/metering`, `/secret-mounts`, `/secrets`, `/encryption-keys`, `/audit` | server-rendered pages with ordinary `/sign-in` for protected reads and public repository browsing; operator entry remains `/login` and `POST /session` |
| `GET /transparency?identity=<taap-id>` | public hosted identity monitor over checkpoint-verified bulk tiles |
| `GET /repository/tree`, `/file`, `/changes`, `/change`, `/issues`, `/issue`, `/releases`, `/webhooks`, `/protection`, `/conflicts`, `/artifacts`, `/artifact`; `POST /conflicts/resolve` | forge pages over authorized repository, review, issue, release, webhook, policy, signed-audit, and libliquid records; conflict pages explain kind, region, and reasons before an audited resolution. |
| `GET /environments?repository_id=`, `/environment?repository_id=&name=`, `/logs?repository_id=&environment=` | platform summaries, deployment history, capability-backed environment/domain forms, and cursor-polling Loki-compatible logs; stale mirror reads show a warning |
| `GET /drive`, `/drive/preview`, `/drive/download`, `/drive/history`, `/drive/history/diff`, `/drive/conflict`, `/drive/share`, `/share/:token` | server-rendered drive list/grid browsing, registered previews with exact-byte fallback, explained conflict comparison, effective access, and scoped links; create, upload, rename, move, delete, restore, conflict resolution, and sharing mutations use POST under `/drive/` |
| `GET /ship/packages`, `/ship/app`; `POST /repository/build`, `/ship/app/build` | authorized tenant images and live build history/logs, plus repository and App build actions |
| `/api/models/*`, `/api/datasets/*`, model/dataset resolve and basic-LFS routes | the documented `huggingface_hub` create, upload, list, info, snapshot, and file-download subset; see [`docs/api/models.md`](docs/api/models.md) |
| `/v2/*`, npm package paths, `/legacy/`, `/simple/*`, `/packages/*` | OCI Distribution, npm publication/install, and Python publication/install protocols; see [`docs/api/registry.md`](docs/api/registry.md) |
| `/buildid/*`, `PUT /_lake/debug/*` | public debuginfod reads and protected executable, debug-information, and source publication |
| `GET /models/:namespace/:name`, `/models/:id` | model revisions, files, deduplication savings, and verified training provenance |
| `GET /v1/<read>`, `POST /v1/<action>` | the capability registry as REST: identity and application authentication, Ship app desired state/logs/events, asynchronous image builds/status/cancellation/logs, short-lived agent-session keys, account recovery, offline grants, tenants, repositories, organization administration, policy, repository search, static secrets, encryption, TUF metadata, transparency monitoring, asynchronous replication, the lake identity and the signed audit stream; bearer required except documented public identity/recovery entry points, transparency reads, and application JWKS |
| `/v1/repositories/<repository-id>/sync/*` | streaming liquid push, pull, missing-object negotiation, chunk upload, and ranged chunk reads through the same registry policy and audit path; offline push accepts a grant UUID and returns per-change reauthorization results |
| `GET /v1/openapi.json` | OpenAPI 3.1, generated from the registry |
| `GET /v1/oauth/<provider>/callback` | OAuth authorization-code callback with one-use state, PKCE, and verified provider identity |
| `GET /sso?organization_id=<uuid>` | organization SSO provider, federation issuer policy, and SCIM provisioning evidence page |
| `/scim/v2/Users`, `/scim/v2/Groups` and `/:id` | provider-bearer SCIM 2.0 filtered list, get, create, PATCH and delete resources |
| `GET /acme/<customer-id>/directory`, ACME resource `POST`s, `HEAD /acme/<customer-id>/new-nonce`, `GET /.well-known/acme-challenge/<token>` | RFC 8555 account, HTTP-01 order, CSR finalize, polling, and certificate download |
| `POST /mcp` | MCP (JSON-RPC 2.0): `initialize`, `tools/list`, `tools/call` — the same registry as tools |
| anything else | `404 {"ok":false,"error":{"code":"not_found"}}` |

`lake version` prints the version. The default port is 4900 (`--listen HOST:PORT`);
`--node NAME` names the node. `--control-db URI` (default `$LAKE_CONTROL_DB_URI`) selects the
Postgres control plane, `--memory-store` an in-memory one for demos; `--admin-token`
(default `$LAKE_ADMIN_TOKEN`) is the bearer every capability except `server_info` requires.
`LAKE_REPLICATION_TOKEN` is the bearer presented to peer Lake instances and defaults to the admin
token when omitted.
Platform sync uses `LAKE_OPERATOR_ZETTA_URL`, secret bearer `LAKE_OPERATOR_ZETTA_TOKEN`,
`LAKE_PUBLIC_HOST`, and optional `LAKE_PLATFORM_SYNC_SECONDS` (default 15). Logs use
`LAKE_LOGS_URL` and optional secret bearer `LAKE_LOGS_TOKEN`; see
[`docs/api/platform.md`](docs/api/platform.md).
Metering and quota configuration uses the `LAKE_PRICE_*` and `LAKE_STORAGE_QUOTA_BYTES`
environment variables documented in [`docs/api/metering.md`](docs/api/metering.md).
Ship app hosts and quotas use `LAKE_APPS_WILDCARD`, `LAKE_APPS_PER_CUSTOMER`, and
`LAKE_APP_REPLICAS_PER_CUSTOMER`; see [`docs/api/web-apps.md`](docs/api/web-apps.md).
Build targets use the authority from `LAKE_INTERNAL_URL`; see
[`docs/api/builds.md`](docs/api/builds.md).
Trusted timestamping uses `LAKE_TSA_URL`, optional PEM `LAKE_TSA_TRUST_CERT`, and the
`LAKE_TSA_RENEWAL_SECONDS` / `LAKE_TSA_RENEW_BEFORE_SECONDS` policy documented in
[`docs/api/transparency.md`](docs/api/transparency.md).
SIGINT/SIGTERM stop it cleanly.

## Container image

`Dockerfile` builds the `lake` binary on Ubuntu 24.04 with clang-19 (LLVM apt channel), libstdc++-14
and CMake 3.31, then ships it on a minimal Ubuntu runtime (OpenSSL 3 + libstdc++ + libpq) as UID/GID 10001,
`ENTRYPOINT lake`, `CMD --listen 0.0.0.0:4900`, port 4900 exposed. It is Zetta's headless-image
recipe minus the GUI and Postgres deps. Images go to the **private** Docker Hub repo
`oramasearch/lake` from the tagged [`image` workflow](.github/workflows/image.yml): an immutable
Git-tag image plus `sha-<short-sha>`. `latest` and workstation-built deployment images are not used.

Once the Actions credentials are configured, publish a deployment image by pushing an immutable
Docker-compatible Git tag:
Without `LAKE_TEST_DB_URI` the Postgres-backed cases skip. To run them against a local Postgres,
create a base database with a role that has `CREATEDB`, apply the schema, and set the two variables.
The [test fixture](docs/build/testing.md) creates and removes a private database for each case:

```sh
createdb lake_test
psql -d lake_test -f db/schema.sql
LAKE_INSTANCE_KEK=$(openssl rand -hex 32) \
LAKE_TEST_DB_URI="postgresql://$USER@127.0.0.1:5432/lake_test" \
  ctest --test-dir build --output-on-failure
```

Run the server with the in-memory control plane and check it answers:

```sh
./build/lake version
./build/lake --listen 127.0.0.1:4900 --node dev --memory-store --admin-token t
```

In another terminal, request the first result:

```sh
curl -sS http://127.0.0.1:4900/v1/server_info | jq
```

For the full PostgreSQL-backed suite, local database setup, and every run option, read
[`docs/build/local.md`](docs/build/local.md). The route reference is
[`docs/serving/http.md`](docs/serving/http.md).

## Deployment status

The dated [`Lake/prod` deployment record](docs/deployment/production-2026-09-08.md) names the
active artifact, its recorded source revision, applied schema, verified public requests, and a
read-only smoke check. [`docs/design/status.md`](docs/design/status.md) distinguishes what is
implemented, exercised locally, deployed, and exercised in deployment.

## Documentation

[`docs/README.md`](docs/README.md) has one row for each documentation topic. Start with
[`docs/design/overview.md`](docs/design/overview.md) for the service model,
[`docs/api/README.md`](docs/api/README.md) for the API, and
[`docs/ui/front-door.md`](docs/ui/front-door.md) for the customer journey.
[`docs/api/auth.md`](docs/api/auth.md) describes bearer credentials and the session migration;
its [measurement proof](docs/build/session-authentication.md) records the regression coverage.

## Repository rules

[`CLAUDE.md`](CLAUDE.md) contains contributor instructions. [`AGENTS.md`](AGENTS.md) restates the
rules used by repository agents.

## Licence

No licence is declared in this repository. The project licence decision is tracked in
[oramasearch/liquid-bridge#2](https://github.com/oramasearch/liquid-bridge/issues/2).
