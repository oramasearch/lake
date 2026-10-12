# Methodologies: Orama's and Your Own, in Python

**Status: implemented 2026-10-11 in the source tree (lake#655, migration `0078_methodology_runs`);
not deployed. Drafts written by Ask Orama (lake#688, no migration) need agent-ask-orama with its
`methodology_draft` turn; not deployed.** It needs Zetta with zetta#1261 (zetta PR #1265), the Orama operator with
orama-operator#291 (operator PR #292) and a published `oramasearch/methodology-runtime` image
(methodology-runtime PR #2). A workspace has two kinds of methodology:

- **Orama's**: the engine's catalogue, M-1 to M-21, built into the engine as native code. They are
  **read-only**: Lake refuses to edit or delete them, whoever asks. Each can be duplicated.
- **Its own**: methodologies written in **Python**, kept in one of the workspace's repositories, so
  they are versioned, diffable and reviewable like any other code. The engine registers them next
  to the catalogue as `W-<8 hex>`; they run as **untrusted code**, in their own container.

The engine never runs the Python (Zetta's ADR-17 stands): a workspace methodology runs in Lake's
methodology runner, the way a Python agent runs outside the engine.

## The interface

A methodology is a folder of a repository with three files.

`methodology.json`, the manifest (interface 1):

```json
{
  "interface": 1,
  "name": "Late payers",
  "description": "Which customers will pay their invoice late.",
  "entrypoint": "methodology.py",
  "inputs": {
    "kinds": ["tabular"],
    "target": {"required": true, "kind": "binary"},
    "required_features": []
  },
  "parameters": [
    {"name": "learning_rate", "kind": "float", "default": 0.1, "minimum": 0.001, "maximum": 1, "log": true},
    {"name": "epochs", "kind": "int", "default": 200, "minimum": 1, "maximum": 5000}
  ],
  "metrics": ["auc", "log_loss", "accuracy"],
  "explain": true,
  "packages": "requirements.lock",
  "resources": {"cpu": "1", "memory": "2Gi", "gpu": 0, "timeout_seconds": 3600}
}
```

- **What it needs:** `inputs.kinds` (one or more of `tabular`, `text`, `time_series`, `graph`,
  `image`, `audio`, `document`), `inputs.target` (`binary`, `multiclass`, `regression`, or `none`
  with `required: false`) and the features it requires.
- **Its parameters**, each with a kind (`int`, `float`, `categorical`, `bool`), a default and its
  bounds or choices: what a run may set, and what an experiment would search over.
- **Its metrics**, as base names. Every run reports each twice: `in_sample_<name>` on the rows it
  trained on and `held_out_<name>` on the rows it did not (`holdout_fraction`, 0.2 by default).
- **Its resources**: CPU, memory, GPUs (and `gpu_class`) and a time limit, up to 16 CPUs, 64 GiB,
  8 GPUs and 24 hours.

`methodology.py` (the entrypoint) defines, at module level:

| Function | Answers |
|---|---|
| `fit(data, params)` | the artifact: `bytes`, or any object that pickles |
| `predict(artifact, data)` | one prediction per row |
| `score(artifact, data)` | a number for every metric in the manifest |
| `explain(artifact, data)` | each row's feature contributions (when `explain` is true) |

`data` is the runtime's `orama_methodology.Data`: `rows` (a list of objects), `columns`, `target`
(a column name or `null`), `features`, `X()` (feature values row by row) and `y()` (the target's).

`requirements.lock` lists the packages it installs, each pinned `name==version` with at least one
`--hash=sha256:<64 hex>` (what `pip-compile --generate-hashes` writes); no index options, links or
local paths. An empty file means the standard library only. The image installs it with
`--require-hashes --no-deps`, wheels only, when it is built, so nothing is installed while it runs.

## Capabilities

All take `customer_id` and `workspace_id`. Reads need `read` on the account and actions `write`; a
token or an agent's credential that carries `workspace_query` may call the reads, and one that
carries `workspace_act` the actions (`granted_by_scope`), so agents use them through their skills.

| Capability | Kind | Input | Output |
|---|---|---|---|
| `list_methodologies` | read | (the engine's, lake#650) | every methodology, each marked `source` (`orama` or `workspace`) and `editable`; the workspace's own after Orama's |
| `get_methodology` | read | `id`, `include_source?` | `{methodology}`; with `include_source`, a workspace methodology's `source {files, findings, repository_id, revision, path}` |
| `draft_methodology` | read (also POST) | `description`, `name?`, `target_kind?`, `task_id?` | `{draft: {name, drafted_by, template?, reason, manifest, files, findings?, orama?}}`; nothing is saved |
| `create_methodology` | action | `name`, then `files` (`{path, contents}`), or `description` (and `target_kind?`) to start from a draft, or `repository_id` (`path?`, `revision?`) of code already in the workspace; `network?` | `{methodology, findings, ready_to_validate}` |
| `update_methodology` | action | `id`, then any of `files` (`{path, contents}` or `{path, delete: true}`), `message`, `name`, `description`, `revision`, `network` | `{methodology, findings, ready_to_validate}` |
| `delete_methodology` | action | `id` | `{deleted: true, repository_id}` |
| `duplicate_methodology` | action | `id`, `name?` | `{methodology, findings}`, the new copy |
| `validate_methodology` | action | `id`, `dataset?`, `target?`, `features?` | `{findings, ready, run}` |
| `run_methodology` | action | `id`, `dataset`, `target?`, `features?`, `params?`, `holdout_fraction?`, `project?`, `graph?` | `{run}` |
| `list_methodology_runs` | read | `methodology_id?`, `limit?` | `{runs}`, newest first |
| `get_methodology_run` | read | `run_id` | `{run}` with its result, model and log |
| `stop_methodology_run` | action | `run_id` | `{run}` |
| `methodology_run_input` | read (also POST) | `run_id`, `offset?`, `limit?` | the runner's: `{spec, rows, next_offset, total_rows}` |
| `methodology_run_output` | action | `run_id`, `result`, `artifact_base64?`, `log?` | the runner's: `{recorded, model_id}` |

The last two answer only the run's own credential (below); a person's call is `unauthorized`.

### Creating one

`create_methodology` with a name and a description drafts the files and commits them to a new
private repository, `methodology-<name>`, in the workspace:

`POST /v1/create_methodology`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "name": "Late payers", "description": "Which customers will pay their invoice late?"}
```

With `files`, it commits those instead; with `repository_id`, it registers code already in that
repository (its manifest must read) and creates nothing. The engine registers it as not yet
validated. `findings` says, without running anything, what Validate will refuse.

### Drafts

A first version comes from **Ask Orama**, written for the description itself (lake#688), or from
Lake's own templates. Nothing is saved either way: the person reads the files, and Create commits
them.

**Ask Orama's draft** goes through the agent path every page uses, Lake's agent bus:

1. `agent_turn` with `agent: "agent-ask-orama"`, `kind: "methodology_draft"`, the description as
   `message` and `context: {name?, target_kind?}`. It answers `{task_id, status}` at once. The turn
   carries Ask Orama's usual key (every read, no action), so it writes nothing; its model tokens are
   counted in the workspace like any turn's.
2. Ask Orama answers `methodology.json`, `methodology.py` and `requirements.lock` (agent-ask-orama
   `docs/methodologies/`): the standard library when it can, otherwise each package pinned
   `name==version` with `--hash=sha256:` for every wheel the Python package index has for that
   version, which it looks up rather than writes. It checks them itself and gets one more round to
   fix what fails.
3. `get_agent_task` follows the turn; once it has finished, `draft_methodology` with the same
   `description` and the turn's `task_id` reads its answer and runs **the checks Validate runs
   first** (the manifest reads, the entrypoint defines `fit`, `predict` and `score`, every lock line
   is pinned and hashed; file paths inside the folder, at most 64 files of 1 MiB). A draft that
   passes is offered: `drafted_by: "orama"`, its `task_id`, the agent's `reason` and the passing
   `findings`.

```json
{"draft": {"name": "Late payers", "drafted_by": "orama", "task_id": "<task id>",
  "reason": "A logistic regression fits a yes-or-no outcome.", "manifest": {"interface": 1, "…": "…"},
  "files": [{"path": "methodology.json", "contents": "…"}, {"path": "methodology.py", "contents": "…"},
            {"path": "requirements.lock", "contents": ""}],
  "findings": [{"check": "manifest", "ok": true, "…": "…"}]}}
```

A draft that fails the checks, or a turn that failed or answered no draft, is never offered: the
answer is Lake's template (below) with `orama: {task_id, offered: false, reason, findings}`, the
checks that failed among `findings`. A turn still working is `conflict` ("Ask Orama is still writing
the draft"); a task that is not an Ask Orama `methodology_draft` turn is `invalid_input` at
`/task_id`.

**Lake's templates** (`drafted_by: "template"`, and `draft_methodology` without `task_id`) use the
standard library only, so the lock file is empty: a yes-or-no outcome (logistic regression), one of
several categories (multinomial logistic regression), an amount (linear regression), or groups with
no target (k-means). The description picks the template (it names groups or segments, an amount,
several categories, or otherwise a yes-or-no outcome), or `target_kind` does. The New Methodology
drawer offers one when Ask Orama is not running on the Lake (`list_agent_library`), and says so.

### Orama's stay read-only

`POST /v1/update_methodology`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "id": "M-1", "name": "Mine now"}
```

```json
{"ok": false, "error": {"code": "forbidden", "path": "/id", "message": "M-1 (Tabular supervised learning) is one of Orama's methodologies, which are read-only and cannot be edited. Duplicate it (duplicate_methodology) to make an editable methodology of your own."}}
```

`delete_methodology` of an `M-n` answers the same. The engine refuses it too (zetta#1261).

### Duplicating

`POST /v1/duplicate_methodology`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "id": "M-1"}
```

A copy is a new methodology of the workspace's own, in a new repository, named `name` or "<name>
(copy)". From one of the workspace's own, every file is copied, its Python source included. From
one of Orama's, which run natively and have no Python source, the copy carries its description
(what it makes, needs and is measured by), the ranges its parameters are tuned over, and the template for its
kind of target; `duplicated_from` names where it came from.

### Editing

Code is edited in its repository: `update_methodology` with `files` commits them on `main` (the
page's editor does this), and with `revision` follows code pushed another way. A new `name` or
`description` is written to `methodology.json`, so the code stays the one source of what the
methodology is. A new revision must be validated again before it runs; the engine resets its
validation. `network: true` lets its runs reach the internet; it is off by default.

## Validate

`POST /v1/validate_methodology`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "id": "W-0a1b2c3d"}
```

Validate checks the code at its current revision in two steps:

1. **Without running anything**: the manifest reads (each missing or wrong field is a finding:
   `manifest.inputs.kinds`, `manifest.metrics`, `manifest.parameters[0]`, …), the entrypoint
   defines `fit`, `predict` and `score` (and `explain` when declared), and every lock line is
   pinned and hashed. Any failure is answered at once in `findings`, recorded on the engine's
   entry as `failed`, and nothing runs (`ready: false`, `run: null`).
2. **In its sandbox**: Lake builds its image (once per revision) and queues a `validate` run on a
   sample: 200 rows of `dataset` when one is given, otherwise a generated sample with three
   numeric features, a text column and a target of the manifest's kind. The runtime imports the
   code, checks the interface, fits, predicts and scores in sample and held out, and reports
   every finding. The run's result marks the methodology `passed` (usable) or `failed`.

Only a methodology whose current revision passed is **runnable**: `run_methodology`, a pipeline
step and the Build a Pipeline picker use it; anything else answers `conflict`, "validate this
methodology first".

## Runs

`POST /v1/run_methodology`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "id": "W-0a1b2c3d", "dataset": "sales.payers", "target": "late", "params": {"epochs": 50}}
```

A training run reads `dataset` (`<database>.<table>`, at most 1,000,000 rows), with `features`
defaulting to every column but the target, and registers the model it trains in the workspace
(kind `python:<id>`, the artifact as its weights, its `in_sample_` and `held_out_` metrics), so it
appears in Train → Models. `params` are checked against the manifest's parameters (a wrong one is
`invalid_input` at `/params/<name>`); defaults fill the rest.

### It runs as untrusted code

Every run, Validate's included, is one `methodology_runs` row (migration `0078`, PostgreSQL holds
only dispatch: ids, images, resources, phases). The Orama operator runs it (orama-operator#291) in
a namespace of its own with Pod Security `restricted` and a default-deny network policy, as three
pods in turn, sharing one volume:

| Pod | Image | Holds | Network |
|---|---|---|---|
| stage | the pristine `oramasearch/methodology-runtime` | the run's credential | Lake only |
| run | the methodology's image (the runtime with its files and locked packages) | **nothing**: no Secret, no service account token, no service links, no Lake or Zetta environment | **none**, DNS included; with `network: true`, the public internet but never cluster, private or metadata addresses |
| collect | the pristine runtime | the run's credential | Lake only |

The run pod runs as a non-root user with a read-only root filesystem, all capabilities dropped, the
runtime's seccomp profile and its CPU, memory and GPU requests equal to its limits, and is killed at
its time limit. The runtime refuses to import the code at all when it sees a credential in its
environment or a token file, or reaches the network it was not granted (methodology-runtime's
`sandbox.credentials` and `sandbox.network` findings).

**Data goes in and the result comes out only through Lake.** The operator generates a credential
`lmr.<run id>.<secret>` for the stage and collect pods and writes only its SHA-256 to
`methodology_run_status`; Lake resolves it by comparing digests in constant time. It acts as no
person, reaches only `methodology_run_input` and `methodology_run_output` for its own run (and a
pull of its own image), and stops resolving when the run ends:

- `methodology_run_input` answers the run's spec (mode, parameters, target, features, holdout,
  seed, network, manifest) and its rows, 5,000 at a time, read from the workspace through the
  engine (`SELECT … LIMIT … OFFSET …`) or generated for Validate.
- `methodology_run_output` takes the result once (a second is `conflict`, "already recorded"):
  its status, findings, metrics (only numbers named `in_sample_` or `held_out_` are kept; others
  are listed in `dropped_metrics`), row counts, the artifact (base64, at most 24 MiB) and the last
  64 KiB of the log. Validate's outcome is written to the engine's entry; a training run's model is
  registered with the engine's `record_methodology_run`.

### Where a run lives

Its dispatch row and the operator's status are in PostgreSQL; what it read and produced (dataset,
target, features, parameters, result, metrics, log, model) is a sealed `lake_methodology_runs`
record in the workspace Zetta, found by the account and methodology. A run's phase is the operator's
(`Building`, `Staging`, `Running`, `Collecting`, `Succeeded`, `Failed`, `Stopped`), with a message in
Lake's words while it waits. A run the operator has not reported on within 30 minutes (its image
build included) is withdrawn with the reason.

### Metering

The operator reports the run pod's resources in `workload_resources` as `methodology_run`, kept
after it finishes. Lake charges each finished run once, its CPU, RAM, GPU and GPU memory for the
time the code ran (`started_at` to `finished_at`), in the run's workspace, as for builds and agent
jobs ([metering.md](metering.md#resources)).

## A pipeline step

A project's pipeline can use a runnable workspace methodology: Build a Pipeline lists it, and
`compile_pipeline`/`compile_intent` with `methodology: "W-…"` builds its shape (read the table,
`python_methodology`, register the model). The engine's Verification refuses one that is not
runnable. When the pipeline runs (`start_run`, `workspace_act` `run`, or `run_pipeline`), the
engine answers `runner_required` with what to run, and Lake dispatches it to the runner as a
training run of that project and pipeline; the answer is `{runner: "lake", status: "queued",
methodology, methodology_run}`. The engine records the Run and its lineage when the result comes
back.

## What is not built yet

- Experiments over a workspace methodology's parameters: the engine lists it with `studies: false`
  (lake#689).
- Serving a workspace methodology's model from Ship → Models: its predictions need the runner, so
  the engine refuses to deploy it (lake#690).
- Artifacts larger than 24 MiB (lake#690).

## Errors

| Code | When |
|---|---|
| `forbidden` | an edit or delete of one of Orama's methodologies |
| `conflict` | a run of a methodology not validated for its current code; a second result; its repository is gone |
| `invalid_input` | a manifest that cannot be read; a file path outside the folder; a parameter of the wrong kind; a dataset not `<database>.<table>` |
| `not_found` | no such methodology or run in this workspace |
| `store_unavailable` | `LAKE_METHODOLOGY_RUNTIME_IMAGE` is not configured, or no repository store |
| `unauthorized` | the runner's capabilities called with anything but that run's credential |

## Configuration

| Variable | Meaning |
|---|---|
| `LAKE_METHODOLOGY_RUNTIME_IMAGE` | the `oramasearch/methodology-runtime` image methodologies are built from and staged with; the operator sets it from `spec.methodologies.runtimeImage` |
| `LAKE_INTERNAL_URL` | the in-cluster Service the image build pushes to (as for app builds) |

## How to verify

```sh
ctest --test-dir build -R 'Methodologies|MethodologiesAreTheWorkspacesOwn|AskOramaDraftsAMethodology|SchemaConformance|Docs'
(cd frontend && npm test)
```

The New Methodology drawer's Ask Orama in a browser at 1440 and 390 px (while it writes, its draft,
a draft Lake refused, and a Lake without Ask Orama), against `npx vite --port 5174` with every `/v1`
call mocked: `PLAYWRIGHT_FROM=<a package.json with playwright> node frontend/checks/methodology-draft.mjs`.

`AgentRuns.AskOramaDraftsAMethodologyThatLakeChecksBeforeOffering` covers Ask Orama's
`methodology_draft` turn and `draft_methodology {task_id}`: a passing draft offered as Ask Orama's, an
unhashed pin and a missing `score` refused with the findings and the template, a path outside the
folder, a failed turn, another agent's task, and the template without a task.
`Methodologies.*` cover the manifest, lock and entrypoint checks, drafts and copies of Orama's, the
Dockerfile, the sample and every template run through the interface with python3.
`WorkspaceRecordStores.MethodologiesAreTheWorkspacesOwnAndOramasStayReadOnly` (memory and
PostgreSQL) covers create, read-only, duplicate both kinds, Validate refusing a broken one and
passing a good one, the runner's credential reaching nothing but its own run, the result accepted
once, training registering a model, the pipeline hand-off, edit and delete.

## How to change it

The pure parts are `src/control/methodologies.cpp` (manifest, checks, drafts) and
`src/control/methodology_templates.cpp` (the Python templates); the capabilities are
`src/api/methodology_capabilities.cpp`; the dispatch rows are in `db/schema.sql` (`0078`) and both
stores. The runner's contract is shared with `oramasearch/methodology-runtime` (`docs/cli.md`) and
the operator (`docs/lake/methodology-runs.md`): change all three together.
