# Customer metering

**Status: implemented.** `metering_usage` reports one authorized customer's usage: hourly
repository storage, transfer, and repository build compute, plus a current `summary` that adds
container images, workspace data, image builds, and workspace runs (training included). The same
report is the Usage page, `GET /metering?customer_id=<uuid>`.

Until lake#444 the report covered only repository bytes and repository build results, so a space
holding 12,000-row datasets, trained models and built images showed a few kilobytes and zero
compute. The summary measures the rest where it lives and names what is still not measured.

## Usage model

The hourly rows are keyed by customer and UTC hour. `stored_bytes` is a gauge sampled from the
customer's libliquid store; transfer and compute are counters.

| Field | Rule |
|---|---|
| `stored_bytes` | Sum the uncompressed lengths in libliquid pack-frame metadata once per digest across `LAKE_STORE_ROOT/<customer>/store`. Content-addressed duplicates are counted once. |
| `transfer_in_bytes` | Count binary request-body bytes accepted by missing-object, push, pull, and chunk-upload paths. |
| `transfer_out_bytes` | Count binary response bytes returned by missing-object, push, pull, and ranged chunk-read paths. |
| `compute_seconds` | Sum sandbox, execution, hash, and store milliseconds from each build result, round each result up to a whole second, and record each result ID once. Queue time is excluded. |

## Current summary

`summary` is measured on every read and is never written anywhere: workspace numbers stay in the
workspace Zetta, and lake's control store keeps only what it kept before (the hourly repository
rows and the build ledger above). It carries no price.

| Field | Rule |
|---|---|
| `storage.repositories_bytes` | The current `stored_bytes` gauge above. |
| `storage.images_bytes` | Unique bytes the OCI registry holds below `<space handle>/`: every manifest a tag points to, plus its config and layers, each digest once across all tags and repositories of the space (build cache under `<space>/cache/` included). `null` when the server has no registry storage. |
| `storage.workspace_data_bytes` | The sum of `size_bytes` from Zetta `list_datasets` in each of the customer's workspaces (the on-disk row chunks of every dataset). |
| `storage.total_bytes` | The three above added. |
| `compute.repository_build_seconds` | The sum of `compute_seconds` in the returned hourly rows. |
| `compute.image_build_seconds`, `image_builds` | Finished image builds (`Succeeded`, `Failed`, `Cancelled`) from `created_at` to the operator's final `updated_at`, each rounded up to a second. The build row has no start time, so time spent queued is included and the page says so. |
| `compute.workspace_run_seconds`, `workspace_runs` | Every Zetta run in each workspace (`list_runs`), training and study trials included, from `created` to `updated`, each rounded up to a second. A run without both timestamps counts nothing. |
| `compute.total_seconds` | The three compute figures added. |
| `workspaces[]` | Per workspace: `name`, `measured`, `data_bytes`, `datasets`, `run_seconds`, `runs`, and the reason in `message` when it could not be read. |
| `not_measured[]` | Plain sentences for what this report could not include: a workspace that is not running, no registry storage, no workspaces on this lake, and model files and indexes inside a workspace, whose size Zetta does not report yet. |
| `priced` | Whether any price is configured. |

`from` and `to` also bound the summary's image builds and workspace runs, by the time each finished.
Storage figures are always current. Each workspace's two reads run concurrently, and every
workspace is read at once, so the Usage page costs two workspace calls per workspace in one round
([page performance](../ui/performance.md)). A workspace that cannot be read is reported in
`workspaces[]` and `not_measured[]`; it never fails the report and never counts as zero bytes.

`cost_micros` still prices only the hourly repository rows (repository storage, transfer, and
repository build time). Images, image builds and workspaces are measured but not priced: how they
are billed is not decided, and this report does not decide it.

## The Usage page

`/metering` (Usage in the sidebar, the Usage tab of an organization) shows the summary in plain
units: byte sizes in B/KiB/MiB/GiB, compute as seconds, minutes and hours, hours as
`YYYY-MM-DD HH:MM UTC`, and an estimated charge in the configured currency with two decimals. With
no prices configured it says there is no charge to show instead of printing zeros. It lists each
workspace, a Not Measured Yet section, the repository storage limit form, and the hourly repository
table.

Reading usage reconciles build records from the customer's repositories into the idempotency
ledger before returning the hourly rows. This lets a repeated report safely observe the same build
history. Migration `0014_metering` owns `metering_usage`, `metering_build_records`, and
`metering_guard`.

