# Lake source

`main.cpp` parses runtime configuration and owns background service loops. Agent-task lease
durations come from `LAKE_AGENT_LEASE_SECONDS`; the bus loop receives the workspace service to
publish retry notices before dispatch. See [the agent bus](../docs/api/agent-bus.md).
Ship App source builds receive their base image from `LAKE_APPS_RUNTIME_IMAGE`; `main.cpp` passes
that trusted setting through `ServerOptions` to the API request context. Workspace methodologies get
theirs from `LAKE_METHODOLOGY_RUNTIME_IMAGE` the same way, and the owner loop withdraws a methodology
run the operator has not reported on within 30 minutes (lake#655).
OCI upload capacity comes from `LAKE_MAX_REGISTRY_BLOB_BYTES`; `main.cpp` passes the byte count to
the server while the HTTP adapter keeps every non-registry body at 64 MiB.
On SIGTERM a watcher thread calls `ApiServer::begin_drain` at once, so workspace stores are handed
to the successor without waiting for the owner loop's tick; the bus loop's pauses end on the stop
request and the loop is joined after `ApiServer::stop`, so it never holds up the store root's
release (lake#536). `LAKE_WORKSPACE_STORE_PREOPEN=false` turns the start-up pre-open off.

`api/`, `control/` and `telemetry/` document their registry, storage and instrumentation contracts
in their own READMEs. Verify changes with the native PostgreSQL-backed static and shared suites.

Threads lake starts (the server's `io_context` thread, the store-root owner thread that opens
liquid stores, the outbound client's thread and the bus loop) are `lake::Thread`s from `include/lake/thread.hpp`, never `std::thread`: they take an
explicit 8 MiB stack instead of the platform default (lake#478,
[thread stacks](../docs/build/local.md#debug-builds-and-thread-stacks)).
