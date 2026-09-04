# lake

**The lake binary.** The hosted service at `lake.live` — forge, drive, platform,
identity, secrets, PKI, artifact registry and search — over one content-addressed store.

liquid is the tool. lake is where it collects.

> **Status: the control surface serves.** `lake` binds, serves the hello page, the `/v1`
> capabilities (users, organizations, customers, repositories over the Postgres control plane), the pages to explore them, the OpenAPI
> document and MCP over Zetta's serving stack (Beast2 on Capy/Corosio), behind the admin bearer. The specification lives in
> [`oramasearch/liquid`](https://github.com/oramasearch/liquid) under `docs/features/` —
> principally [`18-lake-service.md`](https://github.com/oramasearch/liquid/blob/main/docs/features/18-lake-service.md).
> The image `oramasearch/lake` (private) runs under the `Lake` CR on `orama-operator`; see
> **Container image** below. GitHub Actions defines matching pull-request builds and tagged image
> publishing; it becomes active once its required credentials are configured.

## Build and run

Requirements: CMake ≥ 3.28, Ninja, Clang 17+ (Homebrew LLVM on macOS), OpenSSL 3
(`brew install openssl@3`). Clone with submodules — the HTTP stack is vendored.
GitHub Actions repeats this build with the Dockerfile's Ubuntu 24.04, clang-19 and CMake 3.31.5
toolchain on every push and pull request; see [`docs/build/ci.md`](docs/build/ci.md).

```sh
git clone --recurse-submodules git@github.com:oramasearch/lake.git
cd lake
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
./build/lake --listen 127.0.0.1:4900 --node hello
```

| Route | Serves |
|---|---|
| `GET /` | the hello page — server-rendered, self-contained, no script |
| `GET /style.css` | the stylesheet (`public, max-age=86400`) |
| `GET /v1/server_info` | `{ok, value: {name, node, version, api, store}}`, no auth |
| `GET /customers`, `/repositories`, `/repository?id=` | the server-rendered pages behind the sign-in (`/login`, `POST /session`, `POST /logout`) |
| `GET /v1/<read>`, `POST /v1/<action>` | the capability registry as REST: list / get / create for users, organizations, customers and repositories; bearer required |
| `GET /v1/openapi.json` | OpenAPI 3.1, generated from the registry |
| `POST /mcp` | MCP (JSON-RPC 2.0): `initialize`, `tools/list`, `tools/call` — the same registry as tools |
| anything else | `404 {"ok":false,"error":{"code":"not_found"}}` |

`lake version` prints the version. The default port is 4900 (`--listen HOST:PORT`);
`--node NAME` names the node. `--control-db URI` (default `$LAKE_CONTROL_DB_URI`) selects the
Postgres control plane, `--memory-store` an in-memory one for demos; `--admin-token`
(default `$LAKE_ADMIN_TOKEN`) is the bearer every capability except `server_info` requires.
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

```sh
git tag <tag>
git push origin <tag>
```

The `Lake` CR on `orama-operator` runs it: `spec.image: oramasearch/lake:<tag>`,
`spec.pullSecretName: oramasearch-dockerhub` (the Secret must exist in `spec.namespace`), port 4900
by default, no args needed. Details, including the manual dispatch path, are in
[`docs/build/ci.md`](docs/build/ci.md) and [`docs/build/container-image.md`](docs/build/container-image.md).

## Layout

| Path | Role |
|---|---|
| `include/lake/api/server.hpp` | `ApiServer` / `ServerOptions` in Zetta's shape — no HTTP type appears here |
| `src/api/http_server.cpp` | the **one** translation unit that knows Beast2, `boost::http` and Boost.JSON; serves the registry as REST, OpenAPI and MCP |
| `include/lake/api/registry.hpp`, `rest.hpp`, `mcp.hpp`, `capabilities.hpp` | the capability registry and its three renderings (HTTP-free) |
| `include/lake/control/`, `src/control/` | the control-plane store: interface, in-memory, Postgres (`postgres_store.cpp` is the one TU that names libpqxx) |
| `include/lake/api/web_ui.hpp`, `src/api/web_ui.cpp` | pure HTML renderers (string in, string out) |
| `src/main.cpp` | CLI and the owner loop (`wait_for_work` / `drain`) |
| `tests/` | GoogleTest; `ctest --test-dir build` |
| `modules/` | `capy`, `corosio`, `http`, `beast2`, `libpqxx` — vendored at Zetta's exact pins |
| `Dockerfile`, `.dockerignore` | the Linux/amd64 image (see above); the context excludes `build/` and `.git/` |
| `db/schema.sql` | **the control-plane schema, source of truth.** `users`, `organizations`, `customers` (uuid-v4 keys; the customer is the tenant). The operator vendors a byte-identical copy and applies it per Lake through a `PostgresSchema`. |

## What this is

One binary, C++26. **Enterprise.** Control plane on **Postgres** (for now; provisioned per `Lake`
by the operator through CloudNativePG, reaching lake as `LAKE_CONTROL_DB_URI`), data plane on
**Zetta** (the content-addressed store, drips, search), web serving in Zetta's style, and
liquid's open-source store consumed as a library (`liquid::liquid`) for repository data. It runs on [`oramasearch/k0s`](https://github.com/oramasearch/k0s) and is deployed
by [`oramasearch/orama-operator`](https://github.com/oramasearch/orama-operator) (as a `Lake` CR).

| Surface | What it answers |
|---|---|
| Forge | *How did this change?* — change-based review, no CI YAML |
| Drive | *Where is my stuff?* — for people who do not think in commits |
| Platform | *Is it running?* — deploy on drip, preview environments |
| Registry | OCI, npm, PyPI, Maven, apt, models and datasets — one deduplicated store |
| Identity, secrets, PKI | TAAP/1 identities, dynamic secrets, one CA |
| Search | BM25F plus a positional trigram index, over history and inside artifacts |

## Internal documentation

[`docs/`](docs/) — one folder per topic: [serving](docs/serving/), [control-plane](docs/control-plane/),
[deployment](docs/deployment/), [build](docs/build/), [api](docs/api/). Every change to the system lands
its doc there in the same change; this README is the summary.

## Where this sits

Part of the liquid project. See
[`oramasearch/liquid-bridge`](https://github.com/oramasearch/liquid-bridge) for the
cross-module map and the action plan.
