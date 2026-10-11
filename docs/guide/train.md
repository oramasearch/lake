# Train a Model

This page is about training models with a **pipeline**, an execution graph of steps from your data
to a result. A pipeline can hold one model or several, and a model need not come from a pipeline at
all ([Concepts and Glossary](/docs/concepts)). You describe what to predict, lake builds a pipeline in
your workspace, you approve it, and a run trains its models on your data and measures them on rows
they did not see. This page covers building the pipeline, changing it, running it, and reading what
the run says. Experiments that try many settings are on [Experiments](/docs/experiments).

Everything here happens in one workspace: the one in the page address (`/w/<workspace id>/…`) and
the top-bar picker. Every REST and MCP call names it as `workspace_id`, beside `customer_id`, the id
of the account that owns your workspaces (yours, or your organization's).
[Workspaces](/docs/workspaces) says where to find both.

## Concepts

| Term | What it means |
|---|---|
| **Project** | A business question and the decision it should improve (**Design → Projects**). It holds pipelines. |
| **Pipeline** | An execution graph: the steps that turn data into a result (connect, clean, model, index, serve, act), joined by edges. Its id is the `graph` of every call. |
| **Methodology** | The modelling method a pipeline uses: churn classification, segmentation, forecasting, anomaly detection, survival, uplift and the rest (**Design → Methodologies**). |
| **Approval** | A pipeline runs only once it is approved, and must be approved again after any change. |
| **Check** | The workspace's correctness checks, run without training anything. |
| **Run** | One execution of a pipeline, with its report, lineage, outputs, log and trace. |
| **Hold-out** | The share of rows kept out of training to measure the model honestly. |
| **Held-out score** | The score on those rows: `held_out_auc`, `held_out_log_loss`, `held_out_accuracy`, `held_out_rmse` or `held_out_mae`. A score measured on the rows the model learned from is *in-sample* and flatters it. |
| **Model** | A learned function inside the pipeline: a `model` step. A pipeline may have several. Each model a successful run trains is registered and appears on **Train → Models**; a model can also be uploaded on its own, with no pipeline. |
| **Version** | One training of a model. Every run of the pipeline (or trial of an experiment) that trains it adds a version with its own model id; **Train → Models** groups them under the model. |

## Build a Pipeline from a Sentence

1. Open **Design → Projects** and open your project (or **New Project** to make one).
2. Choose **Build a Pipeline**. Under **What should it predict or answer?** write a sentence that
   names the dataset and the column to predict, for example *Predict the churned column of accounts
   from plan, monthly_spend and support_tickets.*
3. Optionally choose a dataset under **From data**: the pipeline reads it, and its runs train on it.
4. Optionally choose a **Methodology** to get that methodology's own pipeline. Only the
   methodologies your workspace's engine runs are listed.
5. Submit. The pipeline page opens.

A pipeline whose methodology does not train yet says **This Pipeline Trains Nothing Yet** instead of
pretending to train.

- REST: the project must exist first. Make it once with `POST /v1/workspace_act`
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "create_project", "input": {"name": "retention", "intent": "Predict which accounts churn"}}`,
  then `POST /v1/compile_pipeline` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "project": "retention", "intent": "Predict the churned column of accounts", "dataset": "datasets.accounts"}`.
  Without the project, `compile_pipeline` answers `not_found` at `/project` and says to create it.
  `dataset` is `<database>.<table>`; `methodology` (for example `M-3`) is optional. The answer's
  `graph` is the new pipeline's name, taken from your description's first words; `template` says
  which template its steps started from.
