# Platform operator handoff

The operator owns deployment reconciliation in its Zetta control plane. Lake mirrors deployment
state for pages and audit, and appends only environment-variable and domain requests. This page is
the contract for the operator leaf; it does not change the `Lake` CRD here.

## Pod environment

| Pod environment | Source | Required behavior |
|---|---|---|
| `LAKE_OPERATOR_ZETTA_URL` | operator Zetta service URL | enables platform sync and writes |
| `LAKE_OPERATOR_ZETTA_TOKEN` | operator-minted Kubernetes Secret | mandatory bearer; never a literal CR field or log value |
| `LAKE_LOGS_URL` | Loki-compatible service URL | enables `environment_logs` and log pages |
| `LAKE_LOGS_TOKEN` | Kubernetes Secret, when the log endpoint requires it | optional bearer; never logged |
| `LAKE_PLATFORM_SYNC_SECONDS` | literal integer | optional, default 15 |

`LAKE_CONTROL_HOST`, else `LAKE_PUBLIC_HOST`, is the Lake namespace value on every operator row. A
white-labelled Lake's public host is the customer's, and the operator keeps the rows named by the
Orama host through `LAKE_CONTROL_HOST`, the same host it keys them on itself (lake#1136,
[`../deployment/white-label-sign-in.md`](../deployment/white-label-sign-in.md)). The operator must inject the
Zetta URL and token together. A logs URL may be omitted; platform state continues to work while
log reads return `store_unavailable`.

## Zetta tables

| Table | Columns | Append owner |
|---|---|---|
| `orama_control.deployments` | `operator, lake, repository, environment, kind, change, image, deployment_id, requested_by, requested_at, phase, namespace, url, message, updated_at, ordering_token` | liquid appends desired rows; operator appends phase, namespace, URL, and message status rows; Lake is read-only |
| `orama_control.environment_env` | `lake, repository, environment, key, value, deleted, set_by, updated_at, ordering_token` | Lake appends plain values/tombstones; operator reconciles them into the environment |
| `orama_control.domains` | `lake, repository, environment, host, tls, status, message, deleted, requested_by, updated_at, ordering_token` | Lake appends requests/tombstones; operator materializes `LakeDomain` and appends status |

The tables are append-only. Lake writes microsecond UTC timestamps and a UUID `ordering_token` for
environment-value and domain rows. The mirror orders rows by `(updated_at, ordering_token, canonical
row)`, so a missing token on an older row sorts as empty and its canonical row resolves any remaining
tie. Domain `tls` is exactly `acme` or `provided`; `deleted = true` is a tombstone. The operator
must namespace every row consistently and preserve historical rows.

The table definitions remain a proposal until liquid #84 publishes its deployment page and
operator #124 adds them to `operatorZettaSchema`. If either changes a column, update
`docs/api/platform.md` and `include/lake/control/zetta_control.hpp` together.

## Runtime behavior

The Zetta bearer is passed through the linked client transport's secret-header facility. The
transport performs I/O and wakes Lake's coroutine; it does not run a second application runtime.
Zetta outage degrades only `healthz.platform`: the Postgres-backed mirror keeps pages readable,
readiness remains ready, and Lake rejects platform mutations until a full three-table sync
succeeds.
