# The agent bus: agent traffic over Zetta pub/sub

**Issue:** [lake#269](https://github.com/oramasearch/lake/issues/269) · **Code:**
`include/lake/control/agent_bus.hpp`, `src/control/agent_bus.cpp`, the bus capabilities in
`src/api/workspace_capabilities.cpp` · **Tests:** `AgentRuns.LibraryAgentsTakeTasksAndAnswerOverTheBus`,
the Ask Orama, Agent Factory, Discover and pipeline tests in `tests/test_http_server.cpp`

**No lake page or capability waits on a library agent inside an HTTP request.** Lake and the
library agents (Ask Orama, the Project Agent, Auto-Suggest, AgentCreator) talk over
[Zetta's pub/sub](https://github.com/oramasearch/zetta/tree/dev/docs/pubsub) on **lake's own Zetta**:
the Lake's `spec.zetta` cluster, reached as `LAKE_DATA_PLANE_URL` with `LAKE_DATA_PLANE_TOKEN`.

| Topic | Published by | Payload (JSON; the correlation id is the task id) |
|---|---|---|
| `topic:agent-tasks.<agent>` | lake, when a turn is asked | `{task_id, customer_id, agent, kind}` |
| `topic:agent-events` | the agent, through lake | `{task_id, agent, type, message, result}`; `type` is `progress`, `result` or `failed` |

Lake creates both (exposed) when it starts: one `agent-tasks.<agent>` topic per agent in
`LAKE_AGENT_ENDPOINTS`.

## A turn, end to end

1. `agent_turn` (or a page: Ask Orama, the Agent Factory, Design → Opportunities, the pipeline's
   ask box) writes the task to `agent_tasks` (migration `0032_agent_tasks`), publishes it on the
   agent's topic and answers `{task_id, status: pending}` at once.
2. The agent's service long-polls `receive_agent_tasks`, then calls `claim_agent_task`. **Exactly one
   claim succeeds.** Lake generates the task's credential, `lat.<task id>.<secret>`, stores only its
   SHA-256 in `agent_task_status`, and answers the task's body with every `"{{credential}}"` in it
   replaced (as `lake_token` and `zetta_token`). The credential acts as the person who asked,
   with the task's scopes, until the task finishes.
3. The agent does the work and reports with `publish_agent_event`: `progress` messages while it
   works, then its `result` (its whole answer, `{ok, result, …}`) or `failed`.
4. Lake's bus loop (a thread in every lake replica) long-polls `agent-events` and applies each event
   to `agent_task_status`. Applying one twice changes nothing, and a finished task stays finished.
   `get_agent_task` and the pages read the task from there.

Anyone who may read a task can follow it live with `receive_agent_events`. Ask Orama uses it to
wait on a subagent it delegated to.

**Nothing secret is on the bus.** Topics carry task ids only. The body travels in the answer to a
claim, and the credential is handed over once and never stored.

**Agents never hold the bus's token.** Every bus call goes through lake, which checks the caller:

| Capability | Kind | Who | What |
|---|---|---|---|
| `receive_agent_tasks {agent, from_sequence?, wait_ms?}` | read | a library agent's service | the tasks on `agent-tasks.<agent>` from `from_sequence`, without those already claimed; `next_sequence` continues |
| `claim_agent_task {task_id, agent}` | action | a library agent's service | take the task; the body with its credential. A second claim, or a claim by another agent, is a `conflict` |
| `publish_agent_event {task_id, agent, type, message?, result?}` | action | a library agent's service | publish on `agent-events`, only for a task that agent claimed and that has not finished |
| `receive_agent_events {customer_id, task_id, from_sequence?, wait_ms?}` | read | whoever asked the task (or the operator) | the task's events from `from_sequence`, and the task as it stands |

A library agent's service authenticates with `LAKE_AGENT_SERVICE_TOKEN` (the operator mounts the same
secret into every library agent as `AGENT_SERVICE_TOKEN`, and `LAKE_URL`). That credential reaches
only the first three. Long polls wait at most 10 seconds (`wait_ms`).

## What the bus loses, the store recovers

Stage-1 pub/sub is in memory and survives a restart only on a best-effort basis
([zetta docs/pubsub/restart.md](https://github.com/oramasearch/zetta/blob/dev/docs/pubsub/restart.md)).
So the truth is the store, and every 15 seconds lake's bus loop:

- publishes again a task nobody claimed in the last 60 seconds;
- fails a task nobody claimed within its deadline (*No agent picked this up within 300 seconds.*),
  and a claimed one that did not finish within it (*The agent did not finish within …*).

## Pub/sub on lake's API

A customer's workspace Zetta has pub/sub too, and it is reachable through lake like the rest of
the workspace: `list_topics` and `receive_events` through `workspace_query` (or
`GET /workspaces/<customer>/zetta/v1/…`), and `create_topic`, `drop_topic` and `publish_event`
through `workspace_act`. As on Zetta, only topics created with `exposed: true` are reachable.
Through lake a `receive_events` long poll waits at most 10 seconds.

## Configuration

| Variable | Meaning |
|---|---|
| `LAKE_DATA_PLANE_URL`, `LAKE_DATA_PLANE_TOKEN` | lake's own Zetta, the bus. It needs an image with pub/sub (zetta `dev` from f8ad170) |
| `LAKE_AGENT_ENDPOINTS` | the library agents; one `agent-tasks.<agent>` topic each |
| `LAKE_AGENT_SERVICE_TOKEN` | what a library agent's service presents to lake |

Without the bus, a library agent's turn answers `store_unavailable`. A lake run with
`--memory-store` uses an in-memory bus. The startup line reports `agent_bus`: `zetta`, `memory` or
`no`.

## Not yet

- One service token is shared by the library agents. Per-agent credentials would let lake tell
  them apart beyond the `agent` they name.
- Custom Python agents on `agent-runtime` still answer within the call.
- Streaming subscriptions (SSE) replace the long poll when Zetta ships them.