Before accepting requests, Lake opens existing customer repositories and walks their result
indexes so the first Metering request does not pay libliquid initialization costs. This warmup
does not reconcile usage or write billing rows; failures are logged and do not stop the server.
Storage-byte reads inspect pack metadata without opening or creating a customer store. A
repository with no local storage has no artifact results and is not created by a usage read.
Usage and build reconciliation still read current data on every request; they are not cached.

## Capability

`GET /v1/metering_usage` accepts:

```json
{"customer_id":"<uuid>","from":"2026-09-01T00:00:00Z","to":"2026-09-30T23:00:00Z","limit":744}
```

`from` and `to` are optional inclusive UTC-hour bounds. `limit` defaults to 744 and is bounded to
1–10,000. The result contains `items`, configured pricing, the total integer `cost_micros`, the
storage quota, and an over-quota warning. Authorization resolves the customer policy object and
calls `authorize` for `read`. Registry dispatch records the read through `audit::record` as
`metering.usage.read` with the observed stored-byte count and quota state.

## Pricing configuration

Prices are integer micro-units in `LAKE_PRICE_CURRENCY` (default `USD`):

| Environment variable | Unit |
|---|---|
| `LAKE_PRICE_STORAGE_GIB_HOUR_MICROS` | one GiB-hour |
| `LAKE_PRICE_TRANSFER_IN_GIB_MICROS` | one inbound GiB |
| `LAKE_PRICE_TRANSFER_OUT_GIB_MICROS` | one outbound GiB |
| `LAKE_PRICE_COMPUTE_SECOND_MICROS` | one compute second |
| `LAKE_STORAGE_QUOTA_BYTES` | unique stored-byte quota; `0` disables the quota |

Fractional byte-based charges round up to one micro-unit. Arithmetic saturates at the unsigned
64-bit maximum rather than wrapping.

`set_storage_quota` stores a customer-specific integer byte quota through the registry. It
requires `admin` on that customer, is represented in OpenAPI and MCP with the same schema, and
records `metering.quota.set` in the signed audit stream. Its value overrides
`LAKE_STORAGE_QUOTA_BYTES` for that customer; an explicit value of `0` disables the customer's
quota. Customers without an override retain the process configuration.

## Quota behavior

Chunk upload computes the exact raw bytes for digests absent from the customer store while holding
the customer-store write lock. A write that would cross the configured quota returns liquid
`busy`, rendered as HTTP 429. Metadata writes are also throttled while the store is at or above the
quota. Existing objects remain readable and complete. Lake never replaces content with pointer
files or another reduced representation. Quota refusal and its rule are included in the signed
capability audit event.

## How to verify

`Metering.*` proves exact storage reporting, hourly pricing, and idempotent build accounting;
`Metering.UsageIncludesWorkspaceDataImagesAndComputeOutsideRepositories` proves the summary against
a workspace double (dataset bytes, run seconds rounded up, the `from`/`to` window, a stopped
workspace reported as not measured, and only finished image builds counted).
`RegistryProtocols.UsageCountsEachImageDigestOnceForItsSpace` pushes real OCI images through the
server, including two tags and a cache image sharing a layer and another space's image, and checks
`images_bytes` counts each of the space's digests exactly once. `WebUi.UsageUsesPlainUnits*`
covers the page's units and names.
`PostgresMetering.*` exercises the production aggregate and build ledger. `SyncHttp.StorageQuota*`
pushes existing content, enables a quota, proves a new write returns `busy`, and then pulls the
original bytes unchanged. `WebUi.Metering*` covers the renderer.

`PagePerformance.EveryRegisteredGetPageKeepsItsWorkspaceCallBudget` creates four repositories
with 128 files before starting a new server. Its very first request is Metering and must report
nonzero stored bytes. That cold request is a correctness fixture rather than a scheduler-sensitive
latency assertion. The same populated page participates in the router-derived 20-render p95 guard.
See [page performance](../ui/performance.md).

## Empty personal customers

Sign-up creates a counted customer row without writing customer repository bytes or a quota
override. The first usage read records zero stored bytes and costs zero even with nonzero prices;
there is no per-tenant minimum. `LAKE_STORAGE_QUOTA_BYTES` applies until explicitly overridden.
The production HTTP proof and transaction contract are in [provisioning.md](provisioning.md).
