# The agent bus: agent traffic over Zetta pub/sub

**Issue:** [lake#269](https://github.com/oramasearch/lake/issues/269) · **Code:**
`include/lake/control/agent_bus.hpp`, `src/control/agent_bus.cpp`, the bus capabilities in
`src/api/workspace_capabilities.cpp` · **Tests:** `AgentRuns.LibraryAgentsTakeTasksAndAnswerOverTheBus`,
the Ask Orama, Agent Factory, Discover and pipeline tests in `tests/test_http_server.cpp`

**No lake page or capability waits on a library agent inside an HTTP request.** Lake and the
library agents (Ask Orama, the Project Agent, ApplicationAgent, ExperimentsAgent, Auto-Suggest,
AgentCreator) talk over
[Zetta's pub/sub](https://github.com/oramasearch/zetta/tree/dev/docs/pubsub). Dispatch uses Lake's
`spec.zetta` cluster (`LAKE_DATA_PLANE_URL` and `LAKE_DATA_PLANE_TOKEN`); events use the task's
explicit workspace Zetta. See the [workspace data-plane design](../architecture/workspace-data-plane.md).

| Topic | Published by | Payload (JSON; the correlation id is the task id) |
|---|---|---|
| Lake bus `topic:agent-tasks.<agent>` | lake, when a turn is asked | `{task_id, customer_id, agent, kind}` |
| Workspace `topic:agent-events` | the agent, through lake | `{task_id, agent, type, message, result}`; `type` is `progress`, `result` or `failed` |

Lake creates exposed dispatch topics at startup with one-hour/10,000-event retention. Workspace
event topics are created lazily with 24-hour/10,000-event retention. Existing managed topics have
their mutable retention settings reconciled. The bus cleanup logs obsolete exposed topics before
dropping them; only configured `agent-tasks.*` remain. This is a cutover, not dual publication.

There is no topic per turn. Streams lists the selected workspace's real topics and the space's
per-agent task rows. Workspace event browsing does not filter other spaces' task IDs: other
workspaces' events are stored on different Zettas.

## A turn, end to end

1. `agent_turn` (or a page: Ask Orama, the Agent Factory, Design → Opportunities, the pipeline's
   ask box, Ship → Apps or Train → Experiments) writes the task to `agent_tasks` (migration
   `0032_agent_tasks`, immutable `workspace_id` added by `0038_workspace_data_plane`), publishes it on the agent's topic and answers
   `{task_id, status: pending}` at once.
2. The agent's service long-polls `receive_agent_tasks`, then calls `claim_agent_task`. **Exactly one
   claim succeeds.** Lake generates the task's credential, `lat.<task id>.<secret>`, stores only its
   SHA-256 in `agent_task_status`, and answers the task's body with every `"{{credential}}"` in it
   replaced (as `lake_token` and `zetta_token`). The credential acts as the person who asked,
   with the task's scopes, until the task finishes.
3. The agent does the work and reports with `publish_agent_event`: `progress` messages while it
   works, then its `result` (its whole answer, `{ok, result, …}`) or `failed`.
4. Lake publishes directly to that workspace and applies accepted progress to `agent_task_status`.
   A finished task stays finished. Moving remaining task bodies/results out of the control store
   is tracked in the architecture audit; no new PostgreSQL event storage is introduced here.
   `get_agent_task` and the pages read the task from there.

The dispatch recovery loop runs separately from HTTP requests. It republishes unclaimed tasks
and expires overdue tasks; it never polls a Lake-bus event topic. Task credentials cannot select
another workspace, even when the same customer owns both workspaces.

Anyone who may read a task can follow it live with `receive_agent_events`. Ask Orama uses it to
wait on a subagent it delegated to. Following a task reads retained events in its workspace and
selects its correlation ID. Cursors belong to that workspace topic. Task creates require
`workspace_id` from `workspaces.id` (resolved through `zetta_service_id`); missing, unknown or
foreign IDs return `invalid_input`. Agent instructions list workspaces rather than inferring
identity from services. Claim replies include the
persisted ID in the envelope and body. Event reads/publication must supply the same ID.

**Nothing secret is on the bus.** Topics carry task ids only. The body travels in the answer to a
claim, and the credential is handed over once and never stored.

**Agents never hold the bus's token.** Every bus call goes through lake, which checks the caller:

| Capability | Kind | Who | What |
|---|---|---|---|
| `receive_agent_tasks {agent, from_sequence?, wait_ms?}` | read | that library agent | the tasks on `agent-tasks.<agent>` from `from_sequence`, without those already claimed; `next_sequence` continues |
| `claim_agent_task {task_id, agent}` | action | that library agent | take the task; the body with its credential. A second claim, or a claim by another agent, is a `conflict` |
| `publish_agent_event {workspace_id, task_id, agent, type, message?, result?}` | action | that library agent | publish on the task workspace's `agent-events`, only for a claimed, unfinished task |
| `receive_agent_events {customer_id, workspace_id, task_id, from_sequence?, wait_ms?}` | read | whoever asked the task (or the operator) | retained task events and current task state |

**Each library agent has its own bus credential.** The operator keeps one Secret,
`lake-<name>-agent-bus`, with a random token per library agent. It mounts the whole Secret into
lake (`LAKE_AGENT_BUS_DIR`, one file per agent) and only that agent's token into each agent
(`AGENT_BUS_TOKEN`, with `LAKE_URL`). A bus credential reaches only the first three capabilities,
and only with its own agent's name.

ApplicationAgent receives only tasks from `agent-tasks.agent-application` and only its
`agent-application` bus credential can claim or answer them. The task itself has kind `app`; its
short-lived task credential carries read tools initially and gains repository/App writes only on a
later task with the person's reviewed proposal.

ExperimentsAgent has the independent topic and credential `agent-tasks.agent-experiments` /
`agent-experiments`. Its task kinds are `experiment`, `experiment_status` and
`experiment_explain`. The bus still carries only the task id. Study definitions, trials and
analysis remain in the selected workspace Zetta, reached with the claimed task credential. An
initial proposal has no study write; a new task carrying the reviewed proposal receives only
workspace `create_study` and its backing `start_study` capability in addition to its reads.

The service token (`LAKE_AGENT_SERVICE_TOKEN`) is never a bus credential: user-created agents on
agent-runtime hold it too, and customer code must not be able to take another person's task. Long
polls accept up to 60 seconds (`wait_ms`), but the authorized upstream currently caps each receive
at 30 seconds; Lake clamps accordingly and allows a 40-second transport timeout. Runtime idle
backoff is required until [Zetta's longer poll](https://github.com/oramasearch/zetta/issues/1174)
lands. The at-most-one-receive-per-minute deployment acceptance is not claimed by this PR.

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
`GET /w/<workspace_id>/workspaces/<customer>/zetta/v1/…`). Only exposed topics are reachable;
topic/message writes use the dedicated [Streams](streams.md) capabilities, not `workspace_act`.
Streams browses without consuming. App/build events are workspace topics, not bus topics.

## Configuration

| Variable | Meaning |
|---|---|
| `LAKE_DATA_PLANE_URL`, `LAKE_DATA_PLANE_TOKEN` | lake's own Zetta, the bus. It needs an image with pub/sub (zetta `dev` from f8ad170) |
| `LAKE_AGENT_ENDPOINTS` | the library agents; one `agent-tasks.<agent>` topic each |
| `LAKE_AGENT_BUS_DIR` | the library agents' bus credentials, a file per agent named by its key (mounted from `lake-<name>-agent-bus`) |

Without the bus, a library agent's turn answers `store_unavailable`. A lake run with
`--memory-store` uses an in-memory bus. The startup line reports `agent_bus`: `zetta`, `memory` or
`no`.

## Verify on a deployment

`tools/e2e/agents.py <base-url> <token-file> <customer-id> [T1 …]` is the agents' live scenario
suite: each library agent over the bus, as the person whose key it holds, with a pass condition per
scenario. T1–T10 are the scenarios tracked at https://github.com/oramasearch/lake/issues/276
through https://github.com/oramasearch/lake/issues/285. T11 builds a data App with
ApplicationAgent, confirms the exact proposal, waits for deployment, and requests its URL. It
prints one line per scenario and exits 1 when one fails. T12 gives ExperimentsAgent a goal,
confirms its exact study, waits for trials and requests the explanation. Run the relevant scenario
after a roll of lake, Zetta or an agent.

## Not yet

- Custom Python agents on `agent-runtime` still answer within the call.
- Streaming subscriptions (SSE) replace the long poll when Zetta ships them.
