# Customer metering

**Status: implemented.** `metering_usage` reports one authorized customer's usage: hourly
repository transfer and repository build compute, kept in each repository's workspace Zetta since
lake#348 and lake#349, plus a current `summary` of repository storage that adds
container images, workspace data, image builds, and workspace runs (training included). The same
report is the Usage page, `GET /metering?customer_id=<uuid>`.

Until lake#444 the report covered only repository bytes and repository build results, so a space
holding 12,000-row datasets, trained models and built images showed a few kilobytes and zero
compute. The summary measures the rest where it lives and names what is still not measured.

## Usage model

Repository usage lives in the workspace Zetta of the repository's workspace (lake#348, lake#349;
every repository names one since lake#315, migration `0051`). Lake's PostgreSQL holds none of it.
Since migration `0053_metering_workspaces` lake no longer writes or reads `metering_usage` or
`metering_build_records`; the operator's one-shot copy moves their rows (see
[Cutover](#cutover-lake348-lake349)).

| Measure | Where it lives | Rule |
|---|---|---|
| Transfer | `lake_metering_usage` in the repository's workspace: one sealed record per workspace and UTC hour | `transfer_in_bytes` counts binary request-body bytes accepted by missing-object, push, pull and chunk-upload paths; `transfer_out_bytes` counts binary response bytes of missing-object, push, pull and ranged chunk-read paths. Exact counters, added with a conditional write. |
| Repository build compute | `lake_metering_build_records` in the repository's workspace: one sealed record per build result | Sum sandbox, execution, hash and store milliseconds of the result, round up to a whole second, and record each result id once (`if_absent`). Queue time is excluded. |
| Repository storage | Nowhere: measured on every read | Sum the uncompressed lengths in libliquid pack-frame metadata once per digest across `LAKE_STORE_ROOT/<customer>/store`. Content-addressed duplicates are counted once. |

Both collections are found by the space (`customer:<id>`) and stamped with the start of their
hour, and they hold only sealed values and blind indexes
([workspace records](../architecture/workspace-records.md)). They carry no price.

**Storage stays a space-level measurement.** One libliquid store holds every repository of a space,
and a digest shared by two repositories is stored once, so its bytes cannot be split between
workspaces. Until lake#380 gives each workspace its own store, repository storage is measured live
from that store on each read, and no hourly history of it is kept anywhere, so nothing space-level
is persisted centrally.

### Transfer off the request path

A sync request never waits for a workspace. It adds its bytes to an in-process counter keyed by
space, workspace and hour (`MeteringRecorder`, `include/lake/api/metering_recorder.hpp`), and the
owner loop writes the counters every `ServerOptions::metering_flush_interval` (five seconds), and
sooner when half the bound is held. Each write reads the hour's record and puts it back with
`if_revision` (or `if_absent` for a new hour), rereading on a `409`, so several lake replicas adding
to one hour never lose an increment. `ApiServer::stop` writes once more after in-flight requests
finish, before the stores close.

- **Bounded.** At most `metering_max_pending` (4,096) counters and queued build records are held.
  An increment to a counter already held always merges; a new counter beyond the bound is dropped
  and counted, never blocked on.
- **Failures.** A write that fails is kept for the next flush. A counter whose workspace has been
  deleted is dropped and counted, because its records went with the workspace; so is one whose
  workspace id is unknown (both stores answer it with `invalid_input`), rather than retried until
  the bound drops it.
- **Repositories without a workspace.** A repository migration `0051` recorded as
  `needs_selection` has nowhere to record transfer. Its bytes are counted as unattributed and not
  recorded, and the report says so.
- **Loss window on a crash.** What was counted since the last successful flush: at most the flush
  interval (five seconds) of transfer, plus any counter whose writes kept failing. A drain
  (SIGTERM) flushes first. Build records lose nothing: the next usage read finds the result in its
  repository again and writes it.
- **Reads see pending transfer.** A usage read adds this process's unflushed counters for the
  space. Another replica's unflushed counters appear after its next flush.

Counters: `lake_metering_records_written_total{collection}`, `lake_metering_flush_errors_total`,
`lake_metering_dropped_bytes_total{reason="dropped"|"no_workspace"}`,
`lake_metering_dropped_builds_total` and the gauge `lake_metering_pending`
([telemetry](../serving/telemetry.md)).

### Build records

A usage read walks every build result of the space's repositories, as before. A result whose
record the read found in its workspace counts once; one that is not recorded yet counts from the
repository and is queued for its repository's workspace, written `if_absent` under the blind key of
its result id by the next flush. A result is never counted twice, however often it is seen, and
a result copied from the retired table and one recorded by a read share the same key.

## Current summary

`summary` is measured on every read and is never written anywhere: workspace numbers stay in the
workspace Zetta, and lake's control store keeps none of them. It carries no price.

| Field | Rule |
|---|---|
| `storage.repositories_bytes` | Repository storage, measured now (above). |
| `storage.images_bytes` | Unique bytes of the space's tenant images (`<space handle>/...`, build cache under `<space>/cache/` included): every manifest a tag points to, plus its config and layers, each digest once across all tags and repositories of the space. Read from the tags in each routed workspace (lake#383, [registry.md](registry.md#where-a-tenant-image-lives)), one read per repository; a workspace that cannot be read is named in `not_measured`. `null` when the lake has no workspaces. |
| `storage.workspace_data_bytes` | The sum of `size_bytes` from Zetta `list_datasets` in each of the customer's workspaces (the on-disk row chunks of every dataset). |
| `storage.total_bytes` | The three above added. |
| `compute.repository_build_seconds` | The sum of `compute_seconds` in the returned hourly rows. |
| `compute.image_build_seconds`, `image_builds` | Finished image builds (`Succeeded`, `Failed`, `Cancelled`) from `created_at` to the operator's final `updated_at`, each rounded up to a second. The build row has no start time, so time spent queued is included and the page says so. |
| `compute.workspace_run_seconds`, `workspace_runs` | Every Zetta run in each workspace (`list_runs`), training and study trials included, from `created` to `updated`, each rounded up to a second. A run without both timestamps counts nothing. |
| `compute.total_seconds` | The three compute figures added. |
| `workspaces[]` | Per workspace: `name`, `measured`, `data_bytes`, `datasets`, `run_seconds`, `runs`, and the reason in `message` when it could not be read. |
| `not_measured[]` | Plain sentences for what this report could not include: repository storage over time (only its current size is measured), a workspace that is not running or whose metering records could not be read, repositories without a workspace, a workspace whose image tags could not be read, no workspaces on this lake, and model files and indexes inside a workspace, whose size Zetta does not report yet. |
| `priced` | Whether any price is configured. |

`from` and `to` also bound the summary's image builds and workspace runs, by the time each finished.
Storage figures are always current. Each workspace's four reads (`list_datasets`, `list_runs`, and
one `list_records` each of its transfer and build records) run concurrently, and every workspace is
read at once, so the Usage page costs four workspace calls per workspace in one round
([page performance](../ui/performance.md)). A workspace that cannot be read is reported in
`workspaces[]` and `not_measured[]`; it never fails the report and never counts as zero bytes.

