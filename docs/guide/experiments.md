# Experiments

An **experiment** searches for the best way to train a model, as an experiment tracker records it.
It runs many **trials** of one pipeline, each a training run with its own settings, measures each
trial on held-out rows, and ranks them. You follow the trials as their curves arrive, compare runs,
and promote the best trial's model.

Open **Train → Experiments**. You can describe a goal to **Experiments Agent** and review the
experiment it proposes, or fill in an experiment yourself with **Start experiment**. Every
experiment and trial lives in your workspace.

Every call below names `customer_id`, the id of the account that owns your workspaces (yours, or
your organization's), and `workspace_id`, the workspace in the page address
([Workspaces](/docs/workspaces)). Each operation is a REST call (`POST /v1/<name>` with a JSON body;
reads also answer `GET`) and an MCP tool of the same name and input.

## Concepts

| Term | What it means |
|---|---|
| **Experiment** | One search: a pipeline, a dataset, a methodology, an objective, the parameter ranges and budgets. Its id is `experiment_id`. |
| **Trial** | One training run of the experiment with one set of parameters, with its own run and curves. Its state is `running`, `pruned`, `failed` or `complete`. |
| **Run** | One training run's record: its metric curves, parameters and artifacts, named by `run_id`. Each trial has one. |
| **Objective** | The metric the experiment optimizes and its direction (maximize or minimize). |
| **Search ranges** | The parameters to try and their ranges. With *from defaults*, Lake fills in the methodology's own. |
| **Budgets** | The most trials, how many run in parallel, the wall time and the cost cap. |
| **Sampler and pruner** | How the next parameters are chosen, and how weak trials are stopped early. |
| **Tracked run** | A run with recorded metrics outside any experiment; it can be charted and compared too. |
| **Promote** | Make a completed trial's registered model the experiment's project model. Nothing is copied. |

### Objective Names

Objectives use the run report's held-out metric names:

| Name | Usually |
|---|---|
| `held_out_auc` | maximize |
| `held_out_log_loss` | minimize |
| `held_out_accuracy` | maximize |
| `held_out_rmse` | minimize |
| `held_out_mae` | minimize |

The workspace still accepts older spellings such as `validation.auc` and stores the canonical name, so an
experiment shows under its `held_out_*` name. The validation split is `validation`; a `test` split is
refused.

## See Experiments and Runs

**Train → Experiments** opens with figures (Experiments, Running, Trials, Tracked runs), a search
over experiments and runs, the state as filter chips and a project filter (**Filter**). Each
experiment row shows its project and objective, its state, the best value so far, trials against
the budget with complete, pruned and failed counts, and cost against the cap. **Tracked Runs Outside
Experiments** lists runs with metrics that belong to no experiment.

- REST: `POST /v1/list_experiments` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "project": "retention", "state": "running"}`
- MCP: `list_experiments`. `project` and `state` are optional. The answer lists `experiments` and
  `runs`.

## Run an Experiment with the Experiments Agent

Experiments Agent turns a goal into a complete experiment you review before anything starts.

1. Open **Train → Experiments** and choose **Ask the Experiments Agent**.
2. Under **Run an Experiment**, describe the goal, for example *Tune a churn model for validation AUC
   with a bounded trial budget*, and choose **Ask the Experiments Agent**.
3. The page shows **Experiments Agent Is Working** while the turn runs in the background. You can
   leave and come back; the task stays in the address.
4. If it asks a question, answer it with **Reply to the Experiments Agent**; it keeps the conversation.
5. A finished turn shows **Experiments Agent Result**: its explanation (**Why these choices**) and the
   **Proposed Experiment**, the complete experiment JSON in an editable box: name, project, pipeline,
   dataset, methodology, objective, parameter ranges and budgets.
6. **Review and edit the experiment JSON.** Change anything you want before it starts: the
   objective, the parameter ranges, the budgets.
7. Choose **Confirm and start experiment**. Only now does anything start. The page checks that the
   proposal is still the one it showed you, and starts a second turn carrying exactly the experiment
   you reviewed. Malformed JSON or a proposal that changed starts nothing.
8. **Open the running experiment** opens it.

The first turn can only read: it has no way to create or start an experiment. Only the confirmed
turn may start one, and only the experiment you confirmed.

Experiments Agent must be running on this Lake. If it is not, the box stays visible and says
*The Experiments Agent is not running on this Lake.*

### The Same Steps Through REST and MCP

Propose:

- REST: `POST /v1/agent_turn` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "agent": "agent-experiments", "kind": "experiment", "message": "Tune retention for held-out AUC with four trials."}`
- MCP: the `agent_turn` tool, with the same input.

The answer is `task_id` and `status: pending` at once. Follow the task until it has finished:

- REST: `GET /v1/get_agent_task?customer_id=<customer id>&workspace_id=<workspace id>&task_id=<task id>`
- MCP: `get_agent_task`.

