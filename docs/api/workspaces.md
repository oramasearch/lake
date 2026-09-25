# Workspaces: Design, Train and Ship on Zetta

**Status: implemented 2026-09-25.** lake's Design, Train and Ship pages delegate to the Zetta
engine. Each customer that uses them gets its own Zetta cluster, a **workspace**, because Zetta has
no tenancy: a cluster's `list_projects`, `list_models` and the rest return everything on it
(liquid-bridge decision 45). The operator side is `orama-operator` `docs/lake/workspaces.md`.

## What a person sees

| Section | Page | Zetta capabilities behind it |
|---|---|---|
| Design | `/design/projects`, `/design/project?name=` | `list_projects`, `get_project`, `create_project`, `capture_intent` then `compile_intent` (build a pipeline from what the project should predict), `approve_plan` (approve a proposed pipeline), `gate` then `run` (check and run an approved pipeline) |
| Train | `/train/data` | `list_connections`, `list_datasets`; **Add a connection** (`create_connection`, Postgres or Snowflake), **Remove** (`delete_connection`), **Pull a dataset** (`create_dataset_from_query` from a saved connection with one read-only `SELECT`) |
| Train | `/train/runs`, `/train/run?id=` | `list_runs`, `get_run_report` |
| Ship | `/ship/models` | `list_models`, `expose_model`, `unexpose_model` (publish a model's `POST /v1/models/<name>/predict` inside the workspace) |
| Ship | `/ship/indexes` | `list_indexes` |
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

All five are in lake's registry, so REST, OpenAPI and MCP have them too. Each resolves the
customer's policy object and is authorized by lake before anything reaches Zetta.

| Capability | Kind | lake action on the customer | What it does |
|---|---|---|---|
| `workspace_status` | read | `read` | `{workspace, requested, phase, message, ready}` |
| `start_workspace` | action | `write` | Appends a `desired` event; repeating it appends nothing |
| `delete_workspace` | action | `admin` | Needs `confirm` equal to the workspace id; appends a `withdrawn` event. Refused with `not_found` when no workspace is requested |
| `workspace_query` | action | `read` | Calls an allowed Zetta read: `server_info`, `list_projects`, `get_project`, `get_graph`, `list_connections`, `list_datasets`, `get_dataset`, `list_runs`, `get_run`, `get_run_report`, `list_models`, `get_model`, `model_health`, `list_indexes`, `get_index`, `search`, `list_jobs`, `get_job` |
| `workspace_act` | action | `write` | Calls an allowed Zetta action: `create_project`, `capture_intent`, `compile_intent`, `approve_plan`, `gate`, `run`, `create_connection`, `delete_connection`, `create_dataset_from_query`, `create_index`, `expose_model`, `unexpose_model`, `expose_index`, `unexpose_index`, `score`, `explain` |

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

## How lake finds and calls a workspace

- **Slug.** `ws-` followed by the first 12 hexadecimal digits of the customer id
  (`control::workspace_slug`). lake stores nothing to find a workspace.
- **Request and status.** `orama_control.workspaces` on the Lake's own data-plane Zetta, an
  append-only table with columns `workspace_id, record_type, lake, customer_id, phase, url,
  message, updated_at`. lake appends `desired` and `withdrawn`; the operator appends `status`
  rows. The workspace is requested when its newest `desired` is later than its newest
  `withdrawn` (a tie counts as withdrawn, as the operator decides). The phase is the latest
  `status` by `updated_at` that is not older than the newest withdrawal, so a deleted workspace's
  old `Ready` never makes a new one look callable. After a withdrawal the operator writes
  `Deleted` once the cluster and its volume are gone; a restarted workspace shows no phase until
  the operator picks it up. Status times are not compared with the request time, which comes from
  lake's clock rather than the operator's.
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

`Workspaces.*` covers the slug, request idempotency and latest-status state, withdrawal and
restart (old statuses ignored, a same-second withdrawal winning), the delete confirmation, readiness needing both
the phase and the token, calls reaching only the customer's own address with its own token, the
allow-lists, and another customer's user being refused. On a deployment: open `/design/projects`,
start the workspace, wait for Ready, create a project, and build a pipeline. Deleting is verified
the same way: tick the confirmation, delete, and watch the page return to **Start your workspace**
while `kubectl -n lake get oramazettacluster` loses `<lake>-<workspace>`.

## Known limits

- A workspace cannot be suspended, only deleted. The quota is per Lake
  (`spec.workspaces.maxWorkspaces`, orama-operator `docs/lake/workspaces.md`), not per customer.
- A file cannot be uploaded as a dataset yet: Zetta has no row-upload capability
  (https://github.com/oramasearch/zetta/issues/1008).
- A published model or index is reachable inside the workspace only; lake does not route to it.
- Workspace reads time out after 15 seconds and actions after 60. Run reports do not load on the
  current workspace image: `get_run_report` never answers (https://github.com/oramasearch/zetta/issues/1007).
