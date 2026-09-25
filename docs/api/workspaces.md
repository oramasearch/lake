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
| `delete_workspace` | action | `admin` | Needs `confirm` equal to the workspace id; marks the default service absent. Refused with `not_found` when no workspace is requested |
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

## Configuration

| Variable | Set by | Meaning |
|---|---|---|
| `LAKE_SERVICE_DIR` | operator | Mounted service connections, including the workspace's token |
| `LAKE_SERVICES_PER_CUSTOMER` | optional | Live services per customer, the workspace included (default 5) |

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
- A file cannot be uploaded as a dataset yet: Zetta has no row-upload capability
  (https://github.com/oramasearch/zetta/issues/1008).
- A published model or index is reachable inside the workspace only; lake does not route to it.
- Workspace reads time out after 15 seconds and actions after 60. Run reports do not load on the
  current workspace image: `get_run_report` never answers (https://github.com/oramasearch/zetta/issues/1007).
