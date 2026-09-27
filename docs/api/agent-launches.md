# Running and talking to agents

**Status: implemented 2026-09-27 (lake#231, epic lake#228).** How lake runs user-created Python
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

`schedule_agent` stores a Python agent's trigger in `agent_schedules` with its next run. lake's
scheduler (`control::AgentScheduler`, every `LAKE_AGENT_SCHEDULER_SECONDS`, default 60) launches a
`job` for each due schedule, acting as whoever set it, and moves it to its next run. Understood
triggers, in UTC: `every N minutes|hours|days` (at least five minutes), `hourly`, `daily`,
`weekly`, `every day at HH:MM`, `every weekday at HH:MM`, `every <weekday> at HH:MM`. A trigger lake
cannot place in time is refused when it is set. Creating a Python agent with a trigger in Train →
Agents schedules it.

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
| `list_agent_launches` | read | `read` | `{agent_id?, limit?}`, newest first, with phase, message, exit code and result |
| `stop_agent_launch` | action | `write` | `{launch_id}`; the operator removes the Job or Deployment |
| `schedule_agent` | action | `write` | `{agent_id, trigger}`; empty removes; Python agents only |
| `agent_turn` | action | `write` | `{agent, message, conversation_id?, context?, page?}` |

Audit kinds: `agent.launch`, `agent.stop`, `agent.schedule`, `agent.turn`.

## Configuration

| Variable | Meaning |
|---|---|
| `LAKE_AGENT_ENDPOINTS` | the library agents this lake runs, `key=url,...` (operator) |
| `LAKE_INTERNAL_URL` | lake's in-cluster address, handed to agents |
| `LAKE_AGENT_SERVICE_TOKEN` | the bearer lake presents to agent services |
| `LAKE_AGENT_SCHEDULER_SECONDS` | schedule poll interval (default 60) |

## Train → Agents

An agent's page lists its runs (started, how, mode, status, result, *Stop*), refreshes every five
seconds while one is starting, and for a Python agent offers **Run now** with an input. *Talk to this
agent in Ask Orama* opens the conversation.

## Verify

```sh
ctest --test-dir build -R 'AgentRuns|PostgresAgentLaunches'
```

`AgentRuns.TriggersBecomeTheirNextRunInUtc`, `…TheSchedulerFiresDueSchedulesAsJobsAndMovesThem`,
`…ALaunchCredentialActsAsTheLauncherWithTheLaunchScopes`,
`…LaunchStopAndScheduleAPythonAgentButNeverANativeOne`,
`…AgentTurnReachesLibraryNativeAndPythonAgents`, and
`PostgresAgentLaunches.LaunchesCredentialsSchedulesAndConversationsFollowTheContract`.

## Known limits

- Event triggers ("when a new dataset lands") are stored on the agent but not fired yet.
- A Python agent's first message starts its service; the person sends it again once it is Ready.
- OpenAI usage is not yet metered per agent.
