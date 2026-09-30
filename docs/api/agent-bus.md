# The agent bus: agent traffic over Zetta pub/sub

**Issue:** [lake#269](https://github.com/oramasearch/lake/issues/269) · **Code:**
`include/lake/control/agent_bus.hpp`, `src/control/agent_bus.cpp`, the bus capabilities in
`src/api/workspace_capabilities.cpp` · **Tests:** `AgentRuns.LibraryAgentsTakeTasksAndAnswerOverTheBus`,
`HttpResponseIsolation.LibraryAgentsFinishEveryTurnKindWithoutAWorkspaceEcho`, the Ask Orama,
Agent Factory, Discover and pipeline tests in `tests/test_http_server.cpp`

**No lake page or capability waits on a library agent inside an HTTP request.** Lake and the
library agents (Ask Orama, the Project Agent, ApplicationAgent, ExperimentsAgent, Auto-Suggest,
AgentCreator) talk over
[Zetta's pub/sub](https://github.com/oramasearch/zetta/tree/dev/docs/pubsub). Dispatch uses Lake's
`spec.zetta` cluster (`LAKE_DATA_PLANE_URL` and `LAKE_DATA_PLANE_TOKEN`); events use the task's
explicit workspace Zetta. See the [workspace data-plane design](../architecture/workspace-data-plane.md).

| Topic | Published by | Payload (JSON; the correlation id is the task id) |
|---|---|---|
| Lake bus `topic:agent-tasks.<agent>` | lake, when a turn is asked | `{task_id, customer_id, workspace_id, agent, kind}` |
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
   claim per attempt succeeds.** Lake generates the task's credential, `lat.<task id>.<secret>`, stores only its
   SHA-256 in `agent_task_status`, and answers the task's body with every `"{{credential}}"` in it
   replaced (as `lake_token` and `zetta_token`). The credential acts as the person who asked,
   with the task's scopes, until the task finishes or is reclaimed. Claim also sets a renewable
   lease and returns `lease_seconds`, `lease_until`, and `attempts`. Neither call requires the agent to echo the
   workspace ID from dispatch.
3. The agent does the work and reports with `publish_agent_event`: `progress` messages while it
   works, then its `result` (its whole answer, `{ok, result, …}`) or `failed`. The workspace ID is
   optional here too.
4. Lake atomically applies the event to `agent_task_status`, then publishes it directly to that
   workspace. Only a `Running` task accepts events. The first terminal write wins; a concurrent
   losing result returns `conflict` and is never published as an accepted answer. If publication
   fails after acceptance, the authoritative task state remains readable through `get_agent_task`.
   A finished task stays finished. Moving remaining task bodies/results out of the control store
   is tracked in the architecture audit; no new PostgreSQL event storage is introduced here.
   `get_agent_task` and the pages read the task from there.

The dispatch recovery loop runs separately from HTTP requests. It republishes unclaimed tasks
and reclaims expired claims; it never polls a Lake-bus event topic. Task credentials cannot select
another workspace, even when the same customer owns both workspaces.

Anyone who may read a task can follow it live with `receive_agent_events`. Ask Orama uses it to
wait on a subagent it delegated to. Following a task reads retained events in its workspace and
selects its correlation ID. Cursors belong to that workspace topic. Task creates require
`workspace_id` from `workspaces.id` (resolved through `zetta_service_id`); missing, unknown or
foreign IDs return `invalid_input`. Agent instructions list workspaces rather than inferring
identity from services. Dispatch and claim replies include the persisted ID for information.
Agent-side receive, claim and publish inputs may omit it; Lake uses the task row's immutable ID.
When an agent supplies an ID, it must match. Person-side event reads still require the ID.

**Nothing secret is on the bus.** Topics carry task ids only. The body travels in the answer to a
claim, and the credential is handed over once and never stored.

**Agents never hold the bus's token.** Every bus call goes through lake, which checks the caller:

| Capability | Kind | Who | What |
|---|---|---|---|
| `receive_agent_tasks {agent, workspace_id?, from_sequence?, wait_ms?}` | read | that library agent | the tasks on `agent-tasks.<agent>` from `from_sequence`, without those already claimed; an optional workspace filters the reply; `next_sequence` continues |
| `claim_agent_task {task_id, agent, workspace_id?}` | action | that library agent | take the task; the body with its credential. An optional workspace must match. A second claim, or a claim by another agent, is a `conflict` |
| `publish_agent_event {task_id, agent, type, workspace_id?, message?, result?}` | action | that library agent | publish on the task workspace's `agent-events`, only for a claimed, unfinished task; an optional workspace must match |
| `receive_agent_events {customer_id, workspace_id, task_id, from_sequence?, wait_ms?}` | read | whoever asked the task (or the operator) | retained task events and current task state |

**Each library agent has its own bus credential.** The operator keeps one Secret,
`lake-<name>-agent-bus`, with a random token per library agent. It mounts the whole Secret into
lake (`LAKE_AGENT_BUS_DIR`, one file per agent) and only that agent's token into each agent
(`AGENT_BUS_TOKEN`, with `LAKE_URL`). A bus credential reaches only the first three capabilities,
and only with its own agent's name.

ApplicationAgent receives only tasks from `agent-tasks.agent-application` and only its
`agent-application` bus credential can claim or answer them. The task itself has kind `app`; its
short-lived task credential carries read tools initially and gains repository/App writes only on a
later task with the person's reviewed proposal. Task credentials also derive the workspace for
Lake capability and Zetta-shaped proxy calls, including clients that use the unprefixed
`/workspaces/<customer>/zetta/v1/...` route.

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
- fails an initially unclaimed task after its waiting deadline (*No agent picked this up within
  300 seconds.*); each retry gets a fresh waiting deadline after its notice is published;
- reclaims a `Running` task whose lease expired, clears its task credential, and increments its
  attempt from 1 to 2, then 2 to 3. It publishes a workspace `agent-events` progress event with
  `reclaimed: true`, `attempts: 2` (or 3), and the message
  **the agent stopped responding; retrying (attempt 2 of 3)**, then republishes its dispatch ID;
- after the third running attempt expires, marks the task `Failed` and publishes a `failed`
  event with `attempts: 3` and **the agent stopped responding**. There is no fourth claim.

Migration `0042_agent_task_leases` adds the lease, duration, attempt count and pending-notice flag
to `agent_task_status`. One indexed `UPDATE ... WHERE phase = 'Running' AND lease_until < now()
RETURNING task_id` performs expiry transitions atomically across replicas. Retry notices remain
pending until workspace publication succeeds, including across Lake restarts; claims wait for
that publication. Publication is at least once, so concurrent sweepers or an interruption between
publication and acknowledgement can repeat the same attempt's notice. A failed workspace
publication leaves that notice pending without preventing the rest of the sweep. Lost dispatch is recovered
by the existing republisher. No per-request sweep or additional page workspace call is added.
Acknowledgements match both attempt and phase: a delayed acknowledgement of the third-attempt
retry must never clear the later final-failure notice, which also has attempt number 3.
Waiting-deadline failures recheck the current waiting clock when applied, so an older sweep
cannot fail a concurrently claimed task or a newly waiting retry.

The default lease is 600 seconds (10 minutes). Accepted `publish_agent_event` calls, direct
`record_agent_task_event` progress, and each capability call authenticated with the current task
credential renew it to `now + lease_seconds`. Terminal events clear the lease. Activity therefore
allows a turn to run beyond the old fixed running deadline; the deadline now covers waiting only.
The registry renews before dispatch, checks the current credential hash and rejects a reclaimed
credential even if its principal was resolved before reclaim. Agents require no heartbeat changes.

`receive_agent_events` exposes the retained retry notices and attempt numbers, and
`get_agent_task` includes `attempts`, `lease_until` and `lease_seconds`.

### Re-delivery and the shared-credential limitation

Agents must remain idempotent under re-delivery. Confirmed ApplicationAgent deploy, Project Agent
confirmation and ExperimentsAgent start turns must reuse the same named objects and avoid
duplicating effects when an earlier attempt already wrote them. Lease recovery retries the same
task body, scopes, workspace and confirmation; it does not grant additional authority.

All replicas of one library agent share its bus credential. An event arriving after reclaim and
before another claim is rejected with `conflict`, as is every event after terminal completion.
After a new claim, a hung-but-alive old claimant can still post progress or the first terminal
result using that shared credential. Progress may interleave; **first terminal write wins**.
Its old task credential cannot make further tool calls. Authenticating event publication with
task-scoped credentials would close the remaining gap, but requires coordinated agent changes
and is a follow-up, not part of this change.

### Schema and operator handoff

Apply the idempotent `db/schema.sql` before rolling the new binary. Existing `Running` rows with
no lease (including a missing start time) receive a default lease once. Reapplying the schema
preserves existing leases and attempt counters. The operator must vendor these exact bytes into
`internal/controller/assets/lake-schema.sql` and use guard `agent_task_leases_guard`.
Operator schema vendoring and `spec.agents.library[].leaseSeconds` validation/environment wiring
are follow-ups outside this Lake worktree. Until then, the Lake default applies; an operator
handoff should map each library entry to `LAKE_AGENT_LEASE_SECONDS` below.

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
| `LAKE_AGENT_LEASE_SECONDS` | optional comma-separated `key=seconds` pairs, e.g. `agent-project=600,agent-experiments=1200`; omitted agents use 600; integers must be 60–3600 inclusive; malformed, unknown or duplicate entries fail startup |
| `LAKE_AGENT_BUS_DIR` | the library agents' bus credentials, a file per agent named by its key (mounted from `lake-<name>-agent-bus`) |

Without the bus, a library agent's turn answers `store_unavailable`. A lake run with
`--memory-store` uses an in-memory bus. The startup line reports `agent_bus`: `zetta`, `memory` or
`no`.

## Verify on a deployment

Native regression tests are `Stores/AgentLeases.*` (memory and PostgreSQL) and
`AgentLeaseConfig.*`. They age clocks without sleeping, exercise three expiries, credential
revocation, event/capability renewal, racing terminal writes, retry publication recovery,
the indexed migration and schema reapplication. Run full CTest with local PostgreSQL
in static and shared builds, including `PagePerformance.*`; see [local builds](../build/local.md).
To change policy, update both stores, config bounds, schema checks and these tests together.

The [#389 regression](https://github.com/oramasearch/lake/issues/389) is covered locally by a fake
library agent receiving, claiming, publishing progress and publishing its result without a
workspace field. The HTTP test runs `app`, `solution:` and `experiment` turns against both stores,
checks dispatch metadata, and reads workspace data with the task credential through the original
unprefixed route. Lake OpenAPI marks the task credential's workspace inputs optional; malformed
JSON still fails validation, and explicit mismatches remain rejected. The shared HTTP helper also
exercises Ask Orama and the Ship App proposal flow without echoing workspace IDs.

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
