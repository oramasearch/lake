# The Agent Factory

**Status: implemented 2026-09-27 (lake#233, epic lake#228).** *Build it with the Agent Factory* in
Train → Agents → Create an Agent (`/train/agents/new`): a conversation with **AgentCreator**
([`oramasearch/agent-creator`](https://github.com/oramasearch/agent-creator)), which asks what it must
and proposes an agent in Studio's shape. **Nothing is created until the person approves.**

## The conversation

`POST /train/agents/new/send` sends one `agent_turn` to `agent-creator` (a turn key limited to the
workspace's reads). AgentCreator answers with one JSON object: a **question**, or a **proposal**
with the **defects** its own rules found (skills, work area, approved Pipelines, code that compiles
and imports only what agent-runtime ships, no secrets, storage that fits the code). The page shows a
proposal as a review panel: type and why, where it is stored and why, work area, manifesto, goal,
skills, trigger, Pipelines, and every file. A proposal with defects offers no approval: the person
tells AgentCreator what to change.

## Approve and create

`POST /train/agents/new/approve` carries the proposal's message id and the **SHA-256 of the proposal
the page showed**. lake recomputes it from the stored proposal; a proposal that changed since, one
with defects, or one that is not in this person's conversation is refused. Approval is recorded as
`agent.factory.approve` with the digest. Then lake, as the person:

| Proposal | lake creates |
|---|---|
| native | `create_agent` with the Orama base model and the manifesto as system prompt |
| python, stored in Zetta | `create_agent` with `definition {source: zetta, instructions, files, entrypoint}` |
| python, stored in lake | a **private** repository `agent-<name>-<4 hex>`, one commit of the files, then `create_agent` with `definition {source: lake, repository_id, revision: commitOid}` |

`instructions.md` is composed from the name, purpose, manifesto and goal when the proposal carries
none; `entrypoint` comes from `agent.toml`. A Python agent with a trigger is scheduled, and a **first
check run** is launched so the new agent's first run is one the person can read. The page then opens
the agent.

Verify: `HttpServer/HttpResponseIsolation.TheAgentFactoryCreatesOnlyTheExactProposalThePersonApproved`.
