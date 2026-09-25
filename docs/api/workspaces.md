# Workspaces: Design, Train and Ship on Zetta

**Status: implemented 2026-09-25.** lake's Design, Train and Ship pages delegate to the Zetta
engine. Each customer that uses them gets its own Zetta cluster, a **workspace**, because Zetta has
no tenancy: a cluster's `list_projects`, `list_models` and the rest return everything on it
(liquid-bridge decision 45). The operator side is `orama-operator` `docs/lake/workspaces.md`.

## What a person sees

| Section | Page | Zetta capabilities behind it |
|---|---|---|
| Design | `/design/projects`, `/design/project?name=` | `list_projects`, `get_project`, `create_project`, `capture_intent` then `compile_intent` (build a pipeline from what the project should predict), `approve_plan` (approve a proposed pipeline), `gate` then `run` (check and run an approved pipeline) |
| Train | `/train/data` | `list_connections`, `list_datasets` |
| Train | `/train/runs`, `/train/run?id=` | `list_runs`, `get_run_report` |
| Ship | `/ship/models` | `list_models`, `expose_model`, `unexpose_model` (publish a model's `POST /v1/models/<name>/predict` inside the workspace) |
| Ship | `/ship/indexes` | `list_indexes` |
| Ship | `/ship/packages` | lake's own registries, not the workspace (`docs/api/registry.md`) |
| Ship | `/deployments` | lake's own platform deployments (`docs/api/platform.md`) |

Every page takes `customer_id` and defaults to the viewer's personal space; a selector lists the
customers the viewer may act for, with their own marked *your personal space*. Until the
customer's workspace is ready, the page offers **Start your workspace** instead, then shows it being
prepared and refreshes every 15 seconds.

## Capabilities

All four are in lake's registry, so REST, OpenAPI and MCP have them too. Each resolves the
customer's policy object and is authorized by lake before anything reaches Zetta.

| Capability | Kind | lake action on the customer | What it does |
|---|---|---|---|
| `workspace_status` | read | `read` | `{workspace, requested, phase, message, ready}` |
| `start_workspace` | action | `write` | Appends a `desired` event; repeating it appends nothing |
| `workspace_query` | action | `read` | Calls an allowed Zetta read: `server_info`, `list_projects`, `get_project`, `get_graph`, `list_connections`, `list_datasets`, `get_dataset`, `list_runs`, `get_run`, `get_run_report`, `list_models`, `get_model`, `model_health`, `list_indexes`, `get_index`, `search`, `list_jobs`, `get_job` |
| `workspace_act` | action | `write` | Calls an allowed Zetta action: `create_project`, `capture_intent`, `compile_intent`, `approve_plan`, `gate`, `run`, `create_connection`, `create_index`, `expose_model`, `unexpose_model`, `expose_index`, `unexpose_index`, `score`, `explain` |

Input for the last two is `{customer_id, capability, input}`, where `input` is the Zetta
capability's own input. Anything off the lists is refused with `invalid_input`, so nothing reaches
Zetta's administrative surface (`sql`, users, cloud provisioning) through lake. Audit records carry
`workspace.query`, `workspace.act` or `workspace.start` with the customer, the workspace and the
Zetta capability.

## How lake finds and calls a workspace

- **Slug.** `ws-` followed by the first 12 hexadecimal digits of the customer id
  (`control::workspace_slug`). lake stores nothing to find a workspace.
- **Request and status.** `orama_control.workspaces` on the Lake's own data-plane Zetta, an
  append-only table with columns `workspace_id, record_type, lake, customer_id, phase, url,
  message, updated_at`. lake appends `desired`; the operator appends `status` rows. The latest
  `status` by `updated_at` is the phase.
- **Credential.** The operator mounts each workspace's admin token as the file
  `$LAKE_WORKSPACE_DIR/<slug>`. lake reads it per call and never stores it.
- **Address.** `$LAKE_WORKSPACE_URL_TEMPLATE` with `{workspace}` replaced by the slug.
- **Ready** means the operator reported `Ready` *and* the token file is present. Calls before that
  fail with `workspace_not_ready`.

Reads are `GET /v1/<capability>` with the input as query parameters; actions are
`POST /v1/<capability>` with a JSON body. lake unwraps Zetta's `{ok, value}` envelope and passes
Zetta's own error message through.

## Configuration

| Variable | Set by | Meaning |
|---|---|---|
| `LAKE_DATA_PLANE_URL`, `LAKE_DATA_PLANE_TOKEN` | operator | The Lake's data-plane Zetta, where `orama_control.workspaces` lives |
| `LAKE_WORKSPACE_DIR` | operator | Directory of mounted workspace tokens |
| `LAKE_WORKSPACE_URL_TEMPLATE` | operator | Workspace address with `{workspace}` |
| `LAKE_NAME` | optional | The Lake's name for the `lake` column; defaults to `--node` |

With any of the first four missing, the pages say Design, Train and Ship are not available on this
lake, and the capabilities answer `store_unavailable`.

## Verify

```sh
ctest --test-dir build -R 'Workspaces|WebUi'
```

`Workspaces.*` covers the slug, request idempotency and latest-status state, readiness needing both
the phase and the token, calls reaching only the customer's own address with its own token, the
allow-lists, and another customer's user being refused. On a deployment: open `/design/projects`,
start the workspace, wait for Ready, create a project, and build a pipeline.

## Known limits

- A customer cannot delete or suspend a workspace yet, and there is no quota on workspaces.
- Adding a connection or a dataset has no form yet; use `workspace_act` with `create_connection`.
- A published model or index is reachable inside the workspace only; lake does not route to it.
- Workspace reads time out after 15 seconds and actions after 60. Run reports do not load on the
  current workspace image: `get_run_report` never answers (https://github.com/oramasearch/zetta/issues/1007).
