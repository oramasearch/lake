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
| `list_apps`, `get_app` | `app_id` for get | desired and observed state, and `stores` (what it mounts, lake#666) |
| `create_app` | name, mode, source or image, framework, port; optional replicas/host/auth/bindings/env/stores; public also requires `confirm_public` | generation 1 and create event |
| `deploy_app`, `rollback_app` | `app_id` (Application Agent may use `app` for deploy), source or image | new desired deployment and generation |
| `delete_app` | `app_id`, `confirm` equal to name | `desired = deleted` |
| `set_app_env`, `unset_app_env` | `app_id`, key; set takes `value`, `secret` (`{mount_id, name, field}`) or `secret_ref` (`lake-secret://…`) | scanned environment update; a secret is checked, never read back |
| `set_app_auth` | `app_id`, auth; public also requires `confirm_public` | lake sign-in or public access |
| `restart_app`, `stop_app`, `start_app` | `app_id` | lifecycle intent; a restart sets `restart_generation`, which the operator puts on the pod template so the pods are replaced ([restart](../control-plane/web-apps.md#restart), lake#1228) |
| `scale_app` | `app_id`, replicas | desired count within customer quota |
| `set_app_zetta_binding` | `app_id`, attached | attach or detach read-only workspace access |
| `bind_service_to_app` | `app_id`, service_id | bind a service of the app's own workspace; needs `admin` on the account, as `service_connection` does, since the app receives the service's credential (lake#945) |
| `set_app_stores` | `app_id`, `stores: [{store_id, mount_path?, read_only?}]` | replace the workspace storage the app mounts ([storage.md](storage.md#mounting-a-store)): a volume at a path (one workload per volume, else `conflict`), a bucket as `STORE_<NAME>_*` environment variables; new generation and a `stores` event |
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

A service binding hands the app that service's connection credential, so it needs `admin` on the account (the right `service_connection` needs) and a service of **the app's own workspace**. A service of another workspace is `not_found` by id and by name (lake#945).

`create_app`, `update_app` and `bind_service_to_app` accept a live service's id or its unique name
in the customer; the stored binding is always the id. An ambiguous or unknown name is `not_found`.

Runtime input uses `source: {repository, revision, path?, entrypoint?}`. Repository is a Lake
repository ID in the customer. Path defaults to `.`; entrypoint defaults to empty so app-runtime
reads the app's `app.toml`. Image mode uses `image`. Deploy and rollback accept the same source or
image input. The old `source` mode value is rejected.

**An image is pinned by digest** (lake#783): `name@sha256:<64 hex digits>`, optionally with a tag
before the digest (`name:1.27@sha256:…`). That is the `LakeApp` CRD's `spec.image` pattern, and
the operator fails any other image app ("image must be pinned by SHA-256 digest"), so
`create_app`, `deploy_app` and `rollback_app` refuse anything else with `invalid_input` at `/image`
before the app is created or changed. Lake does not resolve a tag to a digest. A finished
`build_app` reports its image already pinned (`get_build` `image`), and the app page's *Deploy
build* uses only that. A stdio MCP connection's App (below) is refused with `not_supported` when
`LAKE_MCP_RUNNER_IMAGE` is not pinned. To verify: `create_app` with
`nginxinc/nginx-unprivileged:1.27-alpine` answers `invalid_input` at `/image` and lists no app;
`WebApps.AnImageAppIsRefusedUnlessItsImageIsPinnedByDigest` covers it.

`bindings.zetta` accepts `true`, `["read"]`, or `{"scopes":["read"]}` to bind; false/null/omitted
means no binding. Writes normalize to the quoted contract. Service IDs stay strings. Lake never
adds a workspace token or an opaque token Secret reference to bindings, code, or environment.
`auth` defaults to `lake`; `public` on Application Agent input is normalized to stored `none`.
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
`reports` with `LAKE_APPS_WILDCARD=example.com` becomes `reports.example.com`.

An explicit `host` is held to the same names (lake#947): its label, the part before
`.<LAKE_APPS_WILDCARD>`, may not be a reserved name, so `{name: "ok", host: "app.example.com"}` is
refused with `invalid_input`, `<host> is reserved: <label> is a reserved name`, at `/host`. Nor may it
be one of the Lake's own hosts: its `LAKE_PUBLIC_HOST`, its `LAKE_APEX_HOST` or the host of any region in
`LAKE_REGIONS` (`<host> is a host of the Lake itself`, at `/host`). Those sit one label beneath the
wildcard when the wildcard is the zone itself, as `example.com` is on the managing cluster.

Application Agent must propose a non-reserved name and should leave `host` unset unless the person
asked for a specific address. Its `create_app` tool description identifies platform-host names as
reserved, and the confirmed turn receives the same validation as every other caller.

The operator owns the separate existing-record check: before publishing DNS, it must refuse a host
that already has a DNS record it does not own and report the conflict through App status. That
behavior belongs to the `LakeApp` controller (orama-operator#193);
Lake does not probe or replace DNS records.

Changing `auth` to `none` requires `confirm_public: true`, and so does creating an app with `auth`
`none` or `public` (lake#452): otherwise `invalid_input` at `/confirm_public`. An Application Agent
turn has no `confirm_public` of its own; its `create_app` is accepted when the person who
confirmed the proposal acknowledged that it is public, which lake records as the turn's
`context.confirm_public: true`. Attaching Zetta grants only its fixed
read scope. Detaching it clears the operator token hash immediately; service detach removes only
that service from the normalized binding list. Deploy and rollback events retain their exact
source or image input so the UI can offer history-bound rollback.

## Apps Lake starts for MCP servers

A stdio MCP connection (lake#659, [mcp-connections.md](mcp-connections.md#servers-that-speak-only-stdio))
creates an ordinary App, as the person: `mode: image` with `LAKE_MCP_RUNNER_IMAGE`, `framework:
mcp`, port `8080`, named `mcp-<prefix>-<4 hex>`, with `MCP_RUNNER_TOKEN` and `MCP_STDIO_COMMAND`
from a secret Lake wrote in the person's mount. It is listed, metered, logged, stopped and started
like any App; deleting its connection deletes it.

## Lake-protected app authentication

An app with `auth = lake` uses nginx forward authentication without exposing Lake's browser
session to customer code. The operator configures the app ingress with:

```yaml
nginx.ingress.kubernetes.io/auth-url: http://lake-prod.lake.svc.cluster.local:80/w/<workspace>/v1/apps/auth
nginx.ingress.kubernetes.io/auth-signin: https://<lake public host>/apps/authorize?app=<app id>&rd=$scheme://$host$escaped_request_uri
nginx.ingress.kubernetes.io/auth-always-set-cookie: "true"
nginx.ingress.kubernetes.io/auth-response-headers: X-Orama-User,X-Orama-Space
```

`GET /apps/authorize` hands the app its session only for a **browser session** (the session cookie
on the Lake host). A request carrying `Authorization: Bearer`, with or without a cookie, is `403`
(LSR-208, lake#1058), so no script or tool carries a Lake credential through to a tenant's app host.
The e2e probes (`tools/e2e/agents.py`) reach an app without any Lake credential: a public app
answers 2xx, and a private one redirects an anonymous visitor to `/apps/authorize`.

`GET /v1/apps/auth`, including its `/w/<workspace>/v1/apps/auth` form, resolves a live protected
app by the host of `X-Original-URL` (ingress-nginx sets `Host` to Lake's Service and sends no
`X-Forwarded-Host` on the auth subrequest), falling back to `X-Forwarded-Host` or `Host`. A valid host-only `__Host-lake_app_session` returns 200 with
`X-Orama-User` and `X-Orama-Space`. The signed token carries `app_id`, `user_id`, `workspace_id`,
`iat`, and `exp`; its HMAC-SHA256 key is derived from the instance KEK with HKDF label
`lake/app-session/v1`, and it expires after 12 hours. Lake rechecks current workspace membership
on every request. Invalid, expired, cross-app, or no-longer-authorized tokens return 401.

**Which identity headers an app may trust** (app-runtime#11). Because the Ingress names
`X-Orama-User` and `X-Orama-Space` in `auth-response-headers`, ingress-nginx sets them from this
endpoint's answer and a copy sent by the client never reaches a Lake-protected app. No other header
is set for the app: `X-Orama-User-Id`, `X-Auth-Request-*` and the like arrive as the client wrote them,
on every app. A public app (`auth = none`) has no forward auth at all, so **every** identity header it
receives, `X-Orama-User` included, came from the client. The operator tells the pod which case it is
in `APP_AUTH` (`lake` or `public`). app-runtime's `current_user()` returns the member only when
`APP_AUTH=lake`, reads only `X-Orama-User`, and removes the forgeable headers before app code sees the
request. **An image-mode app that does not use app-runtime must not treat these headers as identity
unless it is Lake-protected**, and must never trust any identity header other than `X-Orama-User` and
`X-Orama-Space`.

When no app cookie is usable, nginx sends the browser to
`GET /apps/authorize?app=<app id>&rd=<original URL>` on Lake's own host. Lake's normal browser
session is required; a signed-out request goes through `/sign-in?return_to=<the authorize request>`
and returns to it after authentication. The return travels twice (lake#561): in the URL, which the
web app reads and honours after it signs the person in, and in the two-minute HttpOnly
`lake_sign_in_return` cookie, which the no-JavaScript hosted pages and the form sign-ins read. A
signed-in visit to `/sign-in?return_to=…` (or `?next=<same-origin path>`) goes straight there; only
an `/apps/authorize` return or a same-origin path is honoured, anything else goes to the usual
page. The redirect destination must be HTTPS on exactly the stored app
host, and the user must still belong to the app workspace. Lake then creates a random, 60-second,
single-use code and redirects to `rd` with `__lake_code` appended. The forward-auth endpoint
atomically spends that code, returns the identity headers, and sets:

```text
__Host-lake_app_session=<token>; Path=/; Secure; HttpOnly; SameSite=Lax
```

The cookie is always `Secure`, so it always carries the `__Host-` prefix (LSR-115): a sibling
host can neither set nor shadow it, and only that name is read.

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
> events → `publish_app_event` for that app only;
> pull → OCI manifest and blob `GET`/`HEAD` for `<space>/<app name>` only.

The operator holds tokens in Secret `app-tokens` and writes SHA-256 hex hashes to
`token_hashes_json`. Lake issues and returns no app tokens. Source calls must also name the app's
exact revision. Zetta calls retain the workspace read allow-list and read-only SQL checks; app-runtime's
`workspace.sql()` (`/zetta/v1/sql`) runs as `workspace_sql`, read-only and at most 5,000 rows a query
([SQL on the Zetta route](workspaces.md#sql-on-this-route), lake#1229). Every
other capability or target is refused. The registry accepts the pull token only as HTTP Basic user
`app`, with the token as the password. It cannot read another app or `<space>/cache/...`, and it
cannot start uploads or publish manifests. Rotation is a token generation the operator
acknowledges: a hash verifies only while the operator's generation matches the one Lake asked for
([token rotation](../control-plane/web-apps.md#token-rotation), lake#1070). New hashes invalidate prior tokens; unbinding or
deletion revokes workload access. Events remain available during teardown until the operator
reports `Deleted`.

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

Plain environment values are scanned; detected credentials return `secret_required`. Bindings and
operator tokens are delivered as mounted Secrets.

## Usage

A running app is metered for its replicas' CPU, RAM, GPU and GPU memory (lake#643) and, since
lake#661, for the bytes it receives and sends through the ingress: the Orama operator reads the
ingress controller's per-Ingress byte counters and reports them by app and hour (`app_transfer`,
migration `0081`), and Lake adds each closed hour to the app's workspace as `transfer` (`in`, `out`,
`object_id` the app), billed at the `transfer` rate. An hour appears on the Usage page once it has
closed ([metering](metering.md#app-traffic)).

## Secret environment

A secret environment value names one field of one secret in a secret mount of the app's own
workspace ([secrets.md](secrets.md)); lake's secret store is its source of truth and a Kubernetes
Secret is only the projection the operator writes at deploy (lake#625,
[secrets.md](secrets.md)).

- `set_app_env` takes `secret: {mount_id, name, field}`, or `secret_ref` as the string
  `lake-secret://<mount id>/<name>#<field>` (the name may contain `/` and `#`; the field follows the
  last `#` and must be a Kubernetes Secret key, `[-._a-zA-Z0-9]+`). Exactly one of `value`, `secret`
  and `secret_ref`.
- `create_app` takes the same in `env`: `{"KEY": {"secret": {"mount_id": "…", "name": "…", "field":
  "…"}}}` or `{"KEY": {"secret": "lake-secret://…"}}` (Application Agent's form).
- Lake stores the reference string in `secret_env_refs` and answers it there; the value is never
  read back or answered.
- Before it writes, lake checks with the caller's own credential that the mount exists and the
  caller may read it (`get_secret_mount`), that it belongs to the app's workspace (else
  `invalid_input`), and that the secret is live and has the field (`read_secret`, else `not_found`).
  Those two reads are audited in the mount's workspace like any other.
- **A confirmed Application Agent turn holds no secret read** (lake#891). When the person confirms a
  proposal, `agent_turn` checks every secret reference in the proposal's `app.env` with the
  person's own `get_secret_mount` and `read_secret`, as above; a reference they cannot satisfy
  refuses the confirmation (path `/context/confirmed_proposal/app/env/<KEY>/secret`). The turn's
  `create_app` then accepts exactly the references its confirmed proposal carries under the same
  key, read from the turn's request record that lake wrote after verifying the proposal against
  `proposal_sha256`, and checks only, server-side, that each mount still exists and belongs to the
  app's workspace. Any other reference the turn sends is checked with the turn's own credential,
  which has no secret read, and refused. A secret deleted between confirmation and `create_app`
  shows when the operator projects it.
- A Kubernetes Secret name (`db-url`, `db-credentials/password`) is refused with `invalid_input`;
  apps written before lake#625 kept theirs in `secret_env_refs`, and none existed on dev.
- The audit event of `set_app_env` names the key and the reference, never a value.

The operator's half (a `LakeSealedSecret` per lake secret, delivered by `mint_sealed_secret` and
refreshed every five minutes, the field copied into the app and the pods rolled on a new version) is
orama-operator `docs/lake/apps.md#lake-secret-references-lake625`.

## Application Agent confirmation

`agent-application` is read-only on its first `app` task. A later task receives repository writes,
`create_app`, and `deploy_app` only when its body contains the exact reviewed proposal at
`context.confirmed_proposal`. `agent_turn` verifies that proposal itself (lake#561): the call names
the turn that proposed it as `proposal_task_id` and the digest the person reviewed as
`proposal_sha256` (`get_agent_task` returns it), and lake compares both with the proposal stored in
that turn's answer. It also checks the proposal's secret references as the person
([above](#secret-environment), lake#891), so the turn never needs a secret read. A public app (`auth` `none` or `public`) also needs `context.confirm_public:
true`. The full input and refusals are in [apps.md](apps.md#applicationagent-and-ship-apps). Ask Orama can delegate app requests through `agent_turn`; it never receives
the App write capabilities itself. See [agent-launches.md](agent-launches.md) and
[the page contract](../ui/apps.md).

## When an app fails

A rollout that ends Degraded, failed or crash-looping, and an app image build that fails, each
record a notification for the person who deployed the app, shown on the Apps list, the app page and
Account → Notifications. *Fix with the Application Agent* (`repair_app`) and *Change this app*
(`change_app`) start Application Agent turns that propose a diff and, confirmed, commit it to the
app's repository and redeploy; opt-in auto-fix (`set_auto_fix`) lets the agent do that itself up to
a number of attempts. `create_app` and `deploy_app` record who deployed the app and its source for
this. See [notifications.md](notifications.md) (lake#1207).

## Verification

`WebApps.*` and `PostgresWebApps.*` cover the memory and Postgres stores, built-in and configured
reserved-name refusal, direct-zone default hosts, purpose restrictions, cross-app/customer/revision
refusal, private repository reads, read-only workspace calls, published/received operator events,
rotation, stop/start and deletion cleanup. Full static and shared native CTest suites exercise
registration, REST/OpenAPI/MCP discovery and auditing.
