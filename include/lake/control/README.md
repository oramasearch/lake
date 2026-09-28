# Public control types

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
back it in both stores.

`agent_library.hpp` registers ApplicationAgent as `agent-application`, a chat-capable service used
from Ship → Apps and Ask Orama. Its bus identity is independent from every other library agent.
