# Agents

An agent is an AI helper that works in your workspace. Lake has two kinds:

- **The Agent Library**: the specialized agents Orama provides, such as the Project Agent and
  ExperimentsAgent. You use them where they appear in the product; you do not create or delete
  them.
- **Your agents**: agents you build in **Train → Agents** for a job of your own. They answer when
  you talk to them, and the ones with a trigger also run on their own.

Conversations happen in [Ask Orama](/docs/ask-orama). **Train → Agents** is where you build agents
and read what they did on their own.

Every call below names `customer_id`, the id of the account that owns your workspaces (yours, or
your organization's), and `workspace_id`, the workspace in the page address
([Workspaces](/docs/workspaces)).

## The Agent Library

Open **Design → Agent Library** (it is also at the foot of **Train → Agents**). Each card says what
the agent does for you, where to use it, and whether this Lake runs it (*Available*, or *Not running
on this Lake*).

| Agent | What it does | Where you meet it |
|---|---|---|
| **Ask Orama** | Answers from what you can read and proposes changes for you to confirm; hands work to the other agents | **Ask Orama** in the top bar of every page |
| **Project Agent** | Builds pipelines and proposes changes to them | **Ask the Project Agent** on a pipeline; a solution's foundation |
| **ApplicationAgent** | Proposes and builds data apps | **Ship → Apps** |
| **ExperimentsAgent** | Proposes editable training experiments, follows their trials and explains results | **Train → Experiments** |
| **Auto-Suggest** | Suggests projects worth building from your data | **Design → Opportunities** |
| **AgentCreator** | Designs a new agent with you | **Train → Agents → Build with the Agent Factory** |

Agents marked *Orama internal* are Orama's own test tooling and are never offered to talk to.

- REST: `GET /v1/list_agent_library`
- MCP: `list_agent_library`. It takes no input and answers each agent with `deployed` and `status`.

The library agents that propose changes never make them on their own: the Project Agent's edits wait
for **Apply**, ApplicationAgent's app for **Confirm and deploy**, and ExperimentsAgent's experiment
for **Confirm and start experiment**.

## Concepts

| Term | What it means |
|---|---|
| **Engagement** | *Conversation* (answers when you talk to it) or *Runs on its own* (it has a trigger). |
| **Orama base model** | The agent runs inside your workspace's engine, on its GPU when there is one. Its instructions are its system prompt. |
| **Frontier model** | The agent runs on Lake's agent runtime with a frontier model (OpenAI). Only these agents can be run unattended and scheduled. |
| **Work area** | Where it works: anywhere in this workspace, one project, or specific datasets. |
| **Skills** | What it may do: *Search data*, *Generate a dataset*, *Train a Model*, *Summarize and explain*, *Call the API*. Granted, never assumed. |
| **Pipelines as tools** | Approved pipelines you grant an agent; it can check, run and report on each one. |
| **Unattended run** | One run of a frontier-model agent as a job, from **Run now** or a schedule. |
| **Schedule** | A cron schedule that runs the agent as a job on every tick, as the person who set it. |

## See Your Agents

**Train → Agents** shows figures (agents, those that run on their own, conversation only, those on
a frontier model) and a card per agent with its purpose, its trigger or *Conversation*, its model and
what it may do. Search, or filter by **Conversation only** and **Runs on its own**.

- REST: `POST /v1/list_agents` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>"}`
- MCP: `list_agents`, with the same input. The engine read `get_agent`, with the agent's `id`
  in `input`, reads one.

## Create an Agent

1. Open **Train → Agents** and choose **New agent**.
2. Fill in the drawer:
   - **Name** and **What it's for**;
   - **Where it works**: anywhere in this workspace, one project, or specific datasets, and
     **Which project or datasets**;
   - **Instructions**: how it should work (stored as its manifesto);
   - **What success looks like**: its goal;
   - **What it may do**: its skills;
   - **Model**: *Orama base model, inside your workspace* or *A frontier model (OpenAI), run by
     Lake*;
   - **Runs on its own**: a trigger in plain words, such as *every Monday at 09:00*. Leave it empty
     for an agent you only talk to.
3. Choose **Create agent**. The agent's page opens.

On the page, a frontier-model agent with a trigger also gets its first schedule. A trigger on a
base-model agent is kept on the agent but does not run it. Over REST or MCP, `create_agent` only
creates the agent: add its schedule with `schedule_agent` ([Schedule It](#schedule-it)).

- REST: `POST /v1/create_agent` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "name": "Weekly churn digest", "purpose": "Summarize which accounts are most at risk", "runtime": "python", "model_name": "gpt-4o", "skills": ["search"], "trigger": "every monday at 09:00", "definition": {"source": "zetta", "instructions": "# Weekly churn digest", "files": [], "entrypoint": ""}}`
- MCP: `create_agent`, with the same input.

A frontier agent's `definition` is `source` `zetta` with `instructions`, `entrypoint` and `files`
(each `path` and `contents`), or `source` `lake` with `repository_id`, `revision` and `entrypoint`.
Lake refuses any other field in it with `invalid_input` and the field's path.

## Build It with the Agent Factory

The Agent Factory designs the agent with you in a conversation with **AgentCreator**.

1. Open **Train → Agents** and choose **Build with the Agent Factory**.
2. Under **Ask it**, say what the agent should do. AgentCreator asks what it must, then shows a
   **Proposal**: what kind of agent and why, where it is stored, its work area, instructions, goal,
   skills, trigger, pipelines and every file.
3. A proposal with defects lists them under *This proposal has defects and cannot be created yet*
   and cannot be approved: tell AgentCreator what to change.
4. Choose **Approve and create**. Lake creates exactly the proposal you reviewed. A frontier agent
   with a trigger is scheduled, and a first check run starts so its first run is one you can read.

Nothing is created until you approve. If the proposal changed since the page showed it, approval is
refused with *The proposal changed since you saw it; review it again.*

- REST: `POST /v1/send_ask_message` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "message": "An agent that summarizes at-risk accounts every Monday", "kind": "factory"}`.
  The answer names the `conversation_id` and the `task_id` to follow; `settle_ask_turn` stores the
  reply once the task has finished.
- Approve: `POST /v1/approve_agent_proposal` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "conversation_id": "<conversation id>", "message_id": "<proposal message id>", "proposal_sha256": "<digest stored with the proposal>"}`.
  The digest is the `proposal_sha256` that `get_ask_conversation` returns with the proposal message.
- MCP: `send_ask_message`, `settle_ask_turn`, `get_ask_conversation` and `approve_agent_proposal`.

## Talk to an Agent

On an agent's page choose **Talk to this agent in Ask Orama**, or pick the agent under **Talk to** in
Ask Orama. A base-model agent answers from the workspace. A frontier-model agent answers from a
running service: the first message starts it and answers *starting*; send the message again once it
is ready.

- REST: `POST /v1/agent_turn` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "agent": "<agent id>", "message": "Which accounts need a call this week?"}`
- MCP: `agent_turn`.

## Grant Pipelines as Tools

On the agent's **Overview** tab, **Tools This Agent May Use** lists the pipelines granted to it, each
with **Revoke**. **Approved Pipelines You Can Grant** lists the workspace's approved pipelines, each
with **Grant**. Only approved pipelines can be granted; each grant is pinned to the approval it was
granted at.

- REST: `workspace_act` with `update_agent`, sending the agent's whole pipeline list.
- MCP: `workspace_act`.

## Run It Now

For a frontier-model agent, open the **Unattended Runs** tab. Under **Run it once with** say what it
should do this time, and choose **Run now**. The run is listed under **What It Did on Its Own** with
how it started, its status and its result; while one starts the page checks again every 5 seconds.
**Stop** ends a run under way. A scheduled run has no **Stop**; remove its schedule instead.

Run:

- REST: `POST /v1/launch_agent` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "agent_id": "<agent id>", "mode": "job", "input": "Summarize this week's at-risk accounts"}`
- MCP: `launch_agent`. `mode` `job` runs it once; `service` keeps it up for conversations.

List runs:

- REST: `GET /v1/list_agent_launches?customer_id=<customer id>&workspace_id=<workspace id>&agent_id=<agent id>&limit=20`
- MCP: `list_agent_launches`. Each run carries its `phase`, `message`, exit code and result.

Stop:

- REST: `POST /v1/stop_agent_launch` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "launch_id": "<launch id>"}`
- MCP: `stop_agent_launch`.

