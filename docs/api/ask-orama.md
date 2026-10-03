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
  builds the list. *Start a new
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
  `experiment` task, whose editable study is reviewed before that agent receives `create_study`.
- **Conversations are lake's.** Every message is an `ask_messages` row (migration `0030`): the
  person's text, and for Ask Orama's the reply with its proposals, reads, delegations and trace id.
  Only the person who had a conversation reads it back, from any replica.

## Routes

| Route | What it does |
|---|---|
| `GET /ask?customer_id=&conversation=&from=&agent=` | the conversation |
| `POST /ask/send` | stores the message, sends one `agent_turn` to the chosen agent with `page.path`, stores the answer |
| `POST /ask/confirm` | runs one stored proposal (`message_id`, `index`) as the person and records the outcome |

## Verify

`HttpResponseIsolation.AskOramaOffersPeoplesAgentsAndSaysWhoIsWorkingOnALongTurn` (lake#451): a
running internal generator is not in *Talk to*; the page waits, then names Ask Orama working; Ask
Orama's turn key delegates to Auto-Suggest, and the person's turn shows the delegation, the
subagent's progress (also published on the workspace's `agent-events` for the asking turn) and its
finish; the answer replaces the working notice. `Workspaces.TalkToOffersOnlyRunningAgentsAPersonUses`
covers the list on its own.

`HttpServer/HttpResponseIsolation.AskOramaAnswersProposesAndRunsOnlyWhatThePersonConfirms`: the link
on every page, a turn carrying the page and a turn key, the proposal shown, nothing created before
confirming, a forged confirm refused, the real one creating the repository as the person, and
*Done* plus *Confirmed* afterwards.
