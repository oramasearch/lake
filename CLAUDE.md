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
the Postgres control plane (schema `0001` + `0002` applied); server-rendered pages for customers
and repositories behind the admin sign-in; liveness, readiness,
Prometheus metrics and JSON request logs. Capabilities and metrics use the admin bearer except
`server_info`; probes are unauthenticated. State and runbook: `docs/deployment/aks.md`; the specification
is `oramasearch/liquid/docs/features/`. Next: the Zetta data plane (operator #125), TAAP
credential resolution in place of the single admin token (lake #4), repositories with content
once liquid Phase 1 lands, incremental migrations (operator #126). The codebase also includes
organization administration, one policy engine, migration `0004`, and pages for organizations,
teams, members, invitations, grants and effective access; those are not yet deployed in the
production state above. It also includes the signed, hash-linked audit stream (one event per
capability call, sign-in and session event, recorded by `Registry::call` and the session routes),
the lake TAAP identity with rotation, audit verification and export, the 400-day retention
floor, migration `0005`, and the `/audit` page; `LAKE_INSTANCE_KEK` is required wherever a
Postgres store runs. Repository lifecycle adds migration `0006_repository_lifecycle`, repository
rename, transfer, visibility and tombstone capabilities, and lifecycle state on the repository
page. Those capabilities use the policy metadata and are recorded by the registry audit stream.
The codebase now also renders repository refs, trees, files, change logs, structural change diffs,
conflict values, paged artifact results with verified attestations and trace summaries, and
effective access; anonymous readers receive only public repositories. These repository
data pages are not part of the production state above.
Lake links libliquid as a library-only submodule and opens one customer store at
`LAKE_STORE_ROOT/<customer id>/store`; liquid #160 and #165's client, server, and protocol contract
are pinned. The source tree serves repository push, pull, missing-object negotiation, chunk upload,
and ranged reads through the registry, with per-change policy and signed audit. These routes are not
part of the production state above until a containing image and operator volume support are
deployed. The source tree also provides signed portable repository export/import, including the
liquid history/object transfer, forge state, direct grants, artifact evidence, optional Git
projection, and its export download page. Recorded GitHub imports map history, issue/review
discussion, and workflow documents with a durable fidelity report. Identity core
adds migrations `0007_identity_core` and `0008_mfa_recovery`, verified TAAP documents, password
plus TOTP with one-use recovery codes, persisted WebAuthn, OAuth, row-resolved sessions, scoped
tokens, device revocation, lake-signed passkey-to-TAAP bindings, and the hosted identity pages.
Identity documents are verified through libliquid's public canonical-CBOR codec.
SSO federation adds `0010_sso_federation`, organization SAML/OIDC provider configuration,
federation issuer-claim policy, SCIM user/group mappings, deactivation session revocation,
provisioning logs, the `/sso` page, and provider-bearer SCIM 2.0 `/scim/v2/Users` and `/Groups`
resources. The operator copies the schema once per wave in its own operator pull request.
Offline authorization and account recovery add `0011_offline_recovery`: lake-signed device-bound
cached-read grants with a seven-day maximum, per-change publication checks against grant revocation
and current policy, one-use recovery codes, email recovery with a 24-hour cooling period,
two-administrator organization recovery, per-device notices, and designated custody of opaque
client-wrapped repository trust-domain keys. Account recovery restores sign-in but cannot recreate
a repository key held only on a lost device. The full contracts are `docs/api/offline.md` and
`docs/api/recovery.md`.
Hot branch-tip search persists blob-deduplicated BM25F and positional-trigram segments at
`LAKE_STORE_ROOT/<customer>/index/`, atomically publishes each change through its manifest,
filters candidates with batched policy authorization, and records the signed query outcome
through `Registry::call`. REST, OpenAPI, MCP, and the `/search` page expose the same capability.
Static secrets and encryption add migration `0009_secrets`: customer-namespaced organization
mounts; immutable JSON versions with soft delete, restore and destroy; named keys with rotation,
encrypt/decrypt/rewrap, Ed25519 signing, HMAC-SHA-256 and stored child derivation; and customer-KEK
rotation that rewraps data keys without rewriting secret ciphertext. The mount, secret and key
pages render metadata only. Every operation uses policy authorization and the signed registry
audit path, including denials.

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
- Pages are **server-rendered and self-contained**; renderers in `web_ui.cpp` are pure functions
  and are the unit-tested surface. The two passkey pages contain the documented single inline
  script required to call `navigator.credentials`; every other page has no script. Data pages call
  capabilities through `Registry::call` and can never do what the API cannot.
- **One auth check.** The API bearer and the UI session cookie (`lake_session`, set by
  `POST /session`) carry the same secret and are resolved by the same code (`context_for`);
  never add a second path. Pages that read the store redirect to `/login` without it; `/metrics`
  uses the same check. `/healthz` and `/readyz` are deliberately unauthenticated Kubernetes probes.
- **One registry.** Every capability is registered from `src/api/capabilities.cpp` or its
  area-specific registration file; REST,
  OpenAPI and MCP are renderings of it and never special-case an entry. `Registry::call` is the
  only path that authorizes and validates. Recipe: `docs/api/README.md`.
- **Two TUs name third parties:** `src/api/http_server.cpp` (Beast2, boost::http, Boost.JSON)
  and `src/control/postgres_store.cpp` (libpqxx). nlohmann::json is the API document type.
- Operational telemetry: `GET /healthz`, `GET /readyz`, authenticated `GET /metrics`, and one JSON
  request log on stdout. Contract and metric names: `docs/serving/telemetry.md`.
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
  `offline_recovery_guard` today, versioned by content hash).
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
- **libliquid** as a library-only submodule supplies Capy, Corosio and nlohmann/json;
  **Boost.HTTP** and **Beast2** provide the HTTP/1 server. `http`, `beast2` and `libpqxx`
  remain vendored under `modules/`. Bump liquid only to its current `main`; never edit it
  through lake's copy.
- `std::expected` at every public boundary; no exceptions across public APIs.
- Deployed as a `Lake` CR on `oramasearch/orama-operator` (the Zetta operator, extended);
  runs on `oramasearch/k0s`, and on our AKS cluster for now.

## Container image

- `Dockerfile` = Zetta's headless recipe (Ubuntu 24.04, clang-19 from apt.llvm.org, CMake 3.31
  because 24.04's 3.28 does not know `cxx_std_26`) minus GUI/Postgres deps, **plus
  `libstdc++-14-dev`**: 24.04 defaults to libstdc++ 13, which has no `<print>`, and lake uses
  `std::println`, and `libpq-dev` for libpqxx. A checksum-verified OpenSSL 3.6.3 source build
  provides Corosio TLS and Argon2id. The runtime carries those OpenSSL shared libraries plus
  `libstdc++6 libpq5 ca-certificates`, numeric non-root `USER 10001:10001` (a named user cannot
  satisfy a `runAsNonRoot` pod security context).
- GitHub Actions uses the same toolchain for pull-request builds and publishes the Dockerfile image
  on a Git tag, only for `linux/amd64`. `ORAMA_SUBMODULES_TOKEN`, `DOCKERHUB_USERNAME` and
  `DOCKERHUB_TOKEN` must be configured by Steve before the workflows can complete. See
  `docs/build/ci.md`.
- Deployment images are CI-built immutable `oramasearch/lake:<tag>` images; the workflow also
  publishes `oramasearch/lake:sha-<short-sha>` and rejects `latest`. The Docker Hub repo is private;
  the `Lake` CR needs `spec.pullSecretName` and the Secret in `spec.namespace`.

## Open decisions

- **Blocking store calls run inline on the io thread** (one statement each). Offload to a
  pool, as Zetta does, when latency shows; not before.
- **Copy the pattern vs link `zetta-api`.** Today lake copies Zetta's serving shape and has
  no source dependency on Zetta. Linking Zetta as a library (control plane, state, auth,
  Clerk issuer) means adding Zetta as a module of lake — decide how lake obtains Zetta before a
  capability requires that source dependency.

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

## Formatting

`.clang-format` at the root is the formatter configuration (LLVM base, 100 columns, 4-space indents,
one statement per line). Format only the lines you change (`git clang-format`) before each commit;
existing code is not reformatted wholesale. On macOS the Homebrew binary is
`/opt/homebrew/opt/llvm/bin/clang-format`; on Linux install `clang-format-19`.
