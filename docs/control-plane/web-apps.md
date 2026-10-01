# Apps: Lake and operator contract

The shared contract is [lake#293](https://github.com/oramasearch/lake/issues/293), implemented by
[orama-operator#199](https://github.com/oramasearch/orama-operator/pull/199). Apps are distinct from
the application identity pools in [apps.md](../api/apps.md). The customer API is
[web-apps.md](../api/web-apps.md).

## Desired and observed state

The contract states:

> `mode` is `runtime | image`.
>
> `source_entrypoint` (text, optional). When empty, app-runtime reads the entrypoint from the
> app's own `app.toml`, so the operator passes `APP_ENTRYPOINT` only when set.
>
> `token_hashes_json` (jsonb, written by the **operator**):
> `{"source": "<sha256 hex>", "zetta": "<sha256 hex>|null", "events": "<sha256 hex>", "pull": "<sha256 hex>"}`.

Migration `0034_web_apps` creates `apps`. Migration `0035_web_apps_operator_contract` upgrades
earlier rows and adds the two agreed columns with defaults `''` and `'{}'::jsonb`. It converts the
earlier mode and deletion values, normalizes binding JSON, and removes the superseded
`app_credentials` table and its retained credentials. Apply it before deploying this binary.
The old partial unique indexes are removed before conversion, preserving deleted rows that reuse
a name or host, then recreated with the new deletion value.
Migration `0038_workspace_data_plane` adds immutable `workspace_id`, required on create.
Migration `0041_app_forward_auth` adds the short-lived, single-use authorization-code table used
to hand a Lake session to an app host without widening the Lake cookie domain. The current
deployment guard is `app_forward_auth_guard`; the operator must vendor the schema.
Workspace assignment and event-client cutover are tracked in
[orama-operator#216](https://github.com/oramasearch/orama-operator/issues/216).

| Columns | Writer |
|---|---|
| `id, customer_id, workspace_id, name, mode` | Lake |
| `source_repository, source_revision, source_path, source_entrypoint, image` | Lake |
| `framework, port, replicas, host, auth` | Lake |
| `bindings_json, env_json, secret_env_refs, desired, generation` | Lake |
| `token_hashes_json` | operator; Lake may only clear it for rotation |
| `phase, url, ready, message, observed_generation` | operator |
| `updated_at` | both |

`app_authorization_codes` is Lake-owned dispatch state, not workspace data. Lake stores only the
SHA-256 hash of each random 32-byte code, plus the app, user, host, expiry, and use time. Exchange
is an atomic consume conditioned on the same live app, host, workspace, and membership; expired
or spent rows cannot authenticate. The owner loop sweeps expired and spent rows every minute.

`source_repository` is the Lake repository ID; the API refuses a repository outside the app's
customer. Source tokens additionally require the exact `source_revision`. Generation increases
on each desired-state edit. Create and update enforce per-customer app and replica quotas under a
customer lock. Hosts are unique and must be one label beneath `LAKE_APPS_WILDCARD`.

The binding contract is:

> `{"zetta": {"scopes": ["read"]} | null, "services": ["<service_id>", ...]}`.
>
> The operator resolves each Service's connection from the lake services Secret
> (`lake-<lake>-services`, key per service id) and mounts it at
> `/var/run/orama/bindings/services/<service name>`.

Lake writes only those scopes and service IDs, with no token or token Secret reference. The
operator resolves the service connection. Plain environment values are scanned for secrets;
secret environment configuration remains represented by references.

## Operator tokens

> Tokens follow the agent-bus pattern: the operator generates them, and lake validates hashes.
> No plaintext token is ever stored in lake's database.
>
> It stores them in Secret `app-tokens` in the app's namespace, mounts them into the pod
> (`APP_TOKEN`, `/var/run/orama/bindings/zetta`) and into its own publisher, places the pull token
> in the app's image-pull Secret, and writes their **SHA-256 hashes** to `token_hashes_json`.

Lake hashes every presented credential and matches a purpose hash. It exposes no token
minting or credential-resolution capability. The registry rechecks the current row, hash, purpose
and target for every call, including MCP calls sharing a context. Matching hashes grant only:

| Purpose | Capabilities | Required target |
|---|---|---|
| `source` | `repository_tree`, `repository_file` | app repository and exact revision, owned by the app customer |
| `zetta` | `workspace_query`, `workspace_sql` | app customer and workspace with the `read` binding; existing read allow-list and SQL validation apply |
| `events` | `publish_app_event` | app ID and workspace, topic `app-events.<app>` |
| `pull` | OCI manifest and blob `GET`/`HEAD` through Basic user `app` | exact `<space>/<app name>` repository |

All other capabilities and registry targets are refused for app credentials. The pull token cannot
read another app or a cache repository and cannot write registry content. Ambiguous hashes are
refused. Hash changes invalidate the old token on the next authentication or registry call.
Clearing hashes revokes access until the operator registers replacement hashes; the operator owns
generation and delivery of replacements. Unbinding revokes Zetta access even before the operator
clears its hash. Source, Zetta and pull access stop at deletion intent. Deletion also drops the
workspace event topic; later publication is refused so teardown cannot recreate it.

## Lifecycle and events

> `desired = running | stopped | deleted`. Stopped means replicas 0, with the `LakeApp` kept.
>
> The operator deletes the `LakeApp` (a finalizer tears down the namespace and DNS record), then
> writes `phase = Deleted`. Lake removes the row after it sees `Deleted`.

Lake keeps the requested replica count at 1–100. `stop_app` writes `stopped`; the operator sets
the workload to zero. `start_app` restores the requested count through `running`. The owner loop
removes rows with both `desired = deleted` and `phase = Deleted` on its cleanup pass; stopped and
unacknowledged deleting rows remain. Status phases are `Pending`, `Deploying`, `Running`,
`Degraded`, `Stopped`, `Failed`, and `Deleted`.

Every desired-state insert/edit sends `NOTIFY lake_apps, '<app id>'` in its transaction. The
operator also re-lists rows every 60 seconds. Lifecycle events carry the acting principal and
generation. The operator's event request is accepted directly:

```json
{"app_id":"<app id>","workspace_id":"<workspace id>","event":{"phase":"Running","ready":true,"message":"available"}}
```

The customer is resolved from the app. Authentication restricts the events bearer to that app.
The supplied workspace must match the persisted app workspace. Events have 24-hour / 5,000-event
retention and never use the Lake bus.
Build Jobs belong to `spec.builds` (orama-operator#195); Lake has no `spec.apps.buildkit` setting.

## Verification and changes

Run both native CTest suites with `LAKE_TEST_DB_URI` set to a local PostgreSQL database.
`WebApps.*` covers capabilities, private source reads, read-only workspace calls, event delivery,
cross-app/customer/revision refusal, rotation and deletion cleanup. `PostgresWebApps.*` covers
operator hash writes, scoped authentication, preserved hashes during desired edits, quotas,
`lake_apps` notifications, status feedback and physical row removal. Apply the full schema twice
to check upgrade repeatability. Any future contract change must update these docs and the operator
contract together.