- MCP: `workspace_act` with `create_project`, then the `compile_pipeline` tool, with the same
  input. `compile_pipeline` is the one recipe: it captures the sentence and compiles it (the
  engine's `capture_intent` and `compile_intent`) in one call, and `mcp_setup`'s *Train a model*
  flow uses it too.

You can also start a pipeline empty, from an n8n export or from Python:
`POST /v1/create_pipeline` with
`{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "project": "retention", "from": "empty", "name": "scoring"}`
(MCP: `create_pipeline`). `from` is `empty`, `n8n` or `python`; the last two take the source text in
`content`.

To begin from a complete business solution instead (the decision, the holdout, the actions and the
return), see [AI Solutions, Methodologies and Opportunities](/docs/solutions).

## Read a Pipeline

The pipeline page (**Design → Projects →** your project **→** the pipeline) draws the graph in stage
lanes, each step coloured by the latest run. Select a step to see its kind, its attributes and its
edges. The side column holds the pipeline's facts and **Approve, Check, Run and Deploy**; the
pipeline's runs are listed below the graph.

- REST: `GET /v1/get_pipeline?customer_id=<customer id>&workspace_id=<workspace id>&project=retention&graph=<pipeline id>`
- MCP: `get_pipeline`.

## Change a Pipeline

### With the Project Agent

On the pipeline page, use **Ask the Project Agent**. Under **What should change?** describe the
change, for example *Train on accounts only and hold out 20% of rows*, and choose
**Propose a change**.

1. The Project Agent reads your data and the pipeline and answers in the background; the page shows
   *The Project Agent is working on it…* while it does.
2. The answer is a proposal: each edit in words, the pipeline after it, and run settings.
3. Choose **Apply** to save every edit at once, or **Discard**. Nothing changes until you apply.
4. If the agent asks a question instead, answer it with **Reply to the Project Agent**; the
   conversation continues. A proposal that cannot be applied lists each reason under
   **Why it cannot be applied**.

The agent never saves, approves or runs anything itself: those stay yours.

- REST: `POST /v1/agent_turn` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "agent": "agent-project", "message": "Train on accounts only and hold out 20% of rows", "context": {"project": "retention", "graph": "<pipeline id>"}}`.
  The answer is a `task_id` and a `status` at once; follow it with `get_agent_task` (below).
- MCP: the `agent_turn` tool, with the same input.

Follow a background turn:
`GET /v1/get_agent_task?customer_id=<customer id>&workspace_id=<workspace id>&task_id=<task id>`
(MCP: `get_agent_task`). Its status moves through `pending`, `running`, then `succeeded` or `failed`.

### By Hand

Select a step to rename it (**Rename step**), change its kind (**Change kind**), connect it
(**Connect**) or remove it (**Remove step**; its edges go too). Under **Change the Pipeline** add
steps (**Add step**) and edges (**Add edge**), or rename the pipeline (**Rename pipeline**).

- REST: `POST /v1/edit_pipeline` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "project": "retention", "graph": "<pipeline id>", "op": "rename_step", "step": "<step id>", "name": "Score accounts"}`.
  `op` is `add_step`, `rename_step`, `kind_step`, `remove_step`, `add_edge`, `remove_edge`,
  `set_attribute` or `rename`; `edits` applies a list of them as one save, all or nothing.
- MCP: `edit_pipeline`.

An edit that would leave duplicate step ids, unknown kinds, edges to missing steps or a cycle is
refused and nothing is saved. **A saved change needs approval again before the pipeline runs.**

## Approve, Check and Run

1. **Approve** the pipeline. The workspace verifies and runs only an approved pipeline.
2. **Check** runs the workspace's correctness checks without running anything. Each check is listed with its
   reason.
3. **Check and run** checks and, when the checks pass, trains. Fill in the **Run settings**:
   - **Train on**: the dataset to learn from.
   - **Hold out for measuring**: the share of rows kept out of training to measure the model. The
     default 0.2 keeps a fifth of the rows out. 0 measures nothing, and every score is in-sample.
   - **Model name**: defaults to the project and pipeline names.
4. The run appears under the pipeline's **Runs** and on **Train → Runs**.

Approve:

