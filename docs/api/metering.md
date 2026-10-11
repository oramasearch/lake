# Customer metering

**Status: implemented.** `metering_usage` reports one authorized customer's usage: hourly
repository transfer and repository build compute, kept in each repository's workspace Zetta since
lake#348 and lake#349, plus a current `summary` of repository storage that adds
container images, workspace data, image builds, and workspace runs (training included). The same
report is the repository usage page, `GET /metering?customer_id=<uuid>`. Usage over time, every
dimension per workspace with limits and the Usage dashboard, follows
([below](#usage-over-time), lake#564); it keeps no price, plan or bill.

Until lake#444 the report covered only repository bytes and repository build results, so a space
holding 12,000-row datasets, trained models and built images showed a few kilobytes and zero
compute. The summary measures the rest where it lives and names what is still not measured.

## Usage model

Repository usage lives in the workspace Zetta of the repository's workspace (lake#348, lake#349;
every repository names one since lake#315, migration `0051`). Lake's PostgreSQL holds none of it.
Since migration `0053_metering_workspaces` lake no longer writes or reads `metering_usage` or
`metering_build_records`; the operator's one-shot copy moved their rows (see
[Cutover](#cutover-lake348-lake349)), and migration `0070_drop_moved_tables` drops both tables.

| Measure | Where it lives | Rule |
|---|---|---|
| Transfer | `lake_metering_usage` in the repository's workspace: one sealed record per workspace and UTC hour | `transfer_in_bytes` counts binary request-body bytes accepted by missing-object, push, pull and chunk-upload paths; `transfer_out_bytes` counts binary response bytes of missing-object, push, pull and ranged chunk-read paths. Exact counters, added with a conditional write. |
| Repository build compute | `lake_metering_build_records` in the repository's workspace: one sealed record per build result | Sum sandbox, execution, hash and store milliseconds of the result, round up to a whole second, and record each result id once (`if_absent`). Queue time is excluded. |
| Repository storage | Nowhere: measured on every read | Sum the uncompressed lengths in libliquid pack-frame metadata once per digest across `LAKE_STORE_ROOT/<customer>/store` (while any workspace of the space is not switched), plus each switched workspace's bucket `usage` (lake#380). Content-addressed duplicates in the space store are counted once. |

Both collections are found by the space (`customer:<id>`) and stamped with the start of their
hour, and they hold only sealed values and blind indexes
([workspace records](../architecture/workspace-records.md)). They carry no price.

**Storage stays a space-level measurement.** Before a workspace is switched, one libliquid store
holds every repository of its space, and a digest shared by two repositories is stored once, so its
bytes cannot be split between workspaces. Repository storage is measured live on each read and no
hourly history of it is kept anywhere, so nothing space-level is persisted centrally. With lake#380
a switched workspace has its own store in its workspace Zetta
([architecture/workspace-store.md](../architecture/workspace-store.md)): a space's storage is its
space store (measured as before) while any of its workspaces is not switched, plus each switched
workspace's bucket `usage` (the encrypted bytes Zetta holds). The space quota is enforced against
that sum, and each workspace's bucket is created with the space's quota as its own. While a space is
part way through the move, the moved repositories are counted in both stores.

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
| `storage.images_bytes` | Unique bytes of the space's tenant images (`<space handle>/...`, build cache under `<space>/cache/` included): every manifest a tag points to, plus its config and layers, each digest once across all tags and repositories of the space. Read from the tags in each routed workspace (lake#383, [registry.md](registry.md#where-a-tenant-image-lives)), one read per repository; a workspace that cannot be read is named in `not_measured`. `null` when the lake has no workspaces. A blob that is an object of a workspace's liquid store (lake#383 phase B) is already in that store's usage, part of `repositories_bytes`, so it is left out here: `images_bytes` is the manifests and the blobs still in the instance registry. |
| `storage.images_in_workspace_stores_bytes` | The tenant image blobs held in workspace stores, each digest once: reported, not added to `total_bytes` again. Whether a blob is stored is decided per workspace, so a digest one workspace stores and another still reads from the instance registry counts in both fields. `null` when the lake has no workspaces. |
| `storage.workspace_data_bytes` | The sum of `size_bytes` from Zetta `list_datasets` in each of the customer's workspaces (the on-disk row chunks of every dataset). |
| `storage.total_bytes` | The three above added. |
| `compute.repository_build_seconds` | The sum of `compute_seconds` in the returned hourly rows. |
| `compute.image_build_seconds`, `image_builds` | Finished image builds (`Succeeded`, `Failed`, `Cancelled`) from `created_at` to the operator's final `updated_at`, each rounded up to a second. The build row has no start time, so time spent queued is included and the page says so. |
| `compute.workspace_run_seconds`, `workspace_runs` | Every Zetta run in each workspace (`list_runs`), training and experiment trials included, from `created` to `updated`, each rounded up to a second. A run without both timestamps counts nothing. |
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

## Usage over time

**Status: implemented (lake#564), not deployed.** Every dimension of a workspace's usage is kept
over time in that workspace's own Zetta, and `usage_over_time` adds it up across the workspaces a
person chooses, by hour, day or month. This is usage tracking and its visualization only: lake keeps
no price, plan, invoice or ledger for it. Billing is not designed yet (the umbrella rule leaves
billing data without a home until Steve decides, and decision 55 left whether billing reads
workspace records or keeps its own store open), so nothing here decides it.

### Where it lives

Two series in each workspace Zetta's labelled time series (zetta `docs/data/time-series.md`), one
bucket an hour, kept 400 days, created by lake on first write:

| Series | Kind | Labels | Written by |
|---|---|---|---|
| `lake_usage` | counter | `dimension`, and by dimension `kind`, `agent`, `launch_id`, `model`, `object_id` | the request path and the owner loop's sample, through the bounded `UsageRecorder` |
| `lake_storage` | gauge (`last`) | `kind` | the owner loop's sample |

The request path only adds to an in-memory value for (workspace, metric, hour, labels)
(`UsageRecorder`, `include/lake/api/usage_recorder.hpp`); the owner loop writes the values with
`record_metric` every ten seconds (`ServerOptions::usage_flush_interval`), at once when half of
`usage_max_pending` (8,192) is held, and once more on stop. It holds at most that many values: an
increment of a value already held merges and a reading replaces an older reading of its hour; a new
value beyond the bound is dropped and counted; values added together (a turn's input and output
tokens) are kept or dropped together. A failed write is kept for the next flush; a deleted or
unknown workspace's values are dropped, and a workspace that defines a series differently refuses
that series alone. The counter is approximate like the request series: a crash loses the last
interval and a write whose answer was lost may count twice. Nothing goes to lake's PostgreSQL or
the lake Zetta. Telemetry: [`../serving/telemetry.md`](../serving/telemetry.md#the-workspace-usage-series).

### Dimensions

Each dimension has one unit. When billing is designed, a price is set per dimension `name` and
multiplies its `unit`; that mapping is the extension point and does not exist yet
(`usage_dimensions()`, `src/api/usage_recorder.cpp`).

| Dimension | Unit | Kinds | Where it comes from |
|---|---|---|---|
| `storage` | bytes, the latest reading of each period | `repositories`, `images`, `workspace_data`, `transcripts`, `drive_files` | the owner loop's sample, below |
| `transfer` | bytes | `in`, `out` | the exact hourly repository transfer records (`lake_metering_usage`, above) |
| `build_compute` | seconds | `repository`, `image` | the repository build records (above) and the operator's finished image builds, at the hour each finished |
| `run_compute` | seconds | `run` | the workspace's runs (`list_runs`), training and experiment trials included, at the time each last changed |
| `speech` | seconds | `transcript` | the audio length of each transcript made from audio (`create_transcript`), rounded up, when the speech-to-text model reports it (`whisper-1`); `gpt-4o-transcribe` reports tokens instead, which count as its `model_tokens` |
| `calls` | calls | `api`, `mcp` | every capability call routed to the workspace, the same attribution as `lake_requests` ([telemetry](../serving/telemetry.md#the-workspace-request-series)), answered or refused |
| `predictions` | calls | — (`object_id` is the model) | each answered `predict` of a model the workspace has |
| `model_tokens` | tokens | `input`, `output` (also `agent`, `launch_id`, `model`) | what an agent reports with `record_model_usage` (below), and the tokens a speech-to-text model reports for a transcript (`model` only) |
| `instances` | instance-seconds | the service kind (`zetta`, `postgres`, …) or `app` (`object_id` is the service or app) | the owner loop's sample of running services and apps |
| `volumes` | GiB-seconds | `service`, `store` (`object_id`) | the owner loop's sample of running services' and stores' volumes |

### What the sampler reads

Every `usage_sample_interval` (fifteen minutes) the store root's owner, and only it, so two lakes
never count one service twice, samples every live workspace on its maintenance thread
(`sample_usage`, `src/api/usage_capabilities.cpp`):

- **Repository storage.** A space with one live workspace: the space's whole store (the same figure
  `metering_usage` reports). With several: each repository's unique bytes in its workspace, so a
  file two workspaces' repositories share counts in both.
- **Container images.** The unique bytes of the images routed to the workspace, each digest once
  per workspace (`tenant_image_bytes`).
- **Workspace data, transcripts and drive files.** `list_datasets` `size_bytes`, the text of the
  workspace's transcripts, and the size of each finished file of its `files` drives. A workspace
  that is not ready is skipped, never sampled as zero.
- **Instance and volume time.** For each service and store the operator reports `Ready`, and each
  app that is running and ready, the seconds since the last sample (at most two intervals, so a gap
  while no lake owned the store root is not counted) times its instances, replicas and GiB. The
  workspace's own engine is a service, and counts.
- **Build results.** Repository build results that finished since a little before the last sample
  are queued for their workspace records, so `build_compute` does not wait for a `metering_usage`
  read.

### Not measured

`usage_over_time` names these in `not_measured` whenever the dimension is asked for:

- CPU and memory of services, stores and apps: the operator does not report them
  ([lake#612](https://github.com/oramasearch/lake/issues/612)).
- Model tokens are counted only when an agent reports them; agent-runtime and the library agents do
  not yet ([lake#248](https://github.com/oramasearch/lake/issues/248)).
- Speech-to-text for dictation outside a transcript (`transcribe`) belongs to no workspace
  ([lake#613](https://github.com/oramasearch/lake/issues/613)).
- Model files and indexes inside a workspace, whose size the workspace does not report.
- Usage before this change: the series start empty, and nothing is copied into them.

### `usage_over_time`

```json
{"customer_id":"<uuid>","workspace_ids":["<uuid>"],"dimensions":["storage","calls"],
 "from":"2026-10-01T00:00:00Z","to":"2026-11-01T00:00:00Z","granularity":"day"}
```

`workspace_ids` defaults to every live workspace of the account; each must be one, or the call is
refused `invalid_input` at `/workspace_ids/<n>` (another account's workspace is refused, never
read). `dimensions` defaults to all. `from` is rounded down and `to` up to whole UTC periods; at
most 2,400 hours, 1,100 days or 36 months. Hours are read from the workspace a step an hour; days
and months a step a day, a month adding up its days (a reading takes its latest day). Each
workspace's five reads (the two series, its runs, its transfer and build records) run at once.

The answer: `periods` (each period's start), and for each dimension `title`, `unit`, `aggregation`
(`sum` or `last`), `total` (a counter's sum; a reading's latest period), `points` (one value per
period, added up across the workspaces; a reading's periods without data are `null`),
`by_workspace` and `by_kind` (`key`, `total`, `points`, largest first), `by_agent`, `by_launch` and
`by_model` for `model_tokens`, and `by_object` for `instances`, `volumes` and `predictions`.
`workspaces[]` says which workspaces were read and why one was not; a workspace that could not be
read never counts as zero. It needs `read` on the customer and is audited centrally as
`metering.usage_over_time.read`. The existing `metering_usage` is unchanged.

### Limits

`set_usage_limit {customer_id, dimension, limit, warn_percent?}` (needs `admin` on the customer,
audited as `metering.limit.set`) sets the account's limit on one dimension in its unit; `0` removes
it. Limits are rows of `customer_usage_limits` (migration `0072_usage_limits`), content-free account
configuration like the storage quota. **Storage's limit is the repository storage quota**:
`set_usage_limit` with `storage` sets `set_storage_quota`'s quota, still enforced on new repository
writes (below), and keeps its warning percentage. Every other limit is per UTC calendar month and
warns without refusing anything; enforcing one is an extension point for when plans are decided.

`usage_limits {customer_id}` answers each limit with what is used (storage: the repository bytes
the quota is enforced against; any other: this month's total across the account's workspaces),
`percent`, `state` (`ok`, `warning` from `warn_percent`, default 80, `over` from 100%, or
`not_measured`), `enforced` and `period` (`now` or `month`), and a `warnings` sentence for each one
warning or over. It needs `read` on the customer and is audited as `metering.limits.read`.

### Model tokens

`record_model_usage {model_name, input_tokens, output_tokens, task_id?}` is how an agent reports the
tokens one of its turns or runs spent. Only an agent's own credential may call it: a task (`lat.`),
launch (`lal.`) or schedule (`las.`) credential, or a library agent's bus credential naming its
`task_id`. Lake adds both counts together to the usage of the workspace the task, launch or schedule
runs in, labelled with the agent's name, the run's id (`launch_id`) and `model_name`, and audits it
in that workspace. Anyone else is refused `unauthorized`. agent-runtime and the library agents do not
call it yet ([lake#248](https://github.com/oramasearch/lake/issues/248)).

### Verify

`tests/test_usage.cpp`: `UsageRecorder.*` (one value per workspace, hour and labels, nothing written
on add, hour buckets, the 400-day definitions, a reading keeping its latest, the bound, values added
together dropped together, a failed write kept, deleted and refusing workspaces dropped);
`UsageOverTime.*` (two workspaces added up by day, one workspace, hours rounded out, months adding
their days, readings taking their latest; another account's workspace, unknown dimensions, a
backwards or too long window, and a stranger refused; each model's answered predictions);
`UsageLimits.*` (a warning at 80%, over at 100%, storage's limit is the quota, `0` removes);
`RecordModelUsage.*` (a launch credential's tokens in its workspace by agent, launch and model; a
person refused); `UsageSample.*` (stored bytes by kind, instance and volume seconds of a running
service and none for a pending one, a transcript's audio seconds rounded up).
`HttpServer.RequestLogsNameRoutesAndEachWorkspaceGetsItsOwnRequestSeries` counts calls through a
real server and writes them only at stop. `SchemaConformance.*` validates the three readable
capabilities' answers over REST and MCP.

### Change

A new dimension is a row in `usage_dimensions()` and an increment where lake sees it (or a reading
in `sample_usage`); `read_usage` folds it in by name. Keep labels bounded by what the workspace
holds: never a row's content, a raw path or a caller's free text.

## The Usage page

Usage in the sidebar is the dashboard at `/usage` over `usage_over_time` and `usage_limits`
([`../ui/usage.md`](../ui/usage.md)). `/metering` (linked from it as repository usage by hour, and the
Usage tab of an organization) shows the summary in plain
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
and do not block `complete`. Nothing is dropped by `0053`; the copy was verified on dev, and
migration `0070_drop_moved_tables` drops both tables (lake#311). The runbook is in
[workspace-records.md](../architecture/workspace-records.md#cutover-runbook-metering-dev-cluster).

## How to verify

`Metering.*` proves exact storage reporting, hourly pricing, and idempotent build accounting;
`Metering.UsageIncludesWorkspaceDataImagesAndComputeOutsideRepositories` proves the summary against
a workspace double (dataset bytes, run seconds rounded up, the `from`/`to` window, a stopped
workspace reported as not measured, and only finished image builds counted).
`RegistryProtocols.UsageCountsEachImageDigestOnceForItsSpace` pushes real OCI images through the
server into two routed workspaces, including two tags and a cache image sharing a layer and another
space's image, and checks `images_bytes` counts each of the space's digests exactly once.
`Stores/WorkspaceRecordStores.RepositoryUsageStaysInEachRepositorysWorkspace/{Memory,Postgres}`
covers the recorder across two spaces with two workspaces each: nothing written on the request
path, one hourly record per workspace, merged counters, a second replica's conditional increment,
a workspace that is not ready (kept), a deleted or unknown workspace (dropped and counted), a repository
without a workspace, the bound, and build records written once. The copy's test and the `0053`
migration test went with the retired tables (migration `0070`).
`PostgresSync.PushAndPullUsePostgresControlPlane` pushes and pulls through a real server and checks
pending transfer in the report, the flush into the repository's workspace, nothing in PostgreSQL,
and the flush on stop. `SyncHttp.StorageQuota*`
pushes existing content, enables a quota, proves a new write returns `busy`, and then pulls the
original bytes unchanged. The Usage page's units and names are the web app's; it reads
`metering_usage`.

## Empty personal customers

Sign-up creates a counted customer row without writing customer repository bytes or a quota
override. The first usage read records zero stored bytes and costs zero even with nonzero prices;
there is no per-tenant minimum. `LAKE_STORAGE_QUOTA_BYTES` applies until explicitly overridden.
The production HTTP proof and transaction contract are in [provisioning.md](provisioning.md).
