# lake

**The lake binary.** The hosted service at `lake.live` — forge, drive, platform,
identity, secrets, PKI, artifact registry and search — over one content-addressed store.

liquid is the tool. lake is where it collects.

> **Status: the control surface serves.** `lake` binds, serves the hello page, the `/v1`
> capabilities (password, TOTP with one-use recovery codes, lake-signed passkey bindings, OAuth
> identity, organization SAML/OIDC providers, federation policy and SCIM 2.0 user/group
> provisioning; row-resolved sessions, devices and scoped tokens; tenants, repositories,
> organization administration and policy over the Postgres control plane), public repository
> reading through libliquid, the pages to explore them, the OpenAPI
> document and MCP over Zetta's serving stack (Beast2 on Capy/Corosio), operational probes,
> Prometheus metrics and JSON request logs. Protected operations accept the operator bearer or a
> row-resolved user credential; probes and documented identity entry points are unauthenticated.
> The specification lives in
> [`oramasearch/liquid`](https://github.com/oramasearch/liquid) under `docs/features/` —
> principally [`18-lake-service.md`](https://github.com/oramasearch/liquid/blob/main/docs/features/18-lake-service.md).
> The image `oramasearch/lake` (private) runs under the `Lake` CR on `orama-operator`; see
> **Container image** below. GitHub Actions defines matching pull-request builds and tagged image
> publishing; it becomes active once its required credentials are configured. Repository lifecycle
> capabilities now create, rename, transfer, set `private` / `organization` / `public` visibility,
> and tombstone repositories in migration `0006_repository_lifecycle`; every outcome is recorded
> through the signed audit registry. Lake links libliquid as a library-only submodule and opens one
> customer store below `LAKE_STORE_ROOT/<customer id>/store`. liquid #160, #165, and #184's
> client, public server, protocol, and reference-update authorization contracts are pinned. Lake
> serves push, pull, missing-object negotiation, chunk upload, and ranged chunk read through the
> registry, with per-change and protected-reference policy,
> offline-grant revocation checks, exact refusal reasons, atomic refusal, and signed outcome audit.
> The source tree also provides signed portable repository export and import, including liquid
> history/object transfer, forge state, grants, artifact evidence, an optional Git projection, and
> a server-rendered export download page. Recorded GitHub imports map Git history, issues,
> reviews, and workflow documents with an explicit fidelity report.
> Migration `0011_offline_recovery` adds bounded cached-read grants, recovery codes, 24-hour email
> recovery, two-administrator organization recovery, device notices, and designated custody of
> client-wrapped repository keys. Account recovery does not recreate a lost repository key. Hot branch-tip
> search is available over those stores through the registry, including REST, OpenAPI, MCP, and
> the `/search` page. It persists blob-deduplicated BM25F and positional-trigram segments at
> `LAKE_STORE_ROOT/<customer>/index/`, publishes a change through atomic manifest replacement,
> filters candidates with the batched policy engine, and records the signed query outcome through
> the registry. Change pages render libliquid's per-path structural diff, text hunks, modes, blob
> identities, moves, and conflict values. Artifact pages provide filtered cursor pages of results
> plus result outputs, timing, cache state, verified attestations, and retained trace summaries.
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
| `GET /healthz`, `GET /readyz` | unauthenticated liveness and control-store readiness probes |
| `GET /metrics` | Prometheus metrics, guarded by the same bearer or session cookie as capabilities |
| `GET /v1/server_info` | `{ok, value: {name, node, version, api, store}}`, no auth |
| `GET /sign-up`, `/sign-in`, `/mfa`, `/passkey-sign-in` | public password, TOTP, OAuth and passkey entry pages |
| `GET /sessions`, `/passkeys`, `/tokens` | authenticated session/device, passkey and scoped-token management pages |
| `GET /customers`, `/repositories`, `/repository?id=`, `/organizations`, `/teams`, `/members`, `/invitations`, `/grants`, `/access`, `/search`, `/secret-mounts`, `/secrets`, `/encryption-keys`, `/audit` | the server-rendered pages behind the sign-in (`/login`, `POST /session`, `POST /logout`) |
| `GET /repository/tree`, `/file`, `/changes`, `/change`, `/conflicts`, `/artifacts`, `/artifact` | repository pages; public repositories are readable without a session, while other repository visibility levels redirect to sign-in. Change diffs and paged artifact results include their libliquid records. |
| `GET /v1/<read>`, `POST /v1/<action>` | the capability registry as REST: identity, account recovery, offline grants, tenants, repositories, organization administration, policy, repository search, static secrets, encryption, the lake identity and the signed audit stream; bearer required except documented public identity/recovery entry points |
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
| `include/lake/control/`, `src/control/` | the control-plane store, policy engine, static-secret lifecycle and encryption service: interface, in-memory, Postgres (`postgres_store.cpp` is the one TU that names libpqxx) |
| `include/lake/telemetry/metrics.hpp`, `src/telemetry/metrics.cpp` | dependency-free Prometheus registry and measured store decorator |
| `include/lake/api/web_ui.hpp`, `src/api/web_ui.cpp` | pure HTML renderers (string in, string out) |
| `include/lake/audit/`, `src/audit/` | canonical-CBOR audit signing, the lake identity lifecycle, JSON-lines export and chain verification (compiled through the Postgres TU so OpenSSL stays confined) |
| `src/main.cpp` | CLI and the owner loop (`wait_for_work` / `drain`) |
| `tests/` | GoogleTest; `ctest --test-dir build` |
| `modules/` | `liquid` (library-only), `http`, `beast2`, `libpqxx` — vendored dependencies |
| `Dockerfile`, `.dockerignore` | the Linux/amd64 image (see above); the context excludes `build/` and `.git/` |
| `db/schema.sql` | **the control-plane schema, source of truth.** Identity, offline authorization and recovery, tenants, repositories, organization administration, SSO federation, policy, audit, static secrets and wrapped encryption keys. The operator vendors a byte-identical copy and applies it per Lake through a `PostgresSchema`. |

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
[deployment](docs/deployment/), [build](docs/build/), [api](docs/api/), [ui](docs/ui/). Every change to the system lands
its doc there in the same change; this README is the summary.

## Where this sits

Part of the liquid project. See
[`oramasearch/liquid-bridge`](https://github.com/oramasearch/liquid-bridge) for the
cross-module map and the action plan.
