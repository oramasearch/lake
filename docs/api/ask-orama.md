# Ask Orama

**Status: implemented 2026-09-27 (lake#236, epic lake#228).** Ask Orama is the general agent for all
of lake and the first entry of the Agent Library. Its code is
[`oramasearch/agent-ask-orama`](https://github.com/oramasearch/agent-ask-orama); this page is how lake
shows it and keeps it honest.

## What a person sees

- **Ask Orama** in the top bar of every signed-in page. It opens `/ask?from=<the page>`, so the
  conversation knows where it started.
- `/ask`: the conversation, a **Talk to** selector (Ask Orama first, then the library's other running
  chat agents a person uses, then the space's own chat-capable agents), and a message box. The
  library's internal tooling (Eval Generator and the two corpus generators, `internal: true` in
  `list_agent_library`) is never offered, even where it runs (lake#451); `control::talk_to_agents`
  builds the list. The space's own agents come from Zetta's `list_agents`, which took 380–690 ms on
  the dev site, so the web app adds them to the selector once that read answers, and the agent a
  conversation talks to stays selected meanwhile (lake#446).
  *Start a new
  conversation* begins another. An agent's reply is rendered as markdown (tables, lists, code) through
  lake's one renderer, which escapes every character first, so a reply cannot inject HTML; what the
  person typed is shown as typed.
- **While a turn runs** the conversation says which agent is working (*Ask Orama is working…*, or
  *Waiting for Ask Orama to start…* before an agent claims it), for how long, the turn's latest
  progress message, and the attempt when lake retried it. When Ask Orama brings in another agent,
  lake reports that on the person's turn: *Ask Orama asked ExperimentsAgent: …*, then each of the
  subagent's progress messages (*ExperimentsAgent: …*) and *ExperimentsAgent finished; writing the
  answer.* (lake#451, see [the agent bus](agent-bus.md#delegated-turns)). The page reads this from
  the `get_agent_task` call it already makes; it adds no workspace call. Replies and delegations
  name agents as people call them (*ExperimentsAgent*, not `agent-experiments`).
- Each of Ask Orama's **proposals** (a change it wants to make) is shown with its capability and its
  impact, and a **Confirm** button. A confirmed proposal shows *Confirmed* and is never offered again.

## How it stays honest

- **It works as the person.** A turn goes through `agent_turn` ([agent-launches.md](agent-launches.md)),
  which hands Ask Orama a key for that turn only, derived from the person's own credential and
  carrying **every read capability and `agent_turn`, never an action**. The key is revoked when the
  turn ends.
- **It never changes anything itself.** A change comes back as a proposal. **Confirm** runs it in
  lake, as the person, through the ordinary registry path with its own authorization and audit.
  Only a proposal lake stored in that person's conversation can be confirmed: a forged index or
  another person's message is refused.
- **A change to an existing pipeline** goes to the Project Agent with `{project, graph}`; its answer is a
  proposal and the conversation shows *Review the proposed change*, a link to the pipeline page where
  it is applied (lake#249).
- **It delegates to any agent** (`delegate` → `agent_turn`): the Project Agent, ApplicationAgent,
  ExperimentsAgent, Auto-Suggest, and any agent the person built; an unattended agent is started by proposing
  `launch_agent`. An ApplicationAgent delegation creates an `app` task, whose proposal is reviewed
  and confirmed before that agent receives App writes. An ExperimentsAgent delegation creates an
  `experiment` task, whose editable experiment is reviewed before that agent receives the engine's
  `create_study`.
- **Conversations live in the workspace** (lake#370). Every message is a sealed record in the
  workspace Zetta of the workspace the conversation was held in (collection `lake_ask_messages`,
  [workspace-records.md](../architecture/workspace-records.md)): the person's text, and for Ask
  Orama's the reply with its proposals, reads, delegations and trace id. The page's `/w/<id>/`
  workspace owns it; another workspace of the same space never shows it. Only the person who had a
  conversation reads it back, from any replica. A workspace that is not ready shows the error
  instead of falling back anywhere. Lake's PostgreSQL `ask_messages` table (migration `0030`) was copied
  into the workspace Zettas by the operator's one-shot copy and dropped by migration
  `0050_drop_ask_messages` (lake#483).
- **`get_ask_conversation`** takes `customer_id`, `workspace_id` and `conversation_id` (`limit`
  optional) and reads the person's conversation in that workspace; a missing, malformed, unknown or
  foreign `workspace_id` is `invalid_input` at `/workspace_id`.

## Capabilities (lake#561)

The conversation is written only by lake. The web app, and the server-rendered page's forms, go
through four capabilities, so a browser never writes a message and only a proposal lake stored can
be run. Each takes `customer_id` and `workspace_id` (the workspace whose Zetta holds the
conversation) and is audited in that workspace's log.

| Capability | Kind | Input | Value |
|---|---|---|---|
| `send_ask_message` | action | `message`; `conversation_id` (absent: a new one); `kind` `ask` (default) or `factory`; `agent` (default `agent-ask-orama`; the Agent Factory always talks to `agent-creator`, any other `agent` is `invalid_input`); `page` `{path, <query values>}` (Ask Orama only) | `{conversation_id, message_id, task_id?, reply_id?}` |
| `settle_ask_turn` | action | `conversation_id`, `task_id`, `kind` | `{settled, task}` |
| `confirm_ask_proposal` | action | `conversation_id`, `message_id`, `index` | `{ok, message_id, capability, error?}` |
| `approve_agent_proposal` | action | `conversation_id`, `message_id`, `proposal_sha256` | `{agent_id, name, schedule_error?, check_launch_id?}` ([agent-factory.md](agent-factory.md)) |

- **`send_ask_message`** stores the person's message first; a message the workspace did not store
  is an error and nothing is asked. It then calls `agent_turn`. A library agent answers in the
  background: the value carries `task_id`. An agent of the space answers at once: its reply is
  stored and `reply_id` names it.
- **`settle_ask_turn`** reads the turn as `get_agent_task` does (`task` is that value). While the
  turn is `pending` or `running`, `settled` is false and nothing is written. Once it finished, the
  agent's message is stored exactly once, under an id derived from the task, so two calls (or the
  page and the web app together) store one message. Ask Orama's message is `answer.reply` with
  `{task_id, agent, proposals, reads, delegations, trace_id}`; AgentCreator's is its question,
  *Here is the agent I propose. Review it below.* or its reply, with `{task_id, agent, answer,
  proposal_sha256?}`; a failed turn is *That did not finish: …*. A task of another conversation is
  `invalid_input` at `/task_id`. The web app polls `get_agent_task` every 2 s and calls this once
  the status is final, then rereads the conversation with `get_ask_conversation`.
- **`confirm_ask_proposal`** takes `payload.proposals[index]` of that stored message (anything else
  is `not_found` *That proposal is not in this conversation.*), calls its capability with its input
  as the person (in the conversation's workspace when the input names none, lake#652), and stores *Done: {capability}.* or *That did not work: {error}* with
  `{agent: "agent-ask-orama", confirmed: {message_id, index}, ok}`. The value's `message_id` is
  that outcome message. A capability that refuses is still a successful confirm with `ok: false`.
  The call's own audit event carries `{ask_confirm: true, conversation_id, capability, ok}`; the
  proposal's capability records its own event.

For example, the web app's `send_ask_message` (`POST /w/<workspace_id>/v1/send_ask_message`):

```json
{"customer_id": "8f1c…", "workspace_id": "4b2e…", "message": "What do I have?", "page": {"path": "/design/project", "name": "churn"}}
```

## Routes

| Route | What it does |
|---|---|
| the web app's `/ask?customer_id=&conversation=&from=&agent=` | the conversation; with `task=`, it calls `settle_ask_turn`; sending calls `send_ask_message` with the page's `from` as `page`, and confirming calls `confirm_ask_proposal` (the server forms were retired in lake#561) |

## Verify

`Stores/WorkspaceRecordStores.*` (lake#370): two spaces with two workspaces each keep one
conversation id apart per workspace; foreign, missing, malformed and unknown workspace ids are
refused; a deleted workspace's conversations go with it; the operator copy moves and verifies
earlier rows.

`HttpResponseIsolation.AskOramaSaysWhoIsWorkingOnALongTurn` (lake#451): `settle_ask_turn`'s task
is pending, then running with Ask Orama's message; Ask Orama's turn key delegates to Auto-Suggest,
and the person's turn carries the delegation, the subagent's progress (also published on the
workspace's `agent-events` for the asking turn) and its finish as the task's message, until the
turn settles with the answer. `Workspaces.TalkToOffersOnlyRunningAgentsAPersonUses` covers which
agents *Talk to* offers.

`HttpServer/HttpResponseIsolation.TheWebAppTalksToAskOramaThroughCapabilities` (lake#561): the same
conversation over `/v1` with the browser session: `send_ask_message` with a page, `settle_ask_turn`
writing nothing while the turn waits and the answer once over two calls, a forged
`confirm_ask_proposal` refused, cross-site requests refused, and the real confirm creating the
repository with its outcome stored.
