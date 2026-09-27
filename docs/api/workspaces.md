# Workspaces: Design, Train and Ship on Zetta

**Status: implemented 2026-09-25.** lake's Design, Train and Ship pages delegate to the Zetta
engine. Each customer that uses them gets its own Zetta cluster, a **workspace**, because Zetta has
no tenancy: a cluster's `list_projects`, `list_models` and the rest return everything on it
(liquid-bridge decision 45). Since decision 46 the workspace is the customer's **default Zetta
service** ([`services.md`](services.md)), requested through lake's control Postgres; the contract
with the operator is [`../control-plane/services.md`](../control-plane/services.md).

## What a person sees

| Section | Page | Zetta capabilities behind it |
|---|---|---|
| Design | `/design/projects`, `/design/project?name=` | `list_projects`, `get_project`, `create_project`, `delete_project` (typed confirmation; the project's runs and models stay listed), `capture_intent` then `compile_intent` (build a pipeline from what the project should predict), `approve_plan` (approve a proposed pipeline), `gate` then `run` (check and run an approved pipeline) |
| Design | `/design/pipeline?project=&graph=&step=` | **The pipeline page** (docs/design/zetta-parity.md): `get_graph` drawn as SVG in stage lanes with each step coloured by the latest run; a step inspector; every edit through `edit_pipeline`; **Ask the Project Agent** (a change request answered with a proposal: the edits in words, the graph after, run settings, and *Apply* as one `edit_pipeline` save, or *Discard*; lake#249); `approve_plan`; *Check* (`gate`, every check with its reason); *Check and run* (`gate` then `run` with a chosen dataset, a hold-out of 0.2 by default and a model name defaulting to `<project>_<pipeline>`); *Deploy* (`deploy`, with its gate); the pipeline's runs. The project page adds an empty pipeline (`save_graph`), *Import from n8n* (`import_n8n` → `compile_n8n`) and *Build from Python* (`compile_python`) |
| Train | `/train/agents`, `/train/agent?id=` | **Agents** ([`train-agents.md`](train-agents.md)): `list_agents`, `get_agent`, `agent_training_status`, `create_agent` (Studio's boxes), `delete_agent` (typed confirmation), `update_agent`, `set_training_schedule`, `agent_run`, `chat` (a `write` in lake, `GET` to Zetta); plus lake's own Agent Library (`list_agent_library`) |
| Train | `/train/data` | `list_connections`, `list_datasets`; **Add a connection** (`create_connection`, Postgres or Snowflake), **Remove** (`delete_connection`), **Pull a dataset** (`create_dataset_from_query` from a saved connection with one read-only `SELECT`), **Upload a file** (`upload_dataset`: one CSV or JSONL file of at most 16 MiB, posted as `multipart/form-data` to `/train/data/upload`), **Connect one of your services** (`connect_service`: the space's own Ready Postgres services in one click, no password typed or shown; other kinds say *not supported yet*), and each connection's *From service* |
| Train | `/train/data?browse=<connection>` | *Browse tables* on a connection: `list_source_tables` with columns, without pulling anything |
| Train | `/train/dataset?database=&name=` | `get_dataset` (columns, partitions, health), its first 20 rows (`workspace_sql`), *Keep durable* (`promote_dataset`), *Query it* |
| Train | `/train/query?statement=&database=` | Read-only SQL over the workspace's data (`workspace_sql`) |
| Train | `/train/data`, *Generate a dataset* | `list_dataset_generators`, `generate_dataset` (never its `path`, which writes a file on the node) |
| Train | `/train/runs`, `/train/run?id=` | `list_runs`, `get_run` (lineage, outputs, log), `get_run_report` rendered as tables (what it learned from, what it leans on, most at risk, caveats, what it cannot tell), and the run's spans from `list_traces` as a waterfall |
| Ship | `/ship/models` | `list_models`, `expose_model`, `unexpose_model` (publish a model's `POST /v1/models/<name>/predict` inside the workspace) |
| Ship | `/ship/indexes` | `list_indexes`, *New index* (`create_index` over a dataset, BM25F full-text) |
| Ship | `/ship/index?id=&q=&field=&top_k=` | `get_index`; *Try a search* (`search`); *Publish* / *Unpublish* (`expose_index` / `unexpose_index`); and, once published, **Call it from your code**: *Create a search key* (a lake API token scoped to `search_index` only) and copy-ready `curl`, JavaScript and Python calls to `GET /v1/search_index`, plus the MCP tool of the same name |
| Ship | `/ship/packages` | lake's own registries, not the workspace (`docs/api/registry.md`) |
| Ship | `/deployments` | lake's own platform deployments (`docs/api/platform.md`) |

Every page takes `customer_id` and defaults to the viewer's personal space; a selector lists the
customers the viewer may act for, with their own marked *your personal space*. Until the
customer's workspace is ready, the page offers **Start your workspace** instead, then shows it being
prepared and refreshes every 15 seconds.

**Delete workspace** sits at the foot of Design → Projects once the workspace is ready. It needs a
ticked confirmation that names the workspace, because the operator then deletes the cluster, its
data volume and its token: every project, dataset, run and model in it is gone and cannot be
recovered. lake's own repositories, packages and deployments are untouched, and the customer can
start a new, empty workspace afterwards. If the operator refuses a workspace because the Lake is
at its `spec.workspaces.maxWorkspaces` limit, the page says so and offers **Cancel workspace**,
which withdraws the request the same way.

## Capabilities

All twelve are in lake's registry, so REST, OpenAPI and MCP have them too. Each resolves the
customer's policy object and is authorized by lake before anything reaches Zetta.

| Capability | Kind | lake action on the customer | What it does |
|---|---|---|---|
| `workspace_status` | read | `read` | `{workspace, requested, phase, message, ready}` |
| `start_workspace` | action | `write` | Appends a `desired` event; repeating it appends nothing |
| `delete_workspace` | action | `admin` | Needs `confirm` equal to the workspace id; marks the default service absent. Refused with `not_found` when no workspace is requested |
| `edit_pipeline` | action | `write` | One validated pipeline edit (`op`: add_step, rename_step, kind_step, remove_step, add_edge, remove_edge, set_attribute (step, name, value; null clears), rename), or `edits`: a list applied in order as one save, all or nothing (how a Project Agent proposal is applied). Every step keeps its `attributes` (source, table, target, algo) through every edit. It reads the graph, applies the edit with `lake::api::pipeline`, refuses one that leaves duplicate ids, unknown kinds, dangling edges or a cycle, and saves the whole graph with `save_graph` by its id. The page's forms, REST and MCP all edit through it |
| `workspace_sql` | read | `read` | One read-only query (`statement` starting SELECT or WITH, at most a trailing semicolon, optional `database`) through Zetta's `sql`; DDL, writes, users and several statements are refused before anything reaches the workspace. Audit kind `workspace.sql` |
| `get_pipeline` | read | `read` | A pipeline's graph, approval, `step_status` from the latest run, and its `runs`, newest first — what the pipeline page shows |
| `run_pipeline` | action | `write` | `gate`, then `run` when it passes, with optional `dataset_path`, `holdout_fraction`, `model_name`; answers `{gate, run}`, or `{gate}` alone when the checks fail. Audit kind `pipeline.run` |
| `create_pipeline` | action | `write` | `from` `empty` (one connect step), `n8n` (`import_n8n` then `compile_n8n` over `content`) or `python` (`compile_python` over `content`). Audit kind `pipeline.create` |
| `connect_service` | action | `write` | Create or refresh a Zetta connection to one of the customer's own Ready services from the connection the operator delivered (`service_id`, optional `name`, default the service's name). Postgres today (`sslmode=require` in the cluster); kinds Zetta cannot read yet are refused with `invalid_input`. The password reaches Zetta only, never the caller. Audit kind `service.connect` |
| `search_index` | read | `read` | Search one **published** index (`index`, `query`, optional `field`, `top_k` ≤ 100): checks `get_index` says it is exposed, then calls Zetta's `search`. The outside-call path: `GET /v1/search_index?customer_id=&index=&query=` with a token scoped to `search_index`, which can do nothing else. Audit kind `index.search` |
| `workspace_query` | action | `read` | Calls an allowed Zetta read: `server_info`, `list_projects`, `get_project`, `get_graph`, `list_connections`, `list_datasets`, `get_dataset`, `list_runs`, `get_run`, `get_run_report`, `list_models`, `get_model`, `model_health`, `list_indexes`, `get_index`, `search`, `list_jobs`, `get_job`, `get_trace`, `list_traces`, `list_source_tables`, `list_dataset_generators` |
| `workspace_act` | action | `write` | Calls an allowed Zetta action: `create_project`, `capture_intent`, `compile_intent`, `approve_plan`, `gate`, `run`, `create_connection`, `delete_connection`, `create_dataset_from_query`, `upload_dataset`, `delete_project`, `create_index`, `expose_model`, `unexpose_model`, `expose_index`, `unexpose_index`, `score`, `explain`, `save_graph`, `deploy`, `import_n8n`, `compile_n8n`, `compile_python`, `promote_dataset`, `generate_dataset` |

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

- **The service.** The workspace is the customer's live `services` row with `is_default` and kind
  `zetta` (migration `0024_services`). **Start your workspace** creates it; asking again returns
  it. The first one takes the slug `ws-` followed by the first 12 hexadecimal digits of the
  customer id (`control::workspace_slug`), which is the name decision 45 gave its cluster, so the
  operator adopts that cluster with its data. A slug is never reused: a workspace started again
  after a deletion gets a `svc-` slug and a new, empty cluster.
- **Status.** The operator's `service_status` row: phase, message and `endpoint`, the in-cluster
  address lake calls.
- **Credential.** The operator mounts `$LAKE_SERVICE_DIR/<slug>.json`, whose `token` is the
  cluster's admin token. lake reads it per call and never stores it.
- **Ready** means the operator reported `Ready` with an address *and* the token is present. Calls
  before that fail with `workspace_not_ready`.
- **Delete workspace** marks the service absent. It then shows *Deleting* on `/services`, and
  Design, Train and Ship offer **Start your workspace** again at once.

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

- **Same gate as every lake caller.** Each call goes through `workspace_query` / `workspace_act`:
  the same allow-lists (`workspace_read_allowed` / `workspace_action_allowed`), `read` or `write`
  on the customer, and the audit kinds `workspace.query` / `workspace.act` with the capability
  named, attributed to whoever holds the bearer (a person, or an agent's token).
- **Answers in Zetta's shape.** The body is the `{ok, value}` / `{ok, error}` envelope. A capability
  outside the allow-list is `404 unknown_capability` ("not available through lake"); a read called
  with `POST` or an action with `GET` is `405 method_not_allowed` with an `Allow` header. Nothing
  refused reaches the workspace.
- **Tools from the filtered OpenAPI.** Agents derive their tool list from this document (the
  `new-agent` rule), so they see exactly what they may call. `sql` is not in it: SQL reaches a
  workspace only through `workspace_sql`, which runs one read-only query.

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
