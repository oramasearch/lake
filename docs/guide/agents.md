# Agents

An agent is an AI helper that works in your workspace. Lake has two kinds:

- **Orama's agents** (the Agent Library): the specialized agents Orama provides, such as the
  Project Agent and ExperimentsAgent. They are read-only: you use them where they appear in the
  product, or duplicate one to make an agent of your own from it.
- **Your agents**: agents you design in **Design → Agents** for a job of your own. They answer when
  you talk to them, and the ones with a trigger also run on their own.

**Design → Agents** is where you design agents: what each is for, its prompt, its skills and the
models and pipelines it may use. **Train → Agents** is where you train and improve the ones that
learn: their performance over time, experiments, and promoting a better version. Conversations
happen in [Ask Orama](/docs/ask-orama).

Every call below names `customer_id`, the id of the account that owns your workspaces (yours, or
your organization's), and `workspace_id`, the workspace in the page address
([Workspaces](/docs/workspaces)).

## Orama's Agents

Open **Design → Agents** (it was *Agent Library*). Orama's agents are marked **By Orama**; filter by
**By Orama** to see only them. Each card says what the agent does for you, where to use it, and
whether this Lake runs it (*Available*, or *Not available here*). Open one to see it, and choose
**Duplicate to edit** to design an agent of your own from it.

| Agent | What it does | Where you meet it |
|---|---|---|
| **Ask Orama** | Answers from what you can read and proposes changes for you to confirm; hands work to the other agents | **Ask Orama** in the top bar of every page |
| **Project Agent** | Builds pipelines and proposes changes to them | **Ask the Project Agent** on a pipeline; a solution's foundation |
| **ApplicationAgent** | Proposes and builds data apps | **Ship → Apps** |
| **ExperimentsAgent** | Proposes editable training experiments, follows their trials and explains results | **Train → Experiments** |
| **Auto-Suggest** | Suggests projects worth building from your data | **Design → Opportunities** |
| **AgentCreator** | Designs a new agent with you | **Design → Agents → Describe it to the Agent Factory** |

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
| **Skills** | What it may do: *Search data*, *Generate a dataset*, *Train a Model*, *Summarize and explain*, *Call the API*, *Use drives*, *Use transcripts*, *Work in repositories*, *Load datasets*, *Read and publish streams*, *Design pipelines*, *Read usage and billing*. Granted, never assumed. They are the agent runtime's own skills (agent-runtime#7), vendored into Lake. A skill lets the agent read what it covers; anything it would change, it proposes, and you confirm it. |
| **Revision** | One saved definition of your agent. Every create and edit is kept; you can restore any of them. |
| **Trained version** | A checkpoint training produced for a base-model agent, with the data it learned from and its held-out score. You promote one, or roll back to one, in **Train → Agents**. |
| **Pipelines as tools** | Approved pipelines you grant an agent; it can check, run and report on each one. |
| **Unattended run** | One run of a frontier-model agent as a job, from **Run now** or a schedule. |
| **Schedule** | A cron schedule that runs the agent as a job on every tick, as the person who set it. |
| **Stream that starts it** | A workspace stream (**Train → Streams**) whose new messages each start a run of the agent, with the message as its input, as the person who chose the stream. |

## See Your Agents

**Design → Agents** shows figures (yours, Orama's, those that run on their own) and a card per agent,
yours marked **Yours** with its purpose, its trigger or *Conversation* and its model, and Orama's
marked **By Orama**. Search, or filter by **By Orama** and **Yours**.

- REST: `GET /v1/list_agents?customer_id=<customer id>&workspace_id=<workspace id>`
- MCP: `list_agents`, with the same input. `get_agent`, with the agent's `id`, reads one.

## Design an Agent

1. Open **Design → Agents** and choose **New agent**.
2. Fill in the drawer:
   - **Name** and **What it's for**;
   - **Instructions**: its prompt, how it should work (stored as its manifesto);
   - **What success looks like**: its goal;
   - **Skills**: what it may do;
   - **Model**: *Orama base model, inside your workspace*, a model trained in this workspace, or
     *A frontier model (OpenAI), run by Lake*;
   - **Pipelines it may use**: the approved pipelines it may call as tools;
   - **Where it works**: anywhere in this workspace, one project, or specific datasets, and
     **Which project or datasets**;
   - **Runs on its own**: a schedule in plain words, such as *every Monday at 09:00*. Leave it
     empty for an agent you only talk to.
   - **Starts on new messages in**: for a frontier-model agent, the workspace streams whose new
     messages each start a run ([Start It From a Stream](#start-it-from-a-stream)).
3. Choose **Create agent**. The agent's Design page opens.

The agent's Design page shows its definition, **where it is used** (the pipelines it calls, its
schedules, the streams that start it, its runs, Ask Orama) and its **Revisions**. **Edit** saves a new revision; **Restore**
on an older revision saves its definition again as the newest; **Duplicate** makes a copy under a
new name; **Run it** opens its runs and schedules.

- REST: `GET /v1/list_agent_revisions?customer_id=<customer id>&workspace_id=<workspace id>&agent_id=<agent id>`
- MCP: `list_agent_revisions`. Restore one with `update_agent` and its `definition`.

On the page, a frontier-model agent with a trigger also gets its first schedule. A trigger on a
base-model agent is kept on the agent but does not run it. Over REST or MCP, `create_agent` only
creates the agent: add its schedule with `schedule_agent` ([Schedule It](#schedule-it)).

- REST: `POST /v1/create_agent` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "name": "Weekly churn digest", "purpose": "Summarize which accounts are most at risk", "runtime": "python", "model_name": "gpt-4o", "skills": ["search"], "trigger": "every monday at 09:00", "definition": {"source": "zetta", "instructions": "# Weekly churn digest", "files": [], "entrypoint": ""}}`
- MCP: `create_agent`, with the same input.

A frontier agent's `definition` is `source` `zetta` with `instructions`, `entrypoint` and `files`
(each `path` and `contents`), or `source` `lake` with `repository_id`, `revision` and `entrypoint`.
Lake refuses any other field in it with `invalid_input` and the field's path.

## Describe It to the Agent Factory

The Agent Factory designs the agent with you in a conversation with **AgentCreator**.

1. Open **Design → Agents** and choose **Describe it to the Agent Factory**.
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

On an agent's page choose **Talk in Ask Orama**, or pick the agent under **Talk to** in
Ask Orama. A base-model agent answers from the workspace. A frontier-model agent answers from a
running service: the first message starts it and answers *starting*; send the message again once it
is ready.

- REST: `POST /v1/agent_turn` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "agent": "<agent id>", "message": "Which accounts need a call this week?"}`
- MCP: `agent_turn`.

## Grant Pipelines as Tools

On the agent's Design page choose **Edit**, and under **Pipelines it may use** pick the approved
pipelines it may call. Only approved pipelines can be granted; each grant is pinned to the approval
it was granted at. **Where It Is Used** lists them.

- REST: `POST /v1/update_agent` with the agent's `id` and its whole `pipelines` list.
- MCP: `update_agent`.

## Run It Now

For a frontier-model agent, choose **Run it** on its Design page (or open it from **Train →
Agents**), then the **Unattended Runs** tab. Under **Run it once with** say what it
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

## Start It From a Stream

A frontier-model agent can be started by a workspace stream as well as by a schedule. In **New
agent** or **Edit**, choose one or more streams under **Starts on new messages in**, then save. From
then on, each new message on a chosen stream starts one run of the agent, with the message as its
input: the run is handed `trigger_id`, `stream` (the stream's name), `sequence` (the message's place
on the stream) and `message` (the message itself).

- A run acts as **you**, the person who chose the stream, as a scheduled run acts as whoever set
  the schedule. If you can no longer act in the workspace, its runs stop starting.
- Only messages that arrive after you choose a stream start runs; the ones already on it do not.
- Each message starts one run. The runs show on the agent's **Runs** tab, marked *stream*.
- The agent's Design page lists the streams under **Where It Is Used**, each with who set it.
- System streams (`agent-events`, `agent-*`, `app-events.*`, `build-events.*`, `run-metrics.*`,
  `study-events.*`) cannot start an agent, so an agent's own events never start it again. An
  agent that publishes to a stream it starts from would start itself: choose another stream for
  its output.

Over REST or MCP, `streams` is the whole set: a stream left out stops starting the agent, and
`[]` removes them all.

- REST: `POST /v1/set_agent_streams` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "agent_id": "<agent id>", "streams": ["orders"]}`
- REST: `GET /v1/list_agent_streams?customer_id=<customer id>&workspace_id=<workspace id>&agent_id=<agent id>`
- MCP: `set_agent_streams` and `list_agent_streams`.

## Train and Improve It

**Train → Agents** lists only the agents with something to train: those on the Orama base model,
those granted a pipeline, and those with a training schedule. An agent on a frontier model that
calls no pipeline has nothing Lake can train, and is not listed. Each card shows whether its
training is stale or due, its versions and its best held-out score.

An agent's **Training** tab shows:

- **Performance**: the held-out score of the version it runs, its versions, the success rate and
  median time of its recent runs, its model tokens this month, and the held-out score over time.
- **Versions and the data they learned from**, with **Promote** for a newer version and **Roll
  back** for an older one. Each is confirmed, and is kept as a revision of its design.
- **Launch experiment**: compare its trained versions, variations of its instructions or the
  models it could run on. ExperimentsAgent turns this into an experiment you review and confirm on
  **Train → Experiments**; nothing starts before you confirm.
- **Training schedule**: when it retrains (only when you start it, when its tools or goals change,
  daily, weekly or monthly), its training data, what it starts from and how many steps.

To change what the agent is, choose **Edit its design**.

- REST: `GET /v1/agent_training_status?customer_id=<customer id>&workspace_id=<workspace id>&id=<agent id>`;
  `POST /v1/set_training_schedule` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "id": "<agent id>", "training": {"cadence": "weekly"}}`;
  promote with `POST /v1/update_agent` and the version's checkpoint as `model`.
- MCP: `agent_training_status`, `set_training_schedule`, `update_agent`.

## Delete an Agent

On the agent's Design page choose **Delete**, type its name and confirm. The agent is deleted, and
its schedules and the streams that start it with it; its revisions are kept.

- REST: `POST /v1/delete_agent` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "id": "<agent id>"}`
- MCP: `delete_agent`.

## Permissions

| What | Needs |
|---|---|
| See agents, revisions, training, runs, schedules, the streams that start them and Orama's agents | `read` on the account that owns the workspace (the library: any signed-in person) |
| Design, change, grant, train, promote, run, stop, schedule, start from a stream, delete, talk | `write` on that account |

A run acts as the person who started it, a scheduled run as the person who set the schedule, and a
run a stream started as the person who chose the stream,
limited to the workspace's reads and actions and the repository and transcript reads. Its
credential is generated for the run and is never stored in Lake.

## Limits

- Only frontier-model agents run unattended, on a schedule or from a stream; base-model agents
  answer in conversation.
- A schedule fires at most once every five minutes; `every 1 minutes` is refused with that rule.
- A run the platform has not started within five minutes is marked `Failed` with *Timed out* and
  withdrawn, so it cannot start late.
- Runs and schedules belong to the workspace they were made in; another workspace never lists them.
- An agent starts from at most 20 streams. Lake reads each stream every 15 seconds and starts at most
  20 runs per stream each time; a busier stream catches up over the next reads.
- Other event triggers ("when a new dataset lands") are kept on the agent but do not fire: publish
  such events to a stream and start the agent from it.

## Troubleshooting

| What you see | What to do |
|---|---|
| **New agent** is missing | The workspace is not ready yet; wait for it ([Workspaces](/docs/workspaces)). |
| Orama's agents are not listed | Lake could not read the Agent Library; reload the page. |
| An agent is missing from **Train → Agents** | It runs on a frontier model and calls no pipeline, so there is nothing to train. Give it the Orama base model or a pipeline in its design. |
| *No Revisions Kept Yet* | The agent was created before Lake kept revisions; its next save is the first. |
| An agent is *Not running on this Lake* | This Lake does not run that library agent; its page or Ask Orama says it is unavailable. |
| `invalid_input` on `schedule_agent` | The trigger is not one Lake understands, or it would run more often than every five minutes. |
| `invalid_input` on `create_agent` naming `/input/definition/…` | Remove the field it names; only the fields above are accepted. |
| `not_found` on `stop_agent_launch` or `unschedule_agent` | The run or schedule belongs to another workspace. |
| `invalid_input` on `set_agent_streams` | The agent is not on a frontier model, or a stream is a system stream. |
| `not_found` on `set_agent_streams` at `/streams/<n>` | The workspace has no stream of that name; create it in **Train → Streams** first. |
| A stream's messages start no run | The person who chose the stream can no longer act in the workspace, or the workspace is stopped. Choose the stream again as someone who can. |
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
