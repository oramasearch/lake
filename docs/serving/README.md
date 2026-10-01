# Serving contracts

[http.md](http.md) defines the Beast2 server, route inventory, response rules, and per-route body
limits. OCI blob uploads stream to storage under the configurable registry limit; all other body
readers retain the 64 MiB ceiling or a smaller route-specific bound.

[configuration.md](configuration.md) lists every command-line flag and environment variable with
its default and owning contract. [telemetry.md](telemetry.md) covers probes, metrics, and request
logs.

Change runtime behavior, its configuration entry, the owning API page, and the focused and full
native proof together.
