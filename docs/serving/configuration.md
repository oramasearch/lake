# Command line and environment

`src/main.cpp` reads the environment first and lets a flag override it. `lake --help` prints the
same list. Every value below is read once at startup; SIGINT and SIGTERM stop the server cleanly.

## Command line

| Flag | Meaning |
|---|---|
| `lake version` (or `--version`) | print the version and exit |
| `--listen HOST:PORT` | the bind address; the default port is 4900 (Zetta is 4800) |
| `--node NAME` | names the node; in a replicated deployment it is the stable region name ([`../control-plane/replication.md`](../control-plane/replication.md)) |
| `--control-db URI` | Postgres URI of the control plane (default `$LAKE_CONTROL_DB_URI`) |
| `--memory-store` | an in-memory control plane for local demos and tests |
| `--store-root PATH` | customer and instance data root (default `$LAKE_STORE_ROOT`) |
| `--external-origin ORIGIN` | external HTTP(S) origin for repository terminal commands (default `$LAKE_EXTERNAL_ORIGIN`; otherwise derived from `$LAKE_PUBLIC_HOST`; hidden only when both are unset) |
| `--admin-token TOKEN` | bearer for `/v1` capabilities and MCP tools (default `$LAKE_ADMIN_TOKEN`) |
| `--identity-seed BASE64` | base64 Ed25519 seed for lake's TAAP identity (default `$LAKE_IDENTITY_SEED`) |
| `--instance-kek VALUE` | key-encryption key for a generated identity and wrapped keys (default `$LAKE_INSTANCE_KEK`) |
| `--tsa-url URL` | RFC 3161 endpoint (default `$LAKE_TSA_URL`) |

With `--memory-store` the control plane is `InMemoryStore`; otherwise a non-empty control-DB URI
selects the Postgres store. `server_info` reports which one as `store` (`memory`, `postgres`, or
`none` when neither is configured).

## Environment

