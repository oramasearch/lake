# Lake source

`main.cpp` parses runtime configuration and owns background service loops. Agent-task lease
durations come from `LAKE_AGENT_LEASE_SECONDS`; the bus loop receives the workspace service to
publish retry notices before dispatch. See [the agent bus](../docs/api/agent-bus.md).

`api/`, `control/` and `telemetry/` document their registry, storage and instrumentation contracts
in their own READMEs. Verify changes with the native PostgreSQL-backed static and shared suites.
