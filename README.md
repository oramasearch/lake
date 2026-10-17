# lake

lake is where a customer designs, trains and ships AI on their own data. Each customer space has
one or more workspace Zettas for data, projects, pipelines, runs and models; lake adds the
accounts, agents, services, apps, Liquid repositories, search and identity around them, through one
capability registry served as REST, OpenAPI and MCP, and a web application (`frontend/`, embedded
in the binary) that calls only that API. Start at the homepage's Getting started (`/docs`).

The [developer tunnel guide](docs/api/tunnel-grants.md) describes admin grant/revoke, recipient-only
CSR signing, relay trust and the session-history provider contract.
[Scoped connection credentials](docs/api/scoped-credentials.md) describes an app's read/write and
optional-schema scope, the operator's workspace-secret reference, rotation and revocation.

## Build and get a first result

Install CMake 3.28+, Ninja, Homebrew LLVM, OpenSSL 3, libpq, Node, and initialise submodules.
Set `LAKE_ZETTA_SOURCE_DIR` to the authorized checkout named in [`docs/build/local.md`](docs/build/local.md).

```sh
cmake -S . -B build -G Ninja \
  -DLAKE_ZETTA_SOURCE_DIR=/path/to/zetta-bridge/modules/zetta \
  -DLAKE_LIQUID_SOURCE_DIR=/path/to/liquid
cmake --build build -j 6
ctest --test-dir build --output-on-failure
```

`LAKE_LIQUID_SOURCE_DIR` names a checkout of `oramasearch/liquid`, which is a sibling module of
liquid-bridge and not a submodule of lake. It defaults to `../liquid`.

The web app's Design, Train, Ship and Ask pages live under `/w/<workspace_id>/...`; the top-bar
selector lists the space's workspaces ([`docs/ui/web-app.md`](docs/ui/web-app.md)). Person-scoped
REST and MCP calls send `workspace_id` explicitly. Library-agent bus and
task-credential calls may omit it: Lake derives the immutable workspace from `agent_tasks` and
rejects a supplied mismatch. Task/app/build events live in that workspace Zetta; only
`agent-tasks.*` remain on the Lake bus. See the
[workspace data-plane design and audit](docs/architecture/workspace-data-plane.md).
Workspace-scoped clients may use `/w/<workspace_id>/v1/<capability>`,
`/w/<workspace_id>/v1/openapi.json`, and `/w/<workspace_id>/mcp`. The path supplies
`workspace_id`; an explicit value must match it.

