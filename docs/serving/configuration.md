# Command line and environment

`src/main.cpp` reads the environment first and lets a flag override it. `lake --help` prints the
same list. Every value below is read once at startup; SIGINT and SIGTERM stop the server cleanly.

## Command line

| Flag | Meaning |
|---|---|
| `lake version` (or `--version`) | print the version and exit |
| `lake migrate [--control-db URI] [--wait-seconds N]` | apply the schema built into the binary to the control database (default `$LAKE_CONTROL_DB_URI`) idempotently, in one transaction under an advisory lock, print what it applied as JSON and exit 0 (applied or current), 1 (failed, rolled back), 2 (usage) or 3 (unreachable within `--wait-seconds`, default 0). Reads no other setting; the server never migrates ([`../deployment/operator-interface.md`](../deployment/operator-interface.md), lake#1119) |
| `lake manifest` | print, as JSON, what this image needs from the operator: version, revision, schema (latest migration, guard tables), Stripe webhook events and products, and every environment variable with `required` and `secret` ([`../deployment/operator-interface.md`](../deployment/operator-interface.md#lake-manifest)) |
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
| `--tunnel-clients-dir PATH` | the tunnel client downloads (default `$LAKE_TUNNEL_CLIENTS_DIR`, then `/usr/share/lake/tunnel-clients`) |

With `--memory-store` the control plane is `InMemoryStore`; otherwise a non-empty control-DB URI
selects the Postgres store. `server_info` reports which one as `store` (`memory`, `postgres`, or
`none` when neither is configured).

## Environment

| Variable | Meaning | Owner |
|---|---|---|
| `LAKE_CONTROL_DB_URI` | Postgres control-plane URI; the operator injects it from the CNPG Secret. `LAKE_CONTROL_DB_PASSWORD` is injected beside it for the operator's use; lake reads the password from the URI | [`../control-plane/postgres.md`](../control-plane/postgres.md) |
| `ZETTA_EGRESS_ALLOW` | The Zettas lake may reach, in zetta's allowlist syntax (comma-separated `host:port`, `*.suffix:port`, CIDR; zetta `docs/governance/egress-policy.md`). Installed at startup as the policy of zetta's client, which refuses all but loopback without it (zetta v0.5.0, lake#1097). The operator renders it: the bus Zetta's `host:4800` and `*.svc.cluster.local:4800` for workspace Zettas. Unset: only Zettas on loopback; malformed: lake stops | zetta's egress policy |
| `LAKE_ADMIN_TOKEN` | the operator bearer every capability except `server_info` and the documented public entry points requires; also the UI operator sign-in | [`../api/auth.md`](../api/auth.md) |
| `LAKE_REPLICATION_TOKEN` | bearer presented to peer Lake instances; no default, never the admin token (lake#915) | [`../api/replication.md`](../api/replication.md) |
| `LAKE_REPLICATION_PEERS` | the peer Lake origins replication may pull from, comma separated; any other source is refused, so the replication token goes only to these | [`../api/replication.md`](../api/replication.md#peers-and-the-replication-credential) |
| `LAKE_REPLICATION_TIMEOUT_SECONDS` | the longest one repository pull may take (default 600) | [`../api/replication.md`](../api/replication.md#peers-and-the-replication-credential) |
| `LAKE_REPLICATION_ALLOW_PRIVATE_ADDRESSES` | `1` lets replication reach loopback and private addresses and plain http; local development and tests only | [`../api/replication.md`](../api/replication.md#peers-and-the-replication-credential) |
| `LAKE_IDENTITY_SEED` | base64 32-byte seed of lake's TAAP signing identity; when absent lake generates one and stores it encrypted | [`../api/audit.md`](../api/audit.md) |
| `LAKE_INSTANCE_KEK` | customer-independent key-encryption key for the generated identity and wrapped keys; required wherever a Postgres store runs | [`../api/audit.md`](../api/audit.md), [`../api/encryption.md`](../api/encryption.md) |
| `LAKE_STORE_ROOT` | root of the per-customer liquid stores (`<customer>/store`), search segments of unswitched workspaces' repositories (`<customer>/index/`; a switched workspace's index is beside its store, lake#381) and the instance log (`<instance>/log/`) | [`../deployment/store-volume.md`](../deployment/store-volume.md), [`../api/search.md`](../api/search.md), [`../api/transparency.md`](../api/transparency.md) |
| `LAKE_STORE_OWNER_WAIT_SECONDS` | how long a starting lake waits for another process to release the store root, and how long each customer-store open waits for its liquid owner lock; default 90; must outlast the predecessor's preStop delay plus `LAKE_DRAIN_SECONDS`. Past it lake exits 1 naming the holder | [`../deployment/store-volume.md`](../deployment/store-volume.md#the-hand-off) |
| `LAKE_SEARCH_MEMORY_BYTES` | bound on the workspace search indexes kept loaded in memory; default 268435456 (256 MiB), at least 1 MiB; idle workspaces are evicted, least recently used first, and loaded again from their workspace Zetta on use (lake#381) | [`../api/search.md`](../api/search.md#where-the-index-lives-lake381-decision-54-s8) |
| `LAKE_DRAIN_SECONDS` | how long SIGTERM waits for in-flight requests before lake closes its stores and releases the store root; default 20 | [`../deployment/store-volume.md`](../deployment/store-volume.md#the-hand-off) |
| `LIQUID_HOME` (else `$HOME/.liquid`) | where liquid keeps the local identity that signs what lake's repositories record (`identity/`); the image's home is `/home/lake`, so a new pod creates a new one. `ApiServer::start` creates or loads it once, under a file lock, before anything opens a repository; a start that finds it unusable fails, naming the directory (lake#534 follow-up) | [`../deployment/store-volume.md`](../deployment/store-volume.md#liquids-local-identity) |
| `LAKE_HTTP_WORKERS` | requests whose routes run at once; default 32. A connection is not a worker: an idle or still-arriving one holds none, nor does a request waiting for the store root, a long poll (`wait_ms` > 0) or a streaming pull (lake#534) | [`http.md`](http.md#connections-and-workers) |
| `LAKE_HTTP_WORKER_WAIT_SECONDS` | how long a request waits for a free worker before it is answered `503` and its connection closed; default 10 | [`http.md`](http.md#connections-and-workers) |
| `LAKE_HTTP_MAX_CONNECTIONS` | open connections, idle ones included; above it a new connection is answered `503` and closed; default 4096, lowered at start to fit the open-file limit (raised to the hard limit) less 512 descriptors | [`http.md`](http.md#connections-and-workers) |
| `LAKE_HTTP_MAX_HEADER_BYTES` | All of a request's headers together, in bytes; larger is `431 header_too_large`. Default 1048576 (1 MiB), from 8 KiB to 16 MiB (lake#1108) | [`http.md`](http.md) |
| `LAKE_HTTP_IDLE_TIMEOUT_SECONDS` | a keep-alive connection waiting this long for its next request is closed; default 75. It must exceed the upstream keep-alive timeout of the proxy in front of lake (nginx: 60 s) | [`http.md`](http.md#connections-and-workers) |
| `LAKE_HTTP_HEADER_TIMEOUT_SECONDS` | a request header must arrive whole within this of its first byte (of the accept for a connection's first request); default 10 | [`http.md`](http.md#connections-and-workers) |
| `LAKE_HTTP_IO_TIMEOUT_SECONDS` | one read of a request body or one write of a response that makes no progress for this long closes the connection; default 60 | [`http.md`](http.md#connections-and-workers) |
| `LAKE_HTTP_MIN_TRANSFER_RATE` | bytes per second a client must move while its request holds a worker, measured as a 10 s bucket of waiting time; default 1024, 0 turns it off | [`http.md`](http.md#connections-and-workers) |
| `LAKE_HTTP_MAX_LONG_REQUESTS` | requests that left their worker (long polls, streaming pulls) at once; past it `429`; default 512 | [`http.md`](http.md#connections-and-workers) |
| `LAKE_HTTP_MAX_LONG_REQUESTS_PER_CALLER` | the same per authenticated caller, or per client address; default 64 | [`http.md`](http.md#connections-and-workers) |
| `LAKE_PUBLIC_HOST` | the Lake namespace value on every operator row; required when the operator Zetta client is enabled | [`../control-plane/platform.md`](../control-plane/platform.md) |
| `LAKE_EXTERNAL_ORIGIN` | browser-facing scheme, host and optional non-default port; used by clone/push instructions, with no default hostname | [`../ui/forge.md`](../ui/forge.md), [`../deployment/private-install.md`](../deployment/private-install.md) |
| `LAKE_INTERNAL_URL` | Lake's HTTP(S) in-cluster Service URL; its authority prefixes build image targets and the full URL is handed to agents | [`../api/builds.md`](../api/builds.md), [`../api/agent-launches.md`](../api/agent-launches.md) |
| `LAKE_MAX_REGISTRY_BLOB_BYTES` | maximum completed OCI blob size accepted by streamed `PATCH` and final `PUT` uploads; defaults to 4294967296 (4 GiB); all non-registry request bodies retain the 64 MiB adapter limit | [`../api/builds.md`](../api/builds.md), [`http.md`](http.md) |
| `LAKE_OPERATOR_ZETTA_URL`, `LAKE_OPERATOR_ZETTA_TOKEN` | operator Zetta service URL and secret bearer; set together, they enable platform sync and writes | [`../control-plane/platform.md`](../control-plane/platform.md) |
| `LAKE_LOGS_URL`, `LAKE_LOGS_TOKEN` | Loki-compatible log endpoint and optional secret bearer; enable `environment_logs` and the log pages | [`../api/platform.md`](../api/platform.md) |
| `LAKE_APPS_WILDCARD` | parent domain required for Ship app hosts; also enables app lifecycle topics on the Lake bus | [`../api/web-apps.md`](../api/web-apps.md) |
| `LAKE_METHODOLOGY_RUNTIME_IMAGE` | the `oramasearch/methodology-runtime` image a workspace methodology's image is built `FROM` (build argument `METHODOLOGY_RUNTIME_IMAGE`) and its stage and collect pods run; supplied by the operator from `spec.methodologies.runtimeImage`; without it Validate and runs answer `store_unavailable` | [`../api/methodologies.md`](../api/methodologies.md) |
| `LAKE_MCP_RUNNER_IMAGE` | the private `oramasearch/lake-mcp-runner` image (pinned tag) the workspace App of a stdio MCP connection runs; without it, `create_mcp_connection` with `stdio` answers `not_supported` | [`../api/mcp-connections.md`](../api/mcp-connections.md#servers-that-speak-only-stdio) |
| `LAKE_TUNNEL_CLIENTS_DIR` | the folder of tunnel client binaries Developer Access hands out (`zetta-tunnel-<platform>`, `SHA256SUMS`, `VERSION`); default `/usr/share/lake/tunnel-clients`, where the image copies them from `oramasearch/zetta-tunnel-clients`. Missing or empty, `tunnel_client_downloads` answers `available: false` and the route `404` | [`../api/tunnel-clients.md`](../api/tunnel-clients.md) |
| `LAKE_REGION`, `LAKE_REGION_LABEL` | this Lake's region (the cloud region name) and its label; set by the operator only where the `LakeSet` member has a region; reported by `server_info` as `region` | [`../api/regions.md`](../api/regions.md) |
| `LAKE_REGIONS` | the deployment's regions as JSON `[{name, label, host, latitude, longitude}]`, this Lake's own included; reported by `server_info` as `regions`; invalid JSON or an invalid entry stops startup naming the variable | [`../api/regions.md`](../api/regions.md#configuration) |
| `LAKE_APEX_HOST` | the host (e.g. `lake.live`) whose every request is the region picker; needs `LAKE_REGIONS` | [`../api/regions.md`](../api/regions.md#the-apex-picker) |
| `LAKE_ORDER_SHEET` | JSON merged (RFC 7386) over the apex order sheet's documented default: offers, prices (whole USD), included credit, providers and regions, trust links, the 5-minute line's switch (`shared.show_promise`, off), the abuse limits, `url` and `enabled`; an invalid value stops startup naming the field | [`../api/orders.md`](../api/orders.md#configuration) |
| `LAKE_RESEND_API_KEY` | Orama's Resend API key (`re_...`), a platform environment variable (on a Lake CR, `spec.env` with `valueFrom.secretKeyRef`), never a workspace secret and never logged: order email to support and customers. Unset, orders are still recorded and their email waits until lake restarts with it | [`../api/orders.md`](../api/orders.md#email) |
| `LAKE_RESEND_FROM` | the sender of order email; default `Lake <orders@lake.live>`; its domain must be verified in Resend | [`../api/orders.md`](../api/orders.md#email) |
| `LAKE_SUPPORT_EMAIL` | where order forms, paid orders and failures are emailed; default `support@orama.com` | [`../api/orders.md`](../api/orders.md#email) |
| `LAKE_BALANCE_FIRST_TOP_UP_USD` | where billing is on, the first top-up of the prepaid balance, whole US dollars (default 20); a new account adds exactly this before anything that costs money | [`../api/billing.md`](../api/billing.md#balance) |
| `LAKE_BALANCE_MINIMUM_TOP_UP_USD` | the smallest later top-up and auto top-up, whole dollars (default 20) | [`../api/billing.md`](../api/billing.md#balance) |
| `LAKE_BALANCE_OVERDRAFT_USD` | how far below $0 a balance may go before paid work is refused with `402 payment_required`, whole dollars (default 50); the operator sets one account's with `set_balance_overdraft` | [`../api/billing.md`](../api/billing.md#the-gate) |
| `LAKE_BALANCE_WARNING_USD` | how close counts as close, whole dollars (default 10): the balance is `low` under it, and the "close to its limit" email goes within it of the overdraft | [`../api/billing.md`](../api/billing.md#emails) |
| `LAKE_BALANCE_REFRESH_SECONDS` | how often the store root's owner refreshes every account's charges from usage, 30 to 86400 (default 300) | [`../api/billing.md`](../api/billing.md#charges) |
| `LAKE_AUTO_TOP_UPS_PER_DAY` | the most auto top-ups one account starts in a UTC day, 1 to 100 (default 5) | [`../api/billing.md`](../api/billing.md#auto-top-up) |
| `LAKE_APPS_RUNTIME_IMAGE` | required base image for `build_app`; supplied by the operator from `spec.apps.runtimeImage` and recorded as `APP_RUNTIME_IMAGE` | [`../api/builds.md`](../api/builds.md) |
| `LAKE_APPS_RESERVED_NAMES` | comma-separated additional Ship app names to refuse; surrounding whitespace is ignored | [`../api/web-apps.md`](../api/web-apps.md) |
| `LAKE_APPS_PER_CUSTOMER` | maximum live Ship apps per account; zero means no configured limit | [`../api/web-apps.md`](../api/web-apps.md) |
| `LAKE_APP_REPLICAS_PER_CUSTOMER` | maximum aggregate desired Ship app replicas per account; zero means no configured limit | [`../api/web-apps.md`](../api/web-apps.md) |
| `LAKE_PLATFORM_SYNC_SECONDS` | platform poll interval, default 15, must be greater than zero | [`../api/platform.md`](../api/platform.md) |
| `LAKE_LOG_MODE` | `air-gapped`, `witnessed` (default) or `anchored`; read only when a store root is set | [`../control-plane/log.md`](../control-plane/log.md) |
| `LAKE_LOG_WITNESSES`, `LAKE_LOG_WITNESS_THRESHOLD`, `LAKE_LOG_AGGREGATOR` | JSON witness endpoint/public-key records, their quorum, and the aggregation endpoint anchored mode requires | [`../control-plane/log.md`](../control-plane/log.md), [`../api/transparency.md`](../api/transparency.md) |
| `LAKE_TSA_URL`, `LAKE_TSA_TRUST_CERT` | RFC 3161 endpoint and optional PEM trust roots for timestamp tokens | [`../api/transparency.md`](../api/transparency.md) |
| `LAKE_TSA_RENEWAL_SECONDS`, `LAKE_TSA_RENEW_BEFORE_SECONDS` | renewal policy for RFC 4998 evidence records; defaults 31536000 and 2592000 | [`../api/transparency.md`](../api/transparency.md) |
| `LAKE_PRICE_CURRENCY` (default `USD`), `LAKE_PRICE_STORAGE_GIB_HOUR_MICROS`, `LAKE_PRICE_TRANSFER_IN_GIB_MICROS`, `LAKE_PRICE_TRANSFER_OUT_GIB_MICROS`, `LAKE_PRICE_COMPUTE_SECOND_MICROS` | integer pricing applied by `metering_usage` and `/metering`; the storage price is reported but applies to nothing since repository storage has no hourly history (lake#348) | [`../api/metering.md`](../api/metering.md) |
| `LAKE_STORAGE_QUOTA_BYTES` | default per-customer storage quota; per-customer overrides are capability-backed | [`../api/metering.md`](../api/metering.md) |
| `LAKE_OPENAI_API_KEY` | Orama's OpenAI API key, a platform environment variable: speech-to-text (`transcribe`, and `create_transcript` from audio) and the model gateway (`/openai/v1/*`, every agent model call); unset, they answer `not_supported`. On a Lake CR it comes from a Secret through `spec.env` `valueFrom.secretKeyRef` | [`../api/transcripts.md`](../api/transcripts.md), [`../api/model-gateway.md`](../api/model-gateway.md), [`../deployment/aks.md`](../deployment/aks.md#speech-to-text-key) |
| `LAKE_MODEL_PROVIDER_URL` | the model gateway's OpenAI-compatible provider base URL; default `https://api.openai.com/v1` | [`../api/model-gateway.md`](../api/model-gateway.md#configuration) |
| `LAKE_TRANSCRIBE_MODEL` | OpenAI transcription model; default `gpt-4o-transcribe` | [`../api/transcripts.md`](../api/transcripts.md) |

Startup refuses to run when `LAKE_OPERATOR_ZETTA_URL` and `LAKE_OPERATOR_ZETTA_TOKEN` are not set
together, when the operator client is enabled without `LAKE_PUBLIC_HOST`, when
`LAKE_PLATFORM_SYNC_SECONDS` is zero, when `LAKE_REPLICATION_PEERS` holds an entry that is not an
origin, when `LAKE_REPLICATION_TIMEOUT_SECONDS` is zero, or when the transparency-log configuration is invalid; each
case exits with status 2 and one line on stderr. A store root still held by another process after
`LAKE_STORE_OWNER_WAIT_SECONDS` exits with status 1 and `lake: cannot start: ...` naming the holder. The startup JSON event records `admin_token_set`
and `replication_token_set` as `yes` or `no`, never a token value, and `replication_peers` as the
normalized peer origins ([`telemetry.md`](telemetry.md)).

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

### Prepaid balance

The six `LAKE_BALANCE_*` and `LAKE_AUTO_TOP_UPS_PER_DAY` variables are read only where
`LAKE_BILLING` is on, at start; an invalid value (not a whole number, out of range) stops lake
with a message naming the variable. The balance's emails go through Resend with
`LAKE_RESEND_API_KEY` from `LAKE_RESEND_FROM`; without the key they wait, and the rest of the
balance works. On a Lake CR they are `spec.env` entries the operator sets with each version that
needs them ([`../api/billing.md`](../api/billing.md#balance), lake#1114).

## Build and test variables

| Variable | Meaning | Owner |
|---|---|---|
| `LAKE_ZETTA_SOURCE_DIR` | CMake cache path to the `zetta-bridge/modules/zetta` checkout; required | [`../build/local.md`](../build/local.md) |
| `LAKE_BUILD_TESTS` | CMake option, on by default | [`../build/local.md`](../build/local.md) |
| `LAKE_USE_SYSTEM_COMPILER` | CMake option that skips the Homebrew or `clang++-NN` auto-selection | [`../build/local.md`](../build/local.md) |
| `LAKE_TEST_DB_URI` | Postgres URI for the Postgres-backed tests; absent means they skip | [`../build/testing.md`](../build/testing.md) |

## Change

A new flag or variable is added to `usage()` and the table above in the same change, with its
default, and the page that owns the behavior links back here. A new variable is also declared in
`environment_variables()` (`src/api/manifest.cpp`), with whether a Lake cannot run without it and
whether it is a secret, so `lake manifest` tells the operator to render it;
`LakeManifest.DeclaresExactlyTheEnvironmentLakeReads` fails until it is.

`LAKE_ORDER_ENTITLEMENT_GRACE_SECONDS` (lake#972) sets the grace on signed order subscription events and initial paid Checkout: default `259200` (three days), integer `0` through `2592000` (30 days). Invalid values stop startup. The policy is captured while entitled or on positive entitlement evidence; a negative event cannot extend an expired order. See [orders](../api/orders.md#recurring-entitlement-lsr-105-lake972).
