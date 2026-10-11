# Running and talking to agents

**Status: implemented 2026-09-27 (lake#231, epic lake#228); cron schedules as CronJobs 2026-09-27; workspace-scoped launches, start timeout and definition checks 2026-10-02 (lake#442).** How lake runs user-created Python
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

**A launch belongs to one workspace (lake#442, migration `0043_agent_launch_workspaces`).**
`launch_agent` records the `workspace_id` it was called with, the workspace whose Zetta holds the
agent. The operator joins `workspaces` on it to place the Job or Deployment in that workspace's
namespace. Before 0043 the column did not exist, so the operator's query failed (`column
l.workspace_id does not exist`) and every launch stayed at phase `""`. `list_agent_launches`,
`stop_agent_launch`, `list_agent_schedules` and `unschedule_agent` see only the workspace's own
runs and schedules, so a run never names an agent that this workspace's `list_agents` cannot
show.

**A waiting launch says why, and stops with a reason (lake#442).** Until the operator reports,
`phase` is `Pending` and `message` says lake is waiting for the Orama operator. After a minute it
says how long the launch has waited. A launch the operator has not reported on within five
minutes reads `Failed` with the message *Timed out: the Orama operator did not start this run
within five minutes.* Lake's minute loop then withdraws it (`desired = absent`, `stop_reason`
set), so it cannot start late. `operator_phase` is the phase the operator wrote, empty until it
reports. Migration 0043 withdraws, in the same way, launches the operator never saw and that are
older than five minutes.

**The pod's credential never touches the database.** The operator generates a random credential
per launch, `lal.<launch id>.<secret>`, puts it in a Secret owned by the Job or Deployment, and
writes only its SHA-256 (`credential_sha256`). lake's stores resolve such a bearer by comparing
digests in constant time (`context_for`): it acts as the person who launched it, limited to the
launch's `scopes`. A launch that is stopped (`desired = absent`) or not yet reported has no usable
credential.

**A run's scopes follow its agent's skills** (agent-runtime#7). `control::agent_launch_scopes_for`
reads the agent record's `skills` when lake stores the launch, schedule or conversation service,
and gives the credential:

- what every run has (`control::agent_launch_scopes`): `workspace_query`, `workspace_act`,
  `workspace_openapi`, `list_agent_library`, `repository_tree`, `repository_file` (a definition
  stored in lake), the workspace's transcripts through `list_transcripts` and `get_transcript`
  (lake#561), and the App and build reads agent-runtime gives every agent (`list_apps`, `get_app`,
  `app_logs`, `receive_app_events`, `list_builds`, `get_build`, `build_logs`,
  `receive_build_events`); and the tools of the workspace's MCP connections through
  `list_mcp_connection_tools`, `read_mcp_tool` and `call_mcp_tool` (lake#652), where lake checks the
  agent's `mcp:<prefix>` skill on every call ([MCP connections](mcp-connections.md#who-may-call-a-tool));
- the **Lake reads** each of its skills grants, and nothing for a skill lake does not know.

A skill's changes are never scopes: agent-runtime turns them into proposals a person confirms, and
the confirmed change runs as that person. Which reads each skill grants is agent-runtime's decision
(`skills.py`, published as `skills.json`); lake vendors it with
`tools/agents/vendor-agent-skills.py <agent-runtime>/skills.json` into
`src/control/agent_skills.cpp` (and the labels into
`frontend/src/pages/train/agent-skills.json`), and `--check` says when lake's copy is behind. See
[agent-factory.md](agent-factory.md#skills) for the skills. A launch or schedule credential that
leaves out `workspace_id` gets its launch's or schedule's workspace, as a task credential gets its
task's (lake#652). A schedule's runs and a conversation
service get the same scopes as a launch of the same agent.

Verify: `AgentRuns.ARunsKeyCarriesTheReadsOfItsAgentsSkillsAndNoChange` and
`AgentRuns.EverySkillLakeOffersIsOneAgentRuntimeDefines` (tests/test_workspaces.cpp).

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
once every five minutes. The refusal states the rule: `every 1 minutes` answers that a schedule runs
at most once every five minutes, so the shortest is `every 5 minutes` (lake#442).

`time_zone` must be a name in the IANA time zone database (`Etc/UTC` when omitted), because the
operator hands it to the CronJob's `spec.timeZone`, which Kubernetes refuses otherwise. lake checks
it before anything is stored (lake#586): `{"cron": "0 9 1 1 *", "time_zone": "Mars/Olympus"}`
answers `invalid_input` at `/time_zone`, *'Mars/Olympus' is not an IANA time zone; give one such as
Etc/UTC, America/Toronto or Europe/Paris*, and no schedule exists. A name that differs only in
letter case is refused with the spelling it most likely meant (`america/toronto` → *did you mean
'America/Toronto'?*). The cron expression is checked first, at `/cron`. The list of names is
`src/control/time_zones.cpp`, generated from tz database 2026c by `tools/tz/generate-time-zones.py`;
regenerate it when IANA publishes a release.

A schedule's `phase` and `message` are lake's, not the operator's (`agent_schedule_progress`,
lake#586): `Pending` until the operator reports (*Waiting for lake to set up this schedule.*),
`Ready` (*Scheduled.*), `Stopped` once removed, and `Failed` with why and what to do: an unknown time
zone (from a schedule stored before the check), runs not enabled on this lake, or otherwise *Lake
could not set it up. Remove it and add it again*. The operator's own text, which names Kubernetes
objects and fields, stays in `agent_cron_schedule_status` for whoever runs lake. A schedule
belongs to its workspace, as a launch does. Creating a Python agent with a trigger in Design → Agents (or through the
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
  editable experiment proposal and its confirmation;
- Design → Opportunities shows *Scanning your workspace…*.

Auto-Suggest builds the workspace profile itself with its read-only credential. Your own Python and
native agents still answer within the call (a follow-up).

## Turns

`agent_turn` delivers one message to any agent and returns its answer:

| Agent | Where the turn goes | What it is handed |
|---|---|---|
| **Ask Orama** (`agent-ask-orama`) | its service, `LAKE_AGENT_ENDPOINTS` | a key for this turn derived from the person's credential, carrying **every read capability and `agent_turn`, never an action**, and `lake_url` |
| **Project Agent** (`agent-project`) | its service | a turn key limited to `workspace_query` / `workspace_openapi`, and `zetta_base_url` = lake's agent path for the workspace. A `context` naming `project` and `graph` is a **change request**: lake adds the graph as it is (`pipeline`), the agent answers with a proposal (agent-project edit mode), and lake keeps it as an Ask Orama message and returns `review_url` (the pipeline page with `&proposal=`), where the person applies it (lake#249) |
| **ApplicationAgent** (`agent-application`) | its service | an `app` task with read-only workspace, dataset, Service, repository and App tools. A completed proposal returns a Ship → Apps `review_url`. A later task carrying the reviewed object at `context.confirmed_proposal`, with `proposal_task_id` and `proposal_sha256` naming the stored proposal it confirms (checked by `agent_turn`), also receives `create_repository`, `commit_repository_files`, `create_app`, and `deploy_app` |
| **ExperimentsAgent** (`agent-experiments`) | its service | an `experiment`, `experiment_status` or `experiment_explain` task and a credential for the selected workspace adapter. Proposal turns can read planning inputs (including `validate_study`, the no-side-effect preflight of `create_study`) but cannot write. A later `experiment` task carrying `context.confirmed_proposal`, with `proposal_task_id` and `proposal_sha256` naming the stored proposal it confirms (checked by `agent_turn`; only the experiment, the agent's `study`, may be edited), receives workspace `create_study` and backing `start_experiment`; status receives trial monitoring/stop tools, and explanation receives promotion only with its own confirmation. Experiment data remains in workspace Zetta |
| **Auto-Suggest** (`agent-autosuggest`) | its service | the workspace's profile, which lake builds (`list_datasets`, then `get_dataset` for up to twelve); no key: it reads nothing itself |
| **AgentCreator** (`agent-creator`) | its service | a turn key limited to `workspace_query` / `workspace_openapi` |
| a **native** agent of the account | the workspace's `chat` (`GET`) | the message |
| a **Python** agent of the account | its running `service` launch | the message and conversation id; with no running service lake starts one and answers `status: starting` |

A turn key is revoked when the turn ends. Offline library agents and library agents this lake
does not run are refused. lake presents `LAKE_AGENT_SERVICE_TOKEN` as the bearer to every agent
service.

## Capabilities

| Capability | Kind | Authorization | What it does |
|---|---|---|---|
| `launch_agent` | action | `write` on the customer | `{workspace_id, agent_id, mode: job\|service, input?}`; Python agents only |
| `list_agent_launches` | read | `read` | `{workspace_id, agent_id?, limit?}`, the workspace's runs newest first, with phase, message, `operator_phase`, `stop_reason`, exit code and result; the schedules' runs are included (trigger `schedule`) |
| `stop_agent_launch` | action | `write` | `{launch_id}`; the operator removes the Job or Deployment |
| `schedule_agent` | action | `write` | `{agent_id, cron \| trigger, time_zone?, input?}`; adds one schedule; Python agents only; an unknown IANA zone is `invalid_input` at `/time_zone` |
| `list_agent_schedules` | read | `read` | `{agent_id?}`, oldest first, with its phase, a message in lake's words (lake#586) and last firing |
| `unschedule_agent` | action | `write` | `{schedule_id}`; the operator deletes its CronJob |
| `agent_turn` | action | `write` | `{agent, message, conversation_id?, context?, page?, kind?}`; Lake creates the conversation ID when omitted and uses it to attach the preceding same-kind turn; ExperimentsAgent accepts `experiment`, `experiment_status`, or `experiment_explain` |

Audit kinds: `agent.launch`, `agent.stop`, `agent.schedule`, `agent.unschedule`, `agent.turn`.

## Model Tokens (lake#564, lake#248)

A launch, a schedule's run or a background turn calls its model through Lake's
[model gateway](model-gateway.md): the OpenAI SDK with its base URL at `<LAKE_URL>/openai/v1` and its
own credential (`LAKE_TOKEN`, or the turn's `lake_token`) as the API key. Lake sends the call to the
provider with Orama's key and counts the provider's own token usage in the workspace the run belongs
to, by agent, run and model; the Usage dashboard and the agent's page show them
([metering](metering.md#model-tokens)). Agent pods hold no provider key. The capability
(`model_completion`) is not in a launch's scopes because it needs none: lake authorizes it by the
credential's kind and finds the run from it. `record_model_usage` remains only for a model call made
outside Lake.

## Configuration

| Variable | Meaning |
|---|---|
| `LAKE_AGENT_ENDPOINTS` | the library agents this lake runs, `key=url,...` (operator) |
| `LAKE_INTERNAL_URL` | lake's in-cluster address, handed to agents |
| `LAKE_AGENT_SERVICE_TOKEN` | the bearer lake presents to agent services |

## Train → Agent: runs and schedules

Running an agent stays on its Train page (`/train/agent?id=&tab=runs`), linked from its Design page
(lake#653). The page lists its runs (started, how, mode, status, result, *Stop*; a scheduled run has no
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
`…AgentTurnReachesLibraryNativeAndPythonAgents`, `…LaunchesAndSchedulesBelongToTheirWorkspace`,
`…AWaitingLaunchSaysWhyAndTimesOutWithAReason`, `…CreateAgentRefusesDefinitionFieldsZettaWouldDrop`,
and `PostgresAgentLaunches.LaunchesCredentialsSchedulesAndConversationsFollowTheContract`.

## Creating a Python agent

A Python agent is created with `workspace_act` `create_agent`. Its `definition` is
`{source: "zetta", instructions, entrypoint, files: [{path, contents}]}`, with each file's text in
`contents`, or `{source: "lake", repository_id, revision, entrypoint}`. Zetta keeps the definition
as a loose object and ignores keys it does not read, so `{path, content}` used to become an empty
file. Lake now refuses any other field in the definition or in a file, and a `files` that is not a
list, with `invalid_input` and the field's path (for example `/input/definition/files/0/content`)
(lake#442). `update_agent` is checked the same way. A refusal from Zetta itself
(`invalid_argument`) is reported as lake's `invalid_input`.

## Known limits

- Event triggers ("when a new dataset lands") are stored on the agent but not fired yet.
- A Python agent's first message starts its service; the person sends it again once it is Ready.
- OpenAI usage is not yet metered per agent.
