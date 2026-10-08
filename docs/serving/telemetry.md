# Operational telemetry

## What

The HTTP server exposes three operational routes:

| Route | Authentication | Response |
|---|---|---|
| `GET /healthz` | none | `200 {"ok":true,"platform":"ready|degraded"}` while the HTTP process is serving; `platform` is omitted when operator sync is not configured |
| `GET /readyz` | none | `200 {"ok":true,"store":"<kind>","store_root":"open|waiting|opening"}` after a successful store ping when the store root is open or is being handed over from a predecessor; otherwise `503` with the JSON error envelope (control store down, store root opening on a cold start or failed, or stopping after SIGTERM). `store_root` is omitted without a store root. See [`../deployment/store-volume.md`](../deployment/store-volume.md#the-hand-off) |
| `GET /metrics` | the existing bearer or `lake_session` cookie | Prometheus text exposition; unauthorized requests receive the normal `403` JSON error |

`lake-core` contains a process-local metrics registry with counters, gauges and histograms. It
renders Prometheus text exposition version 0.0.4 without a client library. Histograms use fixed
latency buckets at 0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5 and 5 seconds.

The exported metric families are:

- `lake_build_info{version,node} 1`
- `lake_up 1`
- `lake_http_requests_total{method,route,status}`
- `lake_http_request_duration_seconds{method,route,status}`
- the gauges `lake_http_connections` (open connections), `lake_http_requests_active` (requests
  holding a worker) and `lake_http_requests_waiting` (requests waiting for one), and the counters
  `lake_http_refusals_total{reason}` (`connection_limit`, `workers_busy`) and
  `lake_http_connections_closed_total{reason}` (`idle_timeout`, `header_timeout`): the connection
  limits ([http](http.md#connections-and-workers), lake#534). A refusal here has no request log
  line; it is answered before any route
- `lake_store_calls_total{store,operation,result}`
- `lake_store_call_duration_seconds{store,operation,result}`
- `lake_metering_records_written_total{collection}`, `lake_metering_flush_errors_total`,
  `lake_metering_dropped_bytes_total{reason}` (`dropped`: the buffer bound or a deleted
  workspace; `no_workspace`: a repository without a workspace),
  `lake_metering_dropped_builds_total` and the gauge `lake_metering_pending`: repository usage on
  its way to the workspace Zettas, written by the owner loop
  ([metering](../api/metering.md#transfer-off-the-request-path), lake#348)
- `lake_audit_workspace_events_written_total`, `lake_audit_workspace_flush_errors_total`,
  `lake_audit_workspace_conflicts_total` (a place another replica took first),
  `lake_audit_workspace_dropped_total{reason}` (`bound`, `workspace_deleted`) and the gauge
  `lake_audit_workspace_pending`: audit events about a workspace's data on their way to that
  workspace's chain, written by the owner loop every second
  ([audit](../api/audit.md#workspace-audit), lake#317)
- `lake_secret_leases_settled_total{outcome}` (`expired`, `settled`, `rescheduled`, `removed`,
  `failed`) and `lake_secret_lease_sweep_errors_total`: the owner loop's dynamic-lease expiry pass,
  which reads the due index and settles each due lease in its workspace
  ([secrets](../api/secrets.md#where-leases-live-lake362), lake#362)
- `lake_request_series_requests_written_total`, `lake_request_series_flush_errors_total`,
  `lake_request_series_dropped_total{reason}` (`bound`, `workspace_deleted`, `refused`) and the
  gauge `lake_request_series_pending`: requests on their way to their workspaces' `lake_requests`
  series, written by the owner loop every ten seconds ([below](#the-workspace-request-series),
  lake#384)
- `lake_workspace_store_requests_total{kind,outcome}` and
  `lake_workspace_store_request_duration_seconds{kind}`: each request lake sends to a workspace
  Zetta's object routes for a switched workspace's repository store, one count per attempt
  (lake#527, [workspace store](../architecture/workspace-store.md#requests-and-latency-lake527)).
  `kind` is `put`, `put_if_absent`, `patch`, `barrier`, `get`, `ranged_get`, `list`, `delete`,
  `lease_acquire`, `lease_renew`, `lease_release`, `usage` or `create_bucket`; `outcome` is `ok`,
  `client_error`, `server_error` or `no_answer`. There is no workspace, bucket or key label. A
  request's own share is in its `Server-Timing` (`store`, `store_<kind>`,
  [performance](../ui/performance.md#reading-server-timing))
- `lake_workspace_deletions_completed_total` and `lake_workspace_deletion_errors_total`: the owner
  loop's pass, every minute, that finishes workspace deletions left pending (tenant image tags,
  search segments, central audit events); an error also logs `lake: workspace deletion pending`
  with the reason ([workspaces](../api/workspaces.md#deletion), lake#514)

`route` is the registered route pattern, or `unmatched` for the trailing 404, never a caller's raw
path. Store `result` is `ok` on success and the store error code on failure.

The control-store interface exposes `ping()`. The in-memory store always succeeds. The Postgres
store runs `SELECT 1` with a one-second transaction-local statement timeout and maps any failure
to `store_unavailable`.

When operator Zetta sync fails, `platform` changes to `degraded` while `/readyz` remains ready as
long as the local control store responds. Environment reads continue from the mirror with a
`stale_since` marker, and platform mutations fail with `store_unavailable`.

The store-root owner writes `{"event":"store_owner",...}` lines to stderr: `waiting` (with the
holder), `open` (with `handoff`, `waited_ms` and `opened_ms`), `failed`, `abandoned`, and
`released` or `not_owned` at shutdown (with `drain_ms` and `unfinished_requests`).

### Request logs

Every completed request writes one JSON object to stdout:

| Field | Value |
|---|---|
| `ts` | RFC 3339 UTC |
| `method` | the HTTP method |
| `route` | the registered route pattern (`/v1/repositories/:repository_id/sync/pull`, `/:package`, `/v2/:space/:repository/manifests/:reference`), or `unmatched` |
| `status`, `duration_ms`, `node` | as before |
| `request_id` | the request's id: the caller's `X-Request-Id` when it is a short token (letters, digits, `-_.:`, at most 128), else the one lake chose; left out otherwise |
| `capability` | the capability a REST `/v1/…` or MCP request called, when it called one |
| `<name>_id` | each identifier the route captured (`repository_id`, `customer_id`, `workspace_id`, `order_id`, …) when it is a UUID |
| `workspace_id` | the workspace the request was routed to ([below](#the-workspace-request-series)), when it was |

The raw path is not logged (decision 54 S9, lake#384): a path can carry a repository, package,
image or file name. A captured value that is not a UUID (`:package`, an OCI `:repository`,
`:digest`, `:filename`, an ACME `:token`) is left out, and the query string never was logged. A
request log line therefore carries no customer content. The `handler_exception` line
([http.md](http.md#handler-exceptions)) dropped its `path` the same way.

Startup is also one JSON object and records `admin_token_set` and `replication_token_set` as `yes`
or `no`, never either token. Authorization and cookie values are never logged.

### The workspace request series

Lake's process metrics and request logs are instance operations data, a named exception to the
rule that everything goes in a workspace Zetta (decision 54 S9). Each workspace still gets its own
view of its traffic: a counter `lake_requests` in its Zetta's labelled time series (zetta
`docs/data/time-series.md`, zetta#1231), created by lake with `create_metric` (counter,
`resolution_ms` 60000, `retention_ms` 30 days).

| Label | Value |
|---|---|
| `route` | the route pattern, as in the request log |
| `capability` | the capability called, only for REST `/v1/…` and MCP requests |
| `status_class` | `1xx` … `5xx` |

A request is counted in the workspace it was routed to: the first workspace one of its capability
calls acted on after Registry::call let its handler run. That is the validated `workspace_id`
(including the `/w/<workspace>/` path, which becomes the call's `workspace_id`), else the workspace
of the repository, secret mount, application, app, build or agent task its input names, the same
lookup that routes a workspace audit event (`acted_workspace`, `src/api/audit_routing.cpp`). A
request refused before its handler ran or for want of authority, a request that called no
capability (health, metrics, the instance registry's npm, Python, debuginfod and OCI routes until
lake#383 gives tenant images a workspace route), and one whose capabilities resolved no workspace
are counted in no workspace. There is no default workspace and no fallback to the lake Zetta or
PostgreSQL.

The request path only adds one to an in-memory count for (workspace, minute, labels)
(`RequestRecorder`, `include/lake/api/request_recorder.hpp`). The owner loop flushes it every ten
seconds (`ServerOptions::request_series_flush_interval`), at once when half of
`request_series_max_pending` (16,384 counts) is held, and once more on stop: it reads each
workspace's routing row, defines the metric once per workspace and process, and writes the counts
with `record_metric` (at most 1000 points per call) through the workspace's own connection, the
path its records take. A count beyond the bound is dropped (`reason="bound"`); a failed write is
kept for the next flush; a deleted or unknown workspace's counts are dropped
(`reason="workspace_deleted"`); a workspace that refuses for good (a different `lake_requests`
definition) has them dropped (`reason="refused"`).

The series is approximate: a crash loses the last interval, and a write whose answer was lost is
retried and may count twice. Exact usage is metering's, in records
([metering](../api/metering.md)); metering never reads this series. No migration: the series starts
empty.

Read it with the workspace's `query_metric`, for example
`{"metric":"lake_requests","from_ms":…,"to_ms":…,"group_by":["route","status_class"]}`.

## Why

Kubernetes can distinguish a serving process from a replica ready to receive traffic, and a
Prometheus scraper can observe request and control-store outcomes without access to secrets in
logs. The registry stays in `lake-core`, free of the HTTP and Postgres client types.

## How to verify

Build and run `ctest --test-dir build --output-on-failure`. The `Telemetry` tests check exposition,
a measured in-memory store ping, both probe routes, and refused and authorized metrics requests.
`RequestLog.*` checks the log line's fields; `RequestRecorder.*` the aggregation, the bound, two
workspaces of one space and another space counted apart, a failed write kept, and deleted, unknown
and refusing workspaces dropped; `HttpServer.RequestLogsNameRoutesAndEachWorkspaceGetsItsOwnRequestSeries`
captures stdout for repository, registry, drive and sync requests (no raw path, no names) and
reads three workspaces' series after the owner loop's flush.
`RequestRecorderZetta.QueryMetricReturnsTheFlushedCounts` runs against a real Zetta node (zetta
dev with #1231) when `LAKE_TEST_ZETTA_URL` and `LAKE_TEST_ZETTA_TOKEN` are set and reads the counts
back with `query_metric`.

For a running memory store:

```sh
./build/lake --listen 127.0.0.1:4900 --memory-store --admin-token testtoken
curl -i localhost:4900/healthz
curl -i localhost:4900/readyz
curl -i localhost:4900/metrics
curl -i -H 'Authorization: Bearer testtoken' localhost:4900/metrics
```

## How to change

Add measurements through `lake::telemetry::Metrics`; do not assemble Prometheus text at call
sites. Keep metric labels bounded and never put raw request paths, customer identifiers or secret
values in them. Never add the raw path, or a captured value that is not a UUID, to the request log
line (`src/api/request_log.hpp`). A new per-workspace series label must be bounded as well. Change the bucket array in `src/telemetry/metrics.cpp` only when both request and
store latency histograms should change together.

Register new HTTP routes before the trailing 404 through `add_route` (which applies
`guard_route`, so an exception is answered 500 and logged as `handler_exception` — see
[http.md](http.md#handler-exceptions)) and wrap their handler with `observe_request`, passing the
literal route pattern. Continue to use `context_for` for any operational route that
requires authentication. A new store implementation must implement `ping()` and return
`store_unavailable` when it cannot serve calls.
