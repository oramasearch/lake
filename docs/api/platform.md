# Platform environments

Lake is a read/write client of the operator's Zetta control plane, not a deployment control
plane. `liquid deploy` appends deployment intent, the operator appends deployment status and
reconciles environment/domain requests, and Lake mirrors those rows for authorized pages and
audit. Lake never appends a deployment row; rollback is another `liquid deploy --to` operation.

## Where environments live (lake#358, lake#359)

A repository's environments and the rows Lake mirrors for it live in the workspace Zetta of the
repository's workspace (`repositories.workspace_id`, lake#315), as two sealed collections of
[workspace records](../architecture/workspace-records.md), both found by the repository:

| Collection | One record per | Key | Written |
|---|---|---|---|
| `lake_platform_environments` | environment | its id, `md5(repository id, newline, name)` as a UUID | `if_absent` when the sync first sees it; two replicas create it once |
| `lake_platform_mirror` | deployment, environment value or domain | `md5(kind, newline, key)` as a UUID | `if_absent`, or at the engine revision read when the stored row is older (the row order below); an older row never replaces a newer one |

The caller never chooses the workspace: it is the repository's, so naming the repository names
it, and nothing falls back to another workspace, the lake Zetta or PostgreSQL. A repository
without a workspace is `conflict` at `/repository_id`; a workspace that is not ready is
`workspace_not_ready`. Deleting a workspace deletes its Zetta and these records with it.

Each environment's `environment` policy object stays in the control store with its grants, under
the environment's id as its external id. Authorization therefore never reads the workspace:
`get_environment`, the logs and the writes find the object from the repository and the name, and
`list_environments` and `list_deployments` answer an empty list, without a workspace call, to a
caller who may read none of the repository's environment objects. PostgreSQL keeps no
environment name, value, domain or deployment row; the object's external id is an md5 of the
repository and the name.

Reads cost one `list_records` per collection: the environments page two, an environment page
three (the environment record, then the rows for the document and for the history). The
Deployments overview reads only the repositories with an environment the caller may read.

## Capabilities

All capabilities resolve the repository's local `environment` policy object and go through
`Registry::call`. Environment objects have `read`, `write`, `deploy`, `share`, and `admin` roles.

| Capability | Action | Input | Value |
|---|---|---|---|
| `list_environments` | `read` per candidate | `repository_id` | environment summaries with id, current phase, change, image, URL, domains, and update time |
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
| `orama_control.deployments` | `operator, lake, repository, environment, kind, change, image, deployment_id, requested_by, requested_at, phase, namespace, url, message, updated_at, ordering_token` | desired rows by liquid; status rows by the operator |
| `orama_control.environment_env` | `lake, repository, environment, key, value, deleted, set_by, updated_at, ordering_token` | Lake; operator reads |
| `orama_control.domains` | `lake, repository, environment, host, tls, status, message, deleted, requested_by, updated_at, ordering_token` | requests by Lake; operator materializes status |

Rows are append-only. Lake writes `updated_at` as a UTC ISO 8601 timestamp with exactly six
fractional digits and a UUID `ordering_token` on every environment-value and domain row. Reads
select rows for `lake = LAKE_PUBLIC_HOST` and choose the lexicographically greatest
`(updated_at, ordering_token, canonical row)` tuple per `deployment_id`,
`(repository, environment, key)`, or `(repository, environment, host)`. This total order means
every permutation of a set of rows produces the same current value and deployment-phase sequence.
Consumers may rely on a later set or tombstone winning when its tuple is greater; an immediate set
followed by an unset therefore resolves to the tombstone, and the reverse resolves to the set.

Rows written before this contract have no `ordering_token`; Lake treats the missing field as the
empty string and expands their second-resolution timestamps to zero microseconds for comparison.
The canonical row is the final tie-break for legacy rows, so historical and mixed rows also reduce
deterministically. There is no Lake Postgres migration: the token remains in the existing mirrored
JSON document, and older Zetta rows project the absent field as null.

`deleted = true` is the environment/domain tombstone. The Zetta SQL capability is GET-bound and
accepts one statement, so Lake emits only fixed-table `SELECT` and `INSERT` statements. Values are
SQL literals quoted by Lake: apostrophes double, values must be scalar, and ASCII control
characters are rejected. The operator bearer is a Zetta `secret_header` and is never logged.

## Mirror and failure behavior

Every `LAKE_PLATFORM_SYNC_SECONDS` (default 15), the owner loop fetches all three tables, selects
the latest rows and groups them by repository. For each repository it opens the repository's
workspace, reads its environments and mirror rows (one `list_records` each), creates an
environment and its policy object on first sight, and writes each row the workspace does not
hold yet. Each newly observed deployment phase records one signed `platform.deployment.phase`
audit event before its row is written, so a sync that fails there records it next time rather than
never. A replica remembers what it last wrote per repository and skips a repository whose rows
have not changed, so a quiet sync makes no workspace call; every 20th sync rereads every
repository's workspace, which rewrites a workspace that lost its records.

All three reads must succeed before a sync writes anything. If the operator control plane is
unreachable, existing reads continue from the workspace mirror and include `stale_since`; health
reports `platform: degraded`, `/readyz` remains governed by the local control store, and platform
writes fail `store_unavailable`. A successful sync clears the stale marker. A repository whose
workspace cannot be written (not ready, or the repository has no workspace) is marked stale on its
own: its reads carry `stale_since` until a later sync writes it, and the service stays healthy. A
row naming a repository Lake does not have belongs to no workspace and is skipped.

The retired PostgreSQL tables `platform_environments` and `platform_mirror` (migration
`0019_platform`) are read only by the one-shot copy; migration `0060_platform_workspaces` records
each row's owner and gives each environment's policy object its derived id. The cutover is in
[workspace-records.md](../architecture/workspace-records.md#cutover-runbook-platform-environments-and-mirror-dev-cluster).

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
