# Ship Apps

Apps are customer-owned desired state reconciled as operator `LakeApp` resources. Application
identity pools remain a separate entity in [apps.md](apps.md). The authoritative
[Lake/operator contract](../control-plane/web-apps.md) records the schema and token ownership.

## Customer capabilities

REST and MCP expose the same registry, customer policy and signed audit records. Reads advertise
`x-lake-action: read`; mutations advertise `x-lake-action: write`.

| Capability | Input beyond `customer_id` | Effect |
|---|---|---|
| `list_apps`, `get_app` | `app_id` for get | desired and observed state |
| `create_app` | name, mode, source or image, framework, port; optional replicas/host/auth/bindings/env | generation 1 and create event |
| `deploy_app`, `rollback_app` | `app_id`, source or image | new desired deployment and generation |
| `delete_app` | `app_id`, `confirm` equal to name | `desired = deleted` |
| `set_app_env`, `unset_app_env` | `app_id`, key; set takes value or secret_ref | scanned environment update |
| `restart_app`, `stop_app`, `start_app` | `app_id` | lifecycle intent |
| `scale_app` | `app_id`, replicas | desired count within customer quota |
| `bind_service_to_app` | `app_id`, service_id | bind a service from the same customer |
| `create_service_and_bind_to_app` | `app_id`, service kind/name and optional size | create and bind in one call |
| `app_logs` | `app_id`, optional since/limit/direction | Loki log page |
| `receive_app_events` | `app_id`, optional from_sequence/wait_ms | ordered events with a cursor |
| `publish_app_event` | `app_id`, event; customer_id optional | scoped operator status publication |

The agreed runtime and binding values are:

> `mode` is `runtime | image`.
>
> `bindings_json` is exactly
> `{"zetta": {"scopes": ["read"]} | null, "services": ["<service_id>", ...]}`.

Runtime input uses `source: {repository, revision, path?, entrypoint?}`. Repository is a Lake
repository ID in the customer. Path defaults to `.`; entrypoint defaults to empty so app-runtime
reads the app's `app.toml`. Image mode uses `image`. Deploy and rollback accept the same source or
image input. The old `source` mode value is rejected.

`bindings.zetta` accepts `true`, `["read"]`, or `{"scopes":["read"]}` to bind; false/null/omitted
means no binding. Writes normalize to the quoted contract. Service IDs stay strings. Lake never
adds a workspace token or an opaque token Secret reference to bindings, code, or environment.
`auth` defaults to `lake`; names are DNS labels and hosts must be one label beneath
`LAKE_APPS_WILDCARD`. `LAKE_APPS_PER_CUSTOMER` and `LAKE_APP_REPLICAS_PER_CUSTOMER` enforce quotas;
zero means no configured limit.

## Tokens and events

> The operator generates them, and lake validates hashes. No plaintext token is ever stored in
> lake's database.
>
> source → `repository_tree`/`repository_file` on that repository only;
> zetta → `workspace_query`/`workspace_sql` reads on that customer only;
> events → `publish_app_event` for that app only.

The operator holds tokens in Secret `app-tokens` and writes SHA-256 hex hashes to
`token_hashes_json`. Lake issues and returns no app tokens. Source calls must also name the app's
exact revision. Zetta calls retain the workspace read allow-list and read-only SQL checks. Every
other capability or target is refused. New hashes invalidate prior tokens; unbinding or deletion
revokes workload access. Events remain available during teardown until the operator reports
`Deleted`.

Protected calls without authentication and well-formed calls with a wrong-purpose token return
`unauthorized`. Malformed input on an authenticated call returns `invalid_input`; input validation
precedes purpose authorization. Tests exercise those cases separately, and Postgres registry
fixtures set the instance encryption key so signed-audit recording can complete.

The operator calls `publish_app_event` with `{"app_id":"...","event":{"phase":"Running"}}`.
Lake derives the customer and publishes to `app-events.<app>`. Customer callers may also supply the
existing `type`, `message`, and `status` fields. Events record the acting app identity or customer
principal and the current generation. `receive_app_events` uses a one-based sequence and supports
long polling.

## Stop, delete and environment

> `desired = running | stopped | deleted`. Stopped means replicas 0, with the `LakeApp` kept.
>
> Lake removes the row after it sees `Deleted`.

The row retains its requested replica count (1–100) while stopped. The operator scales the
workload to zero; starting restores that count. Delete requires the app name, then waits for the
operator's `Deleted` acknowledgement before Lake physically removes the row. Status exposes
`phase`, `url`, `ready`, `message`, and `observed_generation`.

Plain environment values are scanned; detected credentials return `secret_required`. Secret
environment updates use a reference instead of a clear value. Audit records contain the key and
whether it is secret, never its value. Bindings and operator tokens are delivered as mounted
Secrets.

## Verification

`WebApps.*` and `PostgresWebApps.*` cover the memory and Postgres stores, purpose restrictions,
cross-app/customer/revision refusal, private repository reads, read-only workspace calls,
published/received operator events, rotation, stop/start and deletion cleanup. Full static and
shared native CTest suites exercise registration, REST/OpenAPI/MCP discovery and auditing.
