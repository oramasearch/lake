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
CronJob and the credential, which stops resolving at once; runs already written stay.

`schedule_agent` takes either `cron` (five fields, or `@hourly`, `@daily`, `@weekly`, `@monthly`,
`@yearly`) or `trigger` in Studio's plain words, which lake turns into cron: `every N minutes`
(5–59, or whole hours), `every N hours|days`, `hourly`, `daily`, `weekly`, `every day|weekday|<weekday>
at HH:MM`. lake checks each field's range and refuses a schedule that could fire more often than
once every five minutes. Creating a Python agent with a trigger in Train → Agents (or through the
Agent Factory) adds its first schedule; deleting the agent removes all of them.

The per-agent `agent_schedules` table of 0030 and lake's in-process scheduler are retired: the
table is no longer read, and is left for a deliberate drop.

## Turns

`agent_turn` delivers one message to any agent and returns its answer:

| Agent | Where the turn goes | What it is handed |
|---|---|---|
| **Ask Orama** (`agent-ask-orama`) | its service, `LAKE_AGENT_ENDPOINTS` | a key for this turn derived from the person's credential, carrying **every read capability and `agent_turn`, never an action**, and `lake_url` |
| **Project Agent** (`agent-project`) | its service | a turn key limited to `workspace_query` / `workspace_openapi`, and `zetta_base_url` = lake's agent path for the workspace |
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
| `agent_turn` | action | `write` | `{agent, message, conversation_id?, context?, page?}` |

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
