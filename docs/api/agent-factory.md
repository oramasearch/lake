# The Agent Factory

**Status: implemented 2026-09-27 (lake#233, epic lake#228).** *Build it with the Agent Factory* in
Design → Agents → Describe it to the Agent Factory (`/design/agents/new`; `/train/agents/new` redirects there, lake#653): a conversation with **AgentCreator**
([`oramasearch/agent-creator`](https://github.com/oramasearch/agent-creator)), which asks what it must
and proposes an agent in Studio's shape. **Nothing is created until the person approves.**


**AgentCreator's answer (agent-creator#1).** Its turn answers words a person reads in `reply` (the
question, or the proposal in one sentence) and the structured answer in `result`
(`{kind: question | proposal, question?, proposal?, defects, facts}`), so an API or MCP caller reads
`result` without parsing text. The Factory page reads `result`, falling back to the JSON an older
AgentCreator put in `reply`.

## Skills

What a person's agent may do is the **skills** a person grants it, in the Agent Factory's proposal
or the New agent drawer, and shown on the agent page. agent-runtime decides what each grants
([`skills.py`](https://github.com/oramasearch/agent-runtime/blob/main/src/agent_runtime/skills.py),
agent-runtime#7): reads are tools that call Lake, and changes are proposals a person confirms, as App
changes are. The exception is a few engine actions the engine's own policy governs, which the agent
calls directly: `check_pipeline` and `start_run` (*train*), `generate_dataset` (*generate-data*),
`decide` and `record_outcome` (*call-api*); `approve_decision` stays a proposal (agent-runtime#11,
its `docs/skills/README.md`, *Direct actions*). Every skill names Lake's capabilities, the engine's
under their Lake names ([engine-capabilities.md](engine-capabilities.md)). The workspace engine's
agent record accepts exactly these names.

| Skill | Label | What it reaches |
|---|---|---|
| `search` | Search data | datasets, connections, search indexes (creating or exposing one is a proposal), Lake-wide `search`, the Docs, solution reads |
| `generate-data` | Generate a dataset | dataset generators; `generate_dataset` directly |
| `train` | Train a Model | projects, pipelines, runs, jobs, models, experiments (starting or stopping one is a proposal), solution reads; `check_pipeline` and `start_run` directly |
| `summarize` | Summarize and explain | projects, runs, models, model health and traffic, traces, the workspace's status |
| `call-api` | Call the API | models (`score`, `explain`, `predict`), `search_workspace_index`, exposing a model as a proposal, solution reads; `decide` and `record_outcome` directly, `approve_decision` as a proposal |
| `drives` | Use drives | browse, read, preview, history and search; uploads, folders, moves, deletes as proposals |
| `transcripts` | Use transcripts | list and read; adding, editing, deleting and `transcribe` as proposals |
| `repositories` | Work in repositories | files, refs, changes, issues, reviews, releases, artifacts, environments; issues, comments, reviews and commits as proposals |
| `datasets` | Load datasets | datasets, analysis, feature plans, connections and their tables; loading from a drive or a connection, uploads and new connections as proposals |
| `streams` | Read and publish streams | streams and their messages; publishing and stream changes as proposals |
| `pipelines` | Design pipelines | projects, pipelines, methodologies, runs, Opportunities; creating, editing, checking, approving, running and deploying a pipeline, and composing a solution, as proposals |
| `usage` | Read usage and billing | metered usage, usage over time, limits, the billing account, the price list, invoices |

A run's lake key carries the reads of its agent's skills and nothing else
([agent-launches.md](agent-launches.md#launches)): an agent's own code cannot read what its skills do
not grant. Sign-in, recovery, key and certificate minting, secrets, people and access, and the
generic `workspace_query`/`workspace_act` are never any agent's (agent-runtime's
`NEVER_FOR_AGENTS`); agent-runtime's contract test fails when Lake publishes a capability that is in
no skill and not on that list.

The list is vendored from agent-runtime's `skills.json` by `tools/agents/vendor-agent-skills.py`
(`src/control/agent_skills.cpp` for the keys, `frontend/src/pages/train/agent-skills.json` for the
pages). AgentCreator proposes only these skills.

## The conversation

The conversation is held through `send_ask_message` and `settle_ask_turn` with `kind: factory`
([ask-orama.md](ask-orama.md#capabilities-lake561)); the web app's Agent Factory page calls them.
`send_ask_message` sends one `agent_turn` to `agent-creator` (a turn key limited to the
workspace's reads). AgentCreator answers with one JSON object: a **question**, or a **proposal**
with the **defects** its own rules found (skills, work area, approved Pipelines, code that compiles
and imports only what agent-runtime ships, no secrets, storage that fits the code). The page shows a
proposal as a review panel: type and why, where it is stored and why, work area, manifesto, goal,
skills, trigger, Pipelines, and every file. A proposal with defects offers no approval: the person
tells AgentCreator what to change.

## Approve and create

`approve_agent_proposal` carries the proposal's message
id and the **SHA-256 of the proposal the page showed**: SHA-256 of the proposal's compact JSON as
lake serializes it. `settle_ask_turn` stores that digest with the proposal message as
`payload.proposal_sha256`, and `get_ask_conversation` returns it, so the web app sends it back as
it is: a browser's own serialization differs for numbers such as `2.0` (lake#561). lake recomputes
it from the stored proposal; a proposal that is not in this person's conversation is `not_found`
*That proposal is not in this conversation.*, one that changed since is `conflict` *The proposal
changed since you saw it; review it again.*, and one with defects is `invalid_input` *This
proposal still has defects.* Approval is recorded as `agent.factory.approve` with the digest. Then
lake, as the person, through `create_agent_from_proposal`:

| Proposal | lake creates |
|---|---|
| native | `create_agent` with the Orama base model and the manifesto as system prompt |
| python, stored in Zetta | `create_agent` with `definition {source: zetta, instructions, files, entrypoint}` |
| python, stored in lake | a **private** repository `agent-<name>-<4 hex>`, one commit of the files, then `create_agent` with `definition {source: lake, repository_id, revision: commitOid}` |

`instructions.md` is composed from the name, purpose, manifesto and goal when the proposal carries
none; `entrypoint` comes from `agent.toml`. A Python agent with a trigger is scheduled, and a **first
check run** is launched so the new agent's first run is one the person can read. The outcome is
stored in the conversation (*Created {name}.*, with whether its trigger was scheduled and whether a
first check run started; a refusal stores *I could not create it: …*). The value is `{agent_id,
name, schedule_error?, check_launch_id?}`; the page then opens the agent.

Verify: `HttpServer/HttpResponseIsolation.TheAgentFactoryCreatesOnlyTheExactProposalThePersonApproved`
(the page) and `HttpServer/HttpResponseIsolation.TheWebAppApprovesOnlyTheAgentFactoryProposalItWasShown`
(the capabilities, with a proposal holding `2.0`: the stored digest approves it, a stale digest is
`409`, another message is `404`).
