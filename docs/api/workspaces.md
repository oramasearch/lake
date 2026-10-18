# Workspaces: Design, Train and Ship on Zetta

Lake's Design, Train and Ship pages delegate to workspace Zettas. A customer can have many
workspaces; each data-plane call resolves an explicit `workspace_id` (`workspaces.id`). It never
resolves a default implicitly. Zetta has no tenancy within one cluster: its list operations return
that workspace's data. Provisioning uses workspace desired state; a missing or
unready workspace prevents the feature, never redirects data to Lake's bus or PostgreSQL.
The operator contract is [workspaces](../control-plane/workspaces.md), and the cutover/audit is in the
[workspace data-plane design](../architecture/workspace-data-plane.md).

Page paths below have the canonical prefix `/w/<workspace_id>`. The global top-bar selector
lists the account's workspaces, including a sole preselected workspace, and offers New workspace.
Switching preserves the page and filters. Old GET URLs redirect using the validated remembered
workspace cookie or the account's only workspace; otherwise they show a selection page. Forms
carry the ID explicitly and reject a mismatch with the URL. Person-scoped REST and MCP schemas
require it too. A library agent's task credential is the exception: Lake derives the workspace
from the immutable task row when the client omits it and rejects a supplied mismatch.

## What a person sees

| Section | Page | Zetta capabilities behind it |
|---|---|---|
| Design | `/design/projects`, `/design/project?name=` | `list_projects`, `get_project`, `create_project`, `delete_project` (typed confirmation; the project's runs and models stay listed), `capture_intent` then `compile_intent` (build a pipeline from what the project should predict), `approve_plan` (approve a proposed pipeline), `gate` then `run` (check and run an approved pipeline) |
| Design | `/design/solutions`, `/design/solution?id=`, `/design/project?name=` | Orama's solution catalogue beside the workspace's own solutions ([solutions](solutions.md), lake#597), and a solution project. **Start this solution** collects a project name, capability level, dataset, delivery destination and owner, and the customer's value per outcome, investment and currency. A `solution:<project>` Project Agent turn proposes the snapshot, dataset and methodology roles, and policy predicate evidence. The editable proposal is shown before `confirm_solution` calls `create_solution`. The project then shows solution stages and a bounded Runs table with status, stage, error and full-run links. Active runs refresh the page every three seconds. Policy, experiment, monitor and ROI have readable summaries with JSON disclosures; the correctness certificate leads with its verdict, groups applicable checks and places other checks under **Show all**. `run_solution`, `update_policy`, `approve_decision` and `record_outcome` are all workspace actions through lake's authorization and audit path |
| Design | `/design/pipeline?project=&graph=&step=` | **The pipeline page** (docs/design/zetta-parity.md): `get_graph` drawn as SVG in stage lanes with each step coloured by the latest run; a step inspector; every edit through `edit_pipeline`; **Ask the Project Agent** (a change request answered with a proposal: the edits in words, the graph after, run settings, and *Apply* as one `edit_pipeline` save, or *Discard*; a question or absent proposal instead gets a **Reply** box that continues the same Project Agent conversation; lake#249. While the turn works the page reloads itself every three seconds with a script, because a declarative refresh to its own `#proposal` URL is only a fragment jump (the Experiments Agent's working panel, reviewed at `#experiment-proposal`, does the same every two seconds); a proposal that cannot be applied lists each reason as one plain sentence, never a status code or JSON body, followed by the next step; lake#438); `approve_plan`; *Check* (`gate`, every check with its reason); *Check and run* (`gate` then `run` with a chosen dataset, a hold-out of 0.2 by default and a model name defaulting to `<project>_<pipeline>`); *Deploy* (`deploy`, with its gate); the pipeline's runs. The project page adds an empty pipeline (`save_graph`), *Import from n8n* (`import_n8n` → `compile_n8n`) and *Build from Python* (`compile_python`) |
| Design | `/design/agents`, `/design/agent?id=` or `?key=`, `/design/agents/new` | **Agents**, where agents are designed ([`train-agents.md`](train-agents.md), lake#653): Orama's (`list_agent_library`, read-only, Duplicate to edit) beside the workspace's own (`list_agents`, `get_agent`), New agent and Edit (`create_agent`, `update_agent`, each kept as a revision, `list_agent_revisions`), Restore, Duplicate, `delete_agent` (typed confirmation), and the Agent Factory |
| Train | `/train/agents`, `/train/agent?id=` | **Agents**, training the agents that learn ([`train-agents.md`](train-agents.md)): `list_agents` filtered to the trainable, `agent_training_status`, `set_training_schedule`, promote and roll back (`update_agent`), Launch experiment (`agent_turn` to the Experiments Agent), and the agent's runs and schedules; `agent_run`, `chat_with_agent` (a `write` in lake, `GET` to Zetta) |
| Data | `/connections` | Data → Connections (lake#651, [../ui/connections.md](../ui/connections.md)): `list_connections`, **Add connection** (`create_connection`, Postgres or Snowflake), **Delete** with the typed name (`delete_connection`), **Connect one of your services** (`connect_service`: the account's own Ready Postgres services in one click, no password typed or shown; other kinds say *not supported yet*), each connection's kind, host, service, status (from its `credential`) and created date, and *Tables reachable* (`list_source_tables` per connection) |
| Data | `/connections/connection?name=` | One connection: `list_source_tables` with columns, without pulling anything, and **Load into a dataset** (`create_dataset_from_query`, one read-only `SELECT`). `/train/data?browse=<connection>` redirects here |
| Train | `/train/data` | A projected `workspace_sql` dataset list (`SELECT database, dataset FROM system.datasets`, annotated with `name`, `id` and `kind`; engine results grouped under *Results*, lake#447); **Pull a dataset** (`create_dataset_from_query` from one of the saved connections with one read-only `SELECT`), **Upload a file** (`upload_dataset`: one CSV or JSONL file of at most 16 MiB) |
| Train | `/train/dataset?database=&name=` | `get_dataset` (id, partitions, copies, health, the loaded format (`format`, lake#594) labelled *Stored as*), its first 20 rows (`workspace_sql`), column types (`analyze_dataset` over 256 sampled rows, plus the value type in the first rows), *Keep durable* (`promote_dataset`, offered only when a partition has one copy), *Query it* |
| Train | `/train/query?statement=&database=` | Read-only SQL over the workspace's data (`workspace_sql`) |
| Train | `/train/data`, *Generate a dataset* | `list_dataset_generators`, `generate_dataset` (never its `path`, which writes a file on the node) |
| Train | `/train/data` (layout) | **Load Data** first and open (upload a file, pull a table from a connection, generate one); after an upload, *Loaded N rows from your CSV file into <dataset>* with a link, and *<name> was already taken, so it was saved as <name>_2* when the person chose a rename (lake#447). Then datasets; connections are on Data → Connections, and storage (only what can be created today; the rest one *coming later* line) is Data → Drives → Storage (lake#656). A dataset page shows its id, a Columns table with types, its first 20 rows (`LIMIT 20`, also bounded by Lake) and *Storage details* under a disclosure; the Query page shows at most 500 rows (lake#256, lake#257) |
| Design | `/design/project` → Build a Pipeline | *From data* (optional): after `compile_intent`, lake points the first connect step at the chosen dataset (`set_attribute` source/table), removes the other connect steps and opens the pipeline with that dataset preselected for the run (lake#251) |
| Train | `/train/runs`, `/train/run?id=` | A run page leads with what the run says about itself: *Nothing was trained* for a structural run, and each connect step that was not read (lake#251, lake#253, zetta#1022); the report's *What It Learned From* shows *trained on* and *columns not used*. `list_runs` carries the pipeline and project names shown by the list, plus status and start time, without project lookups. The detail page uses `get_run`, `get_run_report` rendered as tables (what it learned from, what it leans on, most at risk, caveats, what it cannot tell), and `list_traces` as a waterfall |
| Train | `/train/models`, `/train/model?id=` | Training a model (lake#599): one `list_models` read, its records grouped into models by model name, pipeline and project (or experiment), each record a version. A model's page reads its version's `get_run_report` (what it learned from) and its pipeline's `get_graph` (its model steps); Retrain is `run_pipeline` with the pipeline and `model_name`; Promote to Ship is `expose_model` for the version and `unexpose_model` for the others ([../ui/train.md](../ui/train.md)) |
| Ship | `/ship/models`, `/ship/model?id=` | Serving a model. One `list_models` record per model carries name and short id, project and pipeline names, run id, score, feature schema, creation time, and published state. The list issues no per-model `get_model` calls. Its action remains `expose_model` / `unexpose_model` by id, and each published model shows a `curl` for `POST /v1/predict` with the page's own `customer_id` and `workspace_id` filled in (lake#250, lake#255, lake#439). Calls, errors and latency come from `model_traffic`; a model's page adds the try-it `predict`, predict keys (`create_token` scoped to `predict`) and monitoring (lake#599, [../ui/ship.md](../ui/ship.md)) |
| Ship | `/ship/indexes` | `list_indexes`, *New index* (`create_index` over a dataset, BM25F full-text). The form's dataset choices load after the page from the projected `system.datasets` read (lake#446) |
| Ship | `/ship/index?id=&q=&field=&top_k=` | `get_index`; *Try a search* (`search`); *Publish* / *Unpublish* (`expose_index` / `unexpose_index`); and, once published, **Call it from your code**: *Create a search key* (a lake API token scoped to `search_index` only) and copy-ready `curl`, JavaScript and Python calls to `GET /v1/search_index` naming the page's `workspace_id` (lake#439), plus the MCP tool of the same name |
| Ship | `/ship/packages` | lake's own registries, not the workspace (`docs/api/registry.md`) |
| Ship | `/deployments` | lake's own platform deployments (`docs/api/platform.md`) |

Every page takes `customer_id` and defaults to the viewer's personal account; a selector lists the
customers the viewer may act for, with their own labelled *Your workspaces*. Until the
customer's workspace is ready, the page offers **Start your workspace** instead, then shows it being
prepared and refreshes every 15 seconds.

The Workspaces page (`/workspaces`) shows name, environment, phase, namespace and created time.
New workspace accepts a name and DNS-label-safe environment. Delete opens a confirmation
(`/workspaces?delete=<workspace_id>`) that lists everything the deletion destroys, with counts,
and requires typing the exact name ([Deletion](#deletion)). Pending and stopped rows are
selectable. A sole workspace is still shown in the top bar.

## Capabilities

All capabilities are in lake's registry, so REST, OpenAPI and MCP have them too. Each resolves the
customer's policy object and is authorized by lake before anything reaches Zetta.

| Capability | Kind | lake action on the customer | What it does |
|---|---|---|---|
| `workspace_status` | read | `read` | `{workspace, requested, phase, message, ready}` |
| `list_workspaces` | read | `read` | Requires `customer_id`; lists contract rows, excluding completed deletions |
| `get_workspace` | read | `read` | Requires `customer_id`, `workspace_id`; returns desired state and operator observations, including `storage_gi` (the desired size of each of its Zetta's volumes, in GiB, default 5) and the operator's `storage_gi_observed`, `storage_phase` and short `storage_reason`; the operator's `storage_message` only for the operator and the instance administrator (migration `0069`; only the operator and the instance administrator change the size, with `set_workspace_storage`, [volumes.md](volumes.md)) |
| `create_workspace` | action | `write` | Requires `customer_id`, `name`, `environment`; atomically creates workspace and primary service desired state. `environment` is a lowercase DNS label of up to 47 characters, usually `dev`, `staging` or `prod` (the schema's `examples`); the operator uses it only as the namespace prefix and the `orama.cloud/environment` label, so it is not an enum (lake#594) |
| `update_workspace` | action | `write` | Requires `customer_id`, `workspace_id`; optional name, environment, desired (`running` or `stopped`); changes increment generation, status fields are refused |
| `delete_workspace` | action | `admin` | Requires `customer_id`, `workspace_id`, `confirm` equal to the workspace name; deletes the workspace and everything it holds ([Deletion](#deletion)). `dry_run: true` deletes nothing and answers what would be destroyed, with counts (`confirm` optional, checked when given) |
| `start_workspace` | action | `write` | Idempotent creation of the account's first workspace, named after the account's handle (lake#638; one created earlier as `workspace` keeps that name and is found again), in `prod`; answers its state with its `workspace_id`. It uses the same contract table and never routes data implicitly: later calls name a workspace, and `create_workspace` adds more |
| `predict` | action (POST, changes nothing) | `read` | Score rows through one **published** model from outside lake (lake#250): `{customer_id, workspace_id, model_id, rows, entity_column?, top_k?}` (`model` is the deprecated name of `model_id`); checks the model is published (`get_model`), then Zetta's `score`. Each `ranked` entry is `{entity, row, risk}`: `row` is the record's position in `rows`, and `entity` the value of its `entity_column` field (default `entity`), omitted when the record has none (lake#450). The engine numbers the rows itself; lake maps each number back. The engine's "score: row 2 has no 'tenure'" (zero-based) is answered `invalid_input` at `/rows/2/tenure`, with the message counting from one and naming the row's key when it is a string or number: "score: row 3 ('partial') has no 'tenure'" (lake#819). Ship → Models shows each published model's `curl`. Every answer about a model the workspace has, a refusal included, is counted in the workspace's per-model series with its status class and duration (lake#599, [../serving/telemetry.md](../serving/telemetry.md#the-per-model-prediction-series)); a model id the workspace does not know is never counted |
| `model_traffic` | read | `read` | A workspace's `predict` traffic (lake#599): `{customer_id, workspace_id, model_id?, from_ms?, to_ms?, step_ms?}`, by default the last 24 hours in about 48 steps (`step_ms` a whole number of minutes, at most 31 days and 10,000 steps). Answers `{from_ms, to_ms, step_ms, models: [{model_id, calls, errors, client_errors, server_errors, mean_ms?, steps: [{at_ms, calls, errors, mean_ms?}]}], requests: {total, by_status_class}, drift: {measured: false, why}}` from the workspace Zetta's `lake_model_calls` and `lake_model_call_ms` series and its `lake_requests` series (`capability` `predict`) through `query_metric`; a series never written reads as no traffic. Drift is not measured: the workspace keeps no reference distribution of a model's training data. Ship → Models and a model's Monitoring view read it |
| `edit_pipeline` | action | `write` | One validated pipeline edit (`op`: add_step, rename_step, kind_step, remove_step, add_edge, remove_edge, set_attribute (step, name, value; null clears), rename), or `edits`: a list applied in order as one save, all or nothing (how a Project Agent proposal is applied). Every step keeps its `attributes` (source, table, target, algo) through every edit. It reads the graph, applies the edit with `lake::api::pipeline`, refuses one that leaves duplicate ids, unknown kinds, dangling edges or a cycle, and saves the whole graph with `save_graph` by its id. The page's forms, REST and MCP all edit through it |
| `workspace_sql` | read (GET, or POST with a JSON body for a long statement, lake#594) | `read` | One read-only query (`statement` starting SELECT or WITH, at most a trailing semicolon, optional `database`) through Zetta's `sql`; DDL, writes, users and several statements are refused before anything reaches the workspace. A statement that does not start with SELECT or WITH is refused at `/statement` naming the word it starts with (`it starts with 'SELEC'`), so a typo reads as one; a read the workspace cannot parse gets the workspace's own error with the token's position (lake#450). The workspace speaks Zetta's SQL subset — one `WHERE` comparison, `COUNT(*)`/`COUNT(col)`/`SUM`/`AVG`/`MIN`/`MAX` with or without `GROUP BY` (without, the whole table is one group and the answer one row), `GROUP BY` on any column including text, `ORDER BY`, `LIMIT`, `OFFSET` (zetta#1039, zetta#1218), no joins or expressions; a column no row carries is an error naming the table's columns — and the description says so, because agents write their queries from it. Audit kind `workspace.sql` |
| `list_datasets` | read | `read` | The direct dataset catalogue. It calls Zetta `list_datasets` and adds a stable `<database>/<name>` `id` (the form `get_dataset` takes) and a `kind` (`data`, or `result` for an engine output table) to each row |
| `list_runs` | read | `read` | A direct REST/MCP run list with optional `project`, `solution` and bounded `limit` filters. Each retained summary is completed concurrently with `get_run` so it includes status, stage reached and the final error line without serial fan-out |
| `get_pipeline` | read | `read` | A pipeline's graph, approval, `step_status` from the latest run, and its `runs`, newest first — what the pipeline page shows |
| `run_pipeline` | action | `write` | `gate`, then `run` when it passes, with optional `dataset_path`, `holdout_fraction`, `model_name`; answers `{gate, run}`, or `{gate}` alone when the checks fail. Audit kind `pipeline.run` |
| `create_pipeline` | action | `write` | `from` `empty` (one connect step), `n8n` (`import_n8n` then `compile_n8n` over `content`) or `python` (`compile_python` over `content`). Audit kind `pipeline.create` |
| `connect_service` | action | `write` | Create or refresh a Zetta connection to one of the customer's own Ready services from the connection the operator delivered (`service_id`, optional `name`, default the service's name). Postgres today (`sslmode=require` in the cluster); kinds Zetta cannot read yet are refused with `invalid_input`. The password reaches Zetta only, never the caller. Audit kind `service.connect` |
| `search_index` | read | `read` | Search one **published** index (`index_id`, `query`, optional `field`, `top_k` ≤ 100; `index` is the deprecated name of `index_id`): checks `get_index` says it is exposed, then calls Zetta's `search`. The outside-call path: `GET /v1/search_index?customer_id=<customer id>&workspace_id=<workspace id>&index_id=<index id>&query=renewal` with a token scoped to `search_index`, which can do nothing else. Optional `fields` (an array; over REST repeated, comma-separated or a JSON array) picks which of the index's fields each hit carries, and is passed to the engine's `search` as it is: absent means all of them, `[]` ids and scores only, and a field the index does not hold is refused with the index's fields listed (zetta#1243). Answers `hits` as `{id, score, fields}`, best first; `fields` holds the matched row's values and `id` is the engine's internal row id, not a table column (lake#596, lake#615). Audit kind `index.search` |
| `start_solution` | action | `write` | `{customer_id, project, solution, level, dataset, delivery: {kind, endpoint, owner}, roi_model: {value_per_outcome, investment, currency}}`. Creates the project and starts the read-only Project Agent task; it does not call `create_solution` |
| `confirm_solution` | action | `write` | `{task_id, edits?}`. Resolves the task's customer, requires the person who started it (or the operator), applies `edits` as a JSON Merge Patch over `result.foundation`, restores the selected dataset and the person's delivery and economics as the proposal base, then calls `create_solution`. The answer includes missing predicates and whether actions are blocked |
| `workspace_query` | action | `read` | Each name below is also its own capability ([engine-capabilities.md](engine-capabilities.md), lake#650), and `capability` carries their `enum`. Calls an allowed Zetta read: `server_info`, `list_projects`, `get_project`, `get_graph`, `list_connections`, `list_datasets`, `get_dataset`, `list_runs`, `get_run`, `get_run_report`, `list_models`, `get_model`, `model_health`, `list_indexes`, `get_index`, `search`, `list_jobs`, `get_job`, `get_trace`, `list_traces`, `list_source_tables`, `list_dataset_generators`, `list_solutions`, `get_solution`, `list_methodologies`, `validate_study`, `analyze_dataset`, `plan_features`, `get_policy`, `get_experiment`, `list_ledger`, `get_monitor`, `get_roi_readout`. Either spelling of an entity is accepted in `input` and sent as the engine capability takes it (`run`/`run_id`, `model`/`model_id`, `index`/`index_id`, `job`/`job_id`; [naming.md](naming.md)); `compare_runs` without `names` compares every metric the runs share, as the top-level `compare_runs` does. Engine refusals carry `path` below `/input` ([errors.md](errors.md)). The record store (`create_collection`, `put_record`, `get_record`, `delete_record`, `list_records`) is in neither allow-list: lake's `lake_*` collections are reached only through [workspace records](../architecture/workspace-records.md) (lake#370) |
| `workspace_act` | action | `write` | Calls an allowed Zetta action: `create_project`, `create_solution`, `run_solution`, `update_policy`, `decide`, `approve_decision`, `record_outcome`, `capture_intent`, `compile_intent`, `approve_plan`, `gate`, `run`, `create_connection`, `delete_connection`, `create_dataset_from_query`, `upload_dataset`, `delete_project`, `create_index`, `expose_model`, `unexpose_model`, `expose_index`, `unexpose_index`, `score`, `explain`, `save_graph`, `deploy`, `import_n8n`, `compile_n8n`, `compile_python`, `promote_dataset`, `generate_dataset`. `expose_model` and `expose_index` answer `call`, the lake capability that serves the published model or index (`predict` or `search_index`) with its method, path and input filled in, instead of the engine's own `/v1/models/<id>/predict` or `/v1/indexes/<id>/search` route, which lake does not serve (lake#450) |

**Project names (lake#452).** Zetta refuses an empty `create_project` name and a name already in
use with one message that names neither. `workspace_act` refuses an empty or blank `name` itself
(`invalid_input` at `/input/name`, *a project needs a name*) and reports the engine's refusal of a
non-empty name as `conflict` at `/input/name`: *a project named '<name>' already exists in this
workspace; choose another name*. The Design page shows the same message.

<a id="a-projects-decision"></a>
**A project's decision (lake#577).** The New Project drawer asks for "The decision it should
improve", a field of its own beside the description. The engine's project keeps a name, an owner
and a description only, and the `intent` its inputs accept is a reserved note it records on the
call's trace and strips before validation, so a decision sent as `intent` was discarded and the
project page showed the description in its place. Lake keeps the decision itself:

- `workspace_act create_project` takes `input.decision` (text, trimmed, at most 4,000 bytes; any
  other type is `invalid_input` at `/input/decision`), sends the engine everything else, reads the
  new project's id back with one `get_project`, and writes the decision once (`if_absent`) as a
  sealed record of `lake_project_decisions` in the workspace Zetta, keyed by that id and found by
  the account (`include/lake/control/project_decisions.hpp`). The answer carries `decision`. If the
  decision cannot be written the call answers that error, saying the project was created but its
  decision was not kept.
- `workspace_query get_project` adds `decision` beside `description` when the project has one. A
  decision that cannot be read now leaves the project readable without it.
- `workspace_act delete_project` removes it. A project created again under the same name has a new
  id, so it never inherits the old decision.

Nothing about a decision is in PostgreSQL. The web app sends `decision` from the drawer and shows
it on the project page's Decision card, with the description below it, and as each project's line
on the Projects list. `Workspaces.AProjectKeepsItsDecisionBesideItsDescription`
(`tests/test_workspaces.cpp`) creates a project with three distinct strings and reads each back in
its place.

**Dataset rules (lake#447).** `workspace_query` and `workspace_act` apply these on top of Zetta:

- `list_datasets` rows gain `id` (`<database>/<name>`) and `kind` (`data` or `result`);
  `get_dataset` answers gain the same. `kind` is `result` for every `*_results` database and for
  `forecasts`, `feedback` and `system`: the tables the engine writes as run outputs.
- `list_datasets` rows and `get_dataset` answers say what the person loaded (lake#594): when the
  engine records `source_format` (`csv` for a CSV upload, which it stores as `jsonl`), `format` is
  that and the engine's own is `storage_format`. A table whose name SQL cannot name (loaded before
  the naming rule below, such as `ux-review-2026-10-01-accounts`) carries `sql_queryable: false`
  and a `sql_note` saying to load it again under a name of letters, digits and underscores; the
  engine has no rename, so lake cannot rename it.
- `get_dataset` reads `dataset` as `<database>/<name>` or `<database>.<name>` as well as a bare
  name; `analyze_dataset` and `plan_features` read `<database>/<name>` as `<database>.<name>`.
- `upload_dataset` and `create_dataset_from_query` refuse a `table_name` or `database` SQL cannot
  query unquoted (`[A-Za-z_][A-Za-z0-9_]*`, at most 63 characters, not a SQL keyword).
- `upload_dataset` with `format: "csv"` refuses a header with an unnamed or repeated column, an
  unclosed quoted field, or a row whose field count differs from the header, naming the line.
  Zetta's reader would otherwise drop extra fields and fill missing ones with null.
- A `table_name` that already exists answers `conflict` unless `input.if_exists` is `"rename"`
  (forwarded on `upload_dataset`, whose Zetta side refuses a taken name the same way and takes the
  same opt-in since zetta#1221; removed for `create_dataset_from_query`). Zetta then takes the
  next free name, and lake adds `renamed_from` with the name asked for, and the new dataset's `id`.

Input for the last two is `{customer_id, capability, input}`, where `input` is the Zetta
capability's own input. Anything off the lists is refused with `invalid_input`, so nothing reaches
Zetta's administrative surface (`sql`, users, cloud provisioning) through lake. Audit records carry
`workspace.query`, `workspace.act`, `workspace.start` or `workspace.delete` with the customer, the workspace and the
Zetta capability.

## Connections and their credential

The connection form posts the password or access token once, to `create_connection` on the
customer's own workspace, which stores it and never returns it (`list_connections` omits it).
lake keeps no copy, never renders it back into the form, and records only the capability name in
its audit event. Pulling a dataset names the saved connection, so the credential does not travel
again.

`create_dataset_from_query` is consent-gated in Zetta: a node started with `--governance` answers
the first call with a `consent_required` challenge that only a different principal can approve.
Workspace clusters run without `--governance` (the operator sets no such flag), so Zetta runs the
query and audits it as `consent_bypassed`; lake's own `write` authorization on the customer is the
gate. Should a workspace ever enforce consent, the page says an approval is needed that lake cannot
give, instead of passing the challenge through.

**Every page action is a capability.** The pipeline page's *Check and run* is `run_pipeline`, its
three ways to start a pipeline are `create_pipeline`, and every edit is `edit_pipeline`, so a REST or
MCP client does exactly what the page does (lake#189, `Workspaces.EveryNewFeatureIsInOpenApiAndWorksOverMcp`).

## How lake finds and calls a workspace

The Data list requests only `database` and `name` through
`SELECT database, name FROM system.datasets`. Row counts, byte totals, format, columns,
partitions and samples belong to the dataset detail page, whose sample uses `LIMIT 20`.
Connections, the projected dataset list, generators, Lake services and storage are independent
reads in one concurrent batch. Zetta currently materializes dataset summaries before SQL
projection, so [zetta#1195](https://github.com/oramasearch/zetta/issues/1195) still needs to remove
its row/file scans; this change reduces response and rendering work, not that upstream scan.

Methodologies lists no longer fetch the solutions catalogue; methodology details still do.
Deployment catalogues and project/pipeline definitions use a bounded five-second memory cache.
Successful workspace writes explicitly invalidate it, and expired entries, changed service
generations, endpoints or credentials cannot be reused. Mutable lists and execution state are
not cached. See [performance](../ui/performance.md) for the invalidation and proof contract.

Project and solution pages use the direct `list_runs` capability. They request at most five
details and begin those detail reads together. Project catalogues, the project/graph definition
batch and the run list begin together; no run detail waits for another run detail. The later
solution operating-state batch remains concurrent and reuses that run result.

- **The workspace.** `workspaces.id` resolves `zetta_service_id`, which resolves the Zetta service.
  The primary service slug uses the workspace UUID. Migration 0039 records the legacy naming
  remap; new workspace and service IDs differ. No default is consulted for data-plane calls.
- **Status.** The operator's `service_status` row: phase, message and `endpoint`, the in-cluster
  address lake calls.
- **Credential.** The operator mounts `$LAKE_SERVICE_DIR/<slug>.json`, whose `token` is the
  cluster's admin token. Lake rereads it per call. The bounded in-memory registry cache includes
  it in the key so token rotation cannot reuse an earlier entry; it is never persisted by Lake.
- **Ready** means the operator reported `Ready` with an address *and* the token is present. Calls
  before that fail with `workspace_not_ready`.
- **Delete workspace** writes desired `deleted` and withdraws its services, in the transaction
  that deletes what the workspace holds ([Deletion](#deletion)). The Workspaces page shows the
  operator's observations until it reports `Deleted`.

## Deletion

Deleting a workspace cascades (decision 54 S12,
[lake#514](https://github.com/oramasearch/lake/issues/514)). It cannot be undone: the workspace
Zetta's volume snapshots ([orama-operator#260](https://github.com/oramasearch/orama-operator/issues/260))
are the only copy left.

**What is destroyed.** `delete_workspace {dry_run: true}` and the confirmation on `/workspaces`
list it, with `counts` (`repositories`, `live_repositories`, `tombstoned_repositories`, `apps`,
`builds`, `images`):

- the workspace Zetta, with every record lake keeps there (the sealed `lake_*` collections of
  [workspace records](../architecture/workspace-records.md)), its datasets, topics and stores, and
  the workspace's own audit chain. The operator deletes them from the workspace's desired state;
- its repositories: live ones, and tombstoned ones still inside their 30-day retention. Each is
  tombstoned with its retention ended (`retention_until` now, `deleted_at` kept where it was set),
  which frees its name in the account. A tombstone whose retention has already ended is gone
  already and is not listed. Each repository's description record (`lake_repositories` in the
  workspace Zetta, lake#315 part 2) goes with the Zetta, like its forge records, so the cascade
  takes no step of its own for it;
- its apps (desired `deleted`) and its unfinished builds (desired `cancelled`);
- its tenant images: every repository routed to it in `oci_repository_routes`
  ([lake#383](https://github.com/oramasearch/lake/issues/383), state `routed`), whose manifests and
  tags go with its Zetta; and what the instance registry still holds from before that split,
  `<space>/<name>` and `<space>/cache/<name>` for each of its apps and each image its builds target
  (state `tagged`), unless an app or build of another live workspace of the account uses the name.

**One PostgreSQL transaction** tombstones the repositories, deletes the apps, cancels the builds,
deletes the workspace's image routes, sets the workspace's desired state to `deleted` with its
services `absent`, and records the
deletion with everything it destroyed in `workspace_deletions` (migration
`0065_workspace_deletions`). The operator reads only the desired state, as before (decision 51).

**After the commit**, lake removes what lives outside PostgreSQL and records the central audit:

1. drops each destroyed repository from its account's search index on lake's volume, and the indexed
   blobs nothing else uses; for a switched workspace, forgets its search index and removes its
   `search/` objects from the workspace Zetta while lake has the workspace's store open (lake#381;
   otherwise they go with the workspace Zetta), before the store is closed;
2. removes whatever the instance registry still holds of each image repository from before
   lake#383 (its tags and manifest pointers), so nothing can pull it. Blobs stay until lake#383
   phase B: the registry has no collection yet, and other manifests may share them;
3. records one central audit event per destroyed repository, app, build and image, and one for the
   cascade (`workspace/<id>`), all with request id `workspace-deletion:<workspace_id>`. The
   workspace's own audit chain goes with its Zetta, so this is where the history survives;
4. marks the deletion complete (`completed_at`).

Each step is idempotent, and an audit event is recorded once per object and request id. If lake
stops part way, or the registry is unavailable, the workspace is still deleted and the deletion
stays pending: `delete_workspace` answers `deletion.completed: false` with the reason in
`deletion.pending`, and lake's owner loop finishes it on its next pass (every minute, on the lake
that owns the store root). Repeating `delete_workspace` for the deleted workspace resumes it too,
and answers the recorded deletion; the dry run of a deleted workspace answers the same record.

The repositories' content stays in the account's liquid store until the collector reclaims it, as
for any tombstone whose retention has ended. A workspace whose repositories moved into its own store
([lake#380](https://github.com/oramasearch/lake/issues/380), [workspace-store.md](workspace-store.md))
has that store closed and its bucket lease released before lake reports the deletion complete
(`WorkspaceContent::close_workspace_store` in `include/lake/api/workspace_deletion.hpp`, set by the
HTTP server to `SyncStore::close_workspace_store`), and the operator's deletion of the namespace and
volume destroys the bytes. Tenant image routes are deleted in the transaction
([lake#383](https://github.com/oramasearch/lake/issues/383) phase A); their blobs move into the
workspace's store, and go with it, in phase B.

**Result.** `delete_workspace` answers the workspace's row (desired `deleted`) and `deletion`:
`requested_by`, `requested_at`, `completed`, `completed_at`, the destroyed `repositories`, `apps`,
`builds` and `images` (each `{id?, name, state}`), `counts`, and `audit_request_id`.

Reads are `GET /v1/<capability>` with the input as query parameters; actions are
`POST /v1/<capability>` with a JSON body. lake unwraps Zetta's `{ok, value}` envelope and passes
Zetta's own error message through.

## Agents reach Zetta through lake

Agents never hold a workspace's admin token (lake#235). lake serves each workspace's Zetta in
Zetta's own REST shape, so a client written against a Zetta node (`ZETTA_BASE_URL`, `ZETTA_TOKEN`)
works unchanged when pointed here:

| Route | What it does |
|---|---|
| `GET /workspaces/<customer_id>/zetta/v1/<capability>?<input>` | An allow-listed **read**. Query values are passed as strings and the workspace coerces them by its own input schema, exactly as a direct call would |
| `POST /workspaces/<customer_id>/zetta/v1/<capability>` | An allow-listed **action**, the JSON body as its input |
| `GET /workspaces/<customer_id>/zetta/v1/openapi.json` | Zetta's OpenAPI document reduced to the allow-listed paths and methods, with `servers` pointing at this path. Served bare, as Zetta serves it. Also the `workspace_openapi` capability |

The canonical person-facing form of these routes is prefixed by `/w/<workspace_id>`. A task
credential may use the unprefixed form shown in the table: Lake resolves the task first, injects
its workspace into the capability input, and advertises `workspace_id` as optional in that
credential's OpenAPI document. A supplied path or input workspace must match the task.

- **Same gate as every lake caller.** Each call goes through `workspace_query` / `workspace_act`:
  the same allow-lists (`workspace_read_allowed` / `workspace_action_allowed`), `read` or `write`
  on the customer, and the audit kinds `workspace.query` / `workspace.act` with the capability
  named, attributed to whoever holds the bearer (a person, or an agent's token).
- **Answers in Zetta's shape.** The body is the `{ok, value}` / `{ok, error}` envelope. A capability
  outside the allow-list is `404 unknown_capability` ("not available through lake"); a read called
  with `POST` or an action with `GET` is `405 method_not_allowed` with an `Allow` header. Nothing
  refused reaches the workspace.
- **The workspace's pub/sub.** `list_topics` and `receive_events` remain allowed reads; only
  exposed topics are reachable, and a `receive_events` long poll waits at most 10 seconds.
  Topic/message writes now use the registered [Streams capabilities](streams.md), not raw
  `workspace_act`, so creator ownership and system-topic protections cannot be bypassed.
  Streams uses non-consuming `browse_topic` and permits bounded waits up to 30 seconds.
- **The solutions catalogue and projects (lake#290, lake#292, zetta#1102).** `list_solutions`, `get_solution`, `get_policy`, `get_experiment`, `list_ledger`, `get_monitor` and `get_roi_readout` are allowed reads. `create_solution`, `run_solution`, `update_policy`, `decide`, `approve_decision` and `record_outcome` are allowed actions. **Design → Solutions** lists modules and *in design* sketches; **Design → Methodologies** (lake#291) lists each methodology's inputs, outputs, metrics and solutions. On an engine without the solution registry, the Solutions page says so.
  - Start never invents customer inputs. Delivery supports `webhook`, `salesforce` or `hubspot`; its endpoint and owner are required. Value per outcome, investment and currency are also required with no defaults.
  - The Project Agent's task credential carries only lake read capabilities. It reads the chosen data with `analyze_dataset`, `get_dataset` and `plan_features`, then answers in `result.foundation`. The proposal includes `sources` and methodology roles, `snapshot`, `dataset`, optional `decision_source`, `predicates`, and `missing_predicates`. Lake supplies the selected source/table and the person's delivery and ROI values before showing it.
  - Zetta's closed `foundation` object accepts snapshot, dataset, delivery, ROI and optional decision source; methodology role mappings are sent as top-level `sources`. Predicate mappings and the confirmed missing list are retained with those sources in the solution manifest, so the durable confirmation—not an earlier agent draft—controls the Run gate. For S-1, missing `in_collections`, `open_complaint` or `vulnerable` evidence is reported and blocks Run; lake never substitutes `false`.
  - Confirmation is explicit and separate from the agent turn. REST and MCP use the same `start_solution` and `confirm_solution` capabilities as the forms. The engine's solution manifest is the durable pointer to policy, experiment, monitor and readout state; lake stores no second copy.
  - Project operating panels use the engine response shapes directly. Experiment rows come from `get_experiment.balance.allocations` and show configured share, assignments, expected count and balance. Decision, delivered-action and held counts plus the latest actions come from `list_ledger` action entries whose `cell` is the foundation `scope` or a child of it. The bounded latest-run decision rows identify held and `pending_approval` A1 entries and provide the existing explicit approval control. `get_roi_readout.roi_statement.reason`, with the incrementality reason as a compatibility fallback, explains an unclaimable ROI in one line.
  - Policy, experiment, ledger, monitor and ROI reads start in one concurrent batch. The bounded decision-table read remains the existing follow-up after the latest Run identifies its output; this rendering correction adds no workspace call or sequential round.
- **What the engine runs (lake#289).**
  - `list_methodologies` (zetta#1061) lists the 21 portfolio methodologies. Each carries `runnable`, and those that do not run yet name their tracking issue.
  - `analyze_dataset` and `plan_features` (zetta#1062) describe a table and plan features for a methodology.
  - All three are reads.
  - `compile_intent` takes `methodology` (`"M-3"`) or `solution` (`"S-5"`). A methodology the engine cannot run is refused with `not_supported`, naming its issue, never compiled as a skeleton.
  - An engine that predates the registry answers `unknown_capability` to `list_methodologies`; lake then treats it as running M-1 (churn / GBDT classification) only.
- **A Project Agent change proposal's compile check (lake#438).** A change request (**Ask the
  Project Agent** on a pipeline: an `agent-project` turn whose context names `project` and
  `graph`) checks its edits against the methodology's compiled graph, so its task credential also
  carries `workspace_act` and `compile_intent`, and nothing else that acts. `workspace_act` refuses
  any action an agent turn's credential does not name (`unauthorized`, "this Project Agent turn
  does not grant save_graph"), so the edits, approval and run stay the person's. Zetta's
  `compile_intent` stores the graph it compiles, so lake confines the call rather than forwarding
  it: the project is the turn's own, the stored graph is lake's check graph `Project Agent check
  <methodology>` (never the pipeline being changed), only `methodology`, `solution` and `hints`
  are passed (the agent's free-text `intent` is not a `compile_intent` input), and a repeat check
  of the same methodology reads that graph back with `get_graph` instead of compiling again. This
  was chosen over compiling in lake on the person's authority at Apply because the agent needs the
  compiled shape while it writes the edits, and the grant reaches one action on one project.
  - **Check graphs are hidden.** The check graph stays on the project in Zetta, one per
    methodology checked, but it is lake's, not a pipeline. Every name starting with
    `project_agent_check_prefix` ("Project Agent check") is skipped through one helper,
    `without_project_agent_checks`, by the project page's Pipelines table (and the per-graph
    reads behind it), the agent page's approved pipelines, and `settle_build`'s pick of the
    project's pipeline after a build.
  - **Build turns get the same check.** A `build:<project>` turn carries the same confined
    `compile_intent` on the project it designs. Because the pick after the build skips check
    graphs, a project holding only a check graph still gets its own pipeline compiled by lake.
  - A foundation (`solution:`) turn still carries no workspace action.
- **Tools from the filtered OpenAPI.** Agents derive their tool list from this document (the
  `new-agent` rule), so they see exactly what they may call. `sql` is not in it: SQL reaches a
  workspace only through `workspace_sql`, which runs one read-only query.
- **Application Agent's Lake tools.** Its client calls `list_datasets` and `get_dataset` directly,
  so Lake exposes those read tools without changing Zetta's ordinary workspace API. Dataset ids
  are `<database>/<dataset>`; the app task's credential binds the customer. Without `revision`,
  `get_dataset` reads the workspace that `workspace_id` or the `/w/<workspace>/` URL names; the
  input no longer has to repeat a workspace the URL already gives.

Verify: `HttpServer/HttpResponseIsolation.AgentsReachAWorkspacesZettaThroughLakeInZettasShape`.

## Configuration

| Variable | Set by | Meaning |
|---|---|---|
| `LAKE_SERVICE_DIR` | operator | Mounted service connections, including the workspace's token |
| `LAKE_SERVICES_PER_CUSTOMER` | optional | Live services per customer, the workspace included (default `0`: no limit) |

Without `LAKE_SERVICE_DIR`, the pages say Design, Train and Ship are not available on this lake,
and the capabilities answer `store_unavailable`. `LAKE_DATA_PLANE_*`, `LAKE_WORKSPACE_DIR` and
`LAKE_WORKSPACE_URL_TEMPLATE` are no longer read for workspaces.

## Verify

```sh
ctest --test-dir build -R 'Workspaces|WebUi'
```

`WorkspaceDeletion.*` (memory and PostgreSQL) covers the cascade: two accounts with two workspaces
each, a workspace with live and tombstoned repositories, an app, builds and images, the dry run's
counts, names freed, the other workspace and account untouched, an interrupted cascade resumed by
the owner loop without repeating an audit event, and that only an administrator of the account
deletes. `RegistryProtocols.DeletingAWorkspaceMakesItsImagesUnpullable` checks pulls against a
running lake, and `Search.DroppingARepositoryRemovesItsSegmentsAndUnsharedBlobs` the index.
`Workspaces.*` covers the slug, the workspace as the default service created once, withdrawal and
restart (a new slug, the old status not carried over), the delete confirmation, readiness needing both
the phase and the token, calls reaching only the customer's own address with its own token, the
allow-lists, and another customer's user being refused. On a deployment: open `/design/projects`,
start the workspace, wait for Ready, create a project, and build a pipeline. Deleting is verified
the same way: open Delete on `/workspaces`, check the listed repositories and images, type the
name, and watch `kubectl -n lake get oramazettacluster` lose `<lake>-<workspace>`.

## Known limits

Status: checked on 2026-10-01 against the dev cluster (lake `36679e1`, workspace Zetta
`dev-13979173`) during the UX review recorded in liquid-bridge `docs/reviews/lake-ux/2026-10-01.md`.

- A workspace is stopped with `update_workspace` (`desired: "stopped"`), which withdraws its
  primary Zetta service as [the contract](../control-plane/workspaces.md) describes, or deleted
  with typed confirmation. It counts toward the customer's service limit and the Lake's
  `spec.services.maxServices` ([`services.md`](services.md)).
- Upload and project deletion need a workspace image with Zetta's `upload_dataset` and
  `delete_project` (zetta PRs #1012 and #1013, both merged); an older image answers them with an
  error. The dev cluster's image has them: an upload loaded 400 rows during the review.
- A published model or index is callable from outside lake through lake, not by Zetta's own
  address: `POST /v1/predict` and `GET /v1/search_index` (or the MCP tools of the same names)
  with a lake key and the `workspace_id`. The review called a published model this way from
  outside the cluster. Unpublishing stops those calls.
- Workspace reads time out after 15 seconds and actions after 60. Run reports load:
  `get_run_report` answers since https://github.com/oramasearch/zetta/issues/1007 was fixed.
