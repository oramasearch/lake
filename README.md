# lake

**The lake binary.** The hosted service at `lake.live` — forge, drive, platform,
identity, secrets, PKI, artifact registry and search — over one content-addressed store.

liquid is the tool. lake is where it collects.

> **Status: hello world serves.** `lake` binds, serves the hello page, its stylesheet and
> `GET /v1/server_info` over Zetta's serving stack (Beast2 on Capy/Corosio), and answers
> every miss with a JSON 404. The specification lives in
> [`oramasearch/liquid`](https://github.com/oramasearch/liquid) under `docs/features/` —
> principally [`18-lake-service.md`](https://github.com/oramasearch/liquid/blob/main/docs/features/18-lake-service.md).
> The image `oramasearch/lake` (private) runs under the `Lake` CR on `orama-operator`; see
> **Container image** below.

## Build and run

Requirements: CMake ≥ 3.28, Ninja, Clang 17+ (Homebrew LLVM on macOS), OpenSSL 3
(`brew install openssl@3`). Clone with submodules — the HTTP stack is vendored.

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
| `GET /v1/server_info` | `{"name":"lake","node":"hello","version":"0.0.1","api":"v1"}` |
| anything else | `404 {"error":"not_found"}` |

`lake version` prints the version. The default port is 4900 (`--listen HOST:PORT`);
`--node NAME` names the node on the page and in `/v1/server_info`. SIGINT/SIGTERM stop
it cleanly.

## Container image

`Dockerfile` builds the `lake` binary on Ubuntu 24.04 with clang-19 (LLVM apt channel), libstdc++-14
and CMake 3.31, then ships it on a minimal Ubuntu runtime (OpenSSL 3 + libstdc++) as user `lake`,
`ENTRYPOINT lake`, `CMD --listen 0.0.0.0:4900`, port 4900 exposed. It is Zetta's headless-image
recipe minus the GUI and Postgres deps. Images go to the **private** Docker Hub repo
`oramasearch/lake`: the mutable `v1-sandbox` tag plus an immutable date or commit tag.

Build it **natively on amd64** — a C++26 build under QEMU on Apple Silicon is far too slow. From an
Apple Silicon Mac, use a BuildKit builder running on the AKS node (see `CLAUDE.md`), then push:

```sh
docker buildx build --builder aks-amd64 --platform linux/amd64 \
  -t oramasearch/lake:v1-sandbox -t oramasearch/lake:<tag> --load .
docker push --all-tags oramasearch/lake
```

The `Lake` CR on `orama-operator` runs it: `spec.image: oramasearch/lake:<tag>`,
`spec.pullSecretName: oramasearch-dockerhub` (the Secret must exist in `spec.namespace`), port 4900
by default, no args needed.

## Layout

| Path | Role |
|---|---|
| `include/lake/api/server.hpp` | `ApiServer` / `ServerOptions` in Zetta's shape — no HTTP type appears here |
| `src/api/http_server.cpp` | the **one** translation unit that knows Beast2, `boost::http` and Boost.JSON |
| `include/lake/api/web_ui.hpp`, `src/api/web_ui.cpp` | pure HTML renderers (string in, string out) |
| `src/main.cpp` | CLI and the owner loop (`wait_for_work` / `drain`) |
| `tests/` | GoogleTest; `ctest --test-dir build` |
| `modules/` | `capy`, `corosio`, `http`, `beast2` — vendored at Zetta's exact pins |
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

## Where this sits

Part of the liquid project. See
[`oramasearch/liquid-bridge`](https://github.com/oramasearch/liquid-bridge) for the
cross-module map and the action plan.
