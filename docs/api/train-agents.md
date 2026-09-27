# Train → Agents and the Agent Library

**Status: implemented 2026-09-27 (lake#229, epic lake#228).** The first Train page. It follows the
Orama Studio design (orama-design `src/studio/studioAgents.js`, `AgentListScreen`): **conversation
happens in Ask Orama; Train → Agents is where people build agents and read what the unattended
ones did.** Keeping the two apart stops autonomous work being dressed up as a chat.

## What a person sees

`/train/agents` has three parts:

| Part | What it is |
|---|---|
| **Your Agents** | The agents people built in this workspace, from the workspace's `list_agents` (records with `kind: orama` are left out). Columns follow Studio: Agent (name and purpose), Engagement (*Conversation*, or *Conversation and unattended* when it has a trigger), Skills, Model, Status. Each links to its page. Needs a ready workspace |
| **Create an Agent** | Studio's boxes: name, purpose, **work area** (this workspace, a single project, specific data, plus which), **manifesto**, **goal**, **skills** (*Search data*, *Generate a dataset*, *Train a Model*, *Summarize and explain*, *Call the API*; granted, never assumed), **model**, and **trigger or schedule** (empty means conversation only). Posts to `/train/agents/create` |
| **Agent Library** | Every specialized agent Orama ships, from `list_agent_library`: what it does, where a person meets it, how it runs, and whether this lake runs it. Shown even before the workspace is ready, because the library is lake's, not the workspace's |

The model choice decides where the agent runs:

- **Orama base model** makes a **native** agent: `runtime: native`, `model` is the base-agent
  checkpoint inside the workspace (`LAKE_BASE_AGENT_MODEL`, default
  `/var/lib/zetta/models/base-agent`), `model_name` *Orama base model*, and the manifesto as its
  system prompt. It runs in the workspace's Zetta, on the GPU when there is one and on CPU when not.
- **Frontier model** makes a **Python** agent: `runtime: python`, `model_name` is
  `LAKE_AGENT_MODEL` (default `gpt-4o`), and a `definition` stored in Zetta (`source: zetta`) whose
  instructions are composed from the name, purpose, manifesto and goal. It runs on
  `agent-runtime` with the runtime's own tool loop (an empty `entrypoint`).

**Pipelines as tools** (lake#238): the agent page lists the Pipelines granted to the agent, each
with *Revoke*, and the workspace's **approved** Pipelines not yet granted, each with *Grant*. A grant
or revoke writes the whole list back through `update_agent`, so Zetta checks every grant is still
approved and pins each to its approval (`version`). agent-runtime turns each grant into a
`pipeline_<graph>` tool that gates, runs and reports; Ask Orama can run any approved Pipeline the
person may run, as a confirmed proposal.

*Build it with the Agent Factory* opens [the Agent Factory](agent-factory.md).

`/train/agent?id=` shows the agent's definition (purpose, engagement, work area, skills, model,
where it runs, manifesto, goal, trigger, status), the **Pipelines** granted to it as tools, its
**Unattended Runs**, its training status for a native agent (`agent_training_status`), and
**Delete Agent**, which needs the agent's name typed.

## The Agent Library

`control::agent_library` (`include/lake/control/agent_library.hpp`) is the catalog, in this order:

| Key (repository) | Name | Where a person meets it | Runs as |
|---|---|---|---|
| `agent-ask-orama` | Ask Orama | The Ask Orama panel on every page; delegates to any agent | its own service |
| `agent-project` | Project Agent | Design: Create a Solution and revised plans; Ask Orama's subagent | its own service |
| `agent-autosuggest` | Auto-Suggest | Design: Discover Opportunities; Ask Orama's subagent | its own service |
| `agent-creator` | AgentCreator | Train: Agents, Create with the Agent Factory | `agent-runtime` |
| `agent-eval-generator` | Eval Generator | Offline: how Orama measures its agents | offline |
| `agent-autosuggest-corpus` | Auto-Suggest Corpus Generator | Offline: training data for Auto-Suggest | offline |
| `agent-project-corpus` | Project Corpus Generator | Offline: training data for the Project Agent | offline |

Status is *Available* when the operator lists the agent in `LAKE_AGENT_ENDPOINTS`, *Not running on
this lake* otherwise, and *Offline tool* for the three agents that produce evaluations and
training data rather than serving people. People do not create or delete library agents.

## Capabilities

| Capability | Kind | Authorization | What it does |
|---|---|---|---|
| `list_agent_library` | read | any signed-in principal | The catalog above with `deployed` and `status` per agent |
| `workspace_query` → `list_agents`, `get_agent`, `agent_training_status` | read | `read` on the customer | The workspace's agents |
| `workspace_act` → `create_agent`, `update_agent`, `delete_agent`, `set_training_schedule`, `agent_run`, `chat` | action | `write` on the customer | Build, change, run and talk to agents |

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
ctest --test-dir build -R 'Workspaces.Agent|Workspaces.TheAgentLibrary|CreateAnAgent'
```

`Workspaces.AgentCallsAreAllowListedAndChatIsAWriteZettaTakesWithGet`,
`Workspaces.TheAgentLibraryListsEverySpecializedAgentAndWhatRuns`,
`Workspaces.AgentsPageShowsYourAgentsTheStudioFormAndTheAgentLibrary`,
`Workspaces.AgentPageShowsTheDefinitionPipelinesRunsAndATypedDelete` and
`HttpServer/HttpResponseIsolation.CreateAnAgentBuildsANativeOrAPythonAgentFromStudiosBoxes`.

## Change it

A new specialized agent is a row in `control::agent_library` plus its key in the operator's
`LAKE_AGENT_ENDPOINTS`. A new Studio box is a field on Zetta's `AgentRecord`
(zetta `docs/agent/README.md`), the form in `agent_create_form`, and the mapping in the
`agent_create` form action.
