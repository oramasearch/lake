# lake

**The lake binary.** The hosted service at `lake.live` — forge, drive, platform,
identity, secrets, PKI, artifact registry and search — over one content-addressed store.

liquid is the tool. lake is where it collects.

> **Status: the control surface serves.** `lake` binds, serves the hello page, the `/v1`
> capabilities (password, TOTP with one-use recovery codes, lake-signed passkey bindings, OAuth
> identity, organization SAML/OIDC providers, federation policy and SCIM 2.0 user/group
> provisioning; row-resolved sessions, devices and scoped tokens; tenants, repositories,
> organization administration and policy over the Postgres control plane), public repository
> reading through libliquid, forge-designed repository, source, review, issue, release, webhook,
> and branch-protection pages, the OpenAPI
> document and MCP over Zetta's serving stack (Beast2 on Capy/Corosio), operational probes,
> Prometheus metrics and JSON request logs. Protected operations accept the operator bearer or a
> row-resolved user credential; probes and documented identity and transparency-monitoring reads
> are unauthenticated.
> The specification lives in
> [`oramasearch/liquid`](https://github.com/oramasearch/liquid) under `docs/features/` —
> principally [`18-lake-service.md`](https://github.com/oramasearch/liquid/blob/main/docs/features/18-lake-service.md).
> The image `oramasearch/lake` (private) runs under the `Lake` CR on `orama-operator`; see
> **Container image** below. GitHub Actions defines matching pull-request builds and tagged image
> publishing; it becomes active once its required credentials are configured. Repository lifecycle
> capabilities now create, rename, transfer, set `private` / `organization` / `public` visibility,
> and tombstone repositories in migration `0006_repository_lifecycle`; every outcome is recorded
> through the signed audit registry. Lake links libliquid as a library-only submodule and opens one
> customer store below `LAKE_STORE_ROOT/<customer id>/store`. liquid #160, #165, and #192's
> client, public server, protocol, and per-reference authorization and outcome contracts are
> pinned. Lake
> serves push, pull, missing-object negotiation, chunk upload, and ranged chunk read through the
> registry, with per-change and protected-reference policy,
> offline-grant revocation checks, exact refusal reasons, independent reference application,
> opt-in atomic refusal, and signed outcome audit.
> The source tree also provides signed portable repository export and import, including liquid
> history/object transfer, forge state, grants, artifact evidence, an optional Git projection, and
> a server-rendered export download page. Recorded GitHub imports map Git history, issues,
> reviews, and workflow documents with an explicit fidelity report.
> Migration `0011_offline_recovery` adds bounded cached-read grants, recovery codes, 24-hour email
> recovery, two-administrator organization recovery, device notices, and designated custody of
> client-wrapped repository keys. Account recovery does not recreate a lost repository key.
> Repository search is available over hot branch tips, immutable release and tag snapshots, exact
> changes, and complete history through the registry, including REST, OpenAPI, MCP, and the
> `/search` page. It persists blob-deduplicated BM25F and positional-trigram segments at
> `LAKE_STORE_ROOT/<customer>/index/`, reports `hot`, `snapshot`, or `scan` mode, publishes through
> atomic manifest replacement, filters candidates with the batched policy engine, and records the
> signed query outcome through the registry. Change pages render libliquid's per-path structural
> review with safe Markdown, text hunks, line comments, modes, blob identities, file statistics,
> conflict values, checks, and attestations. Issue, release, webhook, and branch-protection pages
> expose their existing forge and signed-audit records. Artifact pages provide filtered cursor pages of results
> plus result outputs, timing, cache state, verified attestations, and retained trace summaries.
> Metering migration `0014_metering` aggregates hourly unique stored bytes, sync transfer, and
> build compute. The `metering_usage` capability and `/metering` page apply configured integer
> pricing. Per-customer quotas override the configured default and throttle new writes without
> changing existing content. Organization and personal settings expose only these audited
> capability-backed actions.
> Migration `0009_secrets` adds customer-namespaced organization mounts, versioned
> static secrets with delete/restore/destroy history, and named encryption keys with rotation,
> encrypt/decrypt/rewrap, Ed25519 signatures, HMAC, derivation and customer-KEK rotation. The
> registry records every operation, including mount-grant denials, and the secrets pages expose
> metadata without rendering protected values or key material.
> Customer PKI adds private root and online-intermediate authorities, SPIFFE X.509 and JWT SVIDs,
> client, device, OpenSSH user and OpenSSH host certificates, and a customer-scoped RFC 8555
> ACME HTTP-01 order flow under migration `0013_pki`. ACME requests use one-use nonces and ES256
> account JWS, and finalization signs the submitted CSR key.
> Caller exchanges persist verified non-secret metadata, while root-recovery ceremonies persist
> quorum, expiry, completion, and one-way share evidence. OIDC/JWT claim policy comes from the
> shared federation issuer store. All remain on the signed audit path.
> Drive presents the same repositories as folders with stable-node ACLs, server-rendered previews,
> effective-access sharing, expiring links, path history, and restore-as-a-new-change. Models and
> datasets use native resumable liquid sync with chunk-level deduplication and unique-byte
> metering. The Hugging Face-compatible adapter supports repository creation, file/folder upload,
> basic LFS, listings, model metadata, snapshots, and named or digest downloads. Model reads and
> the model page expose revision storage savings and a verified liquid-attestation chain from the
> training dataset and code revision through the recorded run. Migration `0015_drive` stores link
> expiry and revocation.
> The source tree also serves threshold-signed TUF root, targets, snapshot, and timestamp metadata
> from immutable instance-local objects under `LAKE_STORE_ROOT/<instance>/log/tuf/`. Publication
> refuses expired, rolled-back, mixed, or under-threshold metadata; root rotation requires both the
> trusted and replacement root thresholds, and every outcome is signed in the audit stream.
> Transparency checkpoints use `LAKE_LOG_MODE`: air-gapped makes no outbound call; witnessed
> (the runtime default) requires configured witness cosignatures; anchored additionally retains
> an aggregation proof. `/v1/transparency_log_configuration` and `/audit` expose the effective
> public configuration. The operator handoff is [`docs/control-plane/log.md`](docs/control-plane/log.md).
> Canonical TAAP attestations append to immutable 1,024-entry and interior-hash tiles under the
> instance log. Signed checkpoints commit to their Merkle root. Public tile and compact
> inclusion/consistency-proof capabilities bind responses to exact signed checkpoints. The hosted
> `/transparency` identity monitor recomputes the root before returning matches, so a modified tile
> is refused.
> Active/standby Lake instances also replicate customer repositories through durable liquid
> cursors and replicate the transparency log only from a matching Merkle prefix. Signed
> checkpoints are verified and adopted without rollback. The consistency model and operator
> handoff are in [`docs/api/replication.md`](docs/api/replication.md) and
> [`docs/control-plane/replication.md`](docs/control-plane/replication.md).

> A configured RFC 3161 TSA timestamps checkpoint signature values into DER RFC 4998 evidence
> records under `LAKE_STORE_ROOT/<instance>/log/timestamps/`. Lake verifies TSA trust, nonce, imprint
> and signature, renews each record before its configured age or signer-certificate expiry margin,
> and records timestamp creation, renewal, and reads through the registry audit path.
> Application authentication migration `0016_auth_apps` adds isolated user pools, hosted and
> custom-domain identity pages, per-application JWT signing and JWKS, lifecycle webhooks, and
> operator pages for application users and organizations. The registry authorizes and audits its
> application management capabilities.
> Agent-session migration `0017_agent_sessions` adds short-lived Ed25519 session identities
> delegated to durable agents through logged `delegation/1` attestations. The private seed is
> returned once and never stored; signed drips verify as the durable agent until expiry. The
> contract is [`docs/api/agents.md`](docs/api/agents.md).

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
| `GET /` | the hello page in the server-rendered Orama application shell |
| `GET /style.css` | the token-driven light and dark stylesheet (`public, max-age=86400`) |
| `GET /fonts/<IBM Plex face>.woff2` | embedded Sans and Mono faces (`public, max-age=31536000, immutable`) |
| `GET /healthz`, `GET /readyz` | unauthenticated liveness and control-store readiness probes |
| `GET /metrics` | Prometheus metrics, guarded by the same bearer or session cookie as capabilities |
| `GET /v1/server_info` | `{ok, value: {name, node, version, api, store}}`, no auth |
| `GET /sign-up`, `/sign-in`, `/mfa`, `/passkey-sign-in` | public password, TOTP, OAuth and passkey entry pages |
| `GET /settings`, `/sessions`, `/passkeys`, `/tokens` | authenticated profile, session/device, passkey and scoped-token management pages |
| `GET /applications`, `/application`, `/application/users`, `/application/organizations`, `/application/events` | application registration and operator management pages |
| `GET /apps/<id>/sign-up`, `/sign-in`, `/mfa`, `/passkey-sign-in` | themed application identity pages; a configured custom host serves its equivalent entry pages |
| `GET /v1/apps/<id>/.well-known/jwks.json` | the application's public Ed25519 JSON Web Key Set |
| `GET /customers`, `/repositories`, `/repository?id=`, `/organizations`, `/organization?id=`, `/team?id=`, `/member?id=`, `/teams`, `/members`, `/invitations`, `/grants`, `/access`, `/search`, `/metering`, `/secret-mounts`, `/secrets`, `/encryption-keys`, `/audit` | the server-rendered pages behind the sign-in (`/login`, `POST /session`, `POST /logout`) |
| `GET /transparency?identity=<taap-id>` | public hosted identity monitor over checkpoint-verified bulk tiles |
| `GET /repository/tree`, `/file`, `/changes`, `/change`, `/issues`, `/issue`, `/releases`, `/webhooks`, `/protection`, `/conflicts`, `/artifacts`, `/artifact` | forge pages over authorized repository, review, issue, release, webhook, policy, signed-audit, and libliquid records; file pages include highlighted line anchors and blame, and change pages include review discussion, diff views, checks, and attestations. |
| `GET /drive`, `/drive/preview`, `/drive/download`, `/drive/history`, `/drive/history/diff`, `/drive/conflict`, `/drive/share`, `/share/:token` | server-rendered drive list/grid browsing, registered previews with exact-byte fallback, retained version comparison and conflict values, effective access, and scoped links; create, upload, rename, move, delete, restore, and sharing mutations use POST under `/drive/` |
| `/api/models/*`, `/api/datasets/*`, model/dataset resolve and basic-LFS routes | the documented `huggingface_hub` create, upload, list, info, snapshot, and file-download subset; see [`docs/api/models.md`](docs/api/models.md) |
| `GET /models/:namespace/:name`, `/models/:id` | model revisions, files, deduplication savings, and verified training provenance |
| `GET /v1/<read>`, `POST /v1/<action>` | the capability registry as REST: identity and application authentication, short-lived agent-session keys, account recovery, offline grants, tenants, repositories, organization administration, policy, repository search, static secrets, encryption, TUF metadata, transparency monitoring, asynchronous replication, the lake identity and the signed audit stream; bearer required except documented public identity/recovery entry points, transparency reads, and application JWKS |
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
Metering and quota configuration uses the `LAKE_PRICE_*` and `LAKE_STORAGE_QUOTA_BYTES`
environment variables documented in [`docs/api/metering.md`](docs/api/metering.md).
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
| `include/lake/api/registry.hpp`, `sync_store.hpp`, `rest.hpp`, `mcp.hpp`, `capabilities.hpp` | the HTTP-free capability registry, liquid-sync boundary, and surface renderers |
| `include/lake/control/`, `src/control/` | the control-plane store, policy engine, application authentication, static-secret lifecycle and encryption service: interface, in-memory, Postgres (`postgres_store.cpp` is the one TU that names libpqxx) |
| `include/lake/telemetry/metrics.hpp`, `src/telemetry/metrics.cpp` | dependency-free Prometheus registry and measured store decorator |
| `include/lake/api/web_ui.hpp`, `src/api/web_ui.cpp` | pure HTML renderers (string in, string out) |
| `include/lake/audit/`, `src/audit/` | canonical-CBOR audit signing, the lake identity lifecycle, JSON-lines export and chain verification (compiled through the Postgres TU so OpenSSL stays confined) |
| `include/lake/transparency/`, `src/transparency/` | immutable TUF metadata storage, threshold verification, cross-role binding, root rotation, checkpoint-bound Merkle entry/hash tiles and compact proofs, identity monitoring, RFC 3161 timestamping, and RFC 4998 archive renewal |
| `include/lake/control/agents.hpp`, `src/control/agents.cpp` | short-lived agent-session key minting, canonical TAAP delegation, expiry, and durable drip attribution |
| `src/main.cpp` | CLI and the owner loop (`wait_for_work` / `drain`) |
| `tests/` | GoogleTest; `ctest --test-dir build` |
| `tools/ui/` | reproducible 1440×900 rendering for the static UI visual-direction mocks |
| `assets/fonts/`, `cmake/embed_fonts.cmake` | OFL-licensed IBM Plex WOFF2 sources and their binary embed step |
| `modules/` | `liquid` (library-only), `http`, `beast2`, `libpqxx` — vendored dependencies |
| `Dockerfile`, `.dockerignore` | the Linux/amd64 image (see above); the context excludes `build/` and `.git/` |
| `db/schema.sql` | **the control-plane schema, source of truth.** Identity and application user pools, offline authorization and recovery, tenants, repositories, organization administration, SSO federation, policy, audit, static secrets and wrapped encryption keys. The operator vendors a byte-identical copy and applies it per Lake through a `PostgresSchema`. |

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
| Identity, secrets, PKI | TAAP/1 identities, application authentication, agent-session keys, dynamic secrets, one CA |
| Search | BM25F plus a positional trigram index, over history and inside artifacts |

## Internal documentation

[`docs/`](docs/) — one folder per topic: [design](docs/design/), [serving](docs/serving/),
[control-plane](docs/control-plane/), [deployment](docs/deployment/), [build](docs/build/),
[api](docs/api/), [ui](docs/ui/). Every change to the system lands
its doc there in the same change; this README is the summary.

## Where this sits

Part of the liquid project. See
[`oramasearch/liquid-bridge`](https://github.com/oramasearch/liquid-bridge) for the
cross-module map and the action plan.
