# Platform environments

Lake is a read/write client of the operator's Zetta control plane, not a deployment control
plane. `liquid deploy` appends deployment intent, the operator appends deployment status and
reconciles environment/domain requests, and Lake mirrors those rows for authorized pages and
audit. Lake never appends a deployment row; rollback is another `liquid deploy --to` operation.

## Capabilities

All capabilities resolve the repository's local `environment` policy object and go through
`Registry::call`. Environment objects have `read`, `write`, `deploy`, `share`, and `admin` roles.

| Capability | Action | Input | Value |
|---|---|---|---|
| `list_environments` | `read` per candidate | `repository_id` | environment summaries with current phase, change, image, URL, domains, and update time |
| `get_environment` | `read` | `repository_id`, `environment` | current deployment, deployment history, domains, plain environment values, and optional `stale_since` |
| `list_deployments` | `read` per candidate | `repository_id`, optional `environment`, optional `limit` | latest deployment record per deployment id, newest first |
| `environment_logs` | `read` | `repository_id`, `environment`, optional `since`, `limit`, `direction` | `{items: [{ts,pod,container,line}], next_since}` |
| `set_environment_env` | `write` | `repository_id`, `environment`, `key`, `value` | appended row identity |
| `unset_environment_env` | `write` | `repository_id`, `environment`, `key` | appended tombstone identity |
| `create_domain` | `deploy` | `repository_id`, `environment`, `host`, `tls` | pending domain request; `tls` is `acme` or `provided` |
| `delete_domain` | `deploy` | `repository_id`, `environment`, `host` | pending domain tombstone |

Environment values are plain configuration only. Before appending, Lake runs libliquid's inline
secret detector over `KEY=value`; a match is refused with `use a sealed secret`. Sealed-secret
references are tracked separately in #129.

## Proposed operator rows

These schemas are the Lake-side proposal pending liquid #84 and operator #124. Column names have
one code source in `include/lake/control/zetta_control.hpp`.

| Table | Columns | Writer |
|---|---|---|
| `orama_control.deployments` | `operator, lake, repository, environment, kind, change, image, deployment_id, requested_by, requested_at, phase, namespace, url, message, updated_at` | desired rows by liquid; status rows by the operator |
| `orama_control.environment_env` | `lake, repository, environment, key, value, deleted, set_by, updated_at` | Lake; operator reads |
| `orama_control.domains` | `lake, repository, environment, host, tls, status, message, deleted, requested_by, updated_at` | requests by Lake; status rows by the operator |

Rows are append-only. Reads select rows for `lake = LAKE_PUBLIC_HOST`; latest `updated_at` wins
per `deployment_id`, `(repository, environment, key)`, or `(repository, environment, host)`.
`deleted = true` is the environment/domain tombstone. The Zetta SQL capability is GET-bound and
accepts one statement, so Lake emits only fixed-table `SELECT` and `INSERT` statements. Values are
SQL literals quoted by Lake: apostrophes double, values must be scalar, and ASCII control
characters are rejected. The operator bearer is a Zetta `secret_header` and is never logged.

## Mirror and failure behavior

Every `LAKE_PLATFORM_SYNC_SECONDS` (default 15), the owner loop fetches all three tables, selects
the latest rows, creates `platform_environments` and its policy object on first sight, and upserts
`platform_mirror`. Each newly observed deployment phase records one signed `platform.deployment.phase` audit
event. Migration `0019_platform` holds only these local bindings and last-seen JSON rows; `0017`
and `0018` were already assigned when the platform work landed.

All three reads must succeed before a sync mutates the mirror. If Zetta is unreachable, existing
reads continue from the mirror and include `stale_since`; health reports `platform: degraded`,
`/readyz` remains governed by the local control store, and platform writes fail
`store_unavailable`. A successful sync clears the stale marker.

## Loki-compatible logs

`environment_logs` takes the current deployment namespace and sends:

```text
GET <LAKE_LOGS_URL>/loki/api/v1/query_range
  ?query={namespace="<namespace>"}&start=<since>&limit=<limit>&direction=<direction>
```

`start` is omitted when `since` is absent. Direction is `forward` or `backward`; limits are
clamped to 1–1000. `LAKE_LOGS_TOKEN`, when configured, is a secret bearer header. Lake converts
Loki matrix streams to timestamped pod/container lines and returns the final timestamp as
`next_since`. There is no websocket log API.
