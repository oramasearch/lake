# Agents

## Ask Orama

Press **Ask Orama** at the top of any page. It answers from what you can read, and for anything
that changes your lake it **proposes** the change for you to confirm. It hands work to the right
agent: the Project Agent for pipelines, Auto-Suggest for ideas, or an agent you built.

Agents work in the background: after you send a message the page returns at once and shows that the
agent is working; you can leave and come back, and the answer is kept.

## The Agent Library

**Design → Agent Library** (also at the foot of Train → Agents) lists the agents Orama provides, what each does for you and
where to use it:

- **Project Agent**: builds pipelines and proposes changes to them (Design, on a pipeline).
- **Auto-Suggest**: suggests projects worth building from your data (Design → Opportunities →
  Scan my workspace). The Opportunities are kept until you act on them.
- **AgentCreator**: designs a new agent with you (Train → Agents → Build it with the Agent Factory).

## Build your own

**Train → Agents → Create an Agent**: what it's for, where it works, its instructions, what
success looks like, what it may do, the model, and optionally a schedule so it runs on its own.
Or build it in a conversation with the **Agent Factory**.

An agent's page runs it once (**Run now**), adds **Schedules** (plain words such as *every Monday
at 09:00*, or cron), grants it approved pipelines as tools, and shows every run and its result.
