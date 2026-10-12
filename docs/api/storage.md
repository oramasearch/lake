# Storage: a customer's buckets, filesystems, network filesystems and volumes

**Status: phase 1 implemented 2026-09-26 (lake#196); every bucket and filesystem pair offered and
browsable (lake#664, not deployed: it needs the operator change that reconciles them).** A customer's **stores** follow the
managed-services model ([`services.md`](services.md)): lake records the store the customer wants,
the Orama operator makes it exist and reports back. The contract between them — the type ×
provider model, the `stores` and `store_status` tables (migration `0028_stores`), the `NOTIFY`, the
connection in the services Secret — is [`../control-plane/storage.md`](../control-plane/storage.md).

Phase 1 offered one pair, a **cluster volume** (`volume` / `cluster`: a PersistentVolumeClaim on
the StorageClass the Lake's services use). lake#664 offers every bucket and filesystem pair, each
browsable as a drive ([Browsing a store](#browsing-a-store)):

- **Your own bucket** — `object` on `r2` (Cloudflare R2), `s3` (AWS S3), `b2` (Backblaze B2),
  `gcs` (Google Cloud Storage) or `azure` (Azure Blob): a bucket in the customer's own account,
  opened with **the workspace's credential**, a secret of the workspace's secret store the store
  names (decision 58). Never Orama's: Lake has no cloud account of its own for customers' data.
- **Kept by Lake in the workspace** — `object` or `filesystem` on `zetta`: a sealed tree in the
  workspace Zetta ([below](#browsing-a-store)).
- **A liquid repository** — `object` or `filesystem` on `lake` (*Drive*): a repository of the
  workspace whose `main` branch holds the store, every change kept in its history.

Network filesystems (`nfs`, Azure Files, EFS, Filestore) are listed, not offered (phase 4).

## What a person sees

**Data → Drives → Storage** (`/drives?tab=storage`, lake#656; it was a panel at the end of Data →
Datasets until then, and `/train/data#storage` still leads there) lists the workspace's stores. It
shows whether or not the workspace is ready: stores do not need the workspace. The Lake Console
says *storage* for a store and *drive* for where people work with files; "store" is the API's word.
The page is described in [../ui/drives.md](../ui/drives.md#storage).

- **The figures:** how many stores (or how many are changing), how many open as a drive, the
  volumes, and the GiB asked for in all.
- **The table:** name, type (*Volume*, *Bucket*, …), provider (*cluster disk*, …), size in GiB,
  this month's volume time (`usage_over_time`, dimension `volumes`, by object, linking to Usage),
  the status with the operator's message under it (for a volume: the claim binds when a service first
  mounts it), the endpoint, and the actions. A store with no status yet reads *Waiting for the
  operator*; a Ready one whose connection has not reached lake's mount reads *Connecting*; a
  withdrawn one reads *Deleting* until the operator has removed it, then it leaves the list. The
  page checks again every 3 seconds while any store is changing.
- **Open as a drive** (a Ready bucket or filesystem): a drive with `storage: store` over it
  ([drives.md](drives.md#store-drives)), browsed in place. A volume reads *Shown, not browsed*:
  its contents belong to the service that mounts it.
- **Mounted By** (lake#666): the services and apps that mount the store, each a link (an app to
  its page, a service to Ship → Services searched for its name) with the path a volume is mounted
  at and *read-only* when it is; *Not mounted* otherwise. A volume belongs to the one service or
  app that mounts it.
- **Size** shows the GiB asked for and, under it, the **bytes in use** the operator measured (*3.0
  MiB used (0%)* for a volume), *measured once mounted* for a volume no pod mounts yet, or *not
  measured yet*.
- **Show connection** (once Ready): answers with the Data page itself, showing every field the
  operator delivered (for a volume: `claim`, `namespace`, `size_gi`, `storage_class`) once. It is
  never put in a URL, a redirect or the list, and lake keeps no copy.
- **Delete:** a dialog where the person types the store's name, with a warning that says what
  happens to the contents. A volume (the operator deletes the claim and its data) and a store Lake
  keeps in the workspace Zetta (Lake deletes its items and bytes first) cannot be recovered. Your
  own bucket and a `lake` store's repository stay, with everything in them: Lake only forgets them.
  A mounted store is not deleted: the dialog names the services and apps that mount it, and lake
  answers `conflict` naming them.
- **New Storage:** one *Type and provider* select holding every available pair
  (`value="volume:cluster"`), then a name (lowercase letters, digits and `-`, starting with a
  letter, at most 31 characters) and what the pair needs: a volume's size in GiB (1–100, default
  10); your own bucket's name, the folder in it, the endpoint and region, and the secret mount and
  secret that open it; a repository (or a new one) for a `lake` store; nothing for a `zetta` one
  ([../ui/drives.md](../ui/drives.md#storage)). A store is private: nothing on the page or in the
  API makes one public.

A store belongs to the workspace it was created in (lake#465, migration `0044_store_workspaces`):
the panel lists the current workspace's stores, and the operator places each store in its
workspace's namespace.

The web app's Storage tab calls `create_store`, `delete_store` (with `confirm`) and
`store_connection` under the workspace's `/w/<id>/` path; a failure is shown with its reason. The
server form routes were retired in lake#561.

## Capabilities

All are in lake's registry (REST, OpenAPI, MCP; the *Storage* group in `llms.txt`, the skill and the
MCP page) and authorized on the customer's policy object. Every capability except
`list_store_providers` also requires `workspace_id`, supplied by the `/w/<id>/` URL or an agent
task's workspace when the input leaves it out, and must name a live workspace of the account. A store
of another workspace is `not_found`.

| Capability | Kind | Action on the customer | What it does |
|---|---|---|---|
| `list_store_providers` | read | `read` (checked by the handler, as every `list_` capability) | `{providers: [{type, provider, label, available, unavailable_reason?}]}`, the provider table `control::store_providers` in order: the available pairs first |
| `list_stores` | read | `read` (checked by the handler) | `{stores: [...]}`, oldest first; each store carries `id, customer_id, workspace_id, type, provider, name, slug, size_gi, region, public, desired, generation, phase, message, endpoint, observed_generation, created_at, connected, location`, `mounted_by: [{kind: service\|app, id, name, mount_path, read_only}]` and `used_bytes` (null until the operator has measured it) with `used_bytes_at` (lake#666), and never a credential. `location` is where the contents are: your own bucket's `{bucket, endpoint, region, prefix, credential: {mount_id, name}}` (the secret named, never its value), a `lake` store's `{repository_id}`, else `null`. `size_gi` is 0 for anything but a volume. A withdrawn store the operator has marked `Deleted` is history and is left out |
| `create_store` | action | `write` | `{type, provider, name, …}` recorded in the workspace; what else depends on the pair (below). A pair that is not in the table, or not available, is `invalid_input` naming the reason; a live name is `conflict`. The slug is `st-` and the first 12 hex digits of the id, never reused |
| `delete_store` | action | `admin` | `{store_id, confirm}`, `confirm` equal to the store's name; for a `zetta` store Lake first deletes its items and bytes; then marks it absent and bumps its generation. A store a service or an app mounts is `conflict`, naming them: remove it from them first (`set_service_stores`, `set_app_stores`) |
| `store_connection` | action | `admin` | A Ready store's connection: what the operator wrote to `<slug>.json` beside the services' connections. Refused (`workspace_not_ready`) before Ready, while the connection is still reaching lake, and for a withdrawn store |

`create_store`'s input by pair:

| Pair | Takes | Refused |
|---|---|---|
| `volume` / `cluster` | `size_gi` 1–100 (default 10), `region` | anything a bucket or repository takes |
| `object` / `r2`, `s3`, `b2`, `gcs`, `azure` (your own bucket) | `bucket` (S3: 3–63 lowercase letters, digits, `.`, `-`; Azure: the container, no `.`), `endpoint` (`https://host[:port]`, no path; required for R2 `https://<account id>.r2.cloudflarestorage.com` and Azure `https://<account>.blob.core.windows.net`, and for B2 without a region; AWS S3 defaults to `https://s3.<region>.amazonaws.com`, GCS to `https://storage.googleapis.com`), `region` (S3 default `us-east-1`, others `auto`), `prefix` (the folder in the bucket the store covers) and `credential {mount_id, name}` | `size_gi`; a credential missing, in another workspace's mount (`/credential/mount_id`), that the caller cannot read, or without the fields its protocol needs (`/credential/name`): `access_key_id` and `secret_access_key` (optionally `session_token`) for the S3 protocol, `account_key` (Shared Key, base64) or `sas_token` for Azure |
| `object`, `filesystem` / `zetta` | nothing more | `size_gi`, bucket fields, `repository_id` |
| `object`, `filesystem` / `lake` | `repository_id`, a live repository of this workspace the caller may write; without it Lake creates a private repository named after the store (`create_repository`, as the caller) | `size_gi`, bucket fields; another workspace's repository is `not_found` |

The credential is checked with the caller's own authority when the store is created (the person
must be able to read the secret, in this workspace); browsing then reads it as Lake.

Audit kinds: `store.create`, `store.delete`, `store.connection`, each with the customer, workspace,
store id, type, provider and name. The Postgres store sends `NOTIFY lake_services, '<store id>'` in the
transaction of every create and withdrawal, so the operator reconciles at once.

The worked flow *Create a volume* in `llms.txt` and the skill: `list_store_providers` →
`create_store` (`type: "volume"`, `provider: "cluster"`) → `list_stores` until `Ready` →
`store_connection` → `create_service` with `stores` → `list_stores` (`mounted_by`, `used_bytes`).

## Mounting a store

A service or an app names the stores it mounts (lake#666, migration `0080_store_mounts`; the
contract with the operator is [`../control-plane/storage.md#mounts`](../control-plane/storage.md#mounts)).
`create_service`, `set_service_stores` ([services.md](services.md)), `create_app` and
`set_app_stores` ([web-apps.md](web-apps.md)) take `stores: [{store_id, mount_path?, read_only?}]`,
which replaces what the workload mounted (`[]` mounts nothing). Writing it bumps the workload's
generation and notifies the operator in the same transaction, so its pods roll with the storage.

| Store | `mount_path` | In the workload's containers |
|---|---|---|
| volume | required: absolute, letters, digits, `.`, `_`, `-` and `/`, at most 256 characters, no `..` or `//`, and not `/`, `/data`, `/tmp`, `/proc`, `/sys`, `/dev` or beneath them (the workload's own; the operator skips them too) | mounted at the path; read-only when `read_only` |
| bucket in your own account (`r2`, `s3`, `b2`, `gcs`, `azure`) | none | its connection as environment variables `STORE_<NAME>_BUCKET`, `_ENDPOINT`, `_REGION`, `_PREFIX` and the credential fields |

Refused, each `invalid_input` at the entry's path (`/stores/<i>/…`) unless said otherwise:

- a store of another workspace or account (`not_found`), one being deleted, or one named twice;
- a store Lake keeps (`zetta`, `lake`): it is opened as a drive or read through Lake, not mounted;
- a type not mountable yet (a network filesystem);
- a volume without a path, or with a path on a bucket;
- a service kind whose pods its own operator runs (Zetta, Postgres, Kafka, ClickHouse, TiDB,
  Cassandra, Valkey: `/stores`); SQLite, DuckDB and Polars services and apps mount storage
  (`list_service_kinds` answers `mounts_stores`);
- a volume another service or app mounts: `conflict`, naming it. A volume (`ReadWriteOnce`) is
  mounted by one workload; a bucket by any number.

Deleting a service or an app removes its mounts in the same transaction, and so does deleting its
workspace; deleting a mounted store is refused (above). `list_services`, `list_apps` and `get_app`
answer each workload's `stores: [{store_id, name, type, provider, mount_path, read_only}]`.

**Bytes in use.** The operator writes `store_status.used_bytes` from the kubelet's volume stats of
the pod that mounts a volume; `list_stores` answers it beside `size_gi` (null until mounted and
measured).

## Browsing a store

A Ready bucket or filesystem opens as a drive (`create_drive {storage: "store", store_id}`,
[drives.md](drives.md#store-drives)). Every provider implements one interface,
`control::StoreContents` (list a folder, stat, read a range, write a whole file, make a folder,
move, remove), and passes one conformance suite (`tests/test_store_providers.cpp`). We write our
own clients: no provider SDK.

- **S3 protocol** (`r2`, `s3`, `b2`, `gcs`; `src/control/store_buckets.cpp`): Lake's own client,
  each request signed with AWS Signature Version 4 (`sigv4_sign`, pinned to the published AWS
  examples). `ListObjectsV2` with `/` as delimiter and `encoding-type=url` (continuation followed,
  at most 10,000 items a folder), a ranged `GET`, `PUT`, a server-side copy then `DELETE` for a
  move, and `DELETE`. A folder is every key under `<path>/`; an empty one is kept visible by a
  zero-byte marker object `<path>/`, the convention the providers' consoles use. AWS S3 is
  addressed on the bucket's own host (`<bucket>.s3.<region>.amazonaws.com`); the others in the path
  (`<endpoint>/<bucket>/<key>`). Google Cloud Storage is reached through its XML API with an HMAC
  key.
- **Azure Blob** (`azure`): Lake's own client over the Blob REST API (version `2021-08-06`),
  authorized with Shared Key (`azure_shared_key_signature`) or a SAS. List Blobs with a delimiter,
  Get Blob with `x-ms-range`, Put Blob (`BlockBlob`), Copy Blob (waiting on its status when it is
  still pending) then Delete Blob.
- **Kept by Lake in the workspace** (`zetta`; `src/control/store_kept.cpp`):
  the tree is sealed `lake_store_items` records in the workspace Zetta (one per file and folder,
  found by the store and the folder it is in; ids are blind indexes that say nothing of a path), and
  each file's bytes one object under an opaque key in the store's own bucket `lake_st_<12 hex>`,
  sealed in 1 MiB AES-256-GCM blocks exactly as workspace files are in `lake_files`
  ([drives.md](drives.md#how-file-bytes-are-kept)). A move rewrites records and never copies bytes;
  `delete_store` deletes every item and its bytes.
- **A liquid repository** (`lake`; `src/api/store_repository.cpp`): the repository's `main`
  branch, read through the repository store and written as one liquid change per write, folder,
  move and removal (the drive folder marker `.lake-folder` is never shown). A whole file is read to
  answer a range.

Errors use the API's codes: `not_found` (no such path), `conflict` (a file over a folder, a folder
over a file, a move onto something), `invalid_input`, and `store_unavailable` when the provider
does not answer or refuses — a refused credential says so and names the provider's code (*the
bucket refused the workspace's credential (403 SignatureDoesNotMatch …)*). A folder move or removal
touches at most 10,000 objects; a larger one is done with the provider's own tools.

**Credentials.** For your own bucket Lake reads the secret the store names in the workspace's secret
store, as Lake (one `read_secret` in the workspace's audit), keeps its fields in memory for a minute
so a download's windows do not each read it, and signs with them; they are never answered, logged or
put in a URL. The operator projects the same secret for services that mount the store
([../control-plane/storage.md](../control-plane/storage.md)).

**Where Lake may connect.** External buckets are reached through a transport of their own (the one
MCP connections use, on its own io thread): HTTPS only, and every address a host resolves to must
be public, so a store's endpoint cannot reach Lake's own network (its Postgres, the workspace
Zettas, the cloud metadata address).

## Configuration

Stores use the services configuration: `LAKE_SERVICE_DIR` (the mounted Secret
`lake-<lake>-services`) holds each Ready store's `<slug>.json`. Without it the Storage tab says
Design, Train and Ship are not available, and the store capabilities answer `store_unavailable`.

`LAKE_STORE_ALLOW_PRIVATE_ADDRESSES=1` (local development only) lets a store's endpoint be a
loopback or private address and plain `http` to loopback, for a bucket server on the same machine.

## Verify

```sh
ctest --test-dir build -R 'Stor'
```

- `Workspaces.StoresAreNamedPerCustomerAndOnlyAvailablePairsAreCreated`: the provider table (every
  bucket and filesystem pair available, network filesystems not, the available ones first), the
  slug, private by default, name conflict, unavailable and unknown pairs refused, another customer
  refused, withdrawal and a `Deleted` store leaving the list.
- `Workspaces.StoreCapabilitiesRefuseUnavailablePairsAndCheckTheName`: authorization on the
  customer, a network filesystem refused with its reason, a bucket without its name, size bounds, a
  mismatched `confirm` deleting nothing, and the connection only once Ready and delivered.
- `StoreProviders.*` (lake#664, `tests/test_store_providers.cpp`): SigV4 against the AWS examples,
  Shared Key against an independently computed signature, and one conformance suite (folders,
  writes, ranged reads, replacing, names with blanks and symbols, conflicts, moves, removal) run
  against an S3 bucket path-style and on its own host under a prefix, an Azure container with a
  Shared Key and with a SAS, a store kept in the workspace Zetta (its bytes sealed, its records
  sealed, a move copying nothing) and a liquid repository branch, each over a local fake of the
  provider (`tests/store_provider_fakes.hpp`: an S3 server and an Azure server that check every
  signature); a listing over two pages; a refused credential; the browser's checks of the secret's
  fields.
- `Drives.YourOwnBucketIsNamedWithAWorkspaceSecretAndOpensAsADrive`: every `create_store` refusal
  for a bucket (credential, size, endpoint, bucket, prefix, another workspace's mount, a secret
  without its fields), the store's `location` naming the secret and never its value, the drive
  writing to and reading from the fake bucket, and deletion leaving the bucket's contents.
  `Drives.AStoreKeptInTheWorkspaceZettaIsDeletedWithItsBytes` and
  `Drives.ALakeStoreNamesARepositoryOfItsWorkspace`.
- `Workspaces.EveryNewFeatureIsInOpenApiAndWorksOverMcp`: the five capabilities in OpenAPI and as
  MCP tools, a volume created over MCP.
- `PostgresStores.CreateListWithdrawAndNotifyFollowTheContract`: the store against a real database
  — the routing trigger refusing a store without a workspace, the workspace filter, `spec` as
  `{size_gi, region, public}`, conflict, withdrawal, a `LISTEN lake_services` connection receiving
  each id, and `store_status` joined in with `Deleted` history hidden.
- `PostgresStores.MigrationAssignsTheSoleWorkspaceAndRecordsTheRest`: migration 0044 run twice over
  stores without a workspace — the sole workspace assigned, a two-workspace account recorded as
  `needs_selection`, and the workspace immutable afterwards.
- `WebApps.ServicesAndAppsMountStorageAndListStoresSaysWho`: `stores` on `create_service`,
  `set_service_stores` and `set_app_stores` — a kind the operator does not build refused, a volume
  without a path, another workspace's store, a second workload on a volume (the conflict naming the
  first), a bucket with a path, a Lake-run store, a store named twice; `mounted_by`, `used_bytes`
  and the workloads' `stores`; a mounted store not deleted; deleting the workloads releasing it.
- `PostgresStores.MountsFollowTheirWorkloadAndKeepAVolumeToOne`: the same against a real database,
  with the rows written in the workload's transaction, `used_bytes` read back and the notification.
- `OperatorContract.*`: the operator's own SQL against lake's schema (see
  [`../control-plane/storage.md`](../control-plane/storage.md#verify)).

On a deployment with the operator's store reconciler: on Data → Drives → Storage, create a volume, see it
Ready with *Show connection*, and delete it by typing its name; create your own bucket with a secret
of the workspace, a `zetta` and a `lake` store, open each as a drive, upload, move and delete a
file, and delete each store.

## Changing it

- **A new pair becomes available:** set `available = true` and clear its reason in
  `control::store_providers` (`include/lake/control/store.hpp`) once the operator reconciles it;
  the capabilities, the select and the docs read the table. Move it up among the available ones.
  A bucket or filesystem pair also needs a `StoreContents` that passes the conformance suite.
- **A new type or provider key:** the schema's `store_type_check` / `store_provider_check` must list
  it first — a schema change, copied byte-identically into the operator.
