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

Every completed request writes one JSON object to stdout with RFC 3339 UTC `ts`, `method`,
query-free `path`, `route`, `status`, `duration_ms` and `node`. Startup is also one JSON object and
records `admin_token_set` and `replication_token_set` as `yes` or `no`, never either token.
Authorization and cookie values are never logged.

## Why

Kubernetes can distinguish a serving process from a replica ready to receive traffic, and a
Prometheus scraper can observe request and control-store outcomes without access to secrets in
logs. The registry stays in `lake-core`, free of the HTTP and Postgres client types.

## How to verify

Build and run `ctest --test-dir build --output-on-failure`. The `Telemetry` tests check exposition,
a measured in-memory store ping, both probe routes, and refused and authorized metrics requests.

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
values in them. Change the bucket array in `src/telemetry/metrics.cpp` only when both request and
store latency histograms should change together.

Register new HTTP routes before the trailing 404 through `add_route` (which applies
`guard_route`, so an exception is answered 500 and logged as `handler_exception` — see
[http.md](http.md#handler-exceptions)) and wrap their handler with `observe_request`, passing the
literal route pattern. Continue to use `context_for` for any operational route that
requires authentication. A new store implementation must implement `ping()` and return
`store_unavailable` when it cannot serve calls.
