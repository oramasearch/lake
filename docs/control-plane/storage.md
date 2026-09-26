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
| `filesystem` | a POSIX tree in the customer's space | `lake` (Drive), `zetta` |
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
`generation` (bumped on every desired change), `created_by_taap_id`, `created_at`, `updated_at`.

`store_status` — written by the operator only, one row per store: `phase` (`Provisioning`,
`Ready`, `Failed`, `Deleting`, `Deleted`), `message` (the current step, in words a person reads),
`endpoint`, `observed_generation`, `updated_at`.

lake sends `NOTIFY lake_services, '<store id>'` in the transaction of every insert and every
withdrawal, on the same channel as services, so the operator's listener wakes the Lake at once.

## What the operator does

On the services poll (and on a `lake_services` notification), for a Lake with services enabled,
after the services:

1. **Present stores:** create or update the provider's resource, named `<lake>-<slug>` in the
   Lake's namespace, labelled `orama.cloud/store-id: <id>` and owned by the Lake.
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
  `list_stores`, `create_store` (`{customer_id, type, provider, name, size_gi?, region?}`; refused
  for a pair that is not available), `delete_store` (`{customer_id, store_id, confirm}`, `confirm`
  must be the store's name) and `store_connection`.
- **Train → Data → Storage**: the stores with type, provider, status and endpoint; *New storage*
  (type and provider as one choice of the available pairs, name, size); *Show connection*; delete
  with typed confirmation.

## Verify

`tests/test_workspaces.cpp` (capabilities and page) and the operator's store tests; in a browser,
create a volume, see it Ready with its connection, and delete it.
