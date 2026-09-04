# Operational telemetry

## What

`lake-core` contains a process-local metrics registry with counters, gauges and histograms. It
renders Prometheus text exposition version 0.0.4 without a client library. Histograms use fixed
latency buckets at 0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5 and 5 seconds.

The store decorator records every control-store operation in
`lake_store_calls_total{store,operation,result}` and
`lake_store_call_duration_seconds{store,operation,result}`. `result` is `ok` on success and the
store error code on failure.

The control-store interface also exposes `ping()`. The in-memory store always succeeds. The
Postgres store runs `SELECT 1` with a one-second transaction-local statement timeout and maps any
failure to `store_unavailable`; this is the readiness check used by the HTTP server.

## Why

The registry keeps operational measurements in `lake-core`, free of the HTTP and Postgres client
types, and gives the server one source for Prometheus output.

## How to verify

Build and run `ctest --test-dir build --output-on-failure`. The `Telemetry` tests check a counter,
the cumulative fixed buckets, count and infinite bucket of a histogram, and a measured in-memory
store ping.

## How to change

Add measurements through `lake::telemetry::Metrics`; do not assemble Prometheus text at call
sites. Keep metric labels bounded and never put raw request paths, customer identifiers or secret
values in them. Change the bucket array in `src/telemetry/metrics.cpp` only when both request and
store latency histograms should change together.
