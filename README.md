# lake

lake is the hosted service around liquid: forge, drive, platform, registry and transparency log,
identity, secrets and PKI, and search, served by one C++26 binary with a server-rendered web UI.
The control plane is Postgres, the data plane is Zetta, and repository data goes through
libliquid, consumed as a library. The operator deploys it as a `Lake` custom resource. liquid is
the tool; lake is where it collects. The longer statement, the six surfaces and where lake sits
in the project are in [`docs/design/overview.md`](docs/design/overview.md).

## Status

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
> customer store below `LAKE_STORE_ROOT/<customer id>/store`. Liquid's client, public server,
> sync protocol, per-reference outcomes, and transparency verifier contracts are pinned. Lake
> serves push, pull, missing-object negotiation, chunk upload, and ranged chunk read through the
> registry, with per-change and protected-reference policy,
> offline-grant revocation checks, exact refusal reasons, independent reference application,
> opt-in atomic refusal, and signed outcome audit.
> The source tree also provides signed portable repository export and import, including liquid
> history/object transfer, forge state, grants, artifact evidence, an optional Git projection, and
> a server-rendered export download page. Recorded GitHub imports map Git history, issues,
> reviews, and workflow documents with an explicit fidelity report.
> Platform environments now mirror append-only deployment, plain environment-variable, and domain
> rows from the operator's Zetta control plane. Policy-checked capabilities and server-rendered
> environment/detail/log pages remain readable from the auditable Postgres mirror during an
> operator-control outage; mutations are refused until sync recovers.
> Migration `0011_offline_recovery` adds bounded cached-read grants, recovery codes, 24-hour email
> recovery, two-administrator organization recovery, device notices, and designated custody of
> client-wrapped repository keys. Account recovery does not recreate a lost repository key.
> Repository search is available over hot branch tips, immutable release and tag snapshots, exact
> changes, and complete history through the registry, including REST, OpenAPI, MCP, and the
> `/search` page. It persists blob-deduplicated BM25F and positional-trigram segments at
> `LAKE_STORE_ROOT/<customer>/index/`, reports `hot`, `snapshot`, or `scan` mode, publishes through
> atomic manifest replacement, filters candidates with the batched policy engine, and records the
> signed query outcome through the registry. Change pages render libliquid's per-path structural
> Published artifact outputs are decomposed through libliquid's format registry after sync
> publication and by the audited `reindex_artifacts` action for existing results. Scoped
> `in:artifact:<name>[@<version>]` search returns text and source members with their artifact,
> member path, digest, and line; binary and deeper-nested members remain listed only. Change pages
> render libliquid's per-path structural
> review with safe Markdown, text hunks, line comments, modes, blob identities, file statistics,
> conflict values, checks, output paths, and verified attestation profiles. A Postgres-backed
> scenario proves an unlanded Liquid review, cursor movement after rewrite, approval, publication,
> and graph-derived evidence without repository automation YAML. Issue, release, webhook, and
> branch-protection pages expose their existing forge and signed-audit records. Artifact pages
> provide filtered cursor pages of results plus result outputs, timing, cache state, verified
> attestations, and retained trace summaries.
> Metering migration `0014_metering` aggregates hourly unique stored bytes, sync transfer, and
> build compute. The `metering_usage` capability and `/metering` page apply configured integer
> pricing. Per-customer quotas override the configured default and throttle new writes without
> changing existing content. Organization and personal settings expose only these audited
> capability-backed actions.
> Migration `0009_secrets` adds customer-namespaced organization mounts, versioned
> static secrets with delete/restore/destroy history, and named encryption keys with rotation,
> encrypt/decrypt/rewrap, Ed25519 signatures, HMAC, derivation and customer-KEK rotation. The
> registry records every operation, including mount-grant denials, and the secrets pages expose
> metadata without rendering protected values or key material. Migration `0020_identity_exit`
> adds encrypted, renewable and revocable dynamic-secret leases whose signed lifecycle audit rows
> are committed to the transparency log.
> Customer PKI adds private root and online-intermediate authorities, SPIFFE X.509 and JWT SVIDs,
> client, device, OpenSSH user and OpenSSH host certificates, and a customer-scoped RFC 8555
> ACME HTTP-01 order flow under migration `0013_pki`. ACME requests use one-use nonces and ES256
> account JWS, and finalization signs the submitted CSR key. Agent TAAP public keys now receive
> short-lived certificates with explicit rotation links, revocation, and API status enforcement.
> Caller exchanges persist verified non-secret metadata, while root-recovery ceremonies persist
> quorum, expiry, completion, and one-way share evidence. OIDC/JWT claim policy comes from the
> shared federation issuer store. All remain on the signed audit path.
> Drive presents the same repositories as folders with stable-node ACLs, server-rendered previews,
> effective-access sharing before creation, expiring links with stated revocation consequences, a
> shared-with-me view, path history, and restore-as-a-new-change. Models and
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
> is refused. Lake pins liquid #207's client verifier and the identity-exit scenario verifies a
> dynamic-secret audit leaf against Lake's compact inclusion proof.
> is refused. A self-contained proof-bundle capability exports the identity, trust configuration,
> checkpoint, required tiles, and inclusion path for offline verification by liquid; retained
> equal-size checkpoints with different roots are rejected as a split view.
> OCI Distribution, npm, Python package, and debuginfod routes persist native artifacts under the
> instance registry root. Writes use the administrator bearer and published bytes are public reads.
> The protocol contract and executable-client coverage are in
> [`docs/api/registry.md`](docs/api/registry.md).
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
> application management capabilities. A fresh libliquid TAAP-key assertion can now issue a lake
> session, and that session can complete a one-use PKCE application authorization-code flow whose
> ID token verifies against the application's JWKS.
> Agent-session migration `0017_agent_sessions` adds short-lived Ed25519 session identities
> delegated to durable agents through logged `delegation/1` attestations. The private seed is
> returned once and never stored; signed drips verify as the durable agent until expiry. The
> contract is [`docs/api/agents.md`](docs/api/agents.md).
The control surface serves in production: `Lake/prod` on AKS at https://lake.oramadev.net runs
the capability registry as REST `/v1`, OpenAPI and MCP, the Postgres control plane, and the
server-rendered pages behind the operator sign-in, with the state and runbook in
[`docs/deployment/aks.md`](docs/deployment/aks.md). The source tree goes further than the deployed
image: identity, SSO, forge pages, repository sync, search, secrets and PKI, drive and models,
metering, the transparency log, replication and platform mirroring are implemented and tested
here and wait on a new image, the operator's schema copy and volume support before they run in
production. [`docs/design/status.md`](docs/design/status.md) lists each area with its migration
and contract page.

