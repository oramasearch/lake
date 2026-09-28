# Ship web apps

Ship web apps are named `App` in the API and `LakeApp` in the operator. They are distinct from
the application identity pools documented in [apps.md](apps.md). The customer owns desired state;
the Orama operator reconciles it and reports observed state.

## Capabilities

Every capability is available through REST and MCP, requires a customer ID, authorizes on that
customer's policy object, and records a signed registry audit event. Reads advertise
`x-lake-action: read`; mutations advertise `x-lake-action: write`.

| Capability | Input beyond `customer_id` | Result or effect |
|---|---|---|
| `list_apps` | — | live and deleting apps in the space |
| `get_app` | `app_id` | desired and observed state |
| `create_app` | name, mode, exactly one of source or image, framework, port, optional host/auth/replicas/bindings/env | generation 1 desired row and `create` event |
| `deploy_app`, `rollback_app` | `app_id`, exactly one source revision or image | replacement deployment input and incremented generation |
| `delete_app` | `app_id`, `confirm` equal to the app name | desired `absent` |
| `set_app_env`, `unset_app_env` | `app_id`, key, and one plain value or Secret reference | changed environment references and incremented generation |
| `restart_app`, `stop_app`, `start_app` | `app_id` | lifecycle desired state and incremented generation |
| `scale_app` | `app_id`, replicas | desired replicas within the space quota |
| `bind_service_to_app` | `app_id`, service ID | an opaque service Secret reference |
| `create_service_and_bind_to_app` | `app_id`, service kind/name and optional size | new managed service plus binding in one call |
| `app_logs` | `app_id`, optional cursor, limit and direction | Loki page for the app namespace |
| `get_app_credentials` | `app_id`; operator credential only | Lake-held workspace and event-publisher values for Secret delivery |
| `publish_app_event` | `app_id`, type, optional message/status | operator rollout or pod event ID |
| `receive_app_events` | `app_id`, optional sequence and long-poll wait | ordered lifecycle, rollout and pod events |

Names are lowercase DNS-label prefixes. `auth` defaults to `lake`; `none` is explicit. A host must
be a child of `LAKE_APPS_WILDCARD`, and a live host is unique across the Lake. Source mode requires
repository and revision, with path defaulting to `.`. Image mode requires a nonempty image.

Creation and scale enforce both `LAKE_APPS_PER_CUSTOMER` and
`LAKE_APP_REPLICAS_PER_CUSTOMER`. Zero means no configured limit. Deleting an app releases both
quotas, but its row remains until the operator reports `phase: Deleted`.

## Bindings and environment

`bindings.zetta` accepts `true` for the default read-only set or an explicit nonempty list of
read-only workspace capabilities. Lake mints a distinct `law.` bearer carrying exactly those
capabilities, the app's customer ID, and the creator's live customer authority. The desired row
contains scopes and the opaque `workspace-token` Secret reference, never that bearer or the
workspace administrator bearer. Service bindings contain the service ID and an opaque
`service-<id>` Secret reference.
The operator resolves the workspace and event-publisher references through operator-only
`get_app_credentials`, copies an existing service connection from its managed-service Secret, and
delivers each value as a namespace-local Kubernetes Secret. Neither connection is copied into
source code or `env_json`. PostgreSQL retains minted values under envelope encryption and compares
only SHA-256 digests while authenticating them. Marking an app absent immediately rejects its
workspace bearer.

Plain environment values live in `env_json`. Values detected as credentials are rejected with
`secret_required`; callers put the credential in the Lake-held secret system and pass only its
reference. Those references live in `secret_env_refs`, and setting either representation removes
the other representation of the same key. Audit records contain the key and whether it is secret,
not the value or reference contents.

## Events and logs

Lifecycle mutations publish to `app-events.<app-id>` on the Lake bus Zetta. Events contain the app,
customer, generation, type, acting principal, and non-secret change details. Creation also mints a
`lae.` bearer whose only capability is `publish_app_event` and whose handler accepts only that app
ID. The operator resolves its `app-events-publisher` reference, uses it for rollout and pod
observations, and never delivers it to the app. `receive_app_events` long-polls from a one-based
sequence. The publisher remains valid through deletion reconciliation, then is rejected when the
operator reports `phase: Deleted`. App logs query Loki with the deterministic namespace
`app-<first 12 UUID hex digits>` and otherwise use the cursor contract from
[platform.md](platform.md).

## Verification

`WebApps.*` exercises policy isolation, validation, quotas, desired-to-observed status, lifecycle
changes, minted and app-scoped credentials, operator-only resolution, Secret references, logs, and
published/received events against the memory store. `PostgresWebApps.*` exercises the production
store, encrypted credential retention, `lake_apps` notifications, quota locking and a fake operator
status update. The full PostgreSQL-backed static and shared CTest suites cover REST, OpenAPI, MCP
discovery, auditing, and migration conformance.
