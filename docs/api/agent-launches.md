# Running and talking to agents

**Status: implemented 2026-09-27 (lake#231, epic lake#228); cron schedules as CronJobs 2026-09-27.** How lake runs user-created Python
agents on `agent-runtime`, fires their schedules, and delivers one conversation turn to any agent,
from the Agent Library's specialized agents to the ones people build.

## Launches

A **launch** is one run of a Python agent on `oramasearch/agent-runtime`. lake writes the desired
launch into `agent_launches` (migration `0030_agent_launches`) and `NOTIFY`s `lake_services`; the
Orama operator (orama-operator#185) runs it and writes `agent_launch_status`:

| Mode | What the operator runs | When it ends |
|---|---|---|
| `job` | a Kubernetes Job (`backoffLimit: 0`), `AGENT_MODE=job`, the launch's `input` as `AGENT_INPUT` | the pod exits; `exit_code` and the runtime's summary land in `result` |
| `service` | a Deployment and Service, `AGENT_MODE=service`; `endpoint` is where lake sends turns | `stop_agent_launch` |

**The pod's credential never touches the database.** The operator generates a random credential
per launch, `lal.<launch id>.<secret>`, puts it in a Secret owned by the Job or Deployment, and
writes only its SHA-256 (`credential_sha256`). lake's stores resolve such a bearer by comparing
digests in constant time (`context_for`): it acts as the person who launched it, limited to the
launch's `scopes` (`control::agent_launch_scopes`: `workspace_query`, `workspace_act`,
`workspace_openapi`, `list_agent_library`, `repository_tree`, `repository_file`). A launch that is
stopped (`desired = absent`) or not yet reported has no usable credential.

## Schedules

A Python agent may have **any number of cron schedules**, and **each is its own Kubernetes
CronJob**. The schedule is desired state in lake's Postgres, as launches are: lake writes it, the
Orama operator reconciles it. lake fires nothing itself.

| Table (migration `0031_agent_cron_schedules`) | Written by | What it holds |
|---|---|---|
| `agent_cron_schedules` | lake | agent, `cron` (five fields), `time_zone` (IANA, default `Etc/UTC`), the plain-words `description` it came from, the `input` each run is handed, scopes, `as-` slug, desired, generation, who set it |
| `agent_cron_schedule_status` | the operator | phase, message, when the CronJob last fired, the **SHA-256** of the schedule's credential |
| `agent_cron_runs` | the operator | one row per Job the CronJob started (keyed by the Job's uid): phase, message, exit code, result |

For each present schedule the operator runs a CronJob named `<lake>-<slug>` (`concurrencyPolicy:
Forbid`, a Job template like a job launch's: `backoffLimit: 0`, the runtime image, `AGENT_MODE=job`,
the schedule's `input` as `AGENT_INPUT`). Every run authenticates with the schedule's credential,
`las.<schedule id>.<secret>`, which the operator generates into a Secret the pod reads; lake
resolves it by comparing digests (`context_for`) and it acts as **the person who set the schedule**,
limited to the launch scopes. Removing a schedule (`desired = absent`) makes the operator delete the
CronJob (and with it a run under way) and the credential, which stops resolving at once; the runs
already written stay.

`schedule_agent` takes either `cron` (five fields, or `@hourly`, `@daily`, `@weekly`, `@monthly`,
`@yearly`) or `trigger` in Studio's plain words, which lake turns into cron: `every N minutes`
(5–59, or whole hours), `every N hours|days`, `hourly`, `daily`, `weekly`, `every day|weekday|<weekday>
at HH:MM`. lake checks each field's range and refuses a schedule that could fire more often than
once every five minutes. Creating a Python agent with a trigger in Train → Agents (or through the
Agent Factory) adds its first schedule; deleting the agent removes all of them.

The per-agent `agent_schedules` table of 0030 and lake's in-process scheduler are retired: the
table is no longer read, and is left for a deliberate drop.

## Background turns (lake#269)

**No lake page or capability waits on a library agent inside an HTTP request.** `agent_turn` to
Ask Orama, the Project Agent, ApplicationAgent, ExperimentsAgent, Auto-Suggest or AgentCreator writes a task to `agent_tasks`
(migration `0032_agent_tasks`: agent, kind, conversation, the body, scopes, deadline, who asked),
publishes it on the agent bus, and answers `{task_id, status: pending}` at once. The agent claims
it, is handed the body with a `lat.` credential lake never stores, and publishes its progress and
answer, all over Zetta pub/sub: [agent-bus.md](agent-bus.md).

Lake assigns a conversation ID when a library-agent caller omits one. A later `agent_turn` with
that ID and the same agent kind receives `prior_turn` in its body: the preceding task ID, the
person's message and the agent's answer. This is the transport contract used by REST, MCP and the
page reply forms; it keeps a clarifying answer in the original conversation instead of starting a
replacement goal.

`get_agent_task {task_id}` follows a task, and `receive_agent_events` follows it live. A finished
Project Agent proposal carries `review_url` (the pipeline page with `&task=`). Pages redirect at
once and follow the task:

- Ask Orama and the Agent Factory show *Thinking…* / *Working on it…* and record the answer in the
  conversation once;
- the pipeline page shows *The Project Agent is working on it…*;
- Ship → Apps shows ApplicationAgent working, then a reply form for a question or the exact
  proposal and its confirmation;
- Train → Experiments shows ExperimentsAgent working, then a reply form for a question or an
  editable study proposal and its confirmation;
- Design → Opportunities shows *Scanning your workspace…*.

Auto-Suggest builds the workspace profile itself with its read-only credential. Your own Python and
native agents still answer within the call (a follow-up).

## Turns

`agent_turn` delivers one message to any agent and returns its answer:

| Agent | Where the turn goes | What it is handed |
|---|---|---|
| **Ask Orama** (`agent-ask-orama`) | its service, `LAKE_AGENT_ENDPOINTS` | a key for this turn derived from the person's credential, carrying **every read capability and `agent_turn`, never an action**, and `lake_url` |
| **Project Agent** (`agent-project`) | its service | a turn key limited to `workspace_query` / `workspace_openapi`, and `zetta_base_url` = lake's agent path for the workspace. A `context` naming `project` and `graph` is a **change request**: lake adds the graph as it is (`pipeline`), the agent answers with a proposal (agent-project edit mode), and lake keeps it as an Ask Orama message and returns `review_url` (the pipeline page with `&proposal=`), where the person applies it (lake#249) |
| **ApplicationAgent** (`agent-application`) | its service | an `app` task with read-only workspace, dataset, Service, repository and App tools. A completed proposal returns a Ship → Apps `review_url`. A later task carrying the reviewed object at `context.confirmed_proposal` also receives `create_repository`, `commit_repository_files`, `create_app`, and `deploy_app` |
| **ExperimentsAgent** (`agent-experiments`) | its service | an `experiment`, `experiment_status` or `experiment_explain` task and a credential for the selected workspace adapter. Proposal turns can read planning inputs but cannot write. A later `experiment` task carrying `context.confirmed_proposal` receives workspace `create_study` and backing `start_study`; status receives trial monitoring/stop tools, and explanation receives promotion only with its own confirmation. Study data remains in workspace Zetta |
| **Auto-Suggest** (`agent-autosuggest`) | its service | the workspace's profile, which lake builds (`list_datasets`, then `get_dataset` for up to twelve); no key: it reads nothing itself |
| **AgentCreator** (`agent-creator`) | its service | a turn key limited to `workspace_query` / `workspace_openapi` |
| a **native** agent of the space | the workspace's `chat` (`GET`) | the message |
| a **Python** agent of the space | its running `service` launch | the message and conversation id; with no running service lake starts one and answers `status: starting` |

A turn key is revoked when the turn ends. Offline library agents and library agents this lake
does not run are refused. lake presents `LAKE_AGENT_SERVICE_TOKEN` as the bearer to every agent
service.

## Capabilities

| Capability | Kind | Authorization | What it does |
|---|---|---|---|
| `launch_agent` | action | `write` on the customer | `{agent_id, mode: job\|service, input?}`; Python agents only |
| `list_agent_launches` | read | `read` | `{agent_id?, limit?}`, newest first, with phase, message, exit code and result; the schedules' runs are included (trigger `schedule`) |
| `stop_agent_launch` | action | `write` | `{launch_id}`; the operator removes the Job or Deployment |
| `schedule_agent` | action | `write` | `{agent_id, cron \| trigger, time_zone?, input?}`; adds one schedule; Python agents only |
| `list_agent_schedules` | read | `read` | `{agent_id?}`, oldest first, with the operator's phase and last firing |
| `unschedule_agent` | action | `write` | `{schedule_id}`; the operator deletes its CronJob |
| `agent_turn` | action | `write` | `{agent, message, conversation_id?, context?, page?, kind?}`; Lake creates the conversation ID when omitted and uses it to attach the preceding same-kind turn; ExperimentsAgent accepts `experiment`, `experiment_status`, or `experiment_explain` |

Audit kinds: `agent.launch`, `agent.stop`, `agent.schedule`, `agent.unschedule`, `agent.turn`.

## Configuration

| Variable | Meaning |
|---|---|
| `LAKE_AGENT_ENDPOINTS` | the library agents this lake runs, `key=url,...` (operator) |
| `LAKE_INTERNAL_URL` | lake's in-cluster address, handed to agents |
| `LAKE_AGENT_SERVICE_TOKEN` | the bearer lake presents to agent services |

## Train → Agents

An agent's page lists its runs (started, how, mode, status, result, *Stop*; a scheduled run has no
*Stop*, its schedule does), refreshes every five seconds while one is starting, and for a Python
agent offers **Run now** with an input and a **Schedules** panel: every schedule with its words,
cron, zone, what it asks, the operator's status and last firing, a *Remove* button, and a form that
takes plain words or a cron expression. *Talk to this
agent in Ask Orama* opens the conversation.

## Verify

```sh
ctest --test-dir build -R 'AgentRuns|PostgresAgentLaunches'
```

`AgentRuns.TriggersBecomeCronExpressions`, `…CronExpressionsAreCheckedAsACronJobWouldTakeThem`,
`…AScheduleCredentialActsAsWhoeverSetTheSchedule`,
`…ALaunchCredentialActsAsTheLauncherWithTheLaunchScopes`,
`…LaunchStopAndScheduleAPythonAgentButNeverANativeOne`,
`…AgentTurnReachesLibraryNativeAndPythonAgents`, and
`PostgresAgentLaunches.LaunchesCredentialsSchedulesAndConversationsFollowTheContract`.

## Known limits

- Event triggers ("when a new dataset lands") are stored on the agent but not fired yet.
- A Python agent's first message starts its service; the person sends it again once it is Ready.
- OpenAI usage is not yet metered per agent.
