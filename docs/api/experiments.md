# Training Experiments API

Lake exposes AI model training experiments, their trials, tracked Runs, metric curves, artifacts
and bounded live reads through eleven registered capabilities. An experiment is what an experiment
tracker calls one: it tries settings across many trials, each a run with its own curves, and keeps
the best model. The Zetta engine calls the same thing a *study*; lake translates at its boundary
([Names](#names)). Lake's `get_experiment` is distinct from the solutions holdout experiment that
`workspace_query`'s engine read `get_experiment` returns. The browser, REST clients and MCP clients
use the same registry, account policy, validation and signed audit. No browser receives a workspace
credential.

The contract is Zetta `dev` revision `a1d74b2186bf609fc40b6e06b8d6cdf522a1174b`, as deployed in
`dev-a1d74b21`: `docs/ml/run-metrics.md`, `docs/ml/studies.md` and `schemas/openapi.json` in the
explicitly authorized workspace checkout. Lake's `LAKE_ZETTA_SOURCE_DIR` remains the bridge
checkout; only its client transport is compiled. No engine implementation is vendored.

## Transport and Permissions

Every operation accepts `POST /v1/<name>` with a JSON body. The MCP tool has exactly the same
name and input/output schemas. Reads also retain GET compatibility and remain `readOnlyHint:true`.
`customer_id` and `workspace_id` are required for every call: `customer_id` resolves the selected
account's policy object before any workspace request, and `workspace_id` names the workspace whose
Zetta holds the experiment (lake#439: the examples below carry both). Calls use that account's Lake-held service credential. Capability-scoped
tokens must include the specific tool name. Objects are closed, including typed artifacts and
pattern-restricted scalar parameter/metric dictionaries.

List capabilities follow the registry's candidate-authorization contract: their handlers resolve
and authorize the required parent account before listing experiments, runs or trials. An unauthorized
account never reaches a workspace caller, including through list operations.

| Capability and MCP tool | Permission | Engine operations |
|---|---|---|
| `list_experiments` | read | `list_studies`, `list_runs`, `get_run_metrics` |
| `get_experiment` | read | `get_study` |
| `list_experiment_trials` | read | `list_trials` |
| `get_experiment_analysis` | read | `get_study_analysis` |
| `get_run_metrics` | read | `get_run_metrics` |
| `compare_runs` | read | `get_run_metrics`, `compare_runs` |
| `tail_experiment_events` | read | target resolution, `receive_events` |
| `start_experiment` | write, `confirm:true` | `create_study` |
| `stop_experiment` | write, `confirm:true` | `stop_study` |
| `stop_experiment_trial` | write, `confirm:true` | `stop_trial` |
| `promote_trial` | admin, `confirm:true` | `promote_trial` |

Promotion intentionally requires account administration, separate from permission to spend compute
on training. Start, stop and promote all reject missing or false confirmation. The engine's study
actions on `workspace_act` and the workspace agent adapter (`create_study`, `stop_study`,
`stop_trial`, `promote_trial`) delegate to these registered operations; they cannot bypass
confirmation, tool scope or promotion permission. The upstream names are in the common workspace
allow-lists. `start_experiment` is Lake's confirmed name for `create_study`.
The filtered workspace OpenAPI also marks `confirm:true` required on these action aliases, so
agents deriving workspace tools see the same confirmation requirement before they call.

Examples below show the request followed by the successful `value`. The common REST/MCP envelope
is `{ "ok": true, "value": ..., "duration_ms": 1.0 }`. Experiment examples abbreviate full records
where noted; the generated OpenAPI is the complete schema.

## Names

Lake speaks of experiments; the engine of studies. Lake translates on the way in and on the way out
(`experiment_capabilities.cpp`), so a person, a REST client and an MCP client of lake see only
experiment:

| Lake (REST, MCP, the pages) | Zetta engine |
|---|---|
| `list_experiments` answers `experiments` and `runs` | `list_studies` answers `studies` |
| `get_experiment` | `get_study` |
| `get_experiment_analysis` | `get_study_analysis` |
| `list_experiment_trials` | `list_trials` |
| `start_experiment` | `create_study` |
| `stop_experiment` | `stop_study` |
| `stop_experiment_trial` | `stop_trial` |
| `promote_trial` | `promote_trial` |
| `experiment_id`, in every input and answer | `study` |
| `run_id` in inputs ([naming.md](naming.md)) | `run` |
| `tail_experiment_events` `kind: "experiment"` | the `study-events.<id>` topic |

A refusal the engine words with *study* reaches lake's caller with *experiment*, and a path the
engine names `/study` as `/experiment_id`. The engine's own names stay the engine's: `create_study`,
`get_study`, `list_trials`, `validate_study` and the rest, `study_id` on a run, the `study.id` run
parameter and the `study-events.*` topic. `workspace_query` and `workspace_act` call the engine by
its names, so they keep `study` in and out: `workspace_act` with `create_study`, `stop_study`,
`stop_trial` or `promote_trial` hands the call to the lake capability above, moving `study` to
`experiment_id` on the way in and back on the way out. Experiments Agent and other agents call the
engine this way.

**Renamed in lake#561.** `get_experiment_study` is now `get_experiment`, `start_study` is
`start_experiment` and `stop_study` is `stop_experiment`; `study` is `experiment_id` in their inputs
and in the inputs and answers of `list_experiment_trials`, `get_experiment_analysis`,
`stop_experiment_trial` and `promote_trial`; `list_experiments` answers `experiments` instead of
`studies`; `tail_experiment_events` takes `kind: "experiment"` instead of `"study"`. No deprecated
alias was kept: no caller outside lake used the old names. The agent repositories
(`zetta-bridge/modules/agent-*`, and `oramasearch/agent-experiments`) call the engine's names through
the workspace adapter, which keeps them. A capability-scoped token that named an old tool must name
the new one.

Experiments Agent's own turn protocol keeps the engine's word too, because the agent reads and
writes it: its proposal carries the experiment as `study`, `context.confirmed_proposal.study` is
the edited experiment, and a status or explanation turn reads the experiment's id from
`context.study`. A lake caller may name it `context.experiment_id` instead; `agent_turn` adds
`context.study` for the agent.

## Experiments Agent Turns

Experiments Agent is the Agent Library service `agent-experiments`. Its public operation is the
existing registered `agent_turn` action (`write` on the customer): REST clients use
`POST /v1/agent_turn`, MCP clients use the `agent_turn` tool, and the Experiments page calls the
same capability. Ask Orama and the Project Agent may delegate training and tuning requests to it.
The three accepted turn kinds are:

| Kind | Purpose | Turn credential |
|---|---|---|
| `experiment` | turn a goal into an editable proposal; after confirmation, start that exact experiment | proposal reads; workspace `create_study` and backing `start_experiment` only when `context.confirmed_proposal` is valid |
| `experiment_status` | report the experiment and its trials | experiment, trial, metric and bounded event reads; workspace `stop_trial` and backing `stop_experiment_trial` |
| `experiment_explain` | explain analysis and a best trial | experiment and analysis reads; `promote_trial` only when a confirmed promotion is supplied |

An initial goal returns a background task immediately. Its service receives only a task id on
`agent-tasks.agent-experiments`; when it claims the task, Lake hands it a task credential and a
workspace URL. Experiment data stays in the workspace Zetta. The agent's raw engine tool names are
restricted to the current turn's scopes, and its study actions enter the canonical registered Lake
operations above, retaining customer policy, confirmation and audit checks.

Initial proposal request (`POST /v1/agent_turn`; MCP tool `agent_turn`):

```json
{"customer_id":"account-id","workspace_id":"workspace-id","agent":"agent-experiments","kind":"experiment","message":"Tune retention for validation AUC with four trials."}
```
```json
{"task_id":"task-id","status":"pending"}
```

`get_agent_task` returns a successful proposal with `kind:"proposal"`, `confirm:true`, the editable
experiment as the agent's `study` object, explanations, and its digest as `proposal_sha256`. No
experiment write is present on that task credential. The reviewed object is sent in a second `agent_turn`, naming the turn that
proposed it (`proposal_task_id`) and the digest the person reviewed (`proposal_sha256`):

```json
{"customer_id":"account-id","workspace_id":"workspace-id","agent":"agent-experiments","kind":"experiment","message":"Confirm and start the reviewed experiment proposal.","proposal_task_id":"task-id","proposal_sha256":"<proposal_sha256 from get_agent_task>","context":{"confirmed_proposal":{"kind":"proposal","confirm":true,"study":{"name":"Retention Tuning","project":"retention","pipeline":"training","dataset":"accounts","methodology":"M-1","objective":{"metric":"held_out_auc","direction":"maximize","validation_split":"validation"},"budgets":{"max_trials":4,"max_parallel_trials":1,"max_wall_time_ms":60000,"cost_cap":5}},"explanations":{"objective":"Use held-out AUC."}}}}
```
```json
{"task_id":"confirmed-task-id","status":"pending"}
```

Lake checks the confirmation itself (lake#561): `proposal_task_id` must be a finished
`agent-experiments` `experiment` turn of this workspace that the caller may follow, whose answer is a
proposal with an experiment (`study`); `proposal_sha256` must be that stored proposal's digest; and
`confirmed_proposal` must be that proposal, with only `study` edited. Otherwise the turn is refused
and nothing starts: `That task has no Experiments Agent proposal to confirm.` (400), `The
Experiments Agent proposal changed before confirmation.` (409), or `The edited experiment proposal
must be a JSON object.` (400). The confirmed task alone receives workspace `create_study` and
backing `start_experiment`. Status and explanation use the same REST/MCP operation with
`kind:"experiment_status"` or `kind:"experiment_explain"` and
`context:{"experiment_id":"experiment-id"}`. A promotion is a separate
reviewed confirmation; an explanation turn without it remains read-only.

## Reads

`list_experiments` (MCP: `list_experiments`) returns complete experiment records and summaries of
tracked runs outside experiments. Runs with no params, metrics, artifacts or suppression reason are
omitted. Project and state filters apply to both collections. Trial Run IDs, including those
identified by the engine's `params["study.id"]`, are excluded from the standalone list.

```json
{"customer_id":"account-id","workspace_id":"workspace-id","project":"retention","state":"running"}
```
```json
{"experiments":[],"runs":[]}
```

`get_experiment` (MCP: `get_experiment`) returns the complete experiment, including the objective,
parameter ranges to search, budgets, spent cost, trial states and promoted model. This abbreviated
response shows the key fields; `spec` and trial records are complete in actual responses.

```json
{"customer_id":"account-id","workspace_id":"workspace-id","experiment_id":"experiment-id"}
```
```json
{"id":"experiment-id","spec":{"name":"Retention Tuning","project":"retention"},"status":"running","created":"2026-09-28T18:20:00Z","updated":"2026-09-28T18:20:01Z","trials":[],"spent_cost":0,"stop_reason":"","promoted_model":""}
```

`list_experiment_trials` (MCP: `list_experiment_trials`) accepts an optional `state` of `running`,
`pruned`, `failed` or `complete`. Objective is absent until the engine has a value.

```json
{"customer_id":"account-id","workspace_id":"workspace-id","experiment_id":"experiment-id","state":"complete"}
```
```json
{"experiment_id":"experiment-id","trials":[{"number":1,"run":"run-id","params":{"learning_rate":0.1},"state":"complete","objective":0.83,"seed":1139,"cost":0.5,"intermediate":[{"step":1,"value":0.8}],"reason":"","model":"model-id"}]}
```

`get_experiment_analysis` (MCP: `get_experiment_analysis`) returns best-so-far history, parameter
importance, parallel-coordinate rows and the ranked leaderboard with confidence bounds.

```json
{"customer_id":"account-id","workspace_id":"workspace-id","experiment_id":"experiment-id"}
```
```json
{"best_trial":null,"optimization_history":[],"parameter_importances":[],"parallel_coordinates":[],"leaderboard":[]}
```

`get_run_metrics`, `stop_experiment_trial` and `promote_trial` name the run `run_id`, as every
capability does; `run` is still accepted as a deprecated alias ([naming.md](naming.md)). Their
answers keep the engine's `run` field.

`get_run_metrics` (MCP: `get_run_metrics`) accepts optional metric `names`, `from_step` and
`max_points` (4–10000). Omit those fields for all recorded series. Each point carries its original
step, value and wall time. Engine read-time compaction preserves extrema. Artifacts are closed
typed data for confusion matrices, ROC/PR curves, calibration, feature importance and predictions,
and only a trainer that logs them fills the list. A GBDT **classifier** run with a held-out split
logs three, each named `held_out` and worked out from the rows it never trained on (zetta#1247):
`confusion_matrix` (threshold 0.5, `labels` `["0","1"]`, rows the actual class and columns the
predicted one), `roc_curve` (`x` the false positive rate, `y` the true positive rate, at most 101
points with their score `thresholds`) and `calibration` (ten equal-width bins, each non-empty bin's
mean `predicted` probability, `observed` positive rate and `counts`). A regressor, a run without a
held-out split, or a run suppressed for differential privacy logs none. The capability's
description promises exactly that (lake#615); the run page draws them (see
[the UI page](../ui/experiments.md)).

```json
{"customer_id":"account-id","workspace_id":"workspace-id","run_id":"run-id","names":["train.loss"],"max_points":500}
```
```json
{"run":"run-id","params":{"learning_rate":0.1},"summary":{"train.loss":{"final":0.4,"final_step":10,"best":0.4,"best_step":10,"direction":"min"}},"series":[{"name":"train.loss","points":[{"step":10,"value":0.4,"time":"2026-09-28T18:20:11Z"}]}],"artifacts":[]}
```

`compare_runs` (MCP: `compare_runs`) accepts two to eight unique Run IDs. Optional `names` selects
metrics; without it Lake discovers their union from the recorded runs. `metrics` retains the
engine's exact-step alignment without invented missing points. `details` contains each run's
params, series, summaries and artifacts for relative/wall-time views and parameter differences.

```json
{"customer_id":"account-id","workspace_id":"workspace-id","runs":["run-a","run-b"],"names":["train.loss"]}
```
```json
{"runs":["run-a","run-b"],"metrics":[],"details":[{"run":"run-a","params":{},"summary":{},"series":[],"artifacts":[]},{"run":"run-b","params":{},"summary":{},"series":[],"artifacts":[]}]}
```

**Held-out scores (zetta#1239).** The score that decides between runs is measured on rows the model
never trained on: `held_out_auc`, `held_out_log_loss`, `held_out_accuracy` (classifiers),
`held_out_rmse`, `held_out_mae` (regressors) and `held_out_rows`. Engines with
zetta#1245 or later record them as one-point series at the end
of training, so they appear in `summary` (and in `compare_runs`); older runs have them only in
`get_run_report` and on the model (`list_models` `metrics`). The run and compare pages show them in
a **Held-Out Scores** table, one row per run with the best marked, rather than as curves.

DP Runs retain `metrics_suppressed:"differential_privacy"`. Lake clears their curves in single-run
and comparison responses and omits them from live metric polls; the existing released summary and
artifacts remain available. The UI explains the suppression explicitly.

## Live Reads

`tail_experiment_events` (MCP: `tail_experiment_events`) accepts `kind:"experiment"` or `kind:"run"`
and an exact `id`. It first resolves the record in the selected workspace, then derives only
`study-events.<id>` or `run-metrics.<id>`. Callers cannot supply an arbitrary topic. `wait_ms` is
0–30000 (default 10000); `max_events` is 1–1000 (default 100). The first inclusive sequence defaults
to 1. REST and MCP both support the same bounded asynchronous long poll.

```json
{"customer_id":"account-id","workspace_id":"workspace-id","kind":"run","id":"run-id","from_sequence":10,"wait_ms":10000,"max_events":100}
```
```json
{"events":[{"sequence":10,"id":"event-id","encoding":"json","payload":"{\"step\":10}"}],"next_sequence":11}
```

Continue after the last event in a nonempty batch; use `next_sequence` for an empty batch. This
avoids skipping events when a returned page is smaller than the allocator's current position.
A resolved run or experiment whose ephemeral topic does not exist yet returns an empty batch with
the same cursor. Clients back off and refresh the durable metric and experiment reads to repair
expired-topic
gaps. A missing target record still fails. No new subscription or topic-management operation is
exposed by this capability.

Metric and experiment event topics are engine-owned: Streams treats `run-metrics.*` and
`study-events.*` as browse-only system topics and refuses all message/topic mutations.

## Confirmed Actions

`start_experiment` (MCP: `start_experiment`) forwards the engine's complete experiment definition
(its `create_study` input), with an
additional Lake confirmation field. An explicit typed `search_space` may replace methodology
defaults. Budgets, conditional parameters, samplers, pruners and released DP metrics follow the
engine contract. A `test` validation split is rejected before the workspace call.

`name` and `methodology` are always required. With `from_defaults: true`, Zetta fills the
methodology's parameter ranges and a missing objective (`held_out_auc`, `maximize`, the `validation`
split) and budgets (10 trials, 2 in parallel, 10 minutes, no cost cap), field by field, so
`objective`, `budgets` and each of their fields may be omitted. Without it, Lake refuses a missing
`objective` or `budgets` at `/objective` or `/budgets` before the workspace call (lake#463).

Objective metrics are named by the run report's keys: `held_out_auc`, `held_out_log_loss`,
`held_out_accuracy`, `held_out_rmse`, `held_out_mae`. Zetta accepts older spellings
(`validation.auc`, `ROC-AUC`, `auc`, `log loss`, …) as aliases and stores the canonical name
(zetta#1217).

Every experiment capability passes a Zetta error's `path` through unchanged, so
`{"ok":false,"error":{"code":"invalid_input","message":"required; set from_defaults: true …","path":"/objective"}}`
names the field to repair.

```json
{"customer_id":"account-id","workspace_id":"workspace-id","name":"Retention Tuning","project":"retention","pipeline":"training","dataset":"accounts","methodology":"M-1","objective":{"metric":"held_out_auc","direction":"maximize","validation_split":"validation"},"from_defaults":true,"sampler":"tpe","pruner":"asha","budgets":{"max_trials":10,"max_wall_time_ms":60000,"max_parallel_trials":1,"cost_cap":5},"seed":1138,"confirm":true}
```

Its response is the complete experiment returned by `get_experiment`, with the resolved `spec`,
new `id`, `status:"running"`, initially empty `trials`, zero `spent_cost`, timestamps, empty
`stop_reason` and `promoted_model`.

```json
{"id":"experiment-id","spec":{"name":"Retention Tuning","project":"retention","pipeline":"training","dataset":"accounts","methodology":"M-1","objective":{"metric":"held_out_auc","direction":"maximize","validation_split":"validation"},"search_space":[{"name":"learning_rate","kind":"float","minimum":0.01,"maximum":0.3,"step":0,"log":true,"choices":[],"condition_parameter":"","condition_values":[]}],"sampler":"tpe","pruner":"asha","budgets":{"max_trials":10,"max_wall_time_ms":60000,"max_parallel_trials":1,"cost_cap":5},"seed":1138,"differential_privacy":false,"released_metrics":[]},"status":"running","created":"2026-09-28T18:20:00Z","updated":"2026-09-28T18:20:00Z","trials":[],"spent_cost":0,"stop_reason":"","promoted_model":""}
```

**A workspace methodology's experiment (lake#689).** `methodology` may name one of the workspace's
own methodologies (`W-<8 hex>`). Its experiment searches the parameters its manifest declares;
`dataset` with `target` (and optional `features`), or a `project` and `pipeline` running it, say
what each trial trains on. Lake runs each trial as a training run of the methodology in the Orama
operator's sandbox, metered as any run, and reports its result back; `start_experiment` and
`get_experiment` add `runner` (the trials waiting for a run, each with its `methodology_run`) and
`methodology_runs` (every trial's run and phase), and `stop_experiment` and `stop_experiment_trial`
stop those runs. `stop_experiment_trial` takes an optional `reason`. See
[methodologies.md](methodologies.md#experiments).

`stop_experiment` (MCP: `stop_experiment`) cooperatively stops an experiment and its active trials.

```json
{"customer_id":"account-id","workspace_id":"workspace-id","experiment_id":"experiment-id","confirm":true}
```
```json
{"experiment_id":"experiment-id","status":"stopped","reason":"stopped by caller"}
```

`stop_experiment_trial` (MCP: `stop_experiment_trial`) stops one trial without stopping the
experiment.

```json
{"customer_id":"account-id","workspace_id":"workspace-id","experiment_id":"experiment-id","run_id":"run-id","confirm":true}
```
```json
{"experiment_id":"experiment-id","run":"run-id","state":"failed"}
```

`promote_trial` (MCP: `promote_trial`) asks the engine to promote the registered Model of a complete
trial. It does not copy model weights. The page displays the exact experiment and run and asks for
explicit confirmation before making the request.

```json
{"customer_id":"account-id","workspace_id":"workspace-id","experiment_id":"experiment-id","run_id":"run-id","confirm":true}
```
```json
{"experiment_id":"experiment-id","run":"run-id","model":"model-id","project":"retention"}
```

## Verification and Changes

`Experiments.*` verifies account authorization, confirmation and alias refusal, bounds, DP behavior,
service-credential proxying, audit targets, REST/MCP input and output schema parity, a successful
call per transport per operation, and the agents' guide naming the live read and the confirmation
(`Experiments.AgentDocsNameTheLiveReadAndTheConfirmation`).
`AgentRuns.ExperimentsAgentGrantsEachTurnOnlyItsExperimentOperations` verifies the three turn kinds,
the workspace task URL, confirmed write grants and cross-operation refusal.
`HttpServer/HttpResponseIsolation.ExperimentsAgentRestAndMcpUseTheSameConfirmedTurn` compares
the `agent_turn` REST/OpenAPI and MCP schemas, calls both transports, proves an edited proposal is
the object granted `create_study`, and refuses a stale `proposal_sha256` or a proposal changed
beyond its experiment. `Experiments.LakeSaysExperimentAndTheEngineKeepsItsStudyNames` checks the
translation both ways ([Names](#names)); `ApiNaming.OneNamePerConceptAcrossCapabilities` refuses an
input named `study`, `ApiDescriptions.EveryCapabilitySaysWhatItDoesForAPerson` a description that
says study, `Docs.GuidesSpeakOfExperiments` a guide that does, and the web app's
`frontend/tests/wording.test.ts` a page string that does. The shared
`SchemaConformance.*` fixture validates successful responses using independent JSON Schema
validation. `frontend/tests/experiments.test.ts` (`npm test`, which the frontend build runs)
checks the chart transformations in `frontend/src/pages/experiments/transforms.ts`: EMA,
missing-step alignment, DP suppression, parameter differences and the live read's cursor.

Keep `experiment_schemas.hpp`, registered handlers, these examples and the stateful fake caller
consistent with the authorized engine contract. Run both PostgreSQL-backed native builds and the
token gates. The [UI contract](../ui/experiments.md) records browser checks and deployment limits.

The current engine contract has no `delete_study` or `delete_run`. Deletion requires an upstream
operation and its retention semantics; no local hiding or destructive SQL substitute is defined.
Experiment deletion is explicitly deferred to Zetta #1158.
