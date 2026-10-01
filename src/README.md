# Lake source

`main.cpp` parses runtime configuration and owns background service loops. Agent-task lease
durations come from `LAKE_AGENT_LEASE_SECONDS`; the bus loop receives the workspace service to
publish retry notices before dispatch. See [the agent bus](../docs/api/agent-bus.md).
Ship App source builds receive their base image from `LAKE_APPS_RUNTIME_IMAGE`; `main.cpp` passes
that trusted setting through `ServerOptions` to the API request context.

`api/`, `control/` and `telemetry/` document their registry, storage and instrumentation contracts
in their own READMEs. Verify changes with the native PostgreSQL-backed static and shared suites.
