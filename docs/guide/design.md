# AI Projects and Pipelines

**Design** is where you decide what AI to build. An **AI project** is how you build an **AI
solution**: it starts from a business question, the decision it should improve and how you will
measure it, and holds the **pipelines**, the workflow that turns your data into a result. You describe what a pipeline should predict or answer; the
workspace engine builds the steps; you review, approve and run it.

Everything on this page happens in the workspace selected in the top bar. Every REST and MCP call
names `customer_id` (the id of the account that owns your workspaces, yours or your
organization's) and `workspace_id`; [Getting Started](/docs) says where to find them.

## Concepts

| Term | What it is |
|---|---|
| **AI project** | Where an AI solution is built: a business question and the decision it should improve, with a name, a description, its intent and its pipelines. The pages call it a *project*. |
| **Pipeline** | The project's workflow, an execution graph of steps joined by arrows. Each step has a kind: `connect`, `clean`, `model`, `index`, `serve` or `act`. |
| **Edge** | An arrow between two steps. A solid arrow carries data (`data_flow`); a dashed one only orders the steps (`control`). |
| **Methodology** | The modelling method a pipeline uses, such as churn classification, segmentation or forecasting ([AI Solutions, Methodologies and Opportunities](/docs/solutions)). |
| **Approval** | A pipeline runs only once approved, and must be approved again after any change. |
| **Gate** | The engine's correctness checks, each with its reason. A run starts only when they pass. |
| **Project Agent** | The agent that designs pipelines and proposes changes to them. It never changes a pipeline itself: you apply its proposal. |

## Create a Project

1. Open **Design → Projects** and press **New project**.
2. Fill in **Name** (lowercase letters, digits, `-` and `_`), an optional **Description**, and
   **The decision it should improve**, for example *which accounts customer success calls first
   each week*.
3. Press **Create project**.

`POST /v1/workspace_act`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "create_project", "input": {"name": "retention", "description": "Keep paying accounts", "intent": "Which accounts customer success calls first each week"}}
```

From an AI agent the MCP tool is `workspace_act` with the same JSON.

**Design → Projects** lists every project with its pipelines, how many are approved and its status,
under three figures (Projects, With pipelines, Runs active). Search filters the list. When the
workspace has no project yet, the page points you to **Design → Opportunities**, where Auto-Suggest
recommends projects from your data.

## The Project Page

Open a project. Its tabs are **Overview**, **Pipelines**, **Runs** and, for a project started from
a solution, **Solution**. The side column shows the status, the decision it should improve, the
dataset and the actions (**Opportunities**, **Delete project**).

When a project came from an opportunity or from Ask Orama, the Project Agent may still be designing
its pipeline: the page says *The Project Agent is designing this project's pipeline…* and checks
again every 3 seconds.

## Build a Pipeline From a Sentence

1. On the project, press **Build pipeline**.
2. Under **What should it predict or answer?**, name the dataset and the column, for example
   *Predict the churned column of accounts from plan, monthly_spend and support_tickets*.
3. Optionally choose a dataset under **From data**: the pipeline's first connect step then reads
   it, and the pipeline opens with that dataset chosen for its run.
4. Optionally choose a **Methodology** to get that methodology's own pipeline. Only the
   methodologies your workspace's engine runs are listed.
5. Press **Build pipeline**.

`POST /v1/compile_pipeline`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "project": "retention", "intent": "Predict the churned column of accounts", "dataset": "datasets.accounts"}
```

Add `"methodology": "M-3"` to choose one. The answer names the new pipeline as `graph`. The MCP tool
is `compile_pipeline`.

A pipeline whose methodology does not train yet says *This Pipeline Trains Nothing Yet* instead of
pretending to train. A methodology the engine cannot run is refused with `not_supported`, naming
its tracking issue.

## Other Ways to Start a Pipeline

Under **Other Ways to Start a Pipeline** on the project, **Start from** offers:

- **Empty**: one connect step you build up by hand. Give it a **Name** and press **Create**.
- **From n8n**: paste an n8n workflow export (JSON) and press **Import from n8n**.
- **From Python**: paste a Python pipeline and press **Build from Python**. The engine reads it and
  lowers it to a pipeline; the code never runs.

`POST /v1/create_pipeline`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "project": "retention", "name": "by hand", "from": "empty"}
```

For n8n and Python, set `from` to `"n8n"` or `"python"` and put the export or the code in
`content`.

## The Pipeline Page

Open a pipeline from the project's **Pipelines** tab. The page shows:

- **Graph**: the steps in stage lanes, coloured by their latest run. Solid arrows carry data;
  dashed arrows only order steps. Select a step to inspect or change it.
- **Step**: the selected step's name, kind and attributes.
- **Pipeline**: its facts, including its approval.
- **Approve, Check, Run and Deploy**: the actions below.
- **Ask the Project Agent** and **Change the Pipeline**: the two ways to change it.
- **Runs**: the pipeline's runs with their status, start time and outputs.

Through the API, `get_pipeline` reads the graph, its approval, each step's latest status and its
runs:

`GET /v1/get_pipeline?customer_id=<customer id>&workspace_id=<workspace id>&project=retention&graph=<pipeline name>`

## Change a Pipeline With the Project Agent

1. Under **Ask the Project Agent**, describe the change in **What should change?**, for example
   *train on accounts only and hold out 20%*.
2. Press **Propose a change**. The agent works in the background (*The Project Agent is working on
   it…*); you can leave and come back.
3. Its proposal shows each edit in words and the pipeline after them. *Nothing has changed yet.*
4. Press **Apply** to save every edit as one change, or **Discard**.

If the agent asks a question instead, answer it in **Reply** and press **Reply to the Project
Agent**: it continues the same conversation. A proposal that cannot be applied lists each reason in
one plain sentence.

`POST /v1/agent_turn`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "agent": "agent-project", "kind": "pipeline", "message": "Train on accounts only and hold out 20%", "context": {"project": "retention", "graph": "<pipeline name>"}}
```

