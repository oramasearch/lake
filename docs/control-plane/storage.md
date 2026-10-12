# Storage: the contract between lake and the Orama operator

**Status: phase 1 (lake#196).** A customer's **stores** — buckets, filesystems, network
filesystems and volumes — follow the managed-services model ([`services.md`](services.md),
liquid-bridge decision 46): lake records the desired store, the Orama operator makes it exist and
reports back. Nothing about a customer's store is decided by the operator, and lake never talks to
Kubernetes.

## The model: type × provider

A store has two independent axes: its **type** (what a client gets) and its **provider** (what runs
it). lake's provider table (`control::store_providers`, `include/lake/control/store.hpp`) lists the
valid pairs and which are available on this lake.

| Type | What a client gets | Providers (✅ phase 1) |
|---|---|---|
| `object` (a bucket) | S3-compatible or native object API | `r2` Cloudflare R2, `s3` AWS S3, `b2` Backblaze B2, `azure` Azure Blob, `gcs` Google Cloud Storage, `lake` a liquid repository, `zetta` Zetta large objects |
| `filesystem` | a POSIX tree in the customer's account | `lake` (Drive), `zetta` |
| `network_filesystem` | a shared mount several services attach read-write | `nfs` (in-cluster, run by the operator), `azure` Azure Files, `s3` EFS, `gcs` Filestore |
| `volume` | a block volume one service mounts | ✅ `cluster` (the cluster's StorageClass) |

Phase 1 delivers the model, the table, the capabilities, the UI and the **cluster volume**. Every
other pair is listed as *not yet available* with its reason; phases 2–4 (lake#196) add them.
Cloudflare R2 waits on R2 being enabled on Orama's Cloudflare account and a token that may mint
bucket-scoped keys; the other clouds wait on Orama accounts or bring-your-own credentials.

## Tables (migration `0028_stores`, `db/schema.sql`)

`stores` — written by lake only: `id`, `customer_id`, `type`, `provider`, `name`
(`^[a-z][a-z0-9-]{0,30}$`, unique among a customer's live stores), `slug` (`st-` plus the first 12
hex digits of the id; never reused), `spec` (`{size_gi, region, public}`; `public` is false unless
the customer chose otherwise — **private by default**), `desired` (`present` | `absent`),
`generation` (bumped on every desired change), `created_by_taap_id`, `created_at`, `updated_at`,
and `workspace_id` (migration `0044_store_workspaces`, lake#465): the workspace the store belongs
to. The `enforce_workspace_routing` trigger refuses a new store whose workspace is not a live
workspace of its account, and refuses any later change of `workspace_id` or `customer_id`. Names stay
unique per account, as for services and apps.

**Before 0044 the column did not exist.** Since orama-operator ef005a4 (2026-09-29) the operator's
`listStoresSQL` joins `stores s JOIN workspaces w ON w.id = s.workspace_id`, so every poll failed
with `column s.workspace_id does not exist` and logged `stores deferred: cannot read stores`; no
store was created or deleted. 0044 assigns each earlier store to its account's only live workspace
and records the outcome in `workspace_assignments` (`sole_workspace`), as 0043 did for agent
launches. An account with several live workspaces, or none, is recorded as `needs_selection` with a
warning; the operator's join leaves such a store out until someone sets its workspace. The guard is
`store_workspaces_guard`.

`store_status` — written by the operator only, one row per store: `phase` (`Provisioning`,
`Ready`, `Failed`, `Deleting`, `Deleted`), `message` (the current step, in words a person reads),
`endpoint`, `observed_generation`, `updated_at`.

lake sends `NOTIFY lake_services, '<store id>'` in the transaction of every insert and every
withdrawal, on the same channel as services, so the operator's listener wakes the Lake at once.

## Mounts

Migration `0079_store_mounts` (lake#666) records **which services and apps mount a store** and
**how much of a store is used**.

`store_mounts` — written by lake only, one row per store a workload mounts: `store_id`,
`object_kind` (`service` or `app`), `object_id`, `mount_path`, `read_only`, `created_at`. lake
writes the rows when a service or an app is created or edited with `stores`
(`[{store_id, mount_path?, read_only?}]`), in the transaction that bumps the workload's
`generation` and sends `NOTIFY lake_services`, as for its other edits. Ids, a path and a flag: no
content.

| Store | Mounted by | `mount_path` | What the operator gives the workload's containers |
|---|---|---|---|
| `volume` (`ReadWriteOnce`) | at most one workload; a second is refused (`conflict`, naming who mounts it) | required, absolute (`^/[A-Za-z0-9._/-]{1,255}$`) | the claim `<lake>-<slug>` mounted at `mount_path`, read-only when `read_only` |
| bucket (`r2`, `s3`, `b2`, `gcs`, `azure`) | any number of workloads | none (`''`) | the connection as environment variables, prefix `STORE_<NAME>_` (the store name upper-cased, `-` to `_`): `_BUCKET`, `_ENDPOINT`, `_REGION`, `_PREFIX`, and the credential fields from Secret `<lake>-<slug>` (`_ACCESS_KEY_ID`, `_SECRET_ACCESS_KEY`, `_SESSION_TOKEN` / `_ACCOUNT_KEY`, `_SAS_TOKEN`, each optional) |
| Lake-run (`zetta`, `lake`) | not mountable: lake refuses it (`invalid_input`); the workspace reads it through Lake | | |

Only workloads whose pods the operator builds can mount: **apps** (`LakeApp`) and the service kinds
whose pod template the operator writes itself, `sqlite`, `duckdb` and `polars`
(`control::service_kind_mounts_stores`). The other kinds (`zetta`, `postgres`, `kafka`,
`clickhouse`, `tidb`, `cassandra`, `valkey`) are run by their own operator, which owns their pods,
and are refused (`invalid_input`, with that reason). A store must belong to the workload's
workspace and be present.

Deleting a service or an app removes its mounts in the same transaction. Deleting a store that is
mounted is refused (`conflict`, naming the services and apps that mount it): remove it from them
first, so a workload never loses a volume it was given without being told.

**Used space.** `store_status.used_bytes` and `used_bytes_at` are written by the operator only:
for a volume, from the kubelet's volume stats of the pod that mounts it (null until it is mounted
and measured). The row is not rewritten on every poll unless the value moved by more than 1 MiB or
15 minutes passed.

**What lake answers.** `list_stores` answers, per store, `mounted_by: [{kind, id, name,
mount_path, read_only}]`, `used_bytes` (null: not measured) and `used_bytes_at`. `list_services`
and the app reads answer each workload's `stores: [{store_id, name, type, provider, mount_path,
read_only}]`.

**What the operator reads and writes.** Its polls read the mounts joined to the stores, and it
writes a volume's used bytes; both statements are copied in `tests/test_operator_contract.cpp`
(`listStoreMountsSQL`, `updateStoreUsedBytesSQL`):

```sql
SELECT m.store_id::text, m.object_kind, m.object_id::text, m.mount_path, m.read_only,
       s.slug, s.type, s.provider, s.name, s.desired
  FROM store_mounts m
  JOIN stores s ON s.id = m.store_id
 ORDER BY m.object_kind, m.object_id, s.name;

UPDATE store_status SET used_bytes = $2, used_bytes_at = now() WHERE store_id = $1::uuid;
```

The status upsert (`upsertStoreStatusSQL`) leaves `used_bytes` alone, so a phase change never
erases a measurement.

## What the operator does

On the services poll (and on a `lake_services` notification), for a Lake with services enabled,
after the services:

1. **Present stores:** create or update the provider's resource, named `<lake>-<slug>` in the
   namespace of the store's workspace (`workspaces.namespace`), labelled `orama.cloud/store-id: <id>` and owned by the Lake.
   - `volume` / `cluster`: a PersistentVolumeClaim, `ReadWriteOnce`, `spec.size_gi` GiB (default
     10), the StorageClass the Lake's services use. **Ready** once the claim exists: AKS's
     `managed-csi` binds on first use (`WaitForFirstConsumer`), so `Pending` is the expected state
     until a service mounts it; the message says so.
   - Any other provider: `Failed`, message `<provider> <type> stores are not available on this
     lake yet`. lake does not offer them, so this only guards a row written some other way.
2. **The connection:** `<slug>.json` in Secret `lake-<lake>-services`, beside the services'
   connections, written before `Ready`. For a volume:
   `{"type": "volume", "provider": "cluster", "claim": "<lake>-<slug>", "namespace": "<ns>",
   "size_gi": 10, "storage_class": "<class>"}`.
3. **Absent stores:** delete the resource (for a volume, the claim and so its data; lake asks for a
   typed confirmation first), drop `<slug>.json`, then write `Deleted`. A store already `Deleted`
   is history: skipped.
4. **Status:** upsert `store_status` only when phase, message, endpoint or observed generation
   change.

## What lake does

- **Capabilities** (REST, OpenAPI and MCP, authorized on the customer): `list_store_providers`,
  `list_stores`, `create_store` (`{customer_id, workspace_id, type, provider, name, size_gi?,
  region?}`; refused for a pair that is not available), `delete_store` (`{customer_id,
  workspace_id, store_id, confirm}`, `confirm` must be the store's name; a mounted store is a
  `conflict` naming its services and apps) and `store_connection`. `list_stores` answers
  `mounted_by`, `used_bytes` and `used_bytes_at` ([Mounts](#mounts)).
  Every capability but `list_store_providers` takes the workspace (from the `/w/<id>/` URL, the
  agent task, or the input) and sees only that workspace's stores; another workspace's store is
  `not_found`.
- **Data → Drives → Storage** (lake#656; before, Train → Data → Storage): the stores with type, provider, size and used space, who mounts each, status and endpoint; *New storage*
  (type and provider as one choice of the available pairs, name, size); *Show connection*; delete
  with typed confirmation.

## Verify

`tests/test_workspaces.cpp` (capabilities and page) and the operator's store tests; in a browser,
create a volume, see it Ready with its connection, and delete it.

`tests/test_operator_contract.cpp` holds a copy of every statement the operator runs against
lake's tables (`internal/controller/lake_*_pgx.go`, each commented with its source file). It
prepares each one against a fresh schema and checks that a store, service, app, build, launch and
schedule lake writes appear in the operator's polls in their workspace's namespace. When the
operator changes one of those statements, copy it there again.
