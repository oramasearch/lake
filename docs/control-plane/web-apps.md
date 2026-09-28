# Ship app desired state

Lake owns the desired row for each Ship app. The Orama operator watches `lake_apps`, reconciles one
`LakeApp`, and writes rollout status back to the same row. The API contract is
[web-apps.md](../api/web-apps.md); application identity pools remain a different entity documented
in [apps.md](../api/apps.md).

## `0034_web_apps`

Migration `0034_web_apps` creates `apps` with exactly this shared Lake/operator contract:

| Column | Writer | Meaning |
|---|---|---|
| `id`, `customer_id`, `name` | lake | stable identity and space-local name |
| `mode`, `source_repository`, `source_revision`, `source_path`, `image` | lake | exactly one source or image deployment |
| `framework`, `port`, `replicas`, `host`, `auth` | lake | runtime and ingress desired state |
| `bindings_json`, `env_json`, `secret_env_refs` | lake | non-secret configuration and opaque Secret references |
| `desired`, `generation` | lake | `running`, `stopped`, or `absent`, and monotonic desired revision |
| `phase`, `url`, `ready`, `message`, `observed_generation` | operator | latest rollout observation |
| `updated_at` | both | latest desired or observed update |

Constraints require a lowercase app name, valid mode, one deployment input, port 1–65535,
replicas 1–100, `lake` or `none` auth, and object-shaped JSON. Partial indexes keep live names
unique per customer and live hosts unique for the Lake. A completed deletion stays as history and
is omitted from list results after phase `Deleted`.

Create and desired-state update lock the customer row before counting apps or replicas. This makes
per-space quotas stable under concurrent requests. Every insert or desired-state update executes
`pg_notify('lake_apps', app_id)` in the same transaction. The operator sets
`observed_generation` to the generation it reconciled, so a reader can distinguish current from
stale status.

The Kubernetes namespace is `app-` plus the first 12 hexadecimal characters of the app UUID.
Loki reads use that namespace. The operator creates a `LakeApp` there, resolves binding and
environment Secret references, and never copies credential material into the desired row. Zetta
bindings carry only requested read scopes and a Secret reference; service bindings carry only the
managed-service ID and connection Secret reference.

`app_credentials` is Lake-owned and outside the shared `apps` column contract. App creation
atomically mints an optional read-only workspace bearer and one event-publisher bearer, stores only
their SHA-256 authentication digests plus envelope-encrypted values, and gives each a fixed opaque
reference. The operator calls `get_app_credentials` with its operator credential, writes
`workspace-token` into the app's Kubernetes Secret when requested, and keeps
`app-events-publisher` in its own reconciliation credential. The latter can call only
`publish_app_event` and only with its own app ID. For a service binding, the operator copies the
connection from its existing managed-service Secret into the app namespace under the binding's
opaque reference.

The deployment guard is `web_apps_guard`. The operator must vendor `db/schema.sql` byte-for-byte,
select that guard, and apply it before a binary that reads `apps` is deployed.

## Verification

The memory fake-operator test changes phase, URL, readiness, message, and observed generation after
creation, and proves credential scope and operator-only resolution.
`PostgresWebApps.DesiredStateQuotaNotifyAndOperatorStatusFollowTheContract` proves the same row loop
through PostgreSQL, including encrypted credential retention, notification delivery and quota
refusal.