- REST: `POST /v1/workspace_act` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "approve_plan", "input": {"project": "retention", "graph": "<pipeline id>"}}`
- MCP: `workspace_act`, with the same input.

Check only: the same call with `"capability": "gate"`:
`{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "gate", "input": {"project": "retention", "graph": "<pipeline id>"}}`.

Check and run:

- REST: `POST /v1/run_pipeline` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "project": "retention", "graph": "<pipeline id>", "dataset_path": "datasets.accounts", "holdout_fraction": 0.2, "model_name": "retention_churn"}`.
- MCP: `run_pipeline`.

The answer carries `gate` (the checks) and `run` (the run started). When a check fails it carries
`gate` alone and nothing runs. `holdout_fraction` is 0 to 0.9.

**Deploy** places an approved pipeline in the workspace behind its checks
(`workspace_act` with `deploy`).

## Read the Runs

**Train → Runs** lists every run in the workspace with its pipeline and project, its status
(succeeded, failed, in progress) and when it started. Search and filter by status. Each row links to
**Report**, or **Why it failed** for a failed run, and to **Charts**. A run made by an experiment is
listed as *Trial N of experiment* and the experiment's name, linked to it.

- REST: `GET /v1/list_runs?customer_id=<customer id>&workspace_id=<workspace id>&project=retention&limit=20`
  (`limit` is 1 to 20; `project` and `solution` filter).
- MCP: `list_runs`.

## Read the Report

Open a run. While it is unfinished the page checks it again every 15 seconds; the report loads once
it finishes. The report leads with **the held-out score**, the score on rows the model did not see,
named as the report names it:

| Report key | Shown as |
|---|---|
| `held_out_auc` | Held-out AUC |
| `held_out_log_loss` | Held-out log loss |
| `held_out_accuracy` | Held-out accuracy |
| `held_out_rmse` | Held-out RMSE |
| `held_out_mae` | Held-out MAE |

For example *Held-out AUC 0.708 on 412 rows the model did not see* (`held_out_rows` is the count).

- **Do not quote.** When the workspace attaches a caveat to a score, for example because it was measured on
  too few rows, the figure is marked **Do not quote** with the reason.
- **No hold-out.** A run without a hold-out says *No held-out score was measured for this run*. If
  it reports an in-sample AUC, the page says it was measured on the rows the model learned from and
  flatters it.
- **Nothing was trained.** A structural run says so, and names each connect step that was not read.

Then, in order:

- **What It Learned From**: the dataset it trained on and the columns it did not use.
- **What the Model Leans On**: the features it relied on. A weight is reliance, not a cause.
- **Most at Risk**: the rows the model scores highest.
- **Do Not Read These as Results**: the run's caveats.
- **What This Run Cannot Tell You**: what is missing from it.
- **The raw report**, as the engine wrote it.

Beside the report: **Lineage** (where the data came from), **Outputs**, the **Log** and the
**Trace** as a waterfall of spans, and the run's facts. **Training charts** opens the run's metric
curves on Experiments; **Workspace streams** opens Train → Streams.

