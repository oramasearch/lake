# Ask Orama

**Status: implemented 2026-09-27 (lake#236, epic lake#228).** Ask Orama is the general agent for all
of lake and the first entry of the Agent Library. Its code is
[`oramasearch/agent-ask-orama`](https://github.com/oramasearch/agent-ask-orama); this page is how lake
shows it and keeps it honest.

## What a person sees

- **Ask Orama** in the top bar of every signed-in page. It opens `/ask?from=<the page>`, so the
  conversation knows where it started.
- `/ask`: the conversation, a **Talk to** selector, and a message box. *Talk to* lists Ask Orama
  first, then every other library chat agent a person uses, then the account's own chat-capable
  agents. Each library agent shows its catalogue name, and under the selector the chosen agent's
  one-sentence purpose with *Where you meet it* to expand (lake#763). A library agent this Lake does
  not run stays in the list, marked *not running on this Lake* and not selectable, rather than
  vanishing. No agent is internal (lake#788): the Eval Generator Agent, the Auto-Suggest Corpus
  Agent and the Project Corpus Agent are offered like the others, and lake accepts Ask Orama's
  `agent_turn` to each of them, so a request for an eval or a training corpus can be handed on.
  Whether Ask Orama does so is decided by its own instructions in `oramasearch/agent-ask-orama`.
  `control::talk_to_agents` builds
  the same list on the server. The account's own agents come from Zetta's `list_agents`, which took
  380–690 ms on the dev site, so the web app adds them to the selector once that read answers, and
  the agent a conversation talks to stays selected meanwhile (lake#446).
  *Start a new
  conversation* begins another. An agent's reply is rendered as markdown (tables, lists, code) through
  lake's one renderer, which escapes every character first, so a reply cannot inject HTML; what the
  person typed is shown as typed.
- **While a turn runs** the conversation says which agent is working (*Ask Orama is working…*, or
  *Waiting for Ask Orama to start…* before an agent claims it), for how long, the turn's latest
  progress message, and the attempt when lake retried it. When Ask Orama brings in another agent,
  lake reports that on the person's turn: *Ask Orama asked Experiments Agent: …*, then each of the
  subagent's progress messages (*Experiments Agent: …*) and *Experiments Agent finished; writing the
  answer.* (lake#451, see [the agent bus](agent-bus.md#delegated-turns)). The page reads this from
  the `get_agent_task` call it already makes; it adds no workspace call. Replies and delegations
  name agents as people call them (*Experiments Agent*, not `agent-experiments`).
- Each of Ask Orama's **proposals** (a change it wants to make) is shown with its capability and its
  impact, and a **Confirm** button. Each argument of the call is three rows, its name, its value,
  then its description, so a value has the card's full width and wraps at any character; a value
  longer than six lines (JSON, a long intent) shows its first lines and *Show all N lines*
  (lake#1192, [below](#what-confirm-runs-lake942)). A confirmed proposal that worked shows *Confirmed* and is never
  offered again. One whose confirm did not work shows the stored *That did not work: …* reason
  under it and never shows *Confirmed* (lake#794). It keeps a **Try again** button only when the
  stored outcome says trying again could work (`retry: true`, a transient refusal); otherwise the
  reason says to ask Ask Orama again and no button is shown.

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
- **It delegates to any agent** (`delegate` → `agent_turn`): the Project Agent, Application Agent,
  Experiments Agent, Auto-Suggest Agent, and any agent the person built; an unattended agent is started by proposing
  `launch_agent`. An Application Agent delegation creates an `app` task, whose proposal is reviewed
  and confirmed before that agent receives App writes. An Experiments Agent delegation creates an
  `experiment` task, whose editable experiment is reviewed before that agent receives the engine's
  `create_study`.
- **Conversations live in the workspace** (lake#370). Every message is a sealed record in the
  workspace Zetta of the workspace the conversation was held in (collection `lake_ask_messages`,
  [workspace-records.md](../architecture/workspace-records.md)): the person's text, and for Ask
  Orama's the reply with its proposals, reads, delegations and trace id. The page's `/w/<id>/`
  workspace owns it; another workspace of the same account never shows it. Only the person who had a
  conversation reads it back, from any replica. A workspace that is not ready shows the error
  instead of falling back anywhere. Lake's PostgreSQL `ask_messages` table (migration `0030`) was copied
  into the workspace Zettas by the operator's one-shot copy and dropped by migration
  `0050_drop_ask_messages` (lake#483).
- **`get_ask_conversation`** takes `customer_id`, `workspace_id` and `conversation_id` (`limit`
  optional) and reads the person's conversation in that workspace; a missing, malformed, unknown or
  foreign `workspace_id` is `invalid_input` at `/workspace_id`. A message whose payload has
  `proposals` also carries `proposal_calls`, Lake's own statement of what each one runs
  ([below](#what-confirm-runs-lake942)).

## How-to answers come from the Docs (lake#817)

A question about using Lake ("How do I load my first dataset?", the home's first suggestion) is
answered from the Docs, never from what an assistant assumes a data product looks like. Ask Orama
reads `list_docs` with the subject's words and then `get_doc` for the best guide (both are public
reads every turn key carries), and its instructions in `oramasearch/agent-ask-orama` say to name
pages, tabs and buttons exactly as the guide and the navigation show them, joined by arrows, to
link each page, and to name only the formats and limits the guide states.

So the guides are written for that reader too. Each page a guide sends a person to is written as
the rail shows it and linked to its path, for example
`[**Data → Datasets → Load data**](/train/data)`; the web app's Markdown opens a lake path in the
current workspace. [Datasets and Loading Data](../guide/load-data.md) starts with *Load Your First
Dataset*: the path, the formats Load data accepts (CSV or JSONL, the `format` values
`upload_dataset` takes) and the other ways in (Generate, From a connection, a drive's Load as
dataset). An earlier answer named a "Lake Console → datasets" page, an "Upload dataset" button and
Parquet, none of which Lake has.

`AskOramaGuidance.TheDocsAnswerTheHomesFirstSuggestionWithTheRealPath` (`tests/test_examples.cpp`)
holds this: the home still asks the question about `/train/data`, the rail shows that path as Data →
Datasets, `list_docs` with `load dataset` finds the guide first, the guide links the path as Data →
Datasets → Load data and names every way in, names each format `upload_dataset` accepts and no other
(Parquet, Excel, Avro, …), and never calls a page "Lake Console".

## Capabilities (lake#561)

The conversation is written only by lake. The web app, and the server-rendered page's forms, go
through four capabilities, so a browser never writes a message and only a proposal lake stored can
be run. Each takes `customer_id` and `workspace_id` (the workspace whose Zetta holds the
conversation) and is audited in that workspace's log.

| Capability | Kind | Input | Value |
|---|---|---|---|
| `send_ask_message` | action | `message`; `conversation_id` (absent: a new one); `kind` `ask` (default) or `factory`; `agent` (default `agent-ask-orama`; the Agent Factory always talks to `agent-creator`, any other `agent` is `invalid_input`); `page` `{path, <query values>}` (Ask Orama only) | `{conversation_id, message_id, task_id?, reply_id?}` |
| `settle_ask_turn` | action | `conversation_id`, `task_id`, `kind` | `{settled, task}` |
| `confirm_ask_proposal` | action | `conversation_id`, `message_id`, `index`, `input_sha256` | `{ok, message_id, capability, error?}` |
| `approve_agent_proposal` | action | `conversation_id`, `message_id`, `proposal_sha256` | `{agent_id, name, schedule_error?, check_launch_id?}` ([agent-factory.md](agent-factory.md)) |

- **`send_ask_message`** stores the person's message first; a message the workspace did not store
  is an error and nothing is asked. It then calls `agent_turn`. A library agent answers in the
  background: the value carries `task_id`. An agent of the account answers at once: its reply is
  stored and `reply_id` names it.
- **`settle_ask_turn`** reads the turn as `get_agent_task` does (`task` is that value). While the
  turn is `pending` or `running`, `settled` is false and nothing is written. Once it finished, the
  agent's message is stored exactly once, under an id derived from the task, so two calls (or the
  page and the web app together) store one message. Ask Orama's message is `answer.reply` with
  `{task_id, agent, proposals, reads, delegations, trace_id}`; Agent Creator Agent's is its question,
  *Here is the agent I propose. Review it below.* or its reply, with `{task_id, agent, answer,
  proposal_sha256?}`; a failed turn is *That did not finish: …*. A task of another conversation is
  `invalid_input` at `/task_id`. The web app polls `get_agent_task` every 2 s and calls this once
  the status is final, then rereads the conversation with `get_ask_conversation`.
- **A proposal is checked before it is offered** (lake#794). When `settle_ask_turn` (or
  `send_ask_message`, for an agent that answers at once) stores an answer, each proposal's input is
  checked against its capability's input schema with the registry's own `canonical_input` and
  `validate_input`, the checks `Registry::call` runs on every call, after lake fills
  `customer_id` and `workspace_id` from the conversation as a confirm does (a field the input
  leaves out, or writes as `null` or `""`, is filled; one it names is kept). A proposal the registry
  would refuse (an unknown capability, a missing required field, a wrong type or value) is not
  offered: it moves from `proposals` to `withheld_proposals`, each with `error {code, message,
  path, missing?}`, and the reply gains a plain sentence per proposal that says what is missing and
  asks for it, for example *I could not offer this change yet: create_index needs `table` (the
  source table to index), which this proposal does not name. Tell me what to use and I will propose
  it again.* A proposal missing several required fields names **every** one in one sentence
  (*create_stream needs `zetta` and `topic`, …*), listed in `error.missing`, so one reply settles it;
  `validate_input` itself still stops at the first. Only the proposals that can run keep an index
  for `confirm_ask_proposal`. Nothing is called for these checks. A check that needs the data (a
  topic already taken, a system stream) still happens only when the person confirms, except the
  project check below.
- **A proposal for a project that does not exist is never offered** (lake#1193). After the schema
  check, every proposal whose capability works on an existing project (`compile_intent`,
  `capture_intent`, `compile_n8n`, `compile_python`, `import_n8n`, `save_pipeline`,
  `check_pipeline`, `start_run`, `deploy_pipeline`, `approve_pipeline`, `compile_pipeline`,
  `run_pipeline`, `edit_pipeline`, `create_pipeline`, or one of the engine's names sent through
  `workspace_act`) has its `project` read once with the engine's `get_project`, as the person. A
  project the engine answers `not_found` for withholds the proposal (`error {code: "not_found",
  path: "/project"}`) with *I could not offer this change: there is no project named "…" in this
  workspace, so compile_intent could not run. To build it in a new project, I can propose
  compile_pipeline with `create_project`, which creates the project and its pipeline in one
  confirmation.* A `compile_pipeline` with `create_project` for a project that already exists is
  withheld the other way (`conflict` at `/create_project`). A read that settles nothing (the
  workspace not reachable) leaves the proposal offered, and confirm still checks. Only reads are
  made.
- **Each turn hears what became of the conversation's proposals** (lake#1194). `send_ask_message`
  reads the conversation and hands Ask Orama's turn `context.proposals`: the last ten proposals,
  oldest first, each `{message_id, index?, capability, input, state}` with `state` `pending`
  (offered, not confirmed), `confirmed`, `failed` (with the stored `error`, its `code` and
  `retry`) or `withheld` (with the reason). `agent_turn` passes an Ask Orama turn's `context`
  through. It is information, never authority: the turn's key still carries no action. So after a
  failed confirm, "create it then" is answered with a new proposal that corrects the error, never
  with *confirm the proposal in Lake* about the one that failed ([agent-ask-orama](https://github.com/oramasearch/agent-ask-orama)
  reads it). A conversation that cannot be read sends no `context`, and the turn still runs.
- **The answer is the agent's writing, read by type.** A proposal whose `capability` is absent,
  null or a number is withheld (*a proposed change does not say which capability it runs.*, at
  `/capability`); a `reply` or `trace_id` that is not text is stored as empty. Neither ever fails the
  turn: the reply and every proposal that can run are still stored.
- **`confirm_ask_proposal`** takes `payload.proposals[index]` of that stored message (anything else
  is `not_found` *That proposal is not in this conversation.*), checks `input_sha256` against the
  call Lake states for it ([What Confirm runs](#what-confirm-runs-lake942)), calls its capability
  with exactly that input as the person (in the conversation's account and workspace when the input
  names neither, lake#652), and stores *Done: {capability}.* or *That did not work: {error}* with
  `{agent: "agent-ask-orama", confirmed: {message_id, index}, ok}`. The value's `message_id` is
  that outcome message. A capability that refuses is still a successful confirm with `ok: false`,
  and then both the value and the stored payload carry the refusal's code and whether trying the
  same proposal again could work: the value `{error, code, retry}`, the payload `{error: {code,
  message}, retry}`. `retry` is true only for a refusal that says nothing about the input and may
  pass later: `store_unavailable`, `workspace_not_ready`, `busy`, `rate_limited`, `locked`,
  `store_error` and `io` ([errors.md](errors.md)). Every other refusal (`invalid_input`,
  `conflict`, `not_found`, `unauthorized`, `engine_error`, …) fails the same way every time, since
  the stored input never changes, so its `error` ends *Ask Orama again with what to use and it will
  propose the change again.* and the web app offers no Try again. When it refused the input
  (`invalid_input` at a field), `error` says so in plain words: *`topic` is not valid for
  create_stream: a concrete topic name is required. Ask Orama again with what to use and it will
  propose the change again.*
  The call's own audit event carries `{ask_confirm: true, conversation_id, capability,
  input_sha256, ok}`; the proposal's capability records its own event.

### What Confirm runs (lake#942)

A proposal's `impact` is the model's writing, and content the model read can steer it: an
invitation of an outside address as an owner can be described as *Fixes sharing for your team*.
Confirm runs under the person's own permissions, so the person must see the exact call, not the
model's account of it.

- **`get_ask_conversation` answers `proposal_calls`** beside `payload`, one entry per
  `payload.proposals` entry in order. It is Lake's, computed when the conversation is read, never
  stored and never taken from the model: `{capability, title, kind, input, fields, input_sha256}`,
  where `title` and `kind` are the registry's, `input` is the exact input confirm runs (the stored
  input with `customer_id` and `workspace_id` filled from the conversation and deprecated field
  names renamed, as `Registry::call` would), `fields` is that input field by field
  `{name, description, value}` with the capability schema's own description, and `input_sha256`
  is the SHA-256 of `{"capability", "input"}`. An entry is `null` when Lake cannot state the call:
  the capability no longer exists, or the input is not an object or uses a deprecated name beside
  its current one.
- **Confirm names the call it was shown.** `confirm_ask_proposal` requires `input_sha256`; Lake
  recomputes the call from the stored proposal and runs it only when the digest matches. Another
  digest is `conflict` at `/input_sha256` (*This is not the call you were shown; read the
  conversation again and review it.*), a proposal without a call is `invalid_input` at `/index`,
  and in both cases nothing runs and no outcome is stored. A missing `input_sha256` is
  `invalid_input`; this is a breaking change for API and MCP callers, which read
  `get_ask_conversation` first.
- **The web app shows the call and keeps the model's words apart.** Each proposal shows *Confirm
  runs `<capability>` (<title>, a change) with exactly:* and every field as three rows, its name,
  its value, then its description, then the impact under *Ask Orama's explanation, not checked by
  Lake*. The rows replace a two-column grid whose value column collapsed to one character per line
  beside a long description (lake#1192): a value has the card's full width and wraps at any
  character, a value of several lines or over 80 characters is a wrapped block, and one longer than
  six lines shows its first six with *Show all N lines*; the whole value is always in the page
  (`ProposalCallView` in `frontend/src/pages/ask/proposalCard.ts`). Values are written as
  JSON, so a string keeps its quotes and a leading or trailing blank shows, and every control,
  format or bidirectional character and every whitespace character other than the plain blank
  (a right-to-left override `U+202E`, a zero-width `U+200B`) is written as `\u{…}`. A
  proposal whose entry is `null`, or in a shape the page does not know, shows that it cannot be
  confirmed and has no Confirm (`proposalCall` and `shownValue` in
  `frontend/src/pages/ask/home.ts`).

For example, the web app's `send_ask_message` (`POST /w/<workspace_id>/v1/send_ask_message`):

```json
{"customer_id": "8f1c…", "workspace_id": "4b2e…", "message": "What do I have?", "page": {"path": "/design/project", "name": "churn"}}
```

## A new project in one confirmation (lake#1193)

`compile_intent` works only on a project that exists, so a plan for a new project used to need two
proposals, or failed on confirm. A new project's pipeline is one proposal instead:
`compile_pipeline` with `create_project: true` ([web-app.md](web-app.md#compile_pipeline)) creates
the project, described by the intent, then captures the intent and compiles it (with
`methodology`, that methodology's own pipeline, and with `dataset`, pointed at it). One
confirmation runs both steps. A project that already has the name is refused as `conflict` before
anything is written; a compile that fails after the project was created says the project was
created. Ask Orama proposes this for a new project (its prompt v15), for example:

```json
{"capability": "compile_pipeline", "input": {"project": "Customer Segmentation EDA Dashboard", "intent": "Segment customers from datasets.ux_review_20261018_customers", "dataset": "datasets.ux_review_20261018_customers", "methodology": "M-3", "create_project": true}}
```

The other way, `create_project` then `compile_intent` as two proposals, was not chosen: the second
would be withheld until the first was confirmed, and two confirmations for one plan is the failure
this replaces.

## Deleting a conversation

`delete_ask_conversation {customer_id, workspace_id, conversation_id, dry_run?}` (lake#645)
deletes one of the caller's conversations in that workspace: every message record is removed from
the workspace Zetta by key. Only the person who had it can, because a conversation is found by its
id and its person; another person's id finds nothing (`not_found` at `/conversation_id`).

The agent tasks the conversation started go with it (lake#673): the person's turns in it (kind
`ask` or `factory`, any agent) and every task those turns' agents asked for in turn
(`parent_task_id`, lake#451), such as Auto-Suggest Agent working for Ask Orama. Each task's record in
`lake_agent_tasks` (its request, which repeats the person's message, and the agent's answer) is
removed from the workspace Zetta, then its content-free dispatch row (`agent_tasks`, with its
`agent_task_status`) from PostgreSQL, so `get_agent_task` answers `not_found` for it afterwards.
The records go first, so a deletion that fails part way is finished by calling it again, and are
removed once more after the rows, for an event an agent recorded in between. An agent still working
on a removed task is refused its next event (`not_found`). Tasks of other kinds that name the
conversation (a project's `build:<project>`) are not the conversation's and stay. A task whose
record is missing still answers `get_agent_task`, with an empty request and no answer.

The answer is `{conversation_id, messages, first_message, agent_tasks: [{task_id, agent, kind,
status, created_at}], dry_run, deleted}`; `dry_run: true` removes nothing and lists the tasks that
would go. A conversation whose messages are gone but whose tasks are left (a deletion that stopped
part way) is still found. The event goes to the workspace's audit chain, with the counts. Lake Console offers it beside New conversation
([../ui/deletion.md](../ui/deletion.md)).

## Routes

| Route | What it does |
|---|---|
| the web app's `/ask?customer_id=&conversation=&from=&agent=` | the conversation; with `task=`, it calls `settle_ask_turn`; sending calls `send_ask_message` with the page's `from` as `page`, and confirming calls `confirm_ask_proposal` (the server forms were retired in lake#561) |

## Verify

`Stores/WorkspaceRecordStores.*` (lake#370): two accounts with two workspaces each keep one
conversation id apart per workspace; foreign, missing, malformed and unknown workspace ids are
refused; a deleted workspace's conversations go with it; the operator copy moves and verifies
earlier rows.

`HttpResponseIsolation.AskOramaSaysWhoIsWorkingOnALongTurn` (lake#451): `settle_ask_turn`'s task
is pending, then running with Ask Orama's message; Ask Orama's turn key delegates to the Auto-Suggest Agent,
and the person's turn carries the delegation, the subagent's progress (also published on the
workspace's `agent-events` for the asking turn) and its finish as the task's message, until the
turn settles with the answer. `Workspaces.TalkToOffersEveryAgentAPersonUsesAndMarksTheOnesNotRunning` covers which
agents *Talk to* offers.

`HttpServer/HttpResponseIsolation.AskOramaOffersOnlyProposalsTheCapabilitySchemaAccepts`
(lake#794): an answer proposing `create_index` without `table` and `create_stream` without
`zetta` is stored with neither offered, both in `withheld_proposals` at `/table` and `/zetta`, and
a reply that names each missing field and asks for it; a valid proposal refused when it runs (a
Lake bus stream) is stored as *That did not work: …* with `ok: false`, and one whose topic the
capability refuses names the field in plain words; both refusals are stored with their code and
`retry: false`. The web app's `confirmOutcomes` (`frontend/tests/home.test.ts`) shows only a
confirm that worked as *Confirmed*, and offers Try again only for an outcome with `retry` true.

`HttpServer/HttpResponseIsolation.AskOramaKeepsTheTurnWhenAProposalIsMalformed` (lake#794): an
answer with a null and a numeric `capability`, a `create_stream` with no input, and a
`create_repository` naming only its `name` is stored, on the background path and on the at-once
path (an agent of the workspace answering in `send_ask_message`), with the reply, the repository
proposal offered and the other three withheld; the stream's sentence names both `zetta` and
`topic`. Confirming the offered proposal creates the repository in the conversation's account and
workspace (lake filled `customer_id` and `workspace_id`); on the at-once path the repository
proposal writes `customer_id: null`, which lake fills the same way. A null `reply` or `trace_id` is stored as
empty. `HttpServer/HttpResponseIsolation.AskOramaOffersTryAgainOnlyForATransientRefusal`: a confirm
refused with `store_unavailable` is stored with `retry: true` and no *Ask Orama again*.

`HttpServer/HttpResponseIsolation.AskOramaConfirmShowsAndRunsOnlyTheExactCall` (lake#942): a
`create_invitation` proposal for an outside address as `owner`, described by its impact as a sharing
fix, is answered with `proposal_calls` naming the exact recipient, role and organization; a confirm
without `input_sha256` (400) or with another digest (409) creates no invitation and stores no
outcome; the confirm with the shown digest creates the invitation with exactly the values shown.
`frontend/tests/home.test.ts` checks that the page offers no Confirm without Lake's call and that a
shown value cannot hide characters.

`HttpServer/HttpResponseIsolation.AskOramaNeverOffersAProposalForAMissingProject` (lake#1193): an
answer proposing `compile_intent` on a project the workspace does not have, `compile_pipeline` with
`create_project` for one it has, and `compile_pipeline` with `create_project` for a new one is
stored with only the last offered, the first two withheld at `/project` (`not_found`) and
`/create_project` (`conflict`) with sentences that say what to do, and only `get_project` read to
check; confirming the offered one creates the project, then compiles its pipeline, and stores
*Done: compile_pipeline.*; `compile_pipeline` called directly with `create_project` on an existing
project is 409 and creates nothing.

`HttpServer/HttpResponseIsolation.AskOramaHearsWhatBecameOfItsEarlierProposals` (lake#1194): a first
turn carries no `context`; a `compile_intent` proposal whose project is deleted before the confirm
fails with `not_found`; the next turn ("create it then") carries `context.proposals` with that
proposal `failed`, its code, `retry: false` and the error; the corrected `compile_pipeline` with
`create_project` it answers is offered and confirms; the turn after hears the failed proposal and
the confirmed correction.

`frontend/tests/home.test.ts` *the confirm card gives every value its own full-width row*
(lake#1192) renders the card with a UUID, a long sentence and a JSON object: no `dl`, three rows of
name, value and description, the sentence a whole wrapped block, the JSON laid out with every line
in the page and *Show all N lines*, and the stylesheet's wrapping rules.

`HttpServer/HttpResponseIsolation.TheWebAppTalksToAskOramaThroughCapabilities` (lake#561): the same
conversation over `/v1` with the browser session: `send_ask_message` with a page, `settle_ask_turn`
writing nothing while the turn waits and the answer once over two calls, a forged
`confirm_ask_proposal` refused, cross-site requests refused, and the real confirm creating the
repository with its outcome stored.
