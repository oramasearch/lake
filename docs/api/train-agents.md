# Agents: designed in Design → Agents, trained in Train → Agents

**Status: implemented 2026-09-27 (lake#229, epic lake#228); split into Design and Train 2026-10-11
(lake#653, not deployed).** Steve, 2026-10-11: *"the Agents section should be able to create agents
in the Design section (like solutions do; rename agent library to just agents, and allow agents to
be created (designed) there) ... the Train > Agents page should be dedicated to training
model-based agents and improving them and their performance metrics, launching experiments,
etc..."* Conversation still happens in Ask Orama.

| Where | What it is for |
|---|---|
| **Design → Agents** (`/design/agents`, was *Agent Library*; `/design/agent-library` redirects) | Designing agents: Orama's beside the workspace's own, New agent, the Agent Factory, an agent's definition and its revisions |
| **Train → Agents** (`/train/agents`) | Training and improving the agents that learn: performance over time, trained versions, the training schedule, experiments, promote and roll back |
| **Train → Agent** (`/train/agent?id=`) | One agent's training page, and where it runs: its unattended runs (*Run now*) and schedules, as before |

The pages call Lake's named engine capabilities (`list_agents`, `get_agent`, `create_agent`,
`update_agent`, `delete_agent`, `agent_training_status`, `set_training_schedule`,
[lake#650](engine-capabilities.md)), not `workspace_query`/`workspace_act`.

## Design → Agents

It works the way Design → Solutions does ([solutions](solutions.md), lake#597):

- **Orama's agents** are the [Agent Library](#the-agent-library) (`list_agent_library`), marked
  **By Orama** and **read-only**. Their page (`/design/agent?key=<key>`) says what each does,
  where a person meets it and whether this Lake runs it, and offers **Duplicate to edit**: New
  agent, prefilled from the library entry (its name with *(copy)*, its purpose), for the person to
  finish and create as their own. The library holds no prompt, so the copy's instructions start
  from its purpose. An engine record of kind `orama` is Orama's too, and read-only.
- **The workspace's own agents** (`list_agents`, less records of kind `orama`) are listed beside
  them, marked **Yours**. The toolbar filters **By Orama** or **Yours** and searches names,
  purposes and where an agent is used.
- **New agent** designs one in a drawer; **Describe it to the Agent Factory**
  (`/design/agents/new`, [agent-factory](agent-factory.md); `/train/agents/new` redirects) is the
  other way in, where AgentCreator drafts it from a description.

The definition a person designs, in New agent and Edit:

| Field | Record field |
|---|---|
| Name, What it's for | `name`, `purpose` (and `description`) |
| Instructions (its prompt) | `manifesto`; `system_prompt` for a native agent; composed into a Python agent's `definition.instructions` |
| What success looks like | `goal` |
| Skills | `skills`: whatever agent-runtime offers, from its `skills.json` vendored into `frontend/src/pages/train/agent-skills.json` by `tools/agents/vendor-agent-skills.py` (agent-runtime#7; the twelve and what each grants are in [agent-factory](agent-factory.md)), never a list written in the page; the engine's `create_agent` schema accepts each (a test checks it). A skill the runtime no longer lists that the agent has is kept and shown by its own name |
| Model | Orama base model (`runtime: native`; an empty `model` is filled with the base checkpoint by Lake, on `create_agent` and now also on `update_agent`), a model trained in this workspace (`model:<id>`, from `list_models`), or a frontier model (`runtime: python`, run on agent-runtime) |
| Pipelines it may use | `pipelines`: approved pipelines only (`list_projects`); the engine pins each grant to its approval |
| Where it works | `work_area` `{kind, detail}` |
| Runs on its own | `trigger`; a frontier-model agent's trigger also becomes Lake's schedule (`schedule_agent`) |
| Starts on new messages in | Not an engine field: Lake's stream triggers (`set_agent_streams`, lake#677). The drawer offers the workspace Zetta's own streams from `list_streams` (no system stream, no Lake bus stream) for a frontier-model agent; Edit opens with `list_agent_streams` and sends `set_agent_streams` only when the set changed, and `[]` when the agent moves off a frontier model. A refused set is said beside the saved agent, which is kept ([agent-launches](agent-launches.md#stream-triggers)) |

An agent's Design page (`/design/agent?id=`) shows its definition and **where it is used**: the
pipelines it calls, Lake's schedules for it, the streams that start it (`list_agent_streams`, each
linked to its stream page and with who set it), its recent unattended runs, and Ask Orama. Its
actions are **Edit** (a new revision), **Duplicate** (`create_agent` with its definition under a
new name; streams are not copied), **Delete** (typed name; its schedules go with it, and
`delete_agent` removes its streams, answering `streams_removed`; its revisions are kept), and **Run
it**, which opens its Train page. Running an agent stays where it was.

### Definition revisions

The engine keeps one record per agent and overwrites it on every `update_agent`. Lake keeps each
definition it wrote: after the engine accepts a `create_agent` or `update_agent`, through the
named capability, `workspace_act` or an agent's turn, Lake appends one sealed record to the
collection `lake_agent_revisions` of the agent's workspace Zetta
([workspace records](../architecture/workspace-records.md)), found by the account and the agent's id
(`control::AgentRevision`, `include/lake/control/agent_revisions.hpp`). Its body is the fields a
person designs (`agent_definition_of`: everything `create_agent` takes except `status`), who made
it, and `create` or `update`. The answer carries `definition_revision`: `{recorded: true, id,
created_at}`, or `{recorded: false, message}` when the record could not be written. The change has
happened either way, so that is never the call's failure.

`list_agent_revisions` (read on the account) answers them newest first, each numbered from the first,
with who made it (`created_by_name`) and the definition it set. **Restore** on the Design page is
`update_agent` with that definition, kept as a new revision. Revisions are not the engine's
**trained versions** (`agent_training_status`'s `versions`, the checkpoints training produced),
which Train → Agents promotes and rolls back; a promote or roll back is an `update_agent`, so it is
also a revision.

## Train → Agents

Train → Agents lists only the workspace's agents with something to train
(`trainable`, `frontend/src/lib/agents.ts`): a native agent with a model the engine trains, an
agent granted a pipeline, or one with a training schedule other than *manual*. A frontier-model
agent with no pipeline is not listed, and the page says why when none is left. Each card reads
`agent_training_status`: stale, due, its versions and its best held-out score.

An agent's training page (`/train/agent?id=`, the **Training** tab) shows:

- **Performance:** the held-out score of the version it runs (or the best), the number of trained
  versions, the task success rate and median latency of its recent unattended runs
  (`list_agent_launches`), its cost as model tokens this month (`usage_over_time`, `by_agent`), and
  a chart of the held-out score of each evaluated version over time.
- **Training runs and their data:** every version training produced, newest first, with when, its
  checkpoint, the corpus it learned from, its held-out score and the job that made it.
- **Promote** a newer version and **Roll back** to an older one, each confirmed: `update_agent`
  with `model` set to the version's checkpoint and `model_version` to when it was trained.
- **The pipelines it calls**, each linked to its pipeline and runs.
- **Launch experiment:** what to compare (its trained versions, variations of its instructions, or
  the models it could run on) and what it should improve, sent to ExperimentsAgent as an
  `agent_turn` of kind `experiment` with `context.agent_id`. The proposal opens on Train →
  Experiments, where nothing starts until it is confirmed ([experiments](experiments.md)). The
  experiments that name the agent are listed.
- **The training schedule** (`set_training_schedule`): when it retrains (manual, on change, daily,
  weekly, monthly), its training data, what it starts from, and its steps.

Design-time editing is not on this page; **Edit its design** opens Design → Agent. The
**Unattended Runs** and **Schedules** tabs are as before ([agent launches](agent-launches.md)).

## The Agent Library

`control::agent_library` (`include/lake/control/agent_library.hpp`) is the catalog, in this order:

| Key (repository) | Name | Where a person meets it | Runs as |
|---|---|---|---|
| `agent-ask-orama` | Ask Orama | The Ask Orama panel on every page; delegates to any agent | its own service |
| `agent-project` | Project Agent | Design: Create a Solution and revised plans; Ask Orama's subagent | its own service |
| `agent-application` | ApplicationAgent | Ship → Apps: Build an App; Ask Orama's app delegate | its own service |
| `agent-experiments` | ExperimentsAgent | Train → Experiments: Run an Experiment; Ask Orama and Project Agent training delegates | its own service |
| `agent-autosuggest` | Auto-Suggest | Design: Discover Opportunities; Ask Orama's subagent | its own service |
| `agent-creator` | AgentCreator | Design → Agents: Describe it to the Agent Factory | `agent-runtime` |
| `agent-eval-generator` | Eval Generator | Internal: Orama's own agent tests. A realistic test case on request (a role, their problem, real-looking data), with its own invented tables; publishes nothing (lake#263) | service |
| `agent-autosuggest-corpus` | Auto-Suggest Corpus Generator | Internal: Orama's own agent tests. One example of what is worth building for a situation you describe, checked against the test cases; writes nothing to the corpus (lake#263) | service |
| `agent-project-corpus` | Project Corpus Generator | Internal: Orama's own agent tests. A worked example of building or changing a pipeline on your own data, replayed against your workspace; changes nothing (lake#263) | service |

The three generators carry `internal: true` (lake#451), and `list_agent_library` leaves them out
unless asked with `include_internal: true` (lake#594), so neither the Design → Agents page, nor an API or
MCP caller, nor Ask Orama's *Talk to* offers them by default: they are tooling for Orama's
evaluations, not agents a customer uses. With `include_internal`, Design → Agents badges them
*Orama internal*. `agent_turn` still reaches them by key for that tooling.

Status is *Available* when the operator lists the agent in `LAKE_AGENT_ENDPOINTS`, *Not running on
this lake* otherwise, and *Offline tool* for the three agents that produce evaluations and
training data rather than serving people. People do not create or delete library agents.

ExperimentsAgent is deployed from `oramasearch/agent-experiments:f5be471`. Lake gives it the
independent `agent-experiments` bus topic and credential. It runs as a chat-capable service; its
experiment records and trial data stay in the selected workspace Zetta.

Each of Orama's agents on Design → Agents says what it does for the person and where to use it,
in plain words, with a badge (*Available*, *Not available here*). Every library agent takes a
conversation (lake#263: the three generators were offline tools until 2026-09-27); the three
internal generators are not offered in Ask Orama (lake#451). The definition form's fields are in
[Design → Agents](#design--agents).

## Capabilities

| Capability | Kind | Authorization | What it does |
|---|---|---|---|
| `list_agent_library` | read | any signed-in principal | The catalog above with `deployed` and `status` per agent; the internal generators only with `include_internal: true` |
| `list_agents`, `get_agent`, `agent_training_status` (also through `workspace_query`) | read | `read` on the customer | The workspace's agents and how each one's training stands |
| `create_agent`, `update_agent`, `delete_agent`, `set_training_schedule`, `agent_run`, `chat_with_agent` (also through `workspace_act`, `chat` there) | action | `write` on the customer | Design, change, train, run and talk to agents; `create_agent` and `update_agent` answer `definition_revision` |
| `list_agent_revisions` | read | `read` on the customer | `{customer_id, workspace_id, agent_id, limit?}`: an agent's [definition revisions](#definition-revisions), newest first |
| `set_agent_streams`, `list_agent_streams` | action, read | `write`, `read` on the customer | The workspace streams that start an agent ([agent-launches](agent-launches.md#stream-triggers), lake#677) |

`chat` is a read in Zetta, which takes it with `GET`, but it loads a model and spends compute, so
lake allows it only through `workspace_act` (`write`) and calls it with `GET`
(`workspace_uses_get`). The same holds on the agent path `/workspaces/<customer>/zetta/v1/chat`.

## Configuration

| Variable | Set by | Meaning |
|---|---|---|
| `LAKE_AGENT_ENDPOINTS` | operator | `key=url` pairs for the library agents this lake runs, e.g. `agent-ask-orama=http://agent-ask-orama.lake:8080` |
| `LAKE_BASE_AGENT_MODEL` | operator | The base-agent checkpoint path inside a workspace |
| `LAKE_AGENT_MODEL` | operator | The frontier model new Python agents run on |

## Verify

```sh
ctest --test-dir build -R 'Workspaces.Agent|Workspaces.TheAgentLibrary|Workspaces.EveryAgentDefinition|CreateAgent'
cd frontend && npm test
```

`Workspaces.AgentCallsAreAllowListedAndChatIsAWriteZettaTakesWithGet`,
`Workspaces.TheAgentLibraryListsEverySpecializedAgentAndWhatRuns`,
`Workspaces.EveryAgentDefinitionIsKeptAsARevision` (revisions through both paths, sealed, newest
first, the base model on an update, and an unrecorded revision that does not fail the change) and
`AgentRuns.CreateAgentRefusesDefinitionFieldsZettaWouldDrop`. `frontend/tests/agents.test.ts`
covers the read-only rule for Orama's agents, the By Orama / Yours filter, Duplicate of each kind,
New agent's and Edit's input, the skills vendored from agent-runtime, revision changes, the filter to
trainable agents, promote and roll back, and the streams the form offers and sends (lake#677). `frontend/checks/agent-streams.mjs` screenshots the Design page's streams, Edit and New agent at
1440 and 390 px against mocked reads.

## Change it

A new specialized agent is a row in `control::agent_library` plus its key in the operator's
`LAKE_AGENT_ENDPOINTS`. A new field of the definition is a field on Zetta's `AgentRecord`
(zetta `docs/agent/README.md`), vendored into `src/api/engine_openapi.json`, then
`agent_definition_of` (so revisions keep it), and `AgentForm`/`agentInput` in
`frontend/src/lib/agents.ts` with its field in `AgentFields` (`frontend/src/pages/train/agents.tsx`).
A new skill needs no change here: vendoring agent-runtime's `skills.json` again adds it to the pages.