## Schedule It

For a frontier-model agent, open the **Schedules** tab and choose **Add schedule**. Write when, in
plain words or as cron, and what each run is asked to do, then **Add schedule**. Every tick runs the
agent once, as you. An agent may have several schedules; each row shows its words, its cron and time
zone, what it asks, its status and when it last fired, and **Remove**.

Plain words Lake understands: `every N minutes` (5 to 59), `every N hours`, `every N days`,
`hourly`, `daily`, `weekly`, and `every day at HH:MM`, `every weekday at HH:MM` or
`every monday at HH:MM`. Cron is five fields, or `@hourly`, `@daily`, `@weekly`, `@monthly` or
`@yearly`. The time zone is an IANA name, UTC by default.

- REST: `POST /v1/schedule_agent` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "agent_id": "<agent id>", "trigger": "every monday at 09:00", "time_zone": "Europe/Rome", "input": "Summarize this week's at-risk accounts"}`
  (or `"cron": "0 9 * * 1"` in place of `trigger`).
- MCP: `schedule_agent`.

List and remove:

- REST: `GET /v1/list_agent_schedules?customer_id=<customer id>&workspace_id=<workspace id>&agent_id=<agent id>`
- REST: `POST /v1/unschedule_agent` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "schedule_id": "<schedule id>"}`.
  Removing a schedule stops a run under way; the runs already recorded stay.
