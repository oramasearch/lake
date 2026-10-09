# Serving contracts

[http.md](http.md) defines the Beast2 server, its connections apart from its request workers
with the connection cap, idle and header timeouts (lake#534), route inventory, response rules, per-route body
limits, and the `500` answer and `handler_exception` log line for a handler that throws. OCI blob uploads stream to storage under the configurable registry limit; all other body
readers retain the 64 MiB ceiling or a smaller route-specific bound.

[configuration.md](configuration.md) lists every command-line flag and environment variable with
its default and owning contract. [telemetry.md](telemetry.md) covers probes, metrics, and request
logs, including the dynamic-lease expiry pass's counters (lake#362) and the workspace deletion
resume pass's (lake#514).

Change runtime behavior, its configuration entry, the owning API page, and the focused and full
native proof together.

`POST /repository/create` and the hub's `POST /api/repos/create` take the repository's workspace
(lake#315); see [http.md](http.md) and [../api/models.md](../api/models.md).
