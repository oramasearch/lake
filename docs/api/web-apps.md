# Ship Apps

Apps are customer-owned desired state reconciled as operator `LakeApp` resources. Application
identity pools remain a separate entity in [apps.md](apps.md). The authoritative
[Lake/operator contract](../control-plane/web-apps.md) records the schema and token ownership.

## Customer capabilities

Every app capability requires `workspace_id` (`workspaces.id`), including
event publication by the operator. Creates persist it immutably; omitted, unknown or foreign
IDs return `invalid_input`. There is no default-workspace or shared-bus fallback. App events use
the workspace's `app-events.<id>` with 24-hour/5,000-event retention; deletion drops the topic,
and scheduled cleanup removes orphaned topics. App tokens cannot address another workspace.

REST and MCP expose the same registry, customer policy and signed audit records. Reads advertise
`x-lake-action: read`; mutations advertise `x-lake-action: write`.

| Capability | Input beyond `customer_id` | Effect |
|---|---|---|
| `list_apps`, `get_app` | `app_id` for get | desired and observed state |
| `create_app` | name, mode, source or image, framework, port; optional replicas/host/auth/bindings/env | generation 1 and create event |
| `deploy_app`, `rollback_app` | `app_id` (ApplicationAgent may use `app` for deploy), source or image | new desired deployment and generation |
| `delete_app` | `app_id`, `confirm` equal to name | `desired = deleted` |
| `set_app_env`, `unset_app_env` | `app_id`, key; set takes value or secret_ref | scanned environment update |
| `set_app_auth` | `app_id`, auth; public also requires `confirm_public` | lake sign-in or public access |
| `restart_app`, `stop_app`, `start_app` | `app_id` | lifecycle intent |
| `scale_app` | `app_id`, replicas | desired count within customer quota |
| `set_app_zetta_binding` | `app_id`, attached | attach or detach read-only workspace access |
| `bind_service_to_app` | `app_id`, service_id | bind a service from the same customer |
| `unbind_service_from_app` | `app_id`, service_id | detach an existing service binding |
| `create_service_and_bind_to_app` | `app_id`, service kind/name and optional size | create and bind in one call |
| `app_logs` | `app_id`, optional since/limit/direction | Loki log page |
| `receive_app_events` | `app_id`, optional from_sequence/wait_ms | ordered events with a cursor |
| `publish_app_event` | `app_id`, event; customer_id optional | scoped operator status publication |

The agreed runtime and binding values are:

> `mode` is `runtime | image`.
>
> `bindings_json` is exactly
> `{"zetta": {"scopes": ["read"]} | null, "services": ["<service_id>", ...]}`.

`create_app`, `update_app` and `bind_service_to_app` accept a live service's id or its unique name
in the customer; the stored binding is always the id. An ambiguous or unknown name is `not_found`.

Runtime input uses `source: {repository, revision, path?, entrypoint?}`. Repository is a Lake
repository ID in the customer. Path defaults to `.`; entrypoint defaults to empty so app-runtime
reads the app's `app.toml`. Image mode uses `image`. Deploy and rollback accept the same source or
image input. The old `source` mode value is rejected.

`bindings.zetta` accepts `true`, `["read"]`, or `{"scopes":["read"]}` to bind; false/null/omitted
means no binding. Writes normalize to the quoted contract. Service IDs stay strings. Lake never
adds a workspace token or an opaque token Secret reference to bindings, code, or environment.
`auth` defaults to `lake`; `public` on ApplicationAgent input is normalized to stored `none`.
Names are DNS labels and hosts must be one label beneath
`LAKE_APPS_WILDCARD`. `LAKE_APPS_PER_CUSTOMER` and `LAKE_APP_REPLICAS_PER_CUSTOMER` enforce quotas;
zero means no configured limit.

App names share the wildcard's zone with the platform. Lake refuses `lake`, `www`, `api`, `app`,
`apps`, `admin`, `mcp`, `docs`, `status`, `mail`, `smtp`, `auth`, `login`, `sso`, `id`, `cdn`,
`static`, `assets`, `grafana`, `prometheus`, `argocd`, `registry`, `git`, `dev`, `staging`, `prod`,
and `test`, plus every name that starts with `lake-` or `orama`. Operators add deployment-specific
names with the comma-separated `LAKE_APPS_RESERVED_NAMES`; surrounding whitespace is ignored.
`create_app` returns `invalid_input`, `<name> is reserved`, at `/name` before it writes desired
state. An omitted, null, or empty `host` remains the default `<name>.<LAKE_APPS_WILDCARD>`, so
`reports` with `LAKE_APPS_WILDCARD=oramadev.net` becomes `reports.oramadev.net`.