A finished proposal carries `kind: proposal`, `confirm: true`, the editable experiment (under
Experiments Agent's field `study`), its explanations, and the proposal's digest, `proposal_sha256`.

Confirm: send the reviewed proposal back in a second `agent_turn`, at `context.confirmed_proposal`,
naming the turn that proposed it as `proposal_task_id` and the digest from `get_agent_task` as
`proposal_sha256`. Only the experiment (`study`) may differ from the stored proposal:

- REST: `POST /v1/agent_turn` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "agent": "agent-experiments", "kind": "experiment", "message": "Confirm and start the reviewed experiment proposal.", "proposal_task_id": "<task id>", "proposal_sha256": "<proposal_sha256 from get_agent_task>", "context": {"confirmed_proposal": {"kind": "proposal", "confirm": true, "study": {"name": "Retention Tuning", "project": "retention", "pipeline": "training", "dataset": "accounts", "methodology": "M-1", "objective": {"metric": "held_out_auc", "direction": "maximize", "validation_split": "validation"}, "budgets": {"max_trials": 4, "max_parallel_trials": 1, "max_wall_time_ms": 60000, "cost_cap": 5}}, "explanations": {"objective": "Use held-out AUC."}}}}`
- MCP: `agent_turn`, with the same input.

Lake checks the confirmation itself. It refuses, and nothing starts, when:

- `proposal_task_id` is not a finished Experiments Agent proposal of this workspace that you may
  follow: *That task has no Experiments Agent proposal to confirm.* (`invalid_input`);
- the digest or anything but the experiment differs: *The Experiments Agent proposal changed before
  confirmation.* (`conflict`);
- the edited experiment is not a JSON object: *The edited experiment proposal must be a JSON
  object.*

You can also ask about a running experiment: `kind` `experiment_status` reports the experiment and
its trials, and `experiment_explain` explains the analysis and the best trial, each with the
experiment's id in `context` under `experiment_id`. An explanation turn never promotes without its
own confirmation.

## Start an Experiment Yourself

1. Open **Train → Experiments** and choose **Start experiment**.
2. Fill in **Start an Experiment**: experiment name, project, pipeline, dataset, methodology
   (default `M-1`), the objective metric (default `held_out_auc`), direction (**Maximize** or
   **Minimize**), validation split (default `validation`), sampler, pruner, and the budgets (most
   trials, trials in parallel, wall time in milliseconds, cost cap).
3. Choose **Review and start experiment**. A dialog shows the experiment's parameters and budgets.
4. Confirm. The experiment starts and its page opens.

- REST: `POST /v1/start_experiment` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "name": "Retention Tuning", "project": "retention", "pipeline": "training", "dataset": "accounts", "methodology": "M-1", "objective": {"metric": "held_out_auc", "direction": "maximize", "validation_split": "validation"}, "from_defaults": true, "budgets": {"max_trials": 10, "max_wall_time_ms": 60000, "max_parallel_trials": 1, "cost_cap": 5}, "confirm": true}`
- MCP: `start_experiment`.

`name`, `methodology` and `confirm: true` are required. With `from_defaults: true`, Lake fills in the
methodology's parameter ranges and any missing objective (`held_out_auc`, maximize, the `validation`
split) and budgets (10 trials, 2 in parallel, 10 minutes, no cost cap), field by field. Without it,
a missing `objective` or `budgets` is refused at `/objective` or `/budgets` before anything reaches
the workspace.

### An Experiment on Your Own Methodology

A methodology of your own, once Validate passed, can be tuned the same way. Choose it in
**Methodology** (it is marked *(yours)*), name the dataset and the column to predict (or a project
and pipeline that use it), and pick one of its held-out metrics. Leave the search parameters blank
to try the ranges its `methodology.json` declares. Each trial runs your code as a training run in
its sandbox, metered like any run; the trial's row on the experiment's page shows that run's phase.
A trial whose code fails ends with the reason and the experiment goes on; two failing the same way
stop it.

- REST: `POST /v1/start_experiment` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "name": "Late payers tuning", "methodology": "W-0a1b2c3d", "dataset": "sales.payers", "target": "late", "from_defaults": true, "confirm": true}`
- MCP: `start_experiment`.

## Follow an Experiment

Open an experiment from the list. The header shows its name and state, figures (best objective,
trials, running, cost) and the **Live updates** switch.

- **Optimization History**: each trial's objective and the best-so-far line.
- **Parallel Coordinates**: each trial's parameters against its objective.
- **Parameter Importance**: which parameters mattered (it needs completed trials).
- **Trial Leaderboard**: every trial, sortable, with state chips as filters and a column picker for
  parameters and final metrics; confidence bounds come from the engine's analysis.
- **Status and Budgets**: budgets with meters and trials by state.
- **Latest Trial Curves**: the curves of the latest eight trials.

Each chart has its exact values in a table under it. A promoted trial and the experiment's promoted
model are marked **Promoted**.

| What | REST and MCP |
|---|---|
| The experiment | `get_experiment` |
| Its trials (optionally by `state`) | `list_experiment_trials` |
| History, importance, coordinates, leaderboard | `get_experiment_analysis` |

For example `POST /v1/get_experiment_analysis` with
`{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "experiment_id": "<experiment id>"}`.

### Live Updates

With **Live updates** on and the tab visible, the page waits for new events on the experiment and
its runs and refreshes as they arrive, at most every 10 seconds. A hidden tab or the switch off pauses
it; a failure shows *Reconnecting* and keeps trying.

- REST: `POST /v1/tail_experiment_events` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "kind": "run", "id": "<run id>", "from_sequence": 1, "wait_ms": 10000, "max_events": 100}`
- MCP: `tail_experiment_events`. `kind` is `experiment` or `run`; `wait_ms` is 0 to 30000 and
  `max_events` 1 to 1000. Continue after the last event you received, or from `next_sequence` when
  a batch is empty.