- MCP: `list_agent_schedules` and `unschedule_agent`.

## Training

A base-model agent that has training shows a **Training** tab with its training status
(`workspace_query` with `agent_training_status`).

## Delete an Agent

On the agent's page choose **Delete agent**, type its name and confirm. The agent is deleted, and its
schedules with it.

- REST: `POST /v1/delete_agent` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "id": "<agent id>"}`
- MCP: `delete_agent`.

## Permissions

| What | Needs |
|---|---|
| See agents, runs, schedules and the Agent Library | `read` on the account that owns the workspace (the library: any signed-in person) |
| Create, change, grant, run, stop, schedule, delete, talk | `write` on that account |

A run acts as the person who started it, and a scheduled run as the person who set the schedule,
limited to the workspace's reads and actions and the repository and transcript reads. Its
credential is generated for the run and is never stored in Lake.

## Limits

- Only frontier-model agents run unattended or on a schedule; base-model agents answer in
  conversation.
- A schedule fires at most once every five minutes; `every 1 minutes` is refused with that rule.
- A run the platform has not started within five minutes is marked `Failed` with *Timed out* and
  withdrawn, so it cannot start late.
- Runs and schedules belong to the workspace they were made in; another workspace never lists them.
- Event triggers ("when a new dataset lands") are kept on the agent but do not fire.

## Troubleshooting

| What you see | What to do |
|---|---|
| **New agent** is missing | The workspace is not ready yet; wait for it ([Workspaces](/docs/workspaces)). |
| *The Agent Library is not available* | Lake could not read the library; reload the page. |
| An agent is *Not running on this Lake* | This Lake does not run that library agent; its page or Ask Orama says it is unavailable. |
| `invalid_input` on `schedule_agent` | The trigger is not one Lake understands, or it would run more often than every five minutes. |
| `invalid_input` on `create_agent` naming `/input/definition/…` | Remove the field it names; only the fields above are accepted. |
| `not_found` on `stop_agent_launch` or `unschedule_agent` | The run or schedule belongs to another workspace. |
| `invalid_input` on `approve_agent_proposal` | *This proposal still has defects*: ask AgentCreator to fix them. |
| `conflict` on `approve_agent_proposal` | The proposal changed; review it again and approve the new one. |
| A run stays *Pending* | Lake is waiting for the platform to start it; after five minutes it fails with a reason. |

## Related Pages

- [Ask Orama](/docs/ask-orama)
- [Train a Model](/docs/train)
- [Experiments](/docs/experiments)
- [Apps](/docs/apps)
- [AI Solutions, Methodologies and Opportunities](/docs/solutions)
- [Streams](/docs/streams)
