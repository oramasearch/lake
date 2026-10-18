# Train and Data Pages

The pages in `frontend/src/pages/train/` (lake#561): Agents and one Agent (training, lake#653), the Agent Factory (served at Design → Agents), Runs, one
Run, Models and one Model (lake#599), and the Data group's Datasets page (`/train/data`) with one Dataset and Query. They follow
the page rules in [web-app.md](web-app.md): a header with the purpose and the primary action,
figures from the reads already made, a toolbar over each list, creation in a drawer or dialog,
details as a split with a side column of facts and actions, and confirmations that name what
happens. A header action that needs the workspace (Load data, New agent) shows only once
`workspace_status` answers ready (`useWorkspaceReady` in `workspace.tsx`).

## What each page shows

| Page | Path | Reads | Layout |
|---|---|---|---|
| Datasets | `/w/<id>/train/data` | `system.datasets` through `workspace_sql`; `list_connections` for Load data's pull | Figures (datasets, run outputs); a searchable table of datasets filtered to Datasets (the default) or Run outputs, each with its database, kind and a Query link; "Load data" opens a drawer with Upload (`upload_dataset`), Generate (`generate_dataset`, `list_dataset_generators`) and From a connection (`create_dataset_from_query`, picking from the existing connections, with a link to Data → Connections to add one). `?loaded=…` shows the upload's result as a banner; `?error=` as a warning. Datasets only: connections have their own page ([connections.md](connections.md), lake#651) and storage is Drives' Storage tab ([drives.md](drives.md#storage), lake#656); an old `?browse=<name>`, `#connections` or `#storage` link is sent there |
| Dataset | `/w/<id>/train/dataset?database=…&name=…` | `get_dataset`, `analyze_dataset`, the first 20 rows through `workspace_sql` | Figures (rows, size, columns, copies); a split: tabs Columns, First Rows and Partitions on the left, the columns in the table's own order (`get_dataset`'s `columns`, then the first rows' answer's `columns`; never sorted, lake#812); the facts and the actions (Query it, Keep durable with `promote_dataset` when a partition has one copy) on the right |
| Query | `/w/<id>/train/query?statement=…&database=…` | `workspace_sql` | An editor card (⌘/Ctrl+Enter runs it) and a results card with the first 500 rows, its headers in the answer's `columns` order (SELECT order), not the rows' sorted keys (lake#812, `pages/train/columns.ts`, tested by `tests/columns.test.ts`); the statement and database stay in the URL |
| Agents | `/w/<id>/train/agents` | `list_agents`, `agent_training_status` per agent | Training and improving the agents that learn (lake#653): only the workspace's agents with something to train (a native agent with a model, an agent granted a pipeline, or a training schedule; `trainable` in `lib/agents.ts`), with figures (agents to train, trained versions, stale, due), a search, and a card each with why it is trainable, its model, versions, best held-out score and whether a better version exists; "Design an agent" opens Design → Agents; an empty state says why a workspace's agents are not listed |
| Agent | `/w/<id>/train/agent?id=…&tab=training\|runs\|schedules` | `get_agent`, `agent_training_status`, `list_agent_launches`, `list_agent_schedules`, `list_experiments`, `usage_over_time` | A split: tabs Training for a trainable agent (figures: held-out score, versions, task success rate and median latency of recent runs; the held-out score over time as a chart; versions with the data each learned from, Promote and Roll back confirmed through `update_agent`; the pipelines it calls; Launch experiment, an `agent_turn` to the Experiments Agent that opens its proposal on Train → Experiments; the training schedule through `set_training_schedule`), Unattended Runs (Run now with `launch_agent`, Stop with `stop_agent_launch`, polled every 5 seconds while one starts) and Schedules for a frontier agent (Add schedule in a dialog, Remove; each schedule's status and message are lake's words, never Kubernetes's, lake#586); the facts, Edit its design (Design → Agent) and its model tokens this month on the right. A frontier agent with no pipeline says it has nothing to train |
| Agent Factory | `/w/<id>/design/agents/new?conversation=…&task=…` (`/train/agents/new` redirects, lake#653) | `get_ask_conversation`, `get_agent_task` | Design → Agents' other way in: a conversation card with the Agent Creator Agent (bubbles, example prompts when empty, the working notice, the composer) beside the proposal card with "Approve and create" (`approve_agent_proposal` with the stored digest), which opens the new agent's Design page, or How It Works before there is one |
| Runs | `/w/<id>/train/runs` | `list_runs`, `list_studies` | Figures (runs, succeeded, failed, in progress); a searchable table filtered by status, with status chips, relative start times and Report or Why it failed, and Charts |
| Models | `/w/<id>/train/models` | `list_models`, `list_studies` | Training, one row per model: the records of one model step of one pipeline (or of one experiment) are its versions (`groupModels`, `pages/train/models.ts`). Figures (models and versions, held-out scores, from experiments and pipelines, serving); a searchable table (name, kind, pipeline, project, experiment, features, ids) filtered by source (pipeline, experiment, standalone, no pipeline), each row with the latest held-out score, the versions and runs that trained it with the latest run and the experiment, the dataset and features it learned from, when it was last trained, and the version serving in Ship. A standalone model says "trained on its own" or "brought from a file", never its internal project. **New model** (header and empty state) opens the New Model dialog ([below](#a-standalone-model)); a note says a model repository's file is brought with it |
| Model | `/w/<id>/train/model?id=…` | `list_models`, `list_studies`, the version's `get_run_report`, the pipeline's `get_graph` | Training one model, at the version `id` names. Figures (held-out score, versions and runs, the version serving); the held-out score over versions as a chart (one per held-out figure measured); the versions table (version, trained, score, run, serving, Compare); the comparison of two versions (each figure, better or worse for held-out ones, and Training curves in Compare Runs); what it learned from (the report's dataset, the features). The side: the facts, the pipeline step or steps it belongs to (`modelSteps`: the model step named as the model, else every model step, or "Uploaded, no pipeline", or the experiment that trained it) or, for a standalone model, the Standalone Model card, Retrain (`run_pipeline` with the model's pipeline and name, or `retrain_model` for a standalone model, then the new run) and Promote to Ship (`expose_model` for this version, `unexpose_model` for the others, behind a confirmation that names them, then Ship → Model). "Serving in Ship" in the header |
| Run | `/w/<id>/train/run?id=…` | `get_run` (every 15 seconds while unfinished), `get_run_report` once finished, `list_traces` | A split: notices, the report (the held-out score as a figure with its caveat, what it learned from, drivers, at risk, caveats, missing, the raw report), lineage, outputs, log and trace on the left; the facts on the right; Training charts and Workspace streams in the header |

## A standalone model

lake#654. A model made on its own, not as a project pipeline's step (`docs/api/models.md#a-standalone-model-lake654`).
**New model** on Train → Models opens `NewModelDialog` (`pages/train/NewModel.tsx`), a `Modal sheet`
(full screen on a phone) with two choices:

- **Train one**: Name, Dataset (`list_datasets`), Methodology (the runnable ones of
  `list_methodologies`, M-1 first), Column to predict (the dataset's columns; "None" when the
  methodology's `label` input is not required) and Held-out share (0.2 by default, `20%` taken).
  The name, dataset, methodology, target and share are checked before anything is sent
  (`nameProblem`, `needsTarget`, `holdoutOf` in `models.ts`). `train_model`, then the new run's page.
- **Bring one**: This computer (`.onnx`, `.json`, `.ubj` or `.txt`; `register_model` with the
  file as `weights_base64`), A drive (**Choose a file** opens the drive picker,
  [drives.md](drives.md#the-picker), over every drive of the workspace from `list_drives`, of any
  kind, with folders and search; it lists files and shortcuts to a file, at any depth;
  `load_drive_model` with the drive and the file's key, lake#682) or A model repository
  (repository, path, revision; `resolve_model_file`, then `register_model` with its
  `content_base64`), a Name, Columns (optional, comma-separated, sent as `feature_names`; its help asks for them when the file is `.onnx`, which
  names none; a column named twice is refused before sending, `featureNamesProblem`) and an
  optional held-out AUC measured where it was trained. A banner says what can be brought (ONNX,
  XGBoost, LightGBM or Lake's own weights) and what is refused (lake#681). Then the model's page.

Train → Model, for every model, has a **Weights** card under Facts (lake#681): **Download** calls
`export_model` for the selected version with `format: "zetta"` and saves the answer as its
`file_name` (`weightsFile` in `models.ts` decodes it); **As ONNX** does the same with
`format: "onnx"`, shown when the version is a tree model whose objective is `binary_logistic` or
`squared_error` (or, with no objective recorded, a classifier or regressor; `exportsAsOnnx`). A
refusal shows in the card.

Train → Model for a standalone model:

- The Standalone Model card replaces the pipeline: what it trains on (dataset, the column it
  predicts, methodology, held-out share, from its pipeline's `standalone_training` through
  `get_graph` by the project's id), or for a brought one where its file came from.
- The facts never name its project.
- **Retrain** calls `retrain_model`. **New version** opens `RetrainDialog` (dataset, column and
  held-out share; the methodology is fixed; only what changed is sent), or for a brought model
  `BringDialog` with `versionOf` (the next file, `register_model`'s `version_of`).
- **Move into a project** (a confirmation with the project's name, the model's by default) calls
  `move_model_to_project` and opens the project.
- **Delete** (a confirmation that needs the model's name typed) calls `delete_model` for every
  version; the engine removes the model's project with its last. Ship → Model's Delete stays one
  version.

Ship → Models and Ship → Model serve a standalone model as they serve a project's: the same
versions, publishing, `predict` and traffic; its source reads "trained on its own" or "brought
from a file". An engine without these capabilities answers `not_supported`, which the dialog shows
as it is.

`frontend/checks/standalone-models.mjs` screenshots each of these at 1440 and 390 wide against
`npx vite`, every `/v1` call mocked, and reports sideways scrolling, missing text, page errors and
what Train one sent to `train_model`.

`workspace.tsx` holds the calls and helpers the pages share (`wq`, `wa`, `useWq`, `useSpaceRead`,
`timeAgo`, `useWorkspaceReady`); `agents.tsx` what the agent pages share (the named engine reads,
the definition form `AgentFields`, offering the runtime's skills from `agent-skills.json`), over the pure rules in
`frontend/src/lib/agents.ts`; `datasets.tsx` the lean dataset list and `ObjectTable`; `RunsPage.tsx` the run and experiment
helpers the Run and Models pages share (`useExperimentRefs`, `TrialSubject`).

## Run Report Evidence and Load Data (lake#581, lake#582)

- **Most at Risk** shows each ranked row's rank, entity and risk, and its evidence in words: the
  engine's evidence columns as "feature = value" chips and the row's `reason_codes` as its top
  reasons with their effect ("tenure_months raises risk by 0.42"). Nothing there is raw JSON; the
  whole report stays under **The raw report** (`evidenceOf`, `pages/train/evidence.ts`).
- **Load Data** keeps its primary action (Upload, Generate or Pull dataset) in the drawer's
  footer beside Cancel, as every other drawer does; a tab with nothing to act on (no generators,
  no connections) shows none. Upload picks its file with the kit's `FilePicker` (`lib/ui.tsx`): a
  dashed zone to press or drop a file on, which then shows the file's name and size.

## Deleting a Dataset or a Run (lake#645)

Data → Dataset has **Delete dataset** in its Actions card, confirmed by typing the dataset's name;
the dialog lists its rows, size and the search indexes built on it (`delete_dataset
{dry_run: true}`). Train → Run has **Delete** in its header once the run has finished, confirmed by
the first eight characters of its id; the models it trained are kept (`delete_run`). Both go back
to their list. An older workspace engine answers that it cannot delete yet, which the dialog shows.
See [deletion.md](deletion.md).

## How to verify

`npx tsc --noEmit -p .`, `npm test` and `npm run build` from `frontend/` (the build also runs
`check-effects` and `check-list-errors`). To look at the pages without a lake, serve them with
`npx vite` and answer `/v1/*` from Playwright's `page.route` with fixtures for the reads above
(`workspace_query` answers by its `capability`). `tools/e2e/storage.mjs` drives the Storage card
on a live lake.

## How to change

Keep the reads, actions, URL parameters and guards above: the typed name before `delete_store` and
`delete_agent`, the 16 MiB upload limit and the dataset name pattern, the whole-number row limit of
a pull, the 1–50 clamp of a generator's variants, the consent-required message of a governed
workspace, the report asked for only after a run finishes, and never `path` on
`generate_dataset`.