- REST: `POST /v1/workspace_query` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "get_run_report", "input": {"run_id": "<run id>"}}`;
  `get_run` with the same input reads its status, lineage and log.
- MCP: `workspace_query`, with the same input.

`workspace_query` is a read sent as `POST`; it changes nothing.

## Train → Models

**Train → Models** is about training each model. Serving it is **Ship → Models**
([Models and Search Indexes](/docs/ship)); each page links to the other for the same model.

The list shows one row per model: the records of one model step of one pipeline (or of one
experiment) are its versions. The figures count the models and versions, the ones with a held-out
score, the ones from experiments and pipelines, and the ones serving. Each row shows the latest
held-out score, how many versions and runs trained it with a link to the latest run and to its
experiment, the dataset and features it learned from, when it was last trained, and the version
serving in Ship.

- **Search models, features or ids…** filters the list; **Pipeline**, **Experiment** and **No
  pipeline** filter by where a model came from.
- **Train a model** opens Design → Projects, where pipelines are built and run.
- A model uploaded on its own, with no pipeline, is a model repository on **Data → Repositories**.
  This page does not list model repositories yet.

### A Model's Training Page

Press a model's name to open its training page, at its latest version.

- **Held-out score over versions** charts the score each version measured, oldest first. When the
  versions measured several held-out figures, choose one with the chips.
- **Versions** lists every version: when it was trained, its held-out score (or *in-sample only*),
  the run that trained it and whether it is serving. Press a version to open it.
- **Compare** on a version's row shows it beside the open one: every figure both recorded, with
  held-out figures said **better** or **worse**. **Training curves** opens the two runs in Train →
  Experiments → Compare Runs.
- **What it learned from** names the dataset in the run report and the features the version reads.
- **Pipeline** names the model step or steps it belongs to, linked to the pipeline. A model trained
  by an experiment with no pipeline names the experiment; a model with neither says *Uploaded, no
  pipeline*.

### Retrain a Model

Press **Retrain** on the model's training page. Lake runs the model's pipeline again, after its
checks, under the model's name, and opens the new run. The new run's model becomes the next
version.

- REST: `POST /v1/run_pipeline` with the model's pipeline and name:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "project": "retention", "graph": "<pipeline name>", "model_name": "<model name>"}
```

- MCP: `run_pipeline` with the same input.

### Promote a Version to Ship

Press **Promote v3 to Ship** (the button names the version open) and confirm. Lake publishes that
version, so `predict` answers with it, and unpublishes every other version of the model; a call
naming an unpublished version's id is refused from then on. The model's serving page opens.

- REST: `POST /v1/workspace_act` with `expose_model` for the version, then `unexpose_model` for each
  other published version:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "expose_model", "input": {"model_id": "<model id>"}}
```

- MCP: `workspace_act` with the same input.

## After Training

A successful run registers a model version. Compare it on **Train → Models**, then publish it and
call it from your code on [Models and Search Indexes](/docs/ship). Experiments use the same held-out names as their objectives
([Experiments](/docs/experiments)).

## Permissions

| What | Needs |
|---|---|
| Read projects, pipelines, runs and reports | `read` on the account that owns the workspace |
| Build, edit, approve, check, run and deploy | `write` on that account |
| Ask the Project Agent | `write` (the turn itself only reads) |

A scoped key needs each capability it calls by name ([Tokens and Keys](/docs/tokens)).

## Limits

- A pipeline runs only when approved, and every saved edit returns it to proposed.
- `holdout_fraction` is 0 to 0.9; `list_runs` answers at most 20 runs per call.
- Workspace reads time out after 15 seconds and actions after 60.
- A methodology the engine does not run is refused with `not_supported`, naming its tracking issue,
  instead of being built as an empty pipeline.

## Troubleshooting

| Answer | What to do |
|---|---|
| `workspace_not_ready` | The workspace is starting. Wait until **Design → Projects** stops saying it is being prepared ([Workspaces](/docs/workspaces)). |
| `not_approved` | Approve the pipeline, then run it again. |
| `gate_blocked`, or `run_pipeline` answers only `gate` | A check failed; the message names it. Fix the step and approve again. |
| `invalid_input` at `/workspace_id` or `/customer_id` | The workspace is not one of that account's live workspaces. |
| `engine_error` | The workspace engine could not finish; its message says why. |
| `not_supported` | This workspace's engine does not run that methodology yet. |
| The report says *Nothing was trained* | The pipeline did not read its data: check the connect step's dataset or choose one under **Train on**. |

## Related Pages

- [Datasets and Loading Data](/docs/load-data)
- [AI Projects and Pipelines](/docs/design)
- [Experiments](/docs/experiments)
- [Agents](/docs/agents)
- [Models and Search Indexes](/docs/ship): serving a model
- [Ask Orama](/docs/ask-orama)