ApplicationAgent must propose a non-reserved name and should leave `host` unset unless the person
asked for a specific address. Its `create_app` tool description identifies platform-host names as
reserved, and the confirmed turn receives the same validation as every other caller.

The operator owns the separate existing-record check: before publishing DNS, it must refuse a host
that already has a DNS record it does not own and report the conflict through App status. That
behavior belongs to the [`LakeApp` controller](https://github.com/oramasearch/orama-operator/issues/193);
Lake does not probe or replace DNS records.

Changing `auth` to `none` requires `confirm_public: true`. Attaching Zetta grants only its fixed
read scope. Detaching it clears the operator token hash immediately; service detach removes only
that service from the normalized binding list. Deploy and rollback events retain their exact
source or image input so the UI can offer history-bound rollback.

## Lake-protected app authentication

An app with `auth = lake` uses nginx forward authentication without exposing Lake's browser
session to customer code. The operator configures the app ingress with:

```yaml
nginx.ingress.kubernetes.io/auth-url: http://lake-prod.lake.svc.cluster.local:80/w/<workspace>/v1/apps/auth
nginx.ingress.kubernetes.io/auth-signin: https://<lake public host>/apps/authorize?app=<app id>&rd=$scheme://$host$escaped_request_uri
nginx.ingress.kubernetes.io/auth-always-set-cookie: "true"
```

`GET /v1/apps/auth`, including its `/w/<workspace>/v1/apps/auth` form, resolves a live protected
app by the host of `X-Original-URL` (ingress-nginx sets `Host` to Lake's Service and sends no
`X-Forwarded-Host` on the auth subrequest), falling back to `X-Forwarded-Host` or `Host`. A valid host-only `lake_app_session` returns 200 with
`X-Orama-User` and `X-Orama-Space`. The signed token carries `app_id`, `user_id`, `workspace_id`,
`iat`, and `exp`; its HMAC-SHA256 key is derived from the instance KEK with HKDF label
`lake/app-session/v1`, and it expires after 12 hours. Lake rechecks current workspace membership
on every request. Invalid, expired, cross-app, or no-longer-authorized tokens return 401.

When no app cookie is usable, nginx sends the browser to
`GET /apps/authorize?app=<app id>&rd=<original URL>` on Lake's own host. Lake's normal browser
session is required; a signed-out request goes through `/sign-in` and returns to the authorize
request after authentication. The redirect destination must be HTTPS on exactly the stored app
host, and the user must still belong to the app workspace. Lake then creates a random, 60-second,
single-use code and redirects to `rd` with `__lake_code` appended. The forward-auth endpoint
atomically spends that code, returns the identity headers, and sets:

```text
lake_app_session=<token>; Path=/; Secure; HttpOnly; SameSite=Lax
```

There is deliberately no `Domain` attribute, so customer app code receives only its own app
cookie and never Lake's `lake_session`. `/logout` cannot clear cookies on app hosts. The 12-hour
expiry and per-request membership check are the revocation path. Lake audits code exchanges and
rate-limited denials, but does not audit each successful cookie check.

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

Plain environment values are scanned; detected credentials return `secret_required`. On create,
ApplicationAgent may supply `{"KEY":{"secret":"operator-reference"}}`; Lake separates it into
the App's masked Secret references. Secret environment updates use a reference instead of a clear
value. Audit records contain the key and
whether it is secret, never its value. Bindings and operator tokens are delivered as mounted
Secrets.

## ApplicationAgent confirmation

`agent-application` is read-only on its first `app` task. A later task receives repository writes,
`create_app`, and `deploy_app` only when its body contains the exact reviewed proposal at
`context.confirmed_proposal`. Ship → Apps verifies that proposal against the completed task before
starting the later turn. Ask Orama can delegate app requests through `agent_turn`; it never receives
the App write capabilities itself. See [agent-launches.md](agent-launches.md) and
[the page contract](../ui/apps.md).

## Verification

`WebApps.*` and `PostgresWebApps.*` cover the memory and Postgres stores, built-in and configured
reserved-name refusal, direct-zone default hosts, purpose restrictions, cross-app/customer/revision
refusal, private repository reads, read-only workspace calls, published/received operator events,
rotation, stop/start and deletion cleanup. Full static and shared native CTest suites exercise
registration, REST/OpenAPI/MCP discovery and auditing.