The answer's `task_id` is followed with `get_agent_task`. The Project Agent's key reads your
workspace but cannot save, approve or run anything; applying its proposal is your
`edit_pipeline` call with its `edits`.

## Change a Pipeline by Hand

- Select a step to **Rename step**, **Change kind**, **Connect** it to another step (choosing what
  the arrow carries) or **Remove step**.
- Under **Change the Pipeline**, **Add step** (a kind and a name), **Add edge** (from, to and what
  it carries), or **Rename pipeline**. Existing edges are listed with **Remove**.

Every change is one `edit_pipeline` call. The `op` is one of `add_step`, `rename_step`,
`kind_step`, `remove_step`, `add_edge`, `remove_edge`, `set_attribute` or `rename`. Lake refuses an
edit that would leave duplicate ids, unknown kinds, dangling edges or a cycle.

`POST /v1/edit_pipeline`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "project": "retention", "graph": "<pipeline name>", "op": "add_step", "step": "score", "kind": "model", "name": "Score accounts"}
```

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "project": "retention", "graph": "<pipeline name>", "op": "add_edge", "from": "clean", "to": "score"}
```

Send `edits`, a list of such edits, to apply several as one save, all or nothing.

## Approve, Check, Run and Deploy

A changed pipeline needs **Approve** again before it runs.

1. **Approve**: the engine verifies and runs only an approved pipeline.
2. **Check**: runs the correctness checks without running anything, and shows every check with its
   reason.
3. **Check and run**: choose the dataset under **Train on**, keep **Hold out for measuring** at
   0.2 (a fifth of the rows kept out of training to measure the model; 0 measures nothing), name
   the model under **Model name** (it defaults to the project and pipeline), and run. A passing
   gate opens the run's page; a failing one shows the checks.
4. **Deploy**: places the approved pipeline in the workspace behind its checks. The page then says
   *Deployed*.

Approve is `approve_plan` inside `workspace_act`:

`POST /v1/workspace_act`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "approve_plan", "input": {"project": "retention", "graph": "<pipeline name>"}}
```

Check is `gate` and Deploy is `deploy`, with the same `input`. Check and run is one call:

`POST /v1/run_pipeline`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "project": "retention", "graph": "<pipeline name>", "dataset_path": "datasets.accounts", "holdout_fraction": 0.2, "model_name": "retention_churn"}
```

It answers the gate, and the run when the gate passed. What a run reports is in
[Train a Model](/docs/train).

## Delete a Project

**Delete project** on the project removes the project, its intent and its pipelines; its runs and
models stay listed. Type the project's name to confirm, then press **Delete project**.

`POST /v1/workspace_act`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "delete_project", "input": {"name": "retention"}}
```

## Permissions

| To | You need, on the account that owns the workspace |
|---|---|
| See projects, pipelines and runs | `read` |
| Create projects and pipelines, edit, approve, check, run, deploy, ask the Project Agent, delete | `write` |

You hold `admin` on your own workspaces. In an organization, owners and admins hold `admin`;
members read unless they were granted `write` ([Grants and Effective Access](/docs/access)).

## Limits

- Workspace reads time out after 15 seconds and actions after 60.
- Only the methodologies the workspace's engine runs can be built; the others are listed on
  **Design → Methodologies** with why they do not run yet.
- A project name must be unique in the workspace.

## Troubleshooting

| What you see | Why, and what to do |
|---|---|
| `conflict` at `/input/name`: *a project named … already exists in this workspace* | Choose another name. |
| `invalid_input` at `/input/name`: *a project needs a name* | Give the project a name. |
| `not_approved` (409) | The pipeline changed or was never approved. Press **Approve**. |
| `gate_blocked` (409) | A check failed; the message names it. Fix the pipeline or the dataset and check again. |
| `invalid_input` from `edit_pipeline` | The edit would leave duplicate ids, unknown kinds, dangling edges or a cycle; the message says which. |
| `not_supported` naming an issue | The chosen methodology does not run on this engine yet. |
| *The Project Agent did not propose a change* | It asked a question or found nothing to change; reply or rephrase. |
| `workspace_not_ready` | The workspace is starting; wait until it is Ready. |
| `unauthorized` | You have `read` but not `write` on the account. |

## Related Pages

- [Datasets and Loading Data](/docs/load-data)
- [AI Solutions, Methodologies and Opportunities](/docs/solutions)
- [Train a Model](/docs/train)
- [Agents](/docs/agents)
- [Ask Orama](/docs/ask-orama)
- [Models and Search Indexes](/docs/ship)
