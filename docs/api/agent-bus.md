# The agent bus: agent traffic over Zetta pub/sub

**Issue:** lake#269; where a task lives:
lake#374,
lake#375 · **Code:**
`include/lake/control/agent_bus.hpp`, `src/control/agent_bus.cpp`, the bus capabilities in
`src/api/workspace_capabilities.cpp` · **Tests:** `AgentRuns.LibraryAgentsTakeTasksAndAnswerOverTheBus`,
`HttpResponseIsolation.LibraryAgentsFinishEveryTurnKindWithoutAWorkspaceEcho`, the Ask Orama,
Agent Factory, Discover and pipeline tests in `tests/test_http_server.cpp`

**No lake page or capability waits on a library agent inside an HTTP request.** Lake and the
library agents (Ask Orama, the Project Agent, Application Agent, Experiments Agent, Auto-Suggest Agent,
Agent Creator Agent) talk over
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

There is no topic per turn. Streams lists the selected workspace's real topics and the account's
per-agent task rows. Workspace event browsing does not filter other accounts' task IDs: other
workspaces' events are stored on different Zettas.

## A turn, end to end

1. `agent_turn` (or a page: Ask Orama, the Agent Factory, Design → Opportunities, the pipeline's
   ask box, Ship → Apps or Train → Experiments) writes the task's request to its workspace Zetta
   and then its dispatch row to `agent_tasks` (migration `0032_agent_tasks`, immutable
   `workspace_id` added by `0038_workspace_data_plane`; see
   [where a task lives](#where-a-task-lives-lake374-lake375)), publishes it on the agent's topic
   and answers `{task_id, status: pending}` at once.
2. The agent's service long-polls `receive_agent_tasks`, then calls `claim_agent_task`. **Exactly one
   claim per attempt succeeds.** Lake generates the task's credential, `lat.<task id>.<secret>`, stores only its
   SHA-256 in `agent_task_status`, and answers the task's body (its request, read from the task's
   workspace just before the claim) with every `"{{credential}}"` in it
   replaced (as `lake_token` and `zetta_token`). The credential acts as the person who asked,
   with the task's scopes, until the task finishes or is reclaimed. Claim also sets a renewable
   lease and returns `lease_seconds`, `lease_until`, and `attempts`. Neither call requires the agent to echo the
   workspace ID from dispatch.
3. The agent does the work and reports with `publish_agent_event`: `progress` messages while it
   works, then its `result` (its whole answer, `{ok, result, …}`) or `failed`. The workspace ID is
   optional here too.
4. Lake atomically applies the event to `agent_task_status`, records its message and answer in
   the task's workspace record, then publishes it directly to that workspace. Only a `Running`
   task accepts events. The first terminal write wins; a concurrent losing result returns
   `conflict` and is never recorded or published as an accepted answer. If publication fails after
   acceptance, the task state remains readable through `get_agent_task`. A finished task stays
   finished. `get_agent_task` and the pages read the task from both halves.
5. A finished Auto-Suggest Agent scan (kind `discover`) is saved into the workspace's
   Opportunities in the same call, after its result is accepted, whether or not anyone is reading
   (lake#1195, [Design → Opportunities](discover.md)). A save that fails there leaves the event
   accepted and is retried by the next `list_opportunities`.

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
| `claim_agent_task {task_id, agent, workspace_id?, credentials?}` | action | that library agent | take the task; the body with its credential, or with none when `credentials` is `by_reference` ([below](#task-credentials-by-reference-lake871)). An optional workspace must match. A second claim, or a claim by another agent, is a `conflict` |
| `publish_agent_event {task_id, agent, type, workspace_id?, message?, result?}` | action | that library agent | publish on the task workspace's `agent-events`, only for a claimed, unfinished task; an optional workspace must match |
| `receive_agent_events {customer_id, workspace_id, task_id, from_sequence?, wait_ms?}` | read | whoever asked the task (or the operator) | retained task events and current task state |

**Each library agent has its own bus credential.** The operator keeps one Secret,
`lake-<name>-agent-bus`, with a random token per library agent. It mounts the whole Secret into
lake (`LAKE_AGENT_BUS_DIR`, one file per agent) and only that agent's token into each agent
(`AGENT_BUS_TOKEN`, with `LAKE_URL`). A bus credential reaches only the first three capabilities,
and only with its own agent's name.

Application Agent receives only tasks from `agent-tasks.agent-application` and only its
`agent-application` bus credential can claim or answer them. The task itself has kind `app`; its
short-lived task credential carries read tools initially and gains repository/App writes only on a
later task with the person's reviewed proposal. Task credentials also derive the workspace for
Lake capability and Zetta-shaped proxy calls, including clients that use the unprefixed
`/workspaces/<customer>/zetta/v1/...` route.

Ask Orama's turns have kind `api`, except New Methodology's draft (lake#688): kind
`methodology_draft`, with the person's description as the message and `context: {name?,
target_kind?}`. Its credential is Ask Orama's usual one (the library-agent reads, no action); the answer's
`result` is the draft's files, which `draft_methodology {task_id}` checks before anything is offered
([methodologies.md](methodologies.md#drafts)).

Experiments Agent has the independent topic and credential `agent-tasks.agent-experiments` /
`agent-experiments`. Its task kinds are `experiment`, `experiment_status` and
`experiment_explain`. The bus still carries only the task id. Experiment definitions, trials and
analysis remain in the selected workspace Zetta, reached with the claimed task credential. An
initial proposal has no experiment write; a new task carrying the reviewed proposal receives only
the engine's `create_study` and its backing `start_experiment` capability in addition to its reads.

### Task credentials by reference (lake#871)

A library agent is one shared process, so a task credential it receives sits in the memory of a
process that serves every tenant. Decided under Steve's 2026-11-14 delegation: **credentials never
reach the agent process; Lake adds them to each request.** A claim with
`credentials: by_reference`:

- mints the task's credential and stores its SHA-256 as the claim, exactly as before, but never
  answers it: every `"{{credential}}"` in the body becomes an empty string, and the answer says
  `credentials: by_reference`;
- the agent then calls Lake (capabilities, MCP, the Zetta-shaped `/w/<workspace>/workspaces/...`
  path) with **its own bus credential as the bearer** and the header
  **`X-Lake-Agent-Task: <task id>`**;
- Lake resolves that pair to the task's principal (`control/agent_task_proxy.hpp`) only while the
  task is present, claimed and `Running` on that agent; anything else is refused as an invalid
  credential, never answered as the bus credential. The principal carries the claim's digest, so
  each call renews the lease and a reclaimed or finished task opens nothing, as with the
  credential itself.

A compromised agent process therefore holds no credential it can take elsewhere, and can act only
for the tasks it is running at that moment, only through Lake, with each task's scopes and in each
task's workspace. The default stays `in_body` until the library agents move to `by_reference`
(their own repositories); then the in-body mode is removed.

**One agent process per workspace (enterprise and isolated installs).** A file
`<agent>.<workspace id>` in `LAKE_AGENT_BUS_DIR` is a bus credential bound to that workspace, for
an agent process that serves it alone. With it, `receive_agent_tasks` lists only that workspace's
tasks, a claim or event for another workspace's task is `forbidden`, `record_model_usage` accepts
only that workspace's tasks, `X-Lake-Agent-Task` opens only that workspace's tasks, and every
claim is by reference whatever it asks. Starting the per-workspace process and mounting its
credential is the operator's part (https://github.com/oramasearch/orama-operator/pull/1176), which
it does only for an image whose `lake manifest` lists the feature `agent_bus_workspace_credentials`.

**Lake re-reads `LAKE_AGENT_BUS_DIR` when it changes** (`control::AgentBusCredentials`), so the
operator adds or removes a workspace's credential without restarting lake. Each bus-credential
lookup compares the directory's modification time and the target of Kubernetes' `..data` link
(which a Secret update swaps) with the last read, and reads the files again when either differs:
a file added is accepted, and a file removed stops working, on the first request after the change
reaches the directory. The kubelet's own delay in updating a mounted Secret still applies.

The service token (`LAKE_AGENT_SERVICE_TOKEN`) is never a bus credential, and no user-created agent
holds it (agent-runtime#14). Lake presents it only to the library agents. To a person's service
launch it presents that launch's own token, `agent_launch_service_token`: hex HMAC-SHA256 of
`agent-launch:<launch id>` keyed by the service token, which the Orama operator gives the launch as
its `AGENT_SERVICE_TOKEN`. The person's code in that pod therefore cannot call a library agent, and
one launch's token does not open another. Lake and the operator roll together for it. Long
polls accept up to 60 seconds (`wait_ms`), but the authorized upstream currently caps each receive
at 30 seconds; Lake clamps accordingly and allows a 40-second transport timeout. Runtime idle
backoff is required until Zetta's longer poll (zetta#1174)
lands. The at-most-one-receive-per-minute deployment acceptance is not claimed by this PR.

## A task credential is confined to its workspace

LSR-118 ([lake#985](https://github.com/oramasearch/lake/issues/985)). A task, launch or schedule
credential acts as the person who started the agent, and that person may belong to other accounts
or other workspaces of the same account. The credential reaches none of them, whatever its input
names:

- **Every policy decision of the call is confined.** `prepare_call` (`src/api/registry.cpp`) sets
  `AuthorizationConfinement` on the call's authorization cache: the task's (launch's, schedule's)
  account and workspace. `control::authorize` and `authorize_batch` then allow an object only when
  its whole parent chain, followed past any inheritance cut, reaches that account's `customer`
  object, and every repository on the chain belongs to that workspace. A repository that cannot be
  shown to belong to it, or a row that cannot be read, is outside. Checks that start a fresh cache
  (`control::fresh_cache_like`) keep the confinement, and so does `search`'s check of each hit
  (`search::Query::confined`), so a search finds nothing in another account or workspace. The person's own identity object
  (`principal/<taap id>`) is not an account's object and is decided as before; the task's scopes
  decide whether such a capability may be called at all.
- **An input naming another account or workspace is refused** before any handler reads it: a
  `customer_id` or `workspace_id` other than the task's, any other id in `workspace_ids`, or a
  repository (`repository_id`, `repository`, or `id` on a repository capability) of another account
  or workspace. The answer is `unauthorized`, *this agent credential acts only in its own
  workspace*.
- **A library agent that reads for a person carries a generated read list, not every read.**
  Ask Orama's turn key carries `agent_turn`, `workspace_openapi`, `workspace_query` and each
  capability `library_agent_may_read` accepts (`src/api/workspace_capabilities.cpp`): reads only,
  never one whose object is a secret, secret mount, principal, sign-in, application, certificate,
  session or agent-bus object, and never a named read of secrets, keys, sessions, devices, grants,
  invitations, SSO, SCIM or recovery state (`read_secret`, `list_secrets`, `secret_metadata`,
  `list_sessions`, `list_sso_providers`, ...). Agent-ask-orama's own exclusion list stays as a
  second check.

Tests: `Policy.AConfinedDecisionAllowsOnlyObjectsInsideItsAccountAndWorkspace`,
`Policy.LibraryAgentReadsLeaveOutSecretsAndCredentials` and
`Streams.TaskCredentialReadsOnlyItsOwnWorkspacesResources`.

## What a library agent holds, and the trust placed in it

lake#871. A library agent (Ask Orama, its specialists, the Application Agent and the rest of the
Agent Library) is one shared process per agent that serves every tenant, with one bus credential.
Claiming a task fills the task body's `{{credential}}` with a task credential that acts as the
person who started the turn. What bounds that credential today:

- **It lives only while its task runs.** Every call with it renews the claim
  (`renew_agent_task_lease` in `prepare_call`), which requires the task to be `Running` under the
  same credential digest; a retry clears the digest, and a finished, failed or reclaimed task's
  credential is refused. A compromised process holds the credentials of the tasks it is serving at
  that moment, never earlier ones.
- **It acts only in its task's account and workspace**
  ([above](#a-task-credential-is-confined-to-its-workspace), lake#985), with the task's scopes.
- **The bus credential names only its own agent's running tasks.** `claim_agent_task`,
  `publish_agent_event` and `record_model_usage` refuse a task of another agent, and
  `record_model_usage` also refuses one that is not `Running` (`conflict`, `/task_id`), so the bus
  credential cannot report usage into any workspace whose task it is not serving.

What it does not bound: while a turn runs, the shared process holds that turn's credential in
clear, beside the credentials of the other tenants' turns it is serving at the same moment. Lake
therefore trusts every library-agent image it dispatches to (`LAKE_AGENT_ENDPOINTS`) as it trusts
its own code: they are Orama's private images, pinned by the operator, and never customer code.
Whether the boundary moves (a library-agent process per workspace, or credentials that never reach
the process in clear) is open on lake#871.

Tests: `AgentLeases.ATaskCredentialEndsWithItsTaskAndAnotherAgentCannotNameIt` and
`RecordModelUsage.ABusCredentialReportsOnlyItsOwnRunningTask`.

## Delegated turns

An agent that delegates calls `agent_turn` with its own task credential (Ask Orama's `delegate`).
**Delegation attenuates** (LSR-013, lake#922): a turn asked for with any agent credential holds only
the scopes that credential holds, plus `get_agent_task`, so no agent can mint a turn with more
authority than its own. A confirmation carried by an agent's turn is refused (lake#989).
Lake then records the asking task's ID as `parent_task_id` in the delegated task's body and reports
the delegated turn on the asking turn (lake#451), so whoever follows the asking turn sees which
agent is working without following a second task:

- at dispatch, the asking task's message becomes *Ask Orama asked Experiments Agent: <request>*;
- each accepted `progress` event of the delegated task becomes *Experiments Agent: <message>* on the
  asking task (*Experiments Agent is working.* for an empty message or the generic *Working on it*);
- its `result` becomes *Experiments Agent finished; writing the answer.*, and `failed`
  *Experiments Agent did not finish: <message>*.

Each report is applied to the asking task with `record_agent_task_event` (it stays `Running`; this
renews its lease), written as the asking task's message in its workspace record, and published on
the workspace's `agent-events` as a `progress` event of the asking task, carrying
`delegated_task_id` and `delegated_agent`. Reports are best effort: an asking turn that already
finished, or a workspace write or publication that fails, leaves the delegated turn unaffected.
Lake's own retry notices for the delegated task are not mirrored. The bus still carries only task
IDs. The asking task's ID is the delegated task's `agent_tasks.parent_task_id` (migration `0062`),
so routing a report reads no workspace; the delegated task's request still names it as
`parent_task_id`, as before.

A report that reaches the workspace publication used to end without a `co_return`, so the
`agent_turn` that awaited it destroyed an unset result: a delegated turn whose asking task was still
`Running` and whose workspace was reachable could abort the lake process (lake#476).

## Where a task lives (lake#374, lake#375)

A task has two halves ([workspace-records.md](../architecture/workspace-records.md)).

**PostgreSQL keeps the dispatch half**, which the bus, the claim, the lease and the operator's
schema need without reading a workspace:

- `agent_tasks`: `id`, `customer_id`, `workspace_id`, `agent`, `kind`, `conversation_id`,
  `parent_task_id` (0062), `scopes`, `deadline_seconds`, `desired`, `generation`,
  `requested_by_user_id`, `requested_by_taap_id`, `published_at`, `created_at`, `updated_at`;
- `agent_task_status`: `phase`, `credential_sha256`, `started_at`, `finished_at`,
  `observed_generation`, `events_from`, `lease_until`, `lease_seconds`, `attempts`,
  `retry_notice_pending`, and `notice` (0062).

The credential hash stays because it is the claim. It is the SHA-256 of a credential that lives
only until the task finishes or is reclaimed, and the claim writes it in the same statement as the
phase and the lease. Every call made with a task credential checks it before any workspace is
chosen. `notice` is lake's own text for a task it reclaimed or failed: a retry, the final failure
after three expiries, or nobody picking the task up. It is never what a person or an agent wrote.
It is shown in place of the agent's message until the agent's next event clears it.

**The task's workspace Zetta keeps what was written for it**, as one sealed record per task in
`lake_agent_tasks` (`AgentTaskContentCodec`, `include/lake/control/agent_task_records.hpp`):

- the request body, with `"{{credential}}"` still in place, since the credential is never stored;
- the agent's latest message;
- its answer, and the finished phase.

The record is keyed by the task id and found by its account, and it opens only in that workspace.
Lake assembles the task callers see from both halves, so `get_agent_task`, `receive_agent_events`,
the claim reply and the pages answer exactly as before.

- **Create.** `agent_turn` writes the request record `if_absent` under a task id it makes, then
  inserts the row. A row never names a missing request; a row that fails to insert leaves an
  unreachable record. A workspace that is not ready refuses the turn (`workspace_not_ready`).
- **Claim.** The claim and its lease are one PostgreSQL statement (`claim_agent_task`), with no
  workspace call inside it, so exactly one claim per attempt still succeeds. The reply needs the
  request, which lake reads with one `get_record` just before that statement, and only for a task
  still waiting for that agent. A workspace that cannot answer refuses the claim and leaves the
  task waiting. A task whose request is not in its workspace (one from before the move, not
  copied yet) is refused with `not_found`, never handed out with an empty request.
- **Events.** PostgreSQL decides first: `record_agent_task_event` moves the phase only for a
  `Running` task, so the first terminal write wins there. Lake then writes the message (and, for
  `result` or `failed`, the answer) to the record conditionally on the revision it read, rereading
  on a conflict. A record that already holds a finished answer is never changed, so a late
  progress write cannot erase the answer. If that write fails after the row accepted the event,
  `publish_agent_event` still publishes the event and answers the workspace's error. The task stays
  finished, and its answer is then only in the event stream.
- **Reads.** `get_agent_task`, `receive_agent_events`, a conversation's next turn and the working
  panels each read the record once. There is no fallback to PostgreSQL or to another workspace. A
  workspace that is not ready answers `workspace_not_ready`, and a deleted workspace takes its
  tasks' records with it.

Migration `0062_agent_task_workspaces` adds the two columns, records each task's owner for the
one-shot copy, and drops nothing. The copy was verified on dev, and migration
`0070_drop_moved_tables` drops `agent_tasks.body`, `agent_task_status.message` and
`agent_task_status.result` (lake#311); both tables stay as the dispatch rows. The copy and the cutover are in
[workspace-records.md](../architecture/workspace-records.md#cutover-runbook-agent-tasks-dev-cluster).

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
to `agent_task_status`; since `0062` the retry and failure texts are written to its `notice`
column. One indexed `UPDATE ... WHERE phase = 'Running' AND lease_until < now()
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

Experiments, Apps, Project foundation and pipeline working panels render the same persisted
message that recovery publishes on `agent-events`: lake's notice from the task's row, which
outranks the agent's message. Since lake#375 the agent's own message is in the task's workspace
record, so a working panel reads that record once (`get_record`) on each refresh. `receive_agent_events` exposes the retained retry notices
and attempt numbers, and `get_agent_task` includes `attempts`, `lease_until` and `lease_seconds`.

### Re-delivery and the shared-credential limitation

Agents must remain idempotent under re-delivery. Confirmed Application Agent deploy, Project Agent
confirmation and Experiments Agent start turns must reuse the same named objects and avoid
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
| `LAKE_AGENT_BUS_DIR` | the library agents' bus credentials, a file per agent named by its key (mounted from `lake-<name>-agent-bus`); a file `<agent>.<workspace id>` is a credential bound to that workspace ([by reference](#task-credentials-by-reference-lake871)) |

Without the bus, a library agent's turn answers `store_unavailable`. A lake run with
`--memory-store` uses an in-memory bus. The startup line reports `agent_bus`: `zetta`, `memory` or
`no`.

## Verify on a deployment

Native regression tests are `Stores/AgentLeases.*` (memory and PostgreSQL), `AgentBusKeys.*` and
`AgentLeaseConfig.*`. They age clocks without sleeping, exercise three expiries, credential
revocation, event/capability renewal, racing terminal writes, retry publication recovery, the
retry notice as the task's message, the indexed migration and schema reapplication. Run full CTest
with local PostgreSQL in static and shared builds; see [local builds](../build/local.md).
To change policy, update both stores, config bounds, schema checks and these tests together.

The #389 regression is covered locally by a fake
library agent receiving, claiming, publishing progress and publishing its result without a
workspace field. The HTTP test runs `app`, `solution:` and `experiment` turns against both stores,
checks dispatch metadata, and reads workspace data with the task credential through the original
unprefixed route. Lake OpenAPI marks the task credential's workspace inputs optional; malformed
JSON still fails validation, and explicit mismatches remain rejected. The shared HTTP helper also
exercises Ask Orama and the Ship App proposal flow without echoing workspace IDs.

`tools/e2e/agents.py <base-url> <token-file> <customer-id> <workspace-id> [T1 …]` is the agents' live scenario
suite: each library agent over the bus, as the person whose key it holds, with a pass condition per
scenario. T1–T10 are the scenarios tracked at lake#276
through lake#285. T11 builds a data App with
Application Agent, confirms the exact proposal, waits for deployment, and requests its URL. It
prints one line per scenario and exits 1 when one fails. T12 gives Experiments Agent a goal,
confirms its exact experiment, waits for trials and requests the explanation. Run the relevant scenario
after a roll of lake, Zetta or an agent.

## Local lease proof (2026-09-30)

Native macOS Release builds used `BUILD_SHARED_LIBS=OFF` in `build` and `ON` in
`build-shared`, with separate dependency build directories and at most six build jobs.
The full suites ran one at a time against local PostgreSQL; each database-backed case used
the repository's isolated database fixture.

```sh
export TMPDIR="$PWD/.cache/proof-tmp"
export LAKE_TEST_DB_URI=postgresql://lake@127.0.0.1:5432/lake_issue414_proof_20260930
ctest --test-dir build --output-on-failure -j 1
ctest --test-dir build-shared --output-on-failure -j 1
```

```text
Static:
100% tests passed, 0 tests failed out of 476
Total Test time (real) = 352.38 sec

Shared:
100% tests passed, 0 tests failed out of 476
Total Test time (real) = 352.20 sec
```

Each suite registered 477 cases, with one disabled search benchmark and 14 existing skips:
the optional Liquid CLI browser-to-terminal test, the uninstalled optional Hugging Face client,
and 12 PostgreSQL UI variants with memory-only status-injection fixtures. All lease regressions
ran on both stores. The focused lease, agent-run and stream selection passed all 45 cases.

Apps, Experiments, Project and Pipeline stayed below 100 ms p95 in both page-performance runs,
with unchanged workspace-call budgets. `lake version`, `/v1/server_info` and `/readyz` passed
the manual binary smoke check. No deployment is claimed.

Linux proof deferred: the provided remote-lane helper was absent. No Docker was used.
Operator schema/configuration handoff and task-scoped event credentials remain the follow-ups
described above.

## Not yet

- The library agents still claim `in_body`: each moves to `credentials: by_reference` in its own
  repository (agent-runtime's bus client first), and then Lake drops the in-body mode (lake#871).
- The operator's per-workspace agent process, gated on the manifest feature, is in
  https://github.com/oramasearch/orama-operator/pull/1176 (lake#871).

- Custom Python agents on `agent-runtime` still answer within the call.
- Streaming subscriptions (SSE) replace the long poll when Zetta ships them.
