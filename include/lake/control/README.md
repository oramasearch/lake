# Public control types

`StreamOwner` and its Store methods bind topic management to the creator, customer and actual
workspace service ID. `list_agent_task_stream_scopes` supplies one customer's compact durable task
and agent scope for the shared Agent events and Agent tasks views.
`latest_agent_conversation_task` resolves the preceding task for a same-agent, same-kind follow-up.
`AgentBus::call` carries named
CRUD read requests over the existing service transport; unsupported custom bus implementations
fail explicitly. See `docs/control-plane/streams.md`.

`Session.parent_session_id` records derivation. `Principal.expires_at_unix` carries resolved expiry
in whole Unix seconds to the capability registry. `NewToken.parent_session_id` is supplied by
trusted capability code, and both store implementations persist and enforce its ancestry.

Verify changes with the full PostgreSQL-backed static and shared CTest suites. Keep the identity
contract in `docs/api/identity.md` updated with changes to token derivation.

`Store::list_customers(limit, user_id)` optionally selects the personal owner before limiting.
Audit filters select exact request IDs and continue after event IDs in append order. Both stores
and the telemetry wrapper preserve these filters; see `docs/build/testing.md`.

`agent_bus.hpp` is the agent bus (`docs/api/agent-bus.md`). `AgentBus` is the pub/sub interface,
with a Zetta implementation and an in-memory one for tests. `AgentTaskBus` is lake's side of it.
`Store::claim_agent_task`, `record_agent_task_event`, `fail_agent_task` and `stale_agent_tasks`
back it in both stores. Dispatch includes the task's immutable workspace for information; agent
capability inputs may omit it. `WorkspaceService::call` always receives an explicit resolved
workspace ID.

`web_apps.hpp` defines the protected-app session claims and authorization-code records. `Store`
exposes host resolution, the one-query cookie authorization check, atomic code exchange, and
expiry cleanup; memory and PostgreSQL implementations preserve the same contract. See
`docs/api/web-apps.md`.

`agent_library.hpp` registers ApplicationAgent as `agent-application` and ExperimentsAgent as
`agent-experiments`, chat-capable services used from their Ship/Train pages and Ask Orama. Each bus
identity is independent from every other library agent.

Agent claims carry a renewable lease and up to three attempts. The Store interface exposes
credential-checked renewal and acknowledgement of durable retry notices; the bus pump receives
the workspace service for publication. `parse_agent_leases` validates per-library durations.
See [the lease contract](../../../docs/api/agent-bus.md).