## Build and run

Requirements on macOS: CMake 3.28 or newer, Ninja, Homebrew LLVM (Clang 17 or newer), OpenSSL 3
and libpq (`brew install llvm cmake ninja openssl@3 libpq`), Node for the UI checks in the test
suite, and a checkout of `zetta-bridge` for `LAKE_ZETTA_SOURCE_DIR`. Clone with submodules; the
HTTP stack and libliquid are vendored under `modules/`; the pins and private patch-carrier update
rules are in [`docs/build/dependencies.md`](docs/build/dependencies.md).

```sh
git clone --recurse-submodules git@github.com:oramasearch/lake.git
cd lake
cmake -S . -B build -G Ninja \
  -DLAKE_ZETTA_SOURCE_DIR=/path/to/zetta-bridge/modules/zetta
cmake --build build -j 6
ctest --test-dir build --output-on-failure
```

| Route | Serves |
|---|---|
| `GET /` | the hello page in the server-rendered Orama application shell |
| `GET /style-<hash>.css`, `GET /fonts/<IBM Plex face>-<hash>.woff2` | content-hashed design-system assets (`public, max-age=31536000, immutable`) linked by every page |
| `GET /style.css`, `GET /fonts/<IBM Plex face>.woff2` | compatibility aliases for direct and older links (`no-cache`) |
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
| `GET /environments?repository_id=`, `/environment?repository_id=&name=`, `/logs?repository_id=&environment=` | platform summaries, deployment history, capability-backed environment/domain forms, and cursor-polling Loki-compatible logs; stale mirror reads show a warning |
| `GET /drive`, `/drive/preview`, `/drive/download`, `/drive/history`, `/drive/history/diff`, `/drive/conflict`, `/drive/share`, `/share/:token` | server-rendered drive list/grid browsing, registered previews with exact-byte fallback, retained version comparison and conflict values, effective access, and scoped links; create, upload, rename, move, delete, restore, and sharing mutations use POST under `/drive/` |
| `/api/models/*`, `/api/datasets/*`, model/dataset resolve and basic-LFS routes | the documented `huggingface_hub` create, upload, list, info, snapshot, and file-download subset; see [`docs/api/models.md`](docs/api/models.md) |
| `/v2/*`, npm package paths, `/legacy/`, `/simple/*`, `/packages/*` | OCI Distribution, npm publication/install, and Python publication/install protocols; see [`docs/api/registry.md`](docs/api/registry.md) |
| `/buildid/*`, `PUT /_lake/debug/*` | public debuginfod reads and protected executable, debug-information, and source publication |
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
Platform sync uses `LAKE_OPERATOR_ZETTA_URL`, secret bearer `LAKE_OPERATOR_ZETTA_TOKEN`,
`LAKE_PUBLIC_HOST`, and optional `LAKE_PLATFORM_SYNC_SECONDS` (default 15). Logs use
`LAKE_LOGS_URL` and optional secret bearer `LAKE_LOGS_TOKEN`; see
[`docs/api/platform.md`](docs/api/platform.md).
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
Without `LAKE_TEST_DB_URI` the Postgres-backed cases skip. To run them against a local Postgres,
create a database, apply the schema, and set the two variables:

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
curl http://127.0.0.1:4900/v1/server_info
```

`--control-db` (or `LAKE_CONTROL_DB_URI`) selects a Postgres control plane instead, with
`--instance-kek` and `--store-root` for repository data. Every flag and environment variable is in
[`docs/serving/configuration.md`](docs/serving/configuration.md); the build in more detail, the
test targets and CI are in [`docs/build/local.md`](docs/build/local.md) and
[`docs/build/ci.md`](docs/build/ci.md).

## Documentation

[`docs/README.md`](docs/README.md) indexes one folder per topic. Every change to the system lands
its page there in the same change; this file is only the front door. The most useful pages:

- [`docs/design/overview.md`](docs/design/overview.md): what lake is and what it is built on.
- [`docs/design/status.md`](docs/design/status.md): what the source tree implements, by area.
- [`docs/api/README.md`](docs/api/README.md): the capability registry rendered as REST, OpenAPI
  and MCP, with every capability and how to add one.
- [`docs/serving/http.md`](docs/serving/http.md): the route table and the rules that keep the
  server correct.
- [`docs/serving/configuration.md`](docs/serving/configuration.md): flags and environment.
- [`docs/ui/README.md`](docs/ui/README.md): the server-rendered pages and the session cookie.
- [`docs/control-plane/schema.md`](docs/control-plane/schema.md): `db/schema.sql`, the tenant
  model and how the schema reaches a Lake.
- [`docs/deployment/aks.md`](docs/deployment/aks.md): the production instance and its runbook.
- [`docs/build/layout.md`](docs/build/layout.md): the source tree.

The product specification is `oramasearch/liquid` `docs/features/18-lake-service.md`. lake is
part of the liquid project; [`oramasearch/liquid-bridge`](https://github.com/oramasearch/liquid-bridge)
holds the cross-module map and the action plan.

## Repository rules

[`CLAUDE.md`](CLAUDE.md) is the instruction file for every contributor and agent: the rules that
keep the server Zetta-shaped, what lake is built on, what never changes, and what to do before
finishing a change. [`AGENTS.md`](AGENTS.md) restates the non-negotiables for Codex.

## Licence

No licence is declared in this repository, and `CLAUDE.md` says it stays private. The licence
decision for the liquid project is tracked in
[oramasearch/liquid-bridge#2](https://github.com/oramasearch/liquid-bridge/issues/2).