A workspace's repositories can move into its own encrypted liquid store in its workspace Zetta
(lake#380, migration `0067`): `move_workspace_store` plans, copies, verifies and switches one
workspace at a time, after which they are served from that store and the space's store on lake's
volume stays untouched. It needs liquid `8955519` or later (liquid#332); no workspace is switched on dev. See
[workspace-store.md](docs/architecture/workspace-store.md).

Workspaces are explicit `workspaces` contract rows, not service aliases. The app's `/workspaces`
lists their environment and operator status, creates desired state, and deletes with typed-name
confirmation. Routing resolves `workspaces.zetta_service_id`; migration 0039 and the operator's
`workspaces_guard` handoff are documented in [the contract](docs/control-plane/workspaces.md).

A browser opening any page gets the web app's `index.html`; every other request is the API or a
protocol route. The full route table is [`docs/serving/http.md`](docs/serving/http.md).

| Route | Serves |
|---|---|
| a browser's `GET` of `/`, `/repositories`, `/w/<workspace_id>/design/projects` or any other page | the web app ([`docs/ui/web-app.md`](docs/ui/web-app.md)); its files under `/ui/` |
| `GET /v1/<read>`, `POST /v1/<action>`; the same below `/w/<workspace_id>` | the capability registry as REST, the only thing the web app calls ([`docs/api/README.md`](docs/api/README.md)) |
| `GET /v1/openapi.json`, `POST /mcp`; the same below `/w/<workspace_id>` | the registry as OpenAPI 3.1 and as MCP tools |
| `POST /session`, `POST /logout`, `POST /identity/clerk-session`, `GET /v1/oauth/<provider>/callback` | the sign-in routes that set or clear the HttpOnly `lake_session` cookie |
| `GET /file/raw`, `/file/download`, `/drive/download`, `/exports/download`, `/share/<token>/download` | file bytes; a protected download without a credential redirects to `/sign-in` |
| `GET /apps/<id>/sign-up`, `/sign-in`, `/mfa`, `/passkey-sign-in` (also on a custom sign-in host), `/apps/authorize`, `/v1/apps/auth` | an identity application's hosted sign-in pages, a server-rendered surface, and the Ship app handoff ([`docs/api/apps.md`](docs/api/apps.md), [`docs/api/web-apps.md`](docs/api/web-apps.md)) |
| `GET /healthz`, `/readyz`, `/metrics` | probes and Prometheus metrics |
| `/v1/repositories/<id>/sync/*`, `/v2/*`, npm, Python and debuginfod paths, `/api/models/*`, `/scim/v2/*`, `/acme/*` | liquid sync, the package registries, the Hugging Face subset, SCIM and ACME |
| `GET /llms.txt`, `/llms-full.txt`, `/skills/lake/SKILL.md` | the agents' guide and skill |
| anything else | `404 {"ok":false,"error":{"code":"not_found"}}` |

`lake version` prints the version. The default port is 4900 (`--listen HOST:PORT`);
`--node NAME` names the node. `--control-db URI` (default `$LAKE_CONTROL_DB_URI`) selects the
Postgres control plane, `--memory-store` an in-memory one for demos; `--admin-token`
(default `$LAKE_ADMIN_TOKEN`) is the bearer every capability except `server_info` requires.
`LAKE_REPLICATION_TOKEN` is the bearer presented to peer Lake instances and defaults to the admin
token when omitted.
`--store-root DIR` (default `$LAKE_STORE_ROOT`) is owned by one lake process at a time. A second
process on the same root serves its probes at once, reports Ready, and holds store-touching
requests until the owner drains and releases the root (at most `LAKE_STORE_OWNER_WAIT_SECONDS`,
default 90; it exits 1 naming the owner after that). On SIGTERM lake first hands each workspace
store over (new requests to it answer `503` with `Retry-After`, and its lease is released as soon as
the requests it admitted are done, lake#536), then drains in-flight requests for up to
`LAKE_DRAIN_SECONDS` (default 20), closes its stores and releases the root. A starting lake opens
every running switched workspace's store in the background (`LAKE_WORKSPACE_STORE_PREOPEN`,
default true). See [`docs/deployment/store-volume.md`](docs/deployment/store-volume.md#the-hand-off).
Platform sync uses `LAKE_OPERATOR_ZETTA_URL`, secret bearer `LAKE_OPERATOR_ZETTA_TOKEN`,
`LAKE_PUBLIC_HOST`, and optional `LAKE_PLATFORM_SYNC_SECONDS` (default 15). Logs use
`LAKE_LOGS_URL` and optional secret bearer `LAKE_LOGS_TOKEN`; see
[`docs/api/platform.md`](docs/api/platform.md).
Metering and quota configuration uses the `LAKE_PRICE_*` and `LAKE_STORAGE_QUOTA_BYTES`
environment variables documented in [`docs/api/metering.md`](docs/api/metering.md).
Agent claim leases use optional per-library `LAKE_AGENT_LEASE_SECONDS` overrides; renewal,
bounded retries and the operator handoff are documented in [`docs/api/agent-bus.md`](docs/api/agent-bus.md).
Ship app hosts, reserved names, and quotas use `LAKE_APPS_WILDCARD`,
`LAKE_APPS_RESERVED_NAMES`, `LAKE_APPS_PER_CUSTOMER`, and
`LAKE_APP_REPLICAS_PER_CUSTOMER`; see [`docs/api/web-apps.md`](docs/api/web-apps.md).
App source builds use `LAKE_APPS_RUNTIME_IMAGE`, and build targets use the authority from
`LAKE_INTERNAL_URL`; see
[`docs/api/builds.md`](docs/api/builds.md).
Trusted timestamping uses `LAKE_TSA_URL`, optional PEM `LAKE_TSA_TRUST_CERT`, and the
`LAKE_TSA_RENEWAL_SECONDS` / `LAKE_TSA_RENEW_BEFORE_SECONDS` policy documented in
[`docs/api/transparency.md`](docs/api/transparency.md).
SIGINT/SIGTERM stop it cleanly.

## Continuous integration

Every pull request and every push to `main` runs lake's **full suite on Linux** in the `ci`
workflow: Ubuntu 24.04, clang-19, a Postgres 16 service, the real-Zetta tests against a Zetta
server built at the zetta-bridge pin and cached per pin, and the UI token gates. The run summary
shows the passed, failed and skipped counts, and the job fails if a real-Zetta test skipped. Merge
only on a green `ci` check; macOS-only testing let lake#545 ship a change that broke every
connection on Linux. See [`docs/build/ci.md`](docs/build/ci.md).

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

## Releases

The current release is **v0.1.0** (2026-10-12), the first tagged release.
[`docs/releases/v0.1.0.md`](docs/releases/v0.1.0.md) lists every feature, capability, web app page
and guide in it, what it does not include, and its known issues.
[`docs/releases/README.md`](docs/releases/README.md) is the index, and `CLAUDE.md` has the steps for
cutting one.

## Deployment status

Status: 2026-10-12. `Lake/prod` on the dev AKS cluster (there is no production environment yet)
runs `oramasearch/lake:20261012a` (also tagged `v0.1.0`), built from the v0.1.0 tag (`2e6f6d7`);
`GET /v1/server_info` reports version `0.1.0` and its OpenAPI lists 554 paths. The newest dated [deployment record](docs/deployment/dev-2026-10-01.md) and
[`docs/design/status.md`](docs/design/status.md) describe the 2026-10-01 deployment (`36679e1`) and
have not been brought up to this one. `status.md` distinguishes what is implemented, exercised
locally, deployed, and exercised in deployment, and `DocsStatus.MatchesTheObservedOpenApi` fails
when it calls a capability undeployed that the observed OpenAPI lists.

## Documentation

[`docs/README.md`](docs/README.md) has one row for each documentation topic. Start with
[`docs/design/overview.md`](docs/design/overview.md) for the service model,
[`docs/api/README.md`](docs/api/README.md) for the API, and
[`docs/ui/front-door.md`](docs/ui/front-door.md) for the customer journey.
[`docs/api/auth.md`](docs/api/auth.md) describes bearer credentials and the session migration;
its [measurement proof](docs/build/session-authentication.md) records the regression coverage.
[`docs/control-plane/network.md`](docs/control-plane/network.md) describes secure-networking
desired state, audit, rollout and the operator handoff; [`docs/api/network.md`](docs/api/network.md)
defines its permission-checked REST and MCP capabilities; [`docs/ui/network.md`](docs/ui/network.md)
describes the Network section each workspace has in the console (lake#729), and the
[Network guide](docs/guide/network.md) is what a customer reads.

## Repository rules

[`CLAUDE.md`](CLAUDE.md) contains contributor instructions. [`AGENTS.md`](AGENTS.md) restates the
rules used by repository agents.

## Licence

No licence is declared in this repository. The project licence decision is tracked in
[oramasearch/liquid-bridge#2](https://github.com/oramasearch/liquid-bridge/issues/2).