| Variable | Meaning | Owner |
|---|---|---|
| `LAKE_CONTROL_DB_URI` | Postgres control-plane URI; the operator injects it from the CNPG Secret. `LAKE_CONTROL_DB_PASSWORD` is injected beside it for the operator's use; lake reads the password from the URI | [`../control-plane/postgres.md`](../control-plane/postgres.md) |
| `LAKE_ADMIN_TOKEN` | the operator bearer every capability except `server_info` and the documented public entry points requires; also the UI operator sign-in | [`../api/auth.md`](../api/auth.md) |
| `LAKE_REPLICATION_TOKEN` | bearer presented to peer Lake instances; defaults to the admin token when omitted | [`../api/replication.md`](../api/replication.md) |
| `LAKE_IDENTITY_SEED` | base64 32-byte seed of lake's TAAP signing identity; when absent lake generates one and stores it encrypted | [`../api/audit.md`](../api/audit.md) |
| `LAKE_INSTANCE_KEK` | customer-independent key-encryption key for the generated identity and wrapped keys; required wherever a Postgres store runs | [`../api/audit.md`](../api/audit.md), [`../api/encryption.md`](../api/encryption.md) |
| `LAKE_STORE_ROOT` | root of the per-customer liquid stores (`<customer>/store`), search segments of unswitched workspaces' repositories (`<customer>/index/`; a switched workspace's index is beside its store, lake#381) and the instance log (`<instance>/log/`) | [`../deployment/store-volume.md`](../deployment/store-volume.md), [`../api/search.md`](../api/search.md), [`../api/transparency.md`](../api/transparency.md) |
| `LAKE_STORE_OWNER_WAIT_SECONDS` | how long a starting lake waits for another process to release the store root, and how long each customer-store open waits for its liquid owner lock; default 90; must outlast the predecessor's preStop delay plus `LAKE_DRAIN_SECONDS`. Past it lake exits 1 naming the holder | [`../deployment/store-volume.md`](../deployment/store-volume.md#the-hand-off) |
| `LAKE_SEARCH_MEMORY_BYTES` | bound on the workspace search indexes kept loaded in memory; default 268435456 (256 MiB), at least 1 MiB; idle workspaces are evicted, least recently used first, and loaded again from their workspace Zetta on use (lake#381) | [`../api/search.md`](../api/search.md#where-the-index-lives-lake381-decision-54-s8) |
| `LAKE_DRAIN_SECONDS` | how long SIGTERM waits for in-flight requests before lake closes its stores and releases the store root; default 20 | [`../deployment/store-volume.md`](../deployment/store-volume.md#the-hand-off) |
| `LAKE_PUBLIC_HOST` | the Lake namespace value on every operator row; required when the operator Zetta client is enabled | [`../control-plane/platform.md`](../control-plane/platform.md) |
| `LAKE_EXTERNAL_ORIGIN` | browser-facing scheme, host and optional non-default port; used by clone/push instructions, with no default hostname | [`../ui/forge.md`](../ui/forge.md), [`../deployment/private-install.md`](../deployment/private-install.md) |
| `LAKE_INTERNAL_URL` | Lake's HTTP(S) in-cluster Service URL; its authority prefixes build image targets and the full URL is handed to agents | [`../api/builds.md`](../api/builds.md), [`../api/agent-launches.md`](../api/agent-launches.md) |
| `LAKE_MAX_REGISTRY_BLOB_BYTES` | maximum completed OCI blob size accepted by streamed `PATCH` and final `PUT` uploads; defaults to 4294967296 (4 GiB); all non-registry request bodies retain the 64 MiB adapter limit | [`../api/builds.md`](../api/builds.md), [`http.md`](http.md) |
| `LAKE_OPERATOR_ZETTA_URL`, `LAKE_OPERATOR_ZETTA_TOKEN` | operator Zetta service URL and secret bearer; set together, they enable platform sync and writes | [`../control-plane/platform.md`](../control-plane/platform.md) |
| `LAKE_LOGS_URL`, `LAKE_LOGS_TOKEN` | Loki-compatible log endpoint and optional secret bearer; enable `environment_logs` and the log pages | [`../api/platform.md`](../api/platform.md) |
| `LAKE_APPS_WILDCARD` | parent domain required for Ship app hosts; also enables app lifecycle topics on the Lake bus | [`../api/web-apps.md`](../api/web-apps.md) |
| `LAKE_APPS_RUNTIME_IMAGE` | required base image for `build_app`; supplied by the operator from `spec.apps.runtimeImage` and recorded as `APP_RUNTIME_IMAGE` | [`../api/builds.md`](../api/builds.md) |
| `LAKE_APPS_RESERVED_NAMES` | comma-separated additional Ship app names to refuse; surrounding whitespace is ignored | [`../api/web-apps.md`](../api/web-apps.md) |
| `LAKE_APPS_PER_CUSTOMER` | maximum live Ship apps per space; zero means no configured limit | [`../api/web-apps.md`](../api/web-apps.md) |
| `LAKE_APP_REPLICAS_PER_CUSTOMER` | maximum aggregate desired Ship app replicas per space; zero means no configured limit | [`../api/web-apps.md`](../api/web-apps.md) |
| `LAKE_PLATFORM_SYNC_SECONDS` | platform poll interval, default 15, must be greater than zero | [`../api/platform.md`](../api/platform.md) |
| `LAKE_LOG_MODE` | `air-gapped`, `witnessed` (default) or `anchored`; read only when a store root is set | [`../control-plane/log.md`](../control-plane/log.md) |
| `LAKE_LOG_WITNESSES`, `LAKE_LOG_WITNESS_THRESHOLD`, `LAKE_LOG_AGGREGATOR` | JSON witness endpoint/public-key records, their quorum, and the aggregation endpoint anchored mode requires | [`../control-plane/log.md`](../control-plane/log.md), [`../api/transparency.md`](../api/transparency.md) |
| `LAKE_TSA_URL`, `LAKE_TSA_TRUST_CERT` | RFC 3161 endpoint and optional PEM trust roots for timestamp tokens | [`../api/transparency.md`](../api/transparency.md) |
| `LAKE_TSA_RENEWAL_SECONDS`, `LAKE_TSA_RENEW_BEFORE_SECONDS` | renewal policy for RFC 4998 evidence records; defaults 31536000 and 2592000 | [`../api/transparency.md`](../api/transparency.md) |
| `LAKE_PRICE_CURRENCY` (default `USD`), `LAKE_PRICE_STORAGE_GIB_HOUR_MICROS`, `LAKE_PRICE_TRANSFER_IN_GIB_MICROS`, `LAKE_PRICE_TRANSFER_OUT_GIB_MICROS`, `LAKE_PRICE_COMPUTE_SECOND_MICROS` | integer pricing applied by `metering_usage` and `/metering`; the storage price is reported but applies to nothing since repository storage has no hourly history (lake#348) | [`../api/metering.md`](../api/metering.md) |
| `LAKE_STORAGE_QUOTA_BYTES` | default per-customer storage quota; per-customer overrides are capability-backed | [`../api/metering.md`](../api/metering.md) |

Startup refuses to run when `LAKE_OPERATOR_ZETTA_URL` and `LAKE_OPERATOR_ZETTA_TOKEN` are not set
together, when the operator client is enabled without `LAKE_PUBLIC_HOST`, when
`LAKE_PLATFORM_SYNC_SECONDS` is zero, or when the transparency-log configuration is invalid; each
case exits with status 2 and one line on stderr. A store root still held by another process after
`LAKE_STORE_OWNER_WAIT_SECONDS` exits with status 1 and `lake: cannot start: ...` naming the holder. The startup JSON event records `admin_token_set`
and `replication_token_set` as `yes` or `no`, never a token value
([`telemetry.md`](telemetry.md)).

`LAKE_EXTERNAL_ORIGIN` is independent of the bind address. Set it to the origin customers use, for
example `https://code.example.internal:8443` behind a TLS proxy or `http://127.0.0.1:49127` for a
local instance. The scheme is lowercased and a trailing slash is accepted and removed. Startup
rejects values with credentials, paths, queries, fragments, missing hosts, invalid ports, or a
scheme other than HTTP(S).

Repository terminal commands select their destination in this order: an explicit
`--external-origin` or `LAKE_EXTERNAL_ORIGIN`; then `https://` plus `LAKE_PUBLIC_HOST`; then no
destination. The fallback uses the same browser-facing hostname that OAuth callbacks use. It cannot
express a non-default port or HTTP, so configure `LAKE_EXTERNAL_ORIGIN` for either. When both values
are unset, repository pages explain the missing configuration and show no terminal commands.
`Host`, `Forwarded`, and `X-Forwarded-*` headers never select the destination for a
credential-bearing sync command. Changing either value requires a restart. No new API capability is
introduced; clone and push use the existing sync registry.

## Build and test variables

| Variable | Meaning | Owner |
|---|---|---|
| `LAKE_ZETTA_SOURCE_DIR` | CMake cache path to the `zetta-bridge/modules/zetta` checkout; required | [`../build/local.md`](../build/local.md) |
| `LAKE_BUILD_TESTS` | CMake option, on by default | [`../build/local.md`](../build/local.md) |
| `LAKE_USE_SYSTEM_COMPILER` | CMake option that skips the Homebrew or `clang++-NN` auto-selection | [`../build/local.md`](../build/local.md) |
| `LAKE_TEST_DB_URI` | Postgres URI for the Postgres-backed tests; absent means they skip | [`../build/testing.md`](../build/testing.md) |

## Change

A new flag or variable is added to `usage()` and the table above in the same change, with its
default, and the page that owns the behavior links back here.
