# CLAUDE.md — lake

The lake binary. **Org-wide conventions** live in
[`oramasearch/orama-bridge`](https://github.com/oramasearch/orama-bridge) `CLAUDE.md`
and apply in full. Project conventions live in
[`oramasearch/liquid`](https://github.com/oramasearch/liquid) `CLAUDE.md` — vocabulary,
document rules, confidentiality, and the decisions not to drift past. Read both.

## Status

**The control surface serves, in production.** `Lake/prod` on AKS (3 replicas, image
`oramasearch/lake:20260902c`) runs: one capability registry rendered as REST `/v1`, OpenAPI
(`/v1/openapi.json`) and MCP (`POST /mcp`); users, organizations, customers and repositories over
the Postgres control plane (schema `0001` + `0002` applied); server-rendered pages
(`/customers`, `/repositories`, `/repository?id=`) behind the admin sign-in; everything behind the
admin bearer except `server_info`. State and runbook: `docs/deployment/aks.md`; the specification
is `oramasearch/liquid/docs/features/`. Next: the Zetta data plane (operator #125), TAAP
identities in place of the single admin token (lake #4), repositories with content once liquid
Phase 1 lands, incremental migrations (operator #126).

## Where to read Zetta

lake builds on Zetta, so you will read Zetta constantly. **Read it only from the umbrella
bridge:**

```
/Users/stevesperandeo/dev/homer6/orama-work/modules/zetta-bridge/modules/zetta
```

Never from a standalone clone — one under `~/dev/oramasearch/zetta` was 1,053 commits
behind and lacked the entire HTTP layer lake borrows. The web-serving stack lake copies is
`include/zetta/api/server.hpp` + `src/core/api/http_server.cpp` (Beast2 on capy/corosio,
one adapter TU) and the pure renderers in `include/zetta/api/web_ui.hpp`.

## How the server is built (and the rules that keep it Zetta-shaped)

- `include/lake/api/server.hpp` exposes `ApiServer` / `ServerOptions` and **no HTTP
  type** — no beast2, `boost::http` or Boost.JSON name may appear in a public header.
- `src/api/http_server.cpp` is the **one** adapter TU. Handlers are `Impl` members
  returning `boost::http::route_task`; every response goes through `send_text`, which
  stamps headers Zetta's way (`erase(etag)`, `erase(content_length)`,
  `set_payload_size`) — Beast2 frames from the declared size, never set those by hand.
- Routes are registered in `ApiServer::start()`: `router.use(http::cors())`, the
  `router.add(...)` list, then a **trailing `router.use` 404 handler**. The router falls
  through (`route_next`) past the last route and nothing answers otherwise — without the
  trailing handler an unmatched request hangs. Keep it last.
- Lifecycle is Zetta's: construct `beast2::http_server(io_context, workers, router,
  parser_cfg, serializer_cfg)` → `bind` → `local_endpoint().port()` → `start()` →
  `io_thread` runs `io_context.run()`; `stop()` = `server->stop()`, `io_context.stop()`,
  join. The owner loop calls `wait_for_work()` then `drain()` each tick; both are
  placeholders until lake has a node to marshal onto.
- Pages are **server-rendered, self-contained, no script**; renderers in `web_ui.cpp`
  are pure functions and are the unit-tested surface. Pages read the `Store` directly and can
  never do what the API cannot.
- **One auth check.** The API bearer and the UI session cookie (`lake_session`, set by
  `POST /session`) carry the same secret and are resolved by the same code (`context_for`);
  never add a second path. Pages that read the store redirect to `/login` without it.
- **One registry.** Every capability is a `Capability` in `src/api/capabilities.cpp`; REST,
  OpenAPI and MCP are renderings of it and never special-case an entry. `Registry::call` is the
  only path that authorizes and validates. Recipe: `docs/api/README.md`.
- **Two TUs name third parties:** `src/api/http_server.cpp` (Beast2, boost::http, Boost.JSON)
  and `src/control/postgres_store.cpp` (libpqxx). nlohmann::json is the API document type.
- Expected stderr noise: beast2 logs `read_header error: end of stream` once per
  keep-alive connection a client closes. Not a bug.

## What lake is built on

- **Control plane: Postgres, for now.** The operator provisions a CloudNativePG cluster per
  `Lake` (`spec.postgres`, on by default) and injects `LAKE_CONTROL_DB_URI` /
  `LAKE_CONTROL_DB_PASSWORD` into the pod (optional refs: the pod starts before the DB is
  bootstrapped, so treat an empty value as "not yet"), and `LAKE_ADMIN_TOKEN` from the
  minted-once `lake-<name>-admin` Secret. **Data plane: Zetta.** Recorded in the
  bridge `decisions.md` §6; "for now" is deliberate.
- **Schema.** `db/schema.sql` is the source of truth; the operator carries a byte-identical
  vendored copy at `internal/controller/assets/lake-schema.sql` and applies it per Lake as an
  owned `PostgresSchema` (one transaction, guarded on the table the **newest** migration creates,
  `repositories` today, versioned by content hash).
  Change the schema HERE, then copy it into the operator in the same change; the operator's test
  checks the copy carries the source-of-truth header. Migrations are single-shot today
  (incremental migrations are the PostgresSchema follow-up), so until then a schema change means
  a new guard (the operator's `GuardTable`), idempotent SQL (`IF NOT EXISTS`), and on an existing
  Lake deleting its `PostgresSchema` so the owned object is recreated and the Job re-runs. Migrations never run in lake pods, so
  replica count cannot duplicate them.
- **Tenant model.** user and organization are identity (TAAP `taap_id`); the customer owns
  everything: individual xor organization (`customer_type_check`), one customer per identity,
  one global lowercase handle namespace (`customer_name_unique` + `customer_name_format`); the
  app generates handles and must pre-check to answer 409.

**lake is the enterprise product and builds on Zetta as much as possible** — control
plane, operational state, clustering, and **its web-serving approach**. lake must also
host liquid repositories, so it consumes **liquid's store as a library** for repository
data. liquid is open source and never depends on Zetta; lake sits on the other side of
that licence boundary.

- **Port 4900** (decided; Zetta is 4800). The `Lake` CR's `spec.port` defaults to it.
- **C++26**, CMake ≥ 3.28 + Ninja, Clang 17+ auto-selected as Zetta's `CMakeLists.txt`
  does, OpenSSL 3 and libpq from Homebrew. GoogleTest v1.17.0 via FetchContent (as Zetta), not Catch2.
- **Boost.Capy** coroutines, **Corosio** networking, **Boost.HTTP** router, **Beast2**
  HTTP/1 server — vendored under `modules/` at **Zetta's exact pins**, from the same
  remotes Zetta uses: `capy` (cppalliance `develop`), `corosio` (`oramasearch/corosio`
  `orama-main`), `http` (`oramasearch/http` `zetta-patches`), `beast2` (cppalliance),
  `libpqxx` (jtv, 7.10.7 at `853bc0a4`, on libpq). nlohmann/json 3.11.3 via FetchContent.
  Bump a pin only to a commit Zetta has moved to; never take `cppalliance` corosio or http.
- `std::expected` at every public boundary; no exceptions across public APIs.
- Deployed as a `Lake` CR on `oramasearch/orama-operator` (the Zetta operator, extended);
  runs on `oramasearch/k0s`, and on our AKS cluster for now.

## Container image

- `Dockerfile` = Zetta's headless recipe (Ubuntu 24.04, clang-19 from apt.llvm.org, CMake 3.31
  because 24.04's 3.28 does not know `cxx_std_26`) minus GUI/Postgres deps, **plus
  `libstdc++-14-dev`**: 24.04 defaults to libstdc++ 13, which has no `<print>`, and lake uses
  `std::println`, and `libpq-dev` for libpqxx. Runtime image = `libssl3 libstdc++6 libpq5
  ca-certificates`, numeric non-root `USER 10001:10001` (a named user cannot satisfy a
  `runAsNonRoot` pod security context).
- **Build natively on amd64.** QEMU emulation of a C++26 build on Apple Silicon is far too slow
  and hits `Illegal instruction` (zetta-bridge learned this). From this Mac, build on the AKS
  node with buildx's kubernetes driver (rootless BuildKit pod in namespace `buildkit`):

  ```sh
  kubectl create namespace buildkit
  docker buildx create --name aks-amd64 --driver kubernetes --platform linux/amd64 --bootstrap \
    --driver-opt namespace=buildkit,replicas=1,rootless=true,requests.cpu=2,requests.memory=4Gi,limits.cpu=6,limits.memory=12Gi
  docker buildx build --builder aks-amd64 --platform linux/amd64 -t oramasearch/lake:v1-sandbox -t oramasearch/lake:<tag> --load .
  docker push --all-tags oramasearch/lake
  ```

  `docker buildx rm aks-amd64` removes the pod when done. `--load` then a separate `docker push`
  keeps registry writes explicit.
- Tags: `v1-sandbox` (mutable, what the sample CR pulls) plus an immutable tag per build. The Docker
  Hub repo is private; the `Lake` CR needs `spec.pullSecretName` and the Secret in `spec.namespace`.

## Open decisions

- **Blocking store calls run inline on the io thread** (one statement each). Offload to a
  pool, as Zetta does, when latency shows; not before.
- **Copy the pattern vs link `zetta-api`.** Today lake copies Zetta's serving shape and has
  no source dependency on Zetta. Linking Zetta as a library (control plane, state, auth,
  Clerk issuer) means adding Zetta as a module of lake — decide how lake obtains Zetta
  before Phase 6 work starts.

## Never

- Never run schema migrations from lake pods (the operator's PostgresSchema Job does, once).
- Never add an auth path the API does not have; the UI session is the API bearer.
- Never add a second attestation or signing framework. TAAP/1 is the security model.
- Never make this repository public.
- Never let an HTTP type leak out of `src/api/http_server.cpp`.

## Before finishing any change

1. **Document the change in `docs/<topic>/*.md`** (see `docs/README.md`): what, why, how to
   verify, how to change. New topic → new folder + a row in `docs/README.md`. This is a must.
2. Does `README.md` still describe what builds and serves? Update the status line and tables.
3. Does this file's **Status** and **Open decisions** still hold?
4. `cmake --build build && ctest --test-dir build` green, and the binary smoke-tested by hand.
5. Commit means push.