## Charts and Comparisons

A trial or tracked run opens its own page: every metric grouped by its prefix, its parameters and
its artifacts (confusion matrices, ROC and PR curves, calibration, feature importance, sample
predictions). A GBDT classifier trained with a held-out split shows its **Confusion Matrix**,
**ROC Curve** and **Calibration** on the held-out rows; a regressor shows none. On the list, tick tracked runs and choose **Compare selected runs**; on an
experiment, select trials and **Compare selected runs**. Two to eight runs share charts, with their parameter differences
marked.

The chart toolbar sets the X axis (step, relative time or wall time), smoothing (0 to 99%) and log
scale. Drag selects a region, Shift-drag pans, and buttons zoom, pan and reset. **Chart data** shows
the exact values. A run trained with differential privacy says its curves are suppressed and draws
none.

- Metrics: `POST /v1/get_run_metrics` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "run_id": "<run id>", "names": ["train.loss"], "max_points": 500}`
  (MCP: `get_run_metrics`; `names`, `from_step` and `max_points` are optional).
- Compare: `POST /v1/compare_runs` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "runs": ["<run id>", "<other run id>"]}`
  (MCP: `compare_runs`; `names` is optional, and without it Lake compares every metric the runs
  share).

## Stop and Promote

Each of these asks first, in words, with the exact input shown; **Cancel** or Escape sends nothing.

- **Stop experiment** stops the experiment and its active trials:
  `POST /v1/stop_experiment` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "experiment_id": "<experiment id>", "confirm": true}`
  (MCP: `stop_experiment`). **Stop** on an experiment row in the list does the same.
- **Stop trial**, on a running trial in the leaderboard, stops one trial and leaves the experiment
  running: `POST /v1/stop_experiment_trial` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "experiment_id": "<experiment id>", "run_id": "<run id>", "confirm": true}`
  (MCP: `stop_experiment_trial`).
- **Promote**, on a completed trial with a model, makes its registered model the experiment's
  project model: `POST /v1/promote_trial` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "experiment_id": "<experiment id>", "run_id": "<run id>", "confirm": true}`
  (MCP: `promote_trial`). The promoted model is marked **promoted** on **Train → Models** and **Ship → Models**.

## Permissions

| What | Needs |
|---|---|
| See experiments, trials, charts and comparisons | `read` on the account that owns the workspace |
| Ask the Experiments Agent, start and stop experiments and trials | `write` on that account |
| Promote a trial | `admin` on that account |

The page shows **Stop** only to writers and **Promote** only to administrators; every capability
checks again. A scoped key must name each tool it calls. Starting, stopping and promoting all need
`confirm: true`, on REST and MCP as on the page.

## Limits

- Experiments and runs cannot be deleted: the engine has no delete operation, so the pages have no
  delete control.
- `compare_runs` takes two to eight runs.
- A trial whose curves cannot be read is left out of **Latest Trial Curves** rather than failing the
  page.
- Runs trained with differential privacy keep their summary and artifacts, but not their curves.

## Troubleshooting

| What you see | What to do |
|---|---|
| *The Experiments Agent is not running on this Lake.* | Start an experiment yourself, or ask whoever runs this Lake to deploy the Experiments Agent. |
| `invalid_input` at `/objective` or `/budgets` | Fill them in, or send `from_defaults: true`. |
| `invalid_input` naming a field | The path to the field at fault; correct it and start again. |
| `conflict` on the confirmed `agent_turn` | The proposal changed since you reviewed it; read the task again and confirm the new one. |
| `unauthorized` on `promote_trial` | Promotion needs `admin` on the account. |
| *No Training Curves Yet* | The trials have recorded no metrics yet; turn on **Live updates**. |
| `workspace_not_ready` | The workspace is starting ([Workspaces](/docs/workspaces)). |

## Related Pages

- [Train a Model](/docs/train)
- [Agents](/docs/agents)
- [Models and Search Indexes](/docs/ship)
- [Streams](/docs/streams)
- [Ask Orama](/docs/ask-orama)
