# Storage: the contract between lake and the Orama operator

**Status: phase 1 (lake#196); bucket and filesystem pairs (lake#664, not deployed).** A customer's **stores** — buckets, filesystems, network
filesystems and volumes — follow the managed-services model ([`services.md`](services.md),
liquid-bridge decision 46): lake records the desired store, the Orama operator makes it exist and
reports back. Nothing about a customer's store is decided by the operator, and lake never talks to
Kubernetes.

## The model: type × provider

A store has two independent axes: its **type** (what a client gets) and its **provider** (what runs
it). lake's provider table (`control::store_providers`, `include/lake/control/store.hpp`) lists the
valid pairs and which are available on this lake.

| Type | What a client gets | Providers (✅ offered) |
|---|---|---|
| `object` (a bucket) | S3-compatible or native object API | ✅ `r2` Cloudflare R2, ✅ `s3` AWS S3, ✅ `b2` Backblaze B2, ✅ `azure` Azure Blob, ✅ `gcs` Google Cloud Storage — each the customer's own bucket; ✅ `lake` a liquid repository, ✅ `zetta` kept by Lake in the workspace Zetta |
| `filesystem` | a tree of folders and files | ✅ `lake` (Drive: a liquid repository), ✅ `zetta` |
| `network_filesystem` | a shared mount several services attach read-write | `nfs` (in-cluster, run by the operator), `azure` Azure Files, `s3` EFS, `gcs` Filestore (phase 4) |
| `volume` | a block volume one service mounts | ✅ `cluster` (the cluster's StorageClass) |

Phase 1 delivered the model, the table, the capabilities, the UI and the **cluster volume**.
lake#664 offers every bucket and filesystem pair. A bucket at a cloud provider is **the customer's
own**, opened with a secret of the workspace's secret store (decision 58); Orama provisions no
bucket in an account of its own. Network filesystems wait for phase 4.

## Tables (migration `0028_stores`, `db/schema.sql`)

`stores` — written by lake only: `id`, `customer_id`, `type`, `provider`, `name`
(`^[a-z][a-z0-9-]{0,30}$`, unique among a customer's live stores), `slug` (`st-` plus the first 12
hex digits of the id; never reused), `spec` (`public` is false unless the customer chose otherwise —
**private by default** — and the rest by pair: a volume's `{size_gi, region, public}`; the
customer's own bucket's `{public, bucket, endpoint, region, prefix, credential: {mount_id, name}}`,
the credential a secret of the workspace's secret store, named and never copied; a `lake` store's
`{public, repository_id}`; a `zetta` store's `{public}`), `desired` (`present` | `absent`),
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

## What the operator does

On the services poll (and on a `lake_services` notification), for a Lake with services enabled,
after the services:

1. **Present stores:** create or update the provider's resource, named `<lake>-<slug>` in the
   namespace of the store's workspace (`workspaces.namespace`), labelled `orama.cloud/store-id: <id>` and owned by the Lake.
   - `volume` / `cluster`: a PersistentVolumeClaim, `ReadWriteOnce`, `spec.size_gi` GiB (default
     10), the StorageClass the Lake's services use. **Ready** once the claim exists: AKS's
     `managed-csi` binds on first use (`WaitForFirstConsumer`), so `Pending` is the expected state
     until a service mounts it; the message says so.
   - The customer's own bucket (`object` on `r2`, `s3`, `b2`, `gcs`, `azure`, lake#664): the
     operator checks the spec (a bucket, an `https` endpoint when one is given, a credential mount
     and name; otherwise `Failed` with the reason) and owns a `LakeSealedSecret` `<lake>-<slug>` in
     the Lake's namespace with a `lake` source naming the workspace secret — the sealed projection
     of decision 58, so services that mount the store get the credential and Postgres never holds
     it. `Provisioning` (*delivering the credentials from the workspace's secret store*) until the
     produced Secret exists, then **Ready** (*credentials delivered*), endpoint
     `<endpoint>/<bucket>`. The operator never touches the bucket itself.
   - A store Lake keeps (`object` or `filesystem` on `zetta` or `lake`, lake#664): nothing to
     create in Kubernetes; **Ready** at once (*kept by Lake in the workspace*). Lake creates and
     deletes what is in it.
   - A network filesystem: `Failed`, message `<provider> <type> stores are not available on this
     lake yet`. lake does not offer them, so this only guards a row written some other way.
2. **The connection:** `<slug>.json` in Secret `lake-<lake>-services`, beside the services'
   connections, written before `Ready`. For a volume:
   `{"type": "volume", "provider": "cluster", "claim": "<lake>-<slug>", "namespace": "<ns>",
   "size_gi": 10, "storage_class": "<class>"}`. For the customer's own bucket: `{"type", "provider",
   "bucket", "endpoint", "region", "prefix", "secret": "<lake>-<slug>", "namespace": "<lake ns>"}`
   (the Secret's name, not its values). For a store Lake keeps: `{"type", "provider"}`.
3. **Absent stores:** delete the resource (for a volume, the claim and so its data; lake asks for a
   typed confirmation first; for the customer's own bucket, the `LakeSealedSecret` and its Secret,
   never the bucket — *store removed; the bucket and what is in it stay in your account*), drop
   `<slug>.json`, then write `Deleted`. A store already `Deleted` is history: skipped.
4. **Status:** upsert `store_status` only when phase, message, endpoint or observed generation
   change.

## What lake does

- **Capabilities** (REST, OpenAPI and MCP, authorized on the customer): `list_store_providers`,
  `list_stores`, `create_store` (`{customer_id, workspace_id, type, provider, name, …}` with what
  the pair needs — [../api/storage.md](../api/storage.md#capabilities); refused for a pair that is
  not available), `delete_store` (`{customer_id,
  workspace_id, store_id, confirm}`, `confirm` must be the store's name) and `store_connection`.
  Every capability but `list_store_providers` takes the workspace (from the `/w/<id>/` URL, the
  agent task, or the input) and sees only that workspace's stores; another workspace's store is
  `not_found`.
- **Data → Drives → Storage** (lake#656; before, Train → Data → Storage): the stores with type, provider, status and endpoint; *New storage*
  (type and provider as one choice of the available pairs, name, and what the pair needs); *Show
  connection*; delete with typed confirmation.
- **Browsing** (lake#664): a Ready bucket or filesystem opens as a drive. Lake reaches the
  customer's own bucket over the provider's protocol with the workspace secret, and keeps a `zetta`
  or `lake` store's contents itself ([../api/storage.md](../api/storage.md#browsing-a-store)).

## Verify

`tests/test_workspaces.cpp` (capabilities and page) and the operator's store tests; in a browser,
create a volume, see it Ready with its connection, and delete it.

`tests/test_operator_contract.cpp` holds a copy of every statement the operator runs against
lake's tables (`internal/controller/lake_*_pgx.go`, each commented with its source file). It
prepares each one against a fresh schema and checks that a store, service, app, build, launch and
schedule lake writes appear in the operator's polls in their workspace's namespace. When the
operator changes one of those statements, copy it there again.
