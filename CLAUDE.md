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
`server_info` and documented public identity and transparency-monitoring reads; probes are
unauthenticated. State and runbook: `docs/deployment/aks.md`; the specification
is `oramasearch/liquid/docs/features/`. Next: the Zetta data plane (operator #125), repositories with content
once liquid Phase 1 lands, incremental migrations (operator #126). The codebase also includes
organization administration, one policy engine, migration `0004`, and settings pages for organizations,
teams, members, invitations, grants and effective access; those are not yet deployed in the
production state above. It also includes the signed, hash-linked audit stream (one event per
capability call, sign-in and session event, recorded by `Registry::call` and the session routes),
the lake TAAP identity with rotation, audit verification and export, the 400-day retention
floor, migration `0005`, and the `/audit` page; `LAKE_INSTANCE_KEK` is required wherever a
Postgres store runs. Repository lifecycle adds migration `0006_repository_lifecycle`, repository
rename, transfer, visibility and tombstone capabilities, and lifecycle state on the repository
page. Those capabilities use the policy metadata and are recorded by the registry audit stream.
Platform support links a client-only `zetta-api` from the required `LAKE_ZETTA_SOURCE_DIR`, sharing
Lake's existing Boost.HTTP/Beast2 stack. It mirrors operator-control deployment, environment, and
domain rows into migration `0019_platform`, creates environment policy objects, exposes the eight
policy-checked platform capabilities, Loki-compatible logs, and the server-rendered environment
pages. Operator-control outages degrade platform health, preserve stale mirror reads, and refuse
platform writes without changing readiness.
The codebase now renders forge-designed repository homes, trees, highlighted files with line
anchors and blame, change review with safe Markdown, comments, diff layouts and checks, issues,
releases, webhook history and retry, branch protection with signed refusal audit, explained conflict
values with audited resolution,
paged artifact results with verified attestations and trace summaries, and effective access. The
Postgres-backed forge scenario proves an unlanded Liquid review, cursor movement after rewrite,
approval, publication, and graph-derived evidence without repository automation YAML.
Shared empty, pending, and error states retain the design-system shell and keyboard order;
anonymous readers receive only public repositories. These repository data pages are not part of
the production state above.
Lake links libliquid as a library-only submodule and opens one customer store at
`LAKE_STORE_ROOT/<customer id>/store`; liquid's client, server, sync, per-reference outcome, and
transparency verifier contracts are pinned. The source tree serves repository
push, pull,
missing-object negotiation, chunk upload,
and ranged reads through the registry, with per-change and protected-reference policy and signed
audit. These routes are not
part of the production state above until a containing image and operator volume support are
deployed. The source tree also provides signed portable repository export/import, including the
liquid history/object transfer, forge state, direct grants, artifact evidence, optional Git
projection, and its export download page. Recorded GitHub imports map history, issue/review
discussion, and workflow documents with a durable fidelity report. Identity core
adds migrations `0007_identity_core` and `0008_mfa_recovery`, verified TAAP documents, password
plus TOTP with one-use recovery codes, persisted WebAuthn, OAuth, row-resolved sessions, scoped
tokens, device revocation, lake-signed passkey-to-TAAP bindings, and the hosted identity pages.
Identity documents are verified through libliquid's public canonical-CBOR codec.
Migration `0020_identity_exit` adds fresh, replay-resistant libliquid TAAP-key sign-in and a
one-use application authorization-code flow with exact redirects, PKCE S256, nonce-bound ID
tokens, and the existing Ed25519 application JWKS.
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
Repository search persists blob-deduplicated BM25F and positional-trigram segments at
`LAKE_STORE_ROOT/<customer>/index/`. It serves hot branch tips, immutable release and tag
snapshots, exact changes, and complete history; reports `hot`, `snapshot`, or `scan` mode;
atomically publishes through its manifest; filters candidates with batched policy authorization;
and records the signed query outcome through `Registry::call`. REST, OpenAPI, MCP, and the
`/search` page expose the same capability.
Published artifact outputs are decomposed through libliquid's format registry after sync
publication or through the audited `reindex_artifacts` action. Scoped artifact search reports
indexed text and source members with artifact identity, member path, digest, and line; binary
members and containers nested beyond one inner level remain listed by digest only.
Metering migration `0014_metering` stores hourly unique-byte gauges, sync transfer counters, and
idempotent build-compute counters. `metering_usage` and `/metering` expose configured integer
pricing, with audited per-customer quota overrides. `LAKE_STORAGE_QUOTA_BYTES` remains the
default and throttles new writes at the customer-store boundary while existing content remains
complete and readable. The contract is `docs/api/metering.md`.
Static secrets and encryption add migration `0009_secrets`: customer-namespaced organization
mounts; immutable JSON versions with soft delete, restore and destroy; named keys with rotation,
encrypt/decrypt/rewrap, Ed25519 signing, HMAC-SHA-256 and stored child derivation; and customer-KEK
rotation that rewraps data keys without rewriting secret ciphertext. The mount, secret and key
pages render metadata only. Every operation uses policy authorization and the signed registry
audit path, including denials. Renewable and revocable dynamic-secret leases encrypt the generated
credential and mirror their signed lifecycle audit rows into the transparency log.
Customer PKI adds persisted private roots and online intermediates, SPIFFE X.509 and JWT SVIDs,
client, device, OpenSSH user and OpenSSH host certificates, and a customer-scoped RFC 8555 ACME
HTTP-01 flow. Migration `0013_pki` holds the durable state. ACME consumes one-use nonces, verifies
ES256 account JWS and the final CSR, and runs every resource operation through its registry
transport for signed audit. Agent TAAP Ed25519 public keys receive short-lived X.509 certificates;
rotation links the replacement, revocation is explicit, and API checks reject revoked or expired
rows.
Caller exchanges persist verified non-secret identity metadata before returning a bearer
credential, and OIDC/JWT claims resolve through `federation_issuers`. Root-recovery ceremonies
persist quorum, expiry, completion, and one-way share evidence while keeping shares and
reconstructed root material out of storage. Both use the signed registry audit path.
Drive and models add migration `0015_drive`, stable-node folder ACLs, live recipient grants,
expiring and revocable links, list/grid browse and repository mutations, browser-native and
server-rendered format-registry previews, version comparison, explained conflict presentation and
resolution, per-path
history and restore, effective access before sharing, link management, and shared-with-me pages.
Bearer-link recipients can traverse descendants, preview files and download exact bytes through
public registry capabilities that check link scope, role, expiry and revocation on every call.
The pinned Liquid drive client is covered end to end against an in-process Lake and Postgres:
regular-user sign-up, synchronization, sharing, rendered history, disconnected conflict
preservation, repository pull and materialization, and a native project build.
Models and datasets use
the liquid sync protocol for resumable deduplicated transfer and unique-byte metering. The
compatible Hub routes support create,
file/folder upload, basic LFS, list, info, snapshot, and named or digest downloads. Model metadata
and the server-rendered model page include per-revision storage savings plus the verified liquid
attestation chain from dataset and code revisions through the training run. Linux desktop
presentation remains tracked in liquid #196.
TUF repository metadata adds immutable root, targets, snapshot, and timestamp objects at
`LAKE_STORE_ROOT/<instance>/log/tuf/`, distinct-key threshold verification, version/expiry and
cross-role binding checks, dual-threshold root rotation, and a rehearsed compromised-key
replacement. Its four capabilities use policy metadata and the signed registry audit stream.
Transparency checkpoints read `LAKE_LOG_MODE`, witness endpoints and public keys, quorum, and an
optional aggregation endpoint at startup. Air-gapped checkpoints make no outbound call; witnessed
and anchored checkpoints require the configured distinct witness quorum before they become current;
anchored checkpoints retain an aggregation proof. The effective configuration is a read capability
and appears on `/audit`; the Lake-CR environment mapping is `docs/control-plane/log.md`.
Canonical TAAP attestations append to immutable 1,024-entry and interior-hash tiles, and checkpoints
bind their Merkle root and tree size. Public tile and compact inclusion/consistency-proof
capabilities bind responses to exact signed checkpoints. The hosted `/transparency` identity
monitor verifies the complete checkpoint root before returning matches and refuses modified tiles.
The pinned liquid #207 verifier checks the identity-exit dynamic-secret inclusion proof end to end.
The proof-bundle capability exports all checkpoint trust material and tiles for liquid's offline
verifier. Liquid's persistent checkpoint trust rejects incompatible equal-size witness views.
The source tree also serves OCI Distribution, npm, Python package, and debuginfod protocols from
instance-local registry storage. Protected publication and public exact-byte reads are documented
in `docs/api/registry.md`.
Customer repositories and the transparency log replicate asynchronously from one active Lake to
standbys. Repository pulls persist per-source liquid cursors after successful import. Log pulls
require the destination's exact Merkle prefix, verify the proposed head and signed checkpoint, and
never roll back. `docs/api/replication.md` defines consistency;
`docs/control-plane/replication.md` is the operator handoff.

RFC 3161 timestamping hashes checkpoint signature values, verifies TSA responses and trust, and
stores DER RFC 4998 evidence records at `LAKE_STORE_ROOT/<instance>/log/timestamps/`. The owner
loop renews records by timestamping the preceding archive timestamp before the configured age or
TSA certificate-expiry margin. Creation, renewal, and reads are registry capabilities on the
signed audit path.

Application authentication adds migration `0016_auth_apps`, isolated identity pools, themed
hosted and custom-domain entry pages, per-application Ed25519 JWTs and JWKS, signed lifecycle
webhooks with retained attempts, and authorized operator management for users and organizations.
Its contract is `docs/api/apps.md`.

Agent-session custody adds migration `0017_agent_sessions` and a maximum 15-minute Ed25519 key
delegated to a durable agent by a canonical `delegation/1` attestation. The attestation is appended
to the tiled transparency log before public session metadata is persisted; the private seed is
returned once and never stored. Drip verification binds the session signature back to the durable
agent and rejects the key at expiry. Its contract is `docs/api/agents.md`.

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
  and are the unit-tested surface. Every page has the two documented theme scripts; the two
  passkey pages each add one script required to call `navigator.credentials`. Data pages call
  capabilities through `Registry::call` and can never do what the API cannot.
- IBM Plex Sans and Mono are self-hosted WOFF2 assets embedded into the binary and served below
  content-hashed `/fonts/` URLs with immutable cache headers. Pages use the content-hashed
  stylesheet URL; unhashed stylesheet and font aliases use `no-cache`. The browser never needs a
  third-party font origin.
- Static visual-direction references live under `docs/design/ui/mocks/`. The served design system
  and its complete before/after screenshots are documented in `docs/ui/design-system.md`;
  `tools/ui/screenshot.sh` reproduces the 1440×900 review images.
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
- Idle keep-alive closes are normal and produce no stderr output. Lake pins the
  `oramasearch/beast2` `orama-main` carrier branch that suppresses its former end-of-stream line.

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
  `agent_guard` today, versioned by content hash).
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
- **Broader Zetta linkage.** The operator-control and Loki HTTP clients link the client-only
  transport from the authorized bridge source path. Any further Zetta target remains a separate
  decision and must preserve one copy of the networking stack.

## Never

- Never run schema migrations from lake pods (the operator's PostgresSchema Job does, once).
- Never add an auth path the API does not have; the UI session is the API bearer.
- Never add a second attestation or signing framework. TAAP/1 is the security model.
- Never make this repository public.
- Never let an HTTP type leak out of `src/api/http_server.cpp`.

## Before finishing any change

1. **Document the change in `docs/<topic>/*.md`** (see `docs/README.md`): what, why, how to
   verify, how to change. New topic → new folder + a row in `docs/README.md`. This is a must.
2. `README.md` is the front door only (what it is, build and run, pointers); never append status or
   feature sections to it. Update `docs/design/status.md` for the status of your area and
   `docs/README.md` for any new page.
3. Does this file's **Status** and **Open decisions** still hold?
4. `cmake --build build && ctest --test-dir build` green, and the binary smoke-tested by hand.
5. Commit means push.

## Formatting

`.clang-format` at the root is the formatter configuration (LLVM base, 100 columns, 4-space indents,
one statement per line). Format only the lines you change (`git clang-format`) before each commit;
existing code is not reformatted wholesale. On macOS the Homebrew binary is
`/opt/homebrew/opt/llvm/bin/clang-format`; on Linux install `clang-format-19`.