`items` are the hours with repository transfer or build compute, oldest first, at most `limit`:
`hour`, `transfer_in_bytes`, `transfer_out_bytes`, `compute_seconds` and `cost_micros`. They have no
`stored_bytes` since lake#348: storage has no hourly history. `cost_micros` prices only transfer and
repository build time at the configured prices; repository storage, images, image builds and
workspaces are measured but not priced. How usage is billed is not decided, and this report does
not decide it.

## The Usage page

`/metering` (Usage in the sidebar, the Usage tab of an organization) shows the summary in plain
units: byte sizes in B/KiB/MiB/GiB, compute as seconds, minutes and hours, hours as
`YYYY-MM-DD HH:MM UTC`, and an estimated charge in the configured currency with two decimals. With
no transfer or compute price configured it says there is no charge to show instead of printing
zeros. It lists each workspace, a Not Measured Yet section, the repository storage limit form, and
the hourly repository table (uploaded, downloaded, build time).

Before accepting requests, Lake opens existing customer repositories and walks their result
indexes so the first Metering request does not pay libliquid initialization costs. This warmup
records nothing; failures are logged and do not stop the server.
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
| `LAKE_PRICE_STORAGE_GIB_HOUR_MICROS` | one GiB-hour; reported in `pricing` but applied to nothing since lake#348, because repository storage has no hourly history |
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

## Cutover (lake#348, lake#349)

Migration `0053_metering_workspaces` records each retired row's owner in `workspace_assignments`:
a space with exactly one live workspace owns its rows (`sole_workspace`); every other row is
`needs_selection`. A usage row's id is md5 of its space and hour, a build row's md5 of its space
and result id (neither table has an id column). Usage rows without transfer carry only the retired
storage gauge and compute that the build records hold, so they are neither assigned nor copied.
`copy_workspace_records` with `{"collection":"metering_usage"}` copies each row's transfer under
its legacy id beside lake's own hourly record (a reader adds both), and with
`{"collection":"metering_build_records"}` writes each result under the key a usage read uses, so a
result recorded since the cutover is `already_present`. Rows of a deleted workspace are reported
and do not block `complete`. Nothing is dropped; a later migration drops both tables once `verify`
is complete on dev. The runbook is in
[workspace-records.md](../architecture/workspace-records.md#cutover-runbook-metering-dev-cluster).

## How to verify

`Metering.*` proves exact storage reporting, hourly pricing, and idempotent build accounting;
`Metering.UsageIncludesWorkspaceDataImagesAndComputeOutsideRepositories` proves the summary against
a workspace double (dataset bytes, run seconds rounded up, the `from`/`to` window, a stopped
workspace reported as not measured, and only finished image builds counted).
`RegistryProtocols.UsageCountsEachImageDigestOnceForItsSpace` pushes real OCI images through the
server into two routed workspaces, including two tags and a cache image sharing a layer and another
space's image, and checks `images_bytes` counts each of the space's digests exactly once. `WebUi.UsageUsesPlainUnits*`
covers the page's units and names.
`Stores/WorkspaceRecordStores.RepositoryUsageStaysInEachRepositorysWorkspace/{Memory,Postgres}`
covers the recorder across two spaces with two workspaces each: nothing written on the request
path, one hourly record per workspace, merged counters, a second replica's conditional increment,
a workspace that is not ready (kept), a deleted or unknown workspace (dropped and counted), a repository
without a workspace, the bound, and build records written once.
`Stores/WorkspaceRecordStores.TheOneShotCopyMovesMeteringIntoEachSpacesWorkspace/{Memory,Postgres}`
covers the copy, and `PostgresMetering.MigrationRecordsEachRowsOwnerForTheCopy` migration `0053`.
`PostgresSync.PushAndPullUsePostgresControlPlane` pushes and pulls through a real server and checks
pending transfer in the report, the flush into the repository's workspace, nothing in PostgreSQL,
and the flush on stop. `SyncHttp.StorageQuota*`
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
