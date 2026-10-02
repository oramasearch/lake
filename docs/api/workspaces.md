# Workspaces: Design, Train and Ship on Zetta

Lake's Design, Train and Ship pages delegate to workspace Zettas. A customer can have many
workspaces; each data-plane call resolves an explicit `workspace_id` (`workspaces.id`). It never
resolves a default implicitly. Zetta has no tenancy within one cluster: its list operations return
that workspace's data. Provisioning uses workspace desired state; a missing or
unready workspace prevents the feature, never redirects data to Lake's bus or PostgreSQL.
The operator contract is [workspaces](../control-plane/workspaces.md), and the cutover/audit is in the
[workspace data-plane design](../architecture/workspace-data-plane.md).

Page paths below have the canonical prefix `/w/<workspace_id>`. The global top-bar selector
lists the space's workspaces, including a sole preselected workspace, and offers New workspace.
Switching preserves the page and filters. Old GET URLs redirect using the validated remembered
workspace cookie or the space's only workspace; otherwise they show a selection page. Forms
carry the ID explicitly and reject a mismatch with the URL. Person-scoped REST and MCP schemas
require it too. A library agent's task credential is the exception: Lake derives the workspace
from the immutable task row when the client omits it and rejects a supplied mismatch.

## What a person sees

| Section | Page | Zetta capabilities behind it |
|---|---|---|
| Design | `/design/projects`, `/design/project?name=` | `list_projects`, `get_project`, `create_project`, `delete_project` (typed confirmation; the project's runs and models stay listed), `capture_intent` then `compile_intent` (build a pipeline from what the project should predict), `approve_plan` (approve a proposed pipeline), `gate` then `run` (check and run an approved pipeline) |
| Design | `/design/solutions`, `/design/solution?id=`, `/design/project?name=` | The solution catalogue and a solution project. **Start this solution** collects a project name, capability level, dataset, delivery destination and owner, and the customer's value per outcome, investment and currency. A `solution:<project>` Project Agent turn proposes the snapshot, dataset and methodology roles, and policy predicate evidence. The editable proposal is shown before `confirm_solution` calls `create_solution`. The project then shows solution stages and a bounded Runs table with status, stage, error and full-run links. Active runs refresh the page every three seconds. Policy, experiment, monitor and ROI have readable summaries with JSON disclosures; the correctness certificate leads with its verdict, groups applicable checks and places other checks under **Show all**. `run_solution`, `update_policy`, `approve_decision` and `record_outcome` are all workspace actions through lake's authorization and audit path |
| Design | `/design/pipeline?project=&graph=&step=` | **The pipeline page** (docs/design/zetta-parity.md): `get_graph` drawn as SVG in stage lanes with each step coloured by the latest run; a step inspector; every edit through `edit_pipeline`; **Ask the Project Agent** (a change request answered with a proposal: the edits in words, the graph after, run settings, and *Apply* as one `edit_pipeline` save, or *Discard*; a question or absent proposal instead gets a **Reply** box that continues the same Project Agent conversation; lake#249. While the turn works the page reloads itself every three seconds with a script, because a declarative refresh to its own `#proposal` URL is only a fragment jump (the ExperimentsAgent's working panel, reviewed at `#experiment-proposal`, does the same every two seconds); a proposal that cannot be applied lists each reason as one plain sentence, never a status code or JSON body, followed by the next step; lake#438); `approve_plan`; *Check* (`gate`, every check with its reason); *Check and run* (`gate` then `run` with a chosen dataset, a hold-out of 0.2 by default and a model name defaulting to `<project>_<pipeline>`); *Deploy* (`deploy`, with its gate); the pipeline's runs. The project page adds an empty pipeline (`save_graph`), *Import from n8n* (`import_n8n` → `compile_n8n`) and *Build from Python* (`compile_python`) |
| Train | `/train/agents`, `/train/agent?id=` | **Agents** ([`train-agents.md`](train-agents.md)): `list_agents`, `get_agent`, `agent_training_status`, `create_agent` (Studio's boxes), `delete_agent` (typed confirmation), `update_agent`, `set_training_schedule`, `agent_run`, `chat` (a `write` in lake, `GET` to Zetta); plus lake's own Agent Library (`list_agent_library`) |
| Train | `/train/data` | `list_connections`, a projected `workspace_sql` dataset list; **Add a connection** (`create_connection`, Postgres or Snowflake), **Remove** (`delete_connection`), **Pull a dataset** (`create_dataset_from_query` from a saved connection with one read-only `SELECT`), **Upload a file** (`upload_dataset`: one CSV or JSONL file of at most 16 MiB, posted as `multipart/form-data` to `/train/data/upload`), **Connect one of your services** (`connect_service`: the space's own Ready Postgres services in one click, no password typed or shown; other kinds say *not supported yet*), and each connection's *From service* |
| Train | `/train/data?browse=<connection>` | *Browse tables* on a connection: `list_source_tables` with columns, without pulling anything |
| Train | `/train/dataset?database=&name=` | `get_dataset` (columns, partitions, health), its first 20 rows (`workspace_sql`), *Keep durable* (`promote_dataset`), *Query it* |
| Train | `/train/query?statement=&database=` | Read-only SQL over the workspace's data (`workspace_sql`) |
| Train | `/train/data`, *Generate a dataset* | `list_dataset_generators`, `generate_dataset` (never its `path`, which writes a file on the node) |
| Train | `/train/data` (layout) | **Load Data** first and open (upload a file, pull a table from a connection, generate one); after an upload, *Loaded N rows into <dataset>* with a link. Then connections, datasets and storage (only what can be created today; the rest one *coming later* line). A dataset page shows its columns, its first 20 rows (`LIMIT 20`, also bounded by Lake) and *Storage details* under a disclosure; the Query page shows at most 500 rows (lake#256, lake#257) |
| Design | `/design/project` → Build a Pipeline | *From data* (optional): after `compile_intent`, lake points the first connect step at the chosen dataset (`set_attribute` source/table), removes the other connect steps and opens the pipeline with that dataset preselected for the run (lake#251) |
| Train | `/train/runs`, `/train/run?id=` | A run page leads with what the run says about itself: *Nothing was trained* for a structural run, and each connect step that was not read (lake#251, lake#253, zetta#1022); the report's *What It Learned From* shows *trained on* and *columns not used*. `list_runs` carries the pipeline and project names shown by the list, plus status and start time, without project lookups. The detail page uses `get_run`, `get_run_report` rendered as tables (what it learned from, what it leans on, most at risk, caveats, what it cannot tell), and `list_traces` as a waterfall |
| Ship | `/ship/models` | One `list_models` record per model carries name and short id, project and pipeline names, run id, score, feature schema, creation time, and published state. The list issues no per-model `get_model` calls. Its action remains `expose_model` / `unexpose_model` by id, and each published model shows a `curl` for `POST /v1/predict` with the page's own `customer_id` and `workspace_id` filled in (lake#250, lake#255, lake#439) |
| Ship | `/ship/indexes` | `list_indexes`, *New index* (`create_index` over a dataset, BM25F full-text) |
| Ship | `/ship/index?id=&q=&field=&top_k=` | `get_index`; *Try a search* (`search`); *Publish* / *Unpublish* (`expose_index` / `unexpose_index`); and, once published, **Call it from your code**: *Create a search key* (a lake API token scoped to `search_index` only) and copy-ready `curl`, JavaScript and Python calls to `GET /v1/search_index` naming the page's `workspace_id` (lake#439), plus the MCP tool of the same name |
| Ship | `/ship/packages` | lake's own registries, not the workspace (`docs/api/registry.md`) |
| Ship | `/deployments` | lake's own platform deployments (`docs/api/platform.md`) |

Every page takes `customer_id` and defaults to the viewer's personal space; a selector lists the
customers the viewer may act for, with their own marked *your personal space*. Until the
customer's workspace is ready, the page offers **Start your workspace** instead, then shows it being
prepared and refreshes every 15 seconds.

The Workspaces page (`/workspaces`) shows name, environment, phase, namespace and created time.
New workspace accepts a name and DNS-label-safe environment. Delete requires typing the exact
name, then sets desired state to `deleted`; the operator deletes the namespace and its contents.
Pending and stopped rows are selectable. A sole workspace is still shown in the top bar.

## Capabilities

All capabilities are in lake's registry, so REST, OpenAPI and MCP have them too. Each resolves the
customer's policy object and is authorized by lake before anything reaches Zetta.

| Capability | Kind | lake action on the customer | What it does |
|---|---|---|---|
| `workspace_status` | read | `read` | `{workspace, requested, phase, message, ready}` |
| `list_workspaces` | read | `read` | Requires `customer_id`; lists contract rows, excluding completed deletions |
| `get_workspace` | read | `read` | Requires `customer_id`, `workspace_id`; returns desired state and operator observations |
| `create_workspace` | action | `write` | Requires `customer_id`, `name`, `environment`; atomically creates workspace and primary service desired state |
| `update_workspace` | action | `write` | Requires `customer_id`, `workspace_id`; optional name, environment, desired (`running` or `stopped`); changes increment generation, status fields are refused |
| `delete_workspace` | action | `admin` | Requires `customer_id`, `workspace_id`, `confirm` equal to the workspace name; sets desired `deleted` |
| `start_workspace` | action | `write` | Legacy idempotent creation of the named `workspace` in `prod`; uses the same contract table, never implicit data-plane routing |
| `predict` | action (POST, changes nothing) | `read` | Score rows through one **published** model from outside lake (lake#250): `{customer_id, workspace_id, model, rows, top_k?}`; checks the model is published (`get_model`), then Zetta's `score`; answers rows ranked by risk with their drivers. Ship → Models shows each published model's `curl` |
| `edit_pipeline` | action | `write` | One validated pipeline edit (`op`: add_step, rename_step, kind_step, remove_step, add_edge, remove_edge, set_attribute (step, name, value; null clears), rename), or `edits`: a list applied in order as one save, all or nothing (how a Project Agent proposal is applied). Every step keeps its `attributes` (source, table, target, algo) through every edit. It reads the graph, applies the edit with `lake::api::pipeline`, refuses one that leaves duplicate ids, unknown kinds, dangling edges or a cycle, and saves the whole graph with `save_graph` by its id. The page's forms, REST and MCP all edit through it |
| `workspace_sql` | read | `read` | One read-only query (`statement` starting SELECT or WITH, at most a trailing semicolon, optional `database`) through Zetta's `sql`; DDL, writes, users and several statements are refused before anything reaches the workspace. The workspace speaks Zetta's SQL subset — one `WHERE` comparison, `GROUP BY` one key with `COUNT`/`SUM`/`AVG`/`MIN`/`MAX`, `ORDER BY`, `LIMIT`, `OFFSET` (zetta#1039), no joins or expressions — and the description says so, because agents write their queries from it. Audit kind `workspace.sql` |
| `list_datasets` | read | `read` | ApplicationAgent's direct dataset catalogue. It calls Zetta `list_datasets` and adds a stable `<database>/<name>` id to each row |
| `list_runs` | read | `read` | A direct REST/MCP run list with optional `project`, `solution` and bounded `limit` filters. Each retained summary is completed concurrently with `get_run` so it includes status, stage reached and the final error line without serial fan-out |
| `get_pipeline` | read | `read` | A pipeline's graph, approval, `step_status` from the latest run, and its `runs`, newest first — what the pipeline page shows |
| `run_pipeline` | action | `write` | `gate`, then `run` when it passes, with optional `dataset_path`, `holdout_fraction`, `model_name`; answers `{gate, run}`, or `{gate}` alone when the checks fail. Audit kind `pipeline.run` |
| `create_pipeline` | action | `write` | `from` `empty` (one connect step), `n8n` (`import_n8n` then `compile_n8n` over `content`) or `python` (`compile_python` over `content`). Audit kind `pipeline.create` |
| `connect_service` | action | `write` | Create or refresh a Zetta connection to one of the customer's own Ready services from the connection the operator delivered (`service_id`, optional `name`, default the service's name). Postgres today (`sslmode=require` in the cluster); kinds Zetta cannot read yet are refused with `invalid_input`. The password reaches Zetta only, never the caller. Audit kind `service.connect` |
| `search_index` | read | `read` | Search one **published** index (`index`, `query`, optional `field`, `top_k` ≤ 100): checks `get_index` says it is exposed, then calls Zetta's `search`. The outside-call path: `GET /v1/search_index?customer_id=<customer id>&workspace_id=<workspace id>&index=<index id>&query=renewal` with a token scoped to `search_index`, which can do nothing else. Audit kind `index.search` |
| `start_solution` | action | `write` | `{customer_id, project, solution, level, dataset, delivery: {kind, endpoint, owner}, roi_model: {value_per_outcome, investment, currency}}`. Creates the project and starts the read-only Project Agent task; it does not call `create_solution` |
| `confirm_solution` | action | `write` | `{task_id, edits?}`. Resolves the task's customer, requires the person who started it (or the operator), applies `edits` as a JSON Merge Patch over `result.foundation`, restores the selected dataset and the person's delivery and economics as the proposal base, then calls `create_solution`. The answer includes missing predicates and whether actions are blocked |
| `workspace_query` | action | `read` | Calls an allowed Zetta read: `server_info`, `list_projects`, `get_project`, `get_graph`, `list_connections`, `list_datasets`, `get_dataset`, `list_runs`, `get_run`, `get_run_report`, `list_models`, `get_model`, `model_health`, `list_indexes`, `get_index`, `search`, `list_jobs`, `get_job`, `get_trace`, `list_traces`, `list_source_tables`, `list_dataset_generators`, `list_solutions`, `get_solution`, `list_methodologies`, `validate_study`, `analyze_dataset`, `plan_features`, `get_policy`, `get_experiment`, `list_ledger`, `get_monitor`, `get_roi_readout` |
| `workspace_act` | action | `write` | Calls an allowed Zetta action: `create_project`, `create_solution`, `run_solution`, `update_policy`, `decide`, `approve_decision`, `record_outcome`, `capture_intent`, `compile_intent`, `approve_plan`, `gate`, `run`, `create_connection`, `delete_connection`, `create_dataset_from_query`, `upload_dataset`, `delete_project`, `create_index`, `expose_model`, `unexpose_model`, `expose_index`, `unexpose_index`, `score`, `explain`, `save_graph`, `deploy`, `import_n8n`, `compile_n8n`, `compile_python`, `promote_dataset`, `generate_dataset` |

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
- **Delete workspace** writes desired `deleted` and withdraws its services. The Workspaces page
  shows the operator's observations until it reports `Deleted`.

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
- **ApplicationAgent's Lake tools.** Its client calls `list_datasets` and `get_dataset` directly,
  so Lake exposes those read tools without changing Zetta's ordinary workspace API. Dataset ids
  are `<database>/<dataset>`; the app task's credential binds the customer.

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

`Workspaces.*` covers the slug, the workspace as the default service created once, withdrawal and
restart (a new slug, the old status not carried over), the delete confirmation, readiness needing both
the phase and the token, calls reaching only the customer's own address with its own token, the
allow-lists, and another customer's user being refused. On a deployment: open `/design/projects`,
start the workspace, wait for Ready, create a project, and build a pipeline. Deleting is verified
the same way: tick the confirmation, delete, and watch the page return to **Start your workspace**
while `kubectl -n lake get oramazettacluster` loses `<lake>-<workspace>`.

## Known limits

- A workspace cannot be suspended, only deleted. The workspace counts toward the customer's
  service limit and the Lake's `spec.services.maxServices` ([`services.md`](services.md)).
- Upload and project deletion need a workspace image with Zetta's `upload_dataset` and
  `delete_project` (zetta PRs #1012 and #1013); an older image answers them with an error.
- A published model or index is reachable inside the workspace only; lake does not route to it.
- Workspace reads time out after 15 seconds and actions after 60. Run reports do not load on the
  current workspace image: `get_run_report` never answers (https://github.com/oramasearch/zetta/issues/1007).
