# Workspace Storage Plan

Refs [lake#311](https://github.com/oramasearch/lake/issues/311),
[lake#315](https://github.com/oramasearch/lake/issues/315) (part 2),
[lake#319](https://github.com/oramasearch/lake/issues/319),
[lake#380](https://github.com/oramasearch/lake/issues/380),
[lake#381](https://github.com/oramasearch/lake/issues/381),
[lake#382](https://github.com/oramasearch/lake/issues/382),
[lake#383](https://github.com/oramasearch/lake/issues/383) and
[lake#384](https://github.com/oramasearch/lake/issues/384).

**Status: design, decided 2026-10-03 (decision 54).** Implemented: lake#319 (`repository_refs`
dropped) and lake#315 part 2 (the description in `lake_repositories`), both in migration `0064`;
lake#382 (the log stays with the instance; no migration); lake#384 (route patterns in request logs
and the per-workspace `lake_requests` series, [PR #521](https://github.com/oramasearch/lake/pull/521),
no migration); lake#383 phase A (the tenant image route table and manifests and tags as
workspace records, migration `0068`, [PR #523](https://github.com/oramasearch/lake/pull/523));
lake#514's cascade (migration `0065`); and lake#380, the workspace store and its move
(migration `0067`, [workspace-store.md](workspace-store.md); every workspace on dev is switched;
needs liquid `8955519` or later). With every workspace switched, the records' retired tables and
columns are dropped by migration `0070_drop_moved_tables` (lake#311,
[schema.md](../control-plane/schema.md#drop-moved-tables-migration)), and the operator's
`remove_space_store` removes a space's old store from lake's volume once every workspace of the
space is switched ([workspace-store.md](workspace-store.md)); neither has run on dev yet.
Everything else here is not implemented yet. This page plans the moves that
[workspace records](workspace-records.md) does not cover: the bytes on lake's store volume
(liquid stores, search indexes, the registry, the transparency log), the two repository tables
that sit beside them, and lake's operational telemetry. It states what each item is today, what it
is under the umbrella rule, where its bytes live, how it moves, and the decisions Steve made. The rule is in [workspace-data-plane.md](workspace-data-plane.md): everything goes in a
workspace Zetta except multitenancy, dispatch and billing.

## Decisions

**Decided by Steve on 2026-10-03:** liquid-bridge `docs/decisions.md`
[decision 54](https://github.com/oramasearch/liquid-bridge/blob/main/docs/decisions.md), recorded in
[lake#512](https://github.com/oramasearch/lake/issues/512). Every answer is the recommendation
except S12, where Steve chose cascade over refusal.

| # | Item | Question | Decided | Not chosen |
|---|---|---|---|---|
| S1 | #380 | Where do a workspace's liquid store bytes live? | **In the workspace Zetta**, through a new Zetta object capability ([spec below](#the-zetta-object-capability), [zetta#1005](https://github.com/oramasearch/zetta/issues/1005)) that lake's liquid store backend writes to; the bytes sit on the workspace Zetta's own volume. | A directory per workspace on lake's shared volume; a liquid store server pod per workspace; a cloud bucket per workspace. |
| S2 | #380 | An interim partition of the shared volume by workspace? | **No.** Straight to the target, one migration. | Partition now and move later. |
| S3 | #380, #383 | Encrypt workspace liquid stores at rest? | **Yes**, with a per-workspace data key wrapped under a key derived from `LAKE_INSTANCE_KEK`; the workspace Zetta holds only ciphertext. | Plaintext stores (keeps chunk-delta compression). |
| S4 | #383 | Split the registry? | **Yes.** Tenant OCI repositories (`<space>/<name>`, `<space>/cache/<name>`) move to the workspace; npm, Python, debuginfod and legacy one-component OCI stay with the instance. | Move everything, renaming instance packages. |
| S5 | #383 | Where do a tenant image's blobs live? | **In the workspace's liquid store** as SHA-256 objects; manifests and tags are conditional records. | A separate area of the object capability. |
| S6 | #383 | Do image addresses change? | **No.** `<space>/<name>` stays, through a content-free central route (space and name to workspace). | The workspace in the path. |
| S7 | #382 | Do the transparency log, TUF and timestamp evidence stay with the instance? | **Yes**, named as an instance-level exception in the umbrella rule. | One log per workspace; the lake Zetta. |
| S8 | #381 | Where does a workspace's search index live? | **Beside its liquid store**, as sealed objects, rebuilt rather than copied. | Zetta full-text over a workspace table; a lake-local cache. |
| S9 | #384 | Are lake's process metrics and request logs workspace data? | **No: instance operations**, named as an exception in the umbrella rule. Logs carry route patterns, not raw paths; each workspace gets its own request series in its Zetta. | Every request's metrics and log line in its workspace Zetta. |
| S10 | #315 | What of the `repositories` row moves? | **Only `description`** (and later settings) to `lake_repositories`; the row stays as routing. | Name and visibility too. |
| S11 | #319 | Move or drop `repository_refs`? | **Dropped.** | Move it as a cache. |
| S12 | all | What does deleting a workspace do to its repositories and images? | **Cascade** ([lake#514](https://github.com/oramasearch/lake/issues/514)): deleting a workspace deletes its repositories (live and tombstoned, cutting retention short) and its tenant images. Retained snapshots (S13) are the only copy left. | Refuse while the workspace holds repositories or images (the recommendation). |
| S13 | all | Backups of workspace volumes? | **Scheduled CSI snapshots** of each workspace Zetta volume and of lake's store volume, desired in Postgres and taken by the operator, with one before the cutover ([orama-operator#260](https://github.com/oramasearch/orama-operator/issues/260)). | None until production. |
| S14 | #315, #380 | The `liquid` repository's space has no workspace. | **Create a workspace for that space and assign the repository** ([lake#513](https://github.com/oramasearch/lake/issues/513)). | Delete it; leave it behind. |

## Summary

| Item | Holds today | Dev, 2026-10-03 | Classification | Target |
|---|---|---|---|---|
| #315 part 2 `repositories` | One row per repository | 10 rows (8 live and 1 tombstoned in one workspace, 1 unassigned) | split | Row stays as routing; `description` moves to `lake_repositories`. **Done** (migration `0064`); the column is dropped after the copy is verified |
| #319 `repository_refs` | Copy of each liquid ref | 0 rows | workspace (unused) | **Dropped** (migration `0064`) |
| #380 `<customer>/store` | Liquid objects and history of every repository, Drive, model and dataset | 18.3 MB in 2 customer stores, 10 repositories | workspace | One liquid store per workspace in the workspace Zetta's object capability. **Implemented** ([workspace-store.md](workspace-store.md), migration `0067`); each workspace is switched by `move_workspace_store` |
| #381 `<customer>/index` | Search catalogue, segments and manifest | none written | workspace | Sealed objects beside the workspace store; rebuilt |
| #382 transparency, TUF, timestamps | The instance's log, trust metadata and evidence | none written | instance | Stays with the instance. **Done** (no migration) |
| #383 registry | OCI, npm, Python and debug artifacts | 561 MB: 38 blobs, 7 manifests, one space | split | Tenant images to the workspace store; instance registry stays. Phase A **done** (routes, manifests and tags as records, migration `0068`); blobs wait for phase B |
| #384 metrics and request logs | Process counters and stdout lines | nothing kept (no Prometheus or Loki on the cluster) | instance | Stays; plus a per-workspace request series. **Done** ([PR #521](https://github.com/oramasearch/lake/pull/521)) |

## What is on dev

Measured read-only on 2026-10-03 (`du` in pod `lake-prod-c7cd58699-95l5z`, `SELECT count(*)` on
`lake-prod-controldb-1`). The store volume `lake-prod-store` is a 100 GiB `managed-csi` Azure disk,
581 MB used.

| Path | Size | Contents |
|---|---|---|
| `6bb777ba-…/store` | 16 MB | packs 2.0 MB, liquid chunk index 5.7 MB, leases 7.9 MB |
| `6bb777ba-…/repositories` | 40 KB | 9 repositories (8 live, 1 tombstoned), all assigned to that space's one workspace |
| `da7cf099-…/store` | 2.3 MB | the `liquid` repository; its space has no workspace |
| `prod/registry/blobs` | 561 MB | 38 blobs |
| `prod/registry/manifests` | 48 KB | 7 manifests: one space, `e2e-notes` and its build cache |
| `<customer>/index`, `prod/log`, `prod/registry/{npm,python,debug}` | absent | never written on dev |

There are two Ready workspaces (`prod-ws-6bb777ba498a`, `prod-ws-787217d6ec44`), each a one-node
`OramaZettaCluster` on Zetta `dev-82f617ea` with a 5 GiB `default` (Azure disk, reclaim `Delete`)
volume. The cluster runs no Prometheus, Loki or monitors. Apps: 3 rows; builds: 13 rows.

## Constraints every option must meet

1. **A pod mounts only claims in its own namespace.** Lake runs in `lake`; each workspace Zetta
   runs in `prod-ws-<id>`. Lake cannot mount a workspace's volume. A volume per workspace mounted
   by lake would need every claim in lake's namespace, a lake rollout each time a workspace is
   created (a pod's volumes are fixed), and would meet the node's Azure disk attach limit. So a
   workspace's bytes are either served by a pod in the workspace's namespace or kept on a volume
   lake already mounts.
2. **The operator never reads or writes a workspace Zetta** (decision 51). It may size, snapshot
   and delete a workspace's volume from desired state in Postgres; copying bytes is lake's work,
   started by the operator through a lake capability, as `copy_workspace_records` is.
3. **A liquid store has one writing process** ([store-volume.md](../deployment/store-volume.md)).
   Lake's root lock and liquid's `store.lock` are `flock`s on a local filesystem. A store whose
   bytes are elsewhere needs its own owner lease with fencing, and the hand-off (lake#479) must
   carry over: the successor waits, then owns.
4. **What Zetta offers today** (Zetta `dev` at `82f617e`):
   - the keyed record store (values up to 1 MiB of base64, linearizable per key, conditional
     writes) and labelled time series (zetta#1231), which [workspace records](workspace-records.md)
     already use;
   - large objects (`docs/data/large-objects.md`): whole-file in memory on every read and write,
     capped near 48 MiB by the cluster line size, replicated to every node, no range read, no
     append, reached only through cookie-authenticated web routes (`/blob`), not the capability
     registry. They do not fit a liquid store, whose packs are appended at offsets and read by range;
   - BM25F full-text search over tables (`docs/indexing/`), with no exact-substring, regex,
     history or artifact mode.

   There is **no object or file capability** a remote process can append to and read by range.
5. **What liquid offers today** (liquid `bf09181`, `docs/library/store.md`):
   - `liquid::store_backend` is the extension point for a store's persistence: `put` (atomic
     replace), `put_range` (create or extend at an exact offset), `get_range`, `remove`,
     `list(prefix)` and `barrier`. Only the disk backend exists. A non-disk backend loads index
     runs into memory instead of mapping them;
   - `exclusive_owner` with a backend that has no local root is `errc::unsupported`;
   - **a repository's own state is not written through the backend.** `LIQUID_REPO`,
     `LIQUID_JOURNAL`, the checkpoint and the replication cursor are files read and renamed with
     `std::filesystem` (`src/repository/repository_impl.cpp`). A store on a remote backend
     therefore still needs liquid to route repository state through `store_backend` (a liquid
     change; see [Dependencies](#dependencies)).

## The Zetta object capability

This is the capability S1 decided on. It does not exist yet; it is tracked in
[zetta#1005](https://github.com/oramasearch/zetta/issues/1005), whose 2026-10-03 comment carries
the full specification, with its tests and latency budget. It maps one to one onto `liquid::store_backend`, so lake's backend is a thin
client.

**Model.** A **bucket** is declared once (`create_bucket`, replicated like a record collection's
definition). An **object** is `(bucket, key)`, a key of 1–1024 bytes with `/` as separator. Bytes
live in node-local files under `<data_dir>/objects/<bucket>/`, on the workspace Zetta's volume,
never in the replicated catalogue.

| Operation | Route (binary body, not base64) | Meaning |
|---|---|---|
| put | `PUT /v1/objects/<bucket>/<key>` | Atomically replace the whole object (temporary file, fsync, rename). |
| put range | `PATCH /v1/objects/<bucket>/<key>?offset=<n>` | Create or extend the object at exactly `n`. `n` must equal the current size, or the bytes must equal the bytes already there (a retried append succeeds); otherwise `409 conflict` with the current size. |
| put if absent | `PUT /v1/objects/<bucket>/<key>?if_absent=true` | Conditional create, decided atomically; `409` if the key exists (liquid's exclusive repository create). |
| get range | `GET /v1/objects/<bucket>/<key>`, `Range: bytes=a-b` | Exactly that range, streamed; `416` outside the object (liquid's `corrupt`), `404` absent (`not_found`). |
| remove | `DELETE /v1/objects/<bucket>/<key>` | Delete one object. |
| list | `GET /v1/objects/<bucket>?prefix=&after=&limit=` | Keys and sizes, by key, paged. |
| barrier | `POST /v1/objects/<bucket>:barrier` | Every write this writer had acknowledged is fsynced before the answer. |
| usage | `GET /v1/objects/<bucket>:usage` | Object count and bytes, for lake's storage measurement and quota. |
| owner | `POST /v1/objects/<bucket>:acquire`, `:renew`, `:release` | The bucket's owner lease (below). |

**Ownership and fencing.** `acquire {holder, ttl_ms, wait_ms}` waits up to `wait_ms` for the lease
and answers a **fence**, a number that rises with every new holder. Every mutation carries
`Zetta-Fence: <n>`; one with an older fence is refused `409 fenced`, so a lake process that lost
its lease can never write the store again, even if it is still running. The holder renews well
inside `ttl_ms` and releases on drain. This is the remote form of lake's root lock and liquid's
`store.lock`, and it makes lake#479's hand-off per workspace: a successor's `acquire` waits while
the predecessor drains and returns when it releases.

**Rights.** `objects:read`, `objects:write` and `objects:manage`, object-scoped by bucket prefix
(`ON BUCKET lake_`), held only by lake's workspace credential (the bootstrap admin today). The
routes are not capabilities in the MCP registry and are in neither of lake's `workspace_query` or
`workspace_act` allow-lists: people and agents never reach a lake bucket directly.

**Durability and replication.** On a one-node workspace, every workspace today, the volume is the
only copy (hence S13). On a multi-node workspace, each object is placed on `rf` owners by a
rendezvous hash of `(bucket, key)`, as dataset partition bodies are
(`docs/data/sharded-blob-placement.md`), and a mutation is acknowledged once `rf` owners hold it.
A multi-node workspace must have this before it holds a store.

**Limits.** A request body streams; no operation buffers a whole object. A put range has no size
cap beyond the volume. A bucket has a byte quota set by lake (`create_bucket {quota_bytes}`); a
write past it is `507`.

## Item by item

### lake#380: the liquid store

**Implemented 2026-10-03** as planned below; the design as built, including the encryption format,
is [workspace-store.md](workspace-store.md), and the operator capability is
[move_workspace_store](../api/workspace-store.md). Differences from this plan: the copy does not yet
check the S13 snapshots (the desired state is migration `0069`; the move does not wait on it yet);
the bucket quota is the space's storage quota and does not yet follow `workspaces.storage_gi`; it
needs liquid#332 (merged,
liquid `8955519`), because liquid's index merge had rewritten runs in place.

**Today.** `LAKE_STORE_ROOT/<customer>/store` is one libliquid store per space: packs, object map,
chunk index, leases and collector state. Beside it, `<customer>/repositories/<id>/` holds each
repository's refs, journal, checkpoint and replication cursor. One store per space deduplicates
across every repository, Drive, model and dataset of the space. Liquid pack frames are appended at
remembered offsets; sealed packs are never rewritten.

- **Writers:** lake, for `liquid push` (`/v1/repositories/<id>/sync/*`), Drive uploads and moves,
  `commit_repository_files` and the staging capabilities (agents through MCP), model and dataset
  sync, repository and GitHub import, replication from a peer instance, and the collector.
- **Readers:** liquid clone and pull clients, Drive pages, repository trees and files (people and
  agents), the search indexer, builds that fetch source through lake, and peer instances
  replicating.

**Classification.** Workspace: it is the repositories' content.

**Target (S1).** One liquid store per workspace, opened by lake on a `store_backend` over the
workspace Zetta's object capability, bucket `lake_store`. Every liquid key (`packs/…`, `index/…`,
`leases/…`, `gc/…`, `keys/…`, and with the liquid change, `repositories/<id>/…`) is one object.
The space no longer has a store; a repository's workspace (`repositories.workspace_id`) names its
store. With S3, the store is opened with a keyring whose one trust domain is the workspace.

**Hot paths.**

- **Push.** Liquid appends frames with `put_range`. Lake's backend buffers appends to the open pack
  in memory and sends them in pieces of at least 1 MiB, and always before answering `barrier`,
  which liquid calls before it publishes the object map. A crash before the barrier loses only
  bytes liquid had not yet published, which liquid already recovers from. A push therefore costs
  its bytes plus one fsync per barrier on the workspace Zetta.
- **Pull and reads.** Liquid reads chunks with `get_range`. Sealed pack bytes never change, and the
  open pack only grows, so lake keeps a bounded read cache of aligned 4 MiB windows per workspace
  with no invalidation except on `remove`. Index runs are loaded into memory at open (non-disk
  backends do), so a lookup reads no network.
- **Open.** Lake opens a workspace's store on the first request that needs it, not at start, so
  start-up does not wait on every workspace; held requests wait per workspace, as they do for the
  root today.
- **Budget to prove before cutover:** a push and a pull of the dev repositories, and a 100 MiB
  Drive file, through the backend against a real workspace Zetta, timed beside the same calls on
  the disk backend; the page budget tests stay as they are.

**Ownership.** Lake acquires each workspace bucket's lease when it opens that store and opens
liquid without `exclusive_owner` (the fence gives the single-writer guarantee liquid's contract
asks of the caller). `.lake-owner.lock` stays, covering only what remains on the store volume
(#382 and the instance registry). On SIGTERM lake drains, closes each store and releases each
lease, then the root.

**Backups.** The workspace Zetta's volume is the store's only copy; scheduled CSI snapshots (S13,
[orama-operator#260](https://github.com/oramasearch/orama-operator/issues/260)) are its backup.

**Workspace deletion (S12, cascade, [lake#514](https://github.com/oramasearch/lake/issues/514)).**
Deleting a workspace deletes its repositories, live and tombstoned, cutting their retention short.
Lake closes the workspace's store and releases its bucket lease before it reports the deletion, and
the operator's deletion of the namespace and its volume destroys the bytes. Before the switch, the
collector reclaims the deleted repositories from the space's store, since their retention has
ended. The central audit log records one event per destroyed repository, because the workspace's
own audit chain goes with its Zetta. Retained snapshots are the only copy left.
**Shipped (before the switch):** migration `0065_workspace_deletions` and the cascade in
[workspaces.md](../api/workspaces.md#deletion); closing the store is the
`WorkspaceContent::close_workspace_store` extension point, which lake#380 sets to close the store
and release its lease.

**Migration and cutover.** No dual write; nothing on the store volume is deleted.

1. **Preconditions.** Every workspace Zetta runs an image with the object capability; lake ships the
   backend and the liquid change; every repository has a workspace (S14); a snapshot of the store
   volume exists (S13).
2. **Plan** (`move_workspace_store`, an operator-invoked lake capability like
   `copy_workspace_records`, `mode: plan`): each workspace's repositories and their sizes;
   repositories with no workspace are reported and block.
3. **Copy** (`mode: copy`): for each repository, liquid transfers its whole closure from the space's
   store into the workspace store with its refs, journal and checkpoint (the local form of
   `replicate_from`, content addressed, so a rerun sends only what is missing). Drive, models and
   datasets are repositories and move the same way. Objects no repository reaches stay behind.
4. **Verify** (`mode: verify`): every ref equal on both sides, and a collector dry run (a full
   reachability walk) on the workspace store finds every reachable object, the check lake#479's
   test uses.
5. **Switch** (`mode: switch`), per workspace: lake holds that workspace's repository requests,
   copies what arrived since step 3, verifies again, records the switch in a content-free central
   column (`workspaces.store_moved_at`), and answers the held requests from the new store.
6. **Leave.** The space's old store stays read-only on the volume. Removing it needs Steve's yes
   (the store volume rule), after the copy is verified on dev.

Storage measurement for metering switches from a live `du` of the space's store to the bucket's
`usage`, now per workspace.

### lake#381: the search index

**Interim (lake#380).** Until this item lands, search neither indexes nor returns a switched
workspace's repositories, so none of their content is written to the index on lake's volume
([search.md](../api/search.md)).

**Today.** `<customer>/index/`: `blobs.json` (blob digest to UTF-8 file text, append-only),
`segment-<repository>-<change>-<kind>-<ref>.json` (path-to-blob occurrences), and `manifest.json`.
Lake loads them into oramacore-cpp BM25F adapters at start, per space, and republishes from
memory. It is derived entirely from the liquid store and holds file text in the clear.

- **Writers:** lake, on sync publication, release creation and `reindex_artifacts`.
- **Readers:** lake's `search` capability (REST, MCP, which agents use) and `/search`.

**Classification.** Workspace: it is repository content.

**Target (S8).** Sealed objects in the workspace's object area, bucket `lake_search`, beside the
store: the catalogue, segments and manifest, each sealed with the workspace's record seal key
(`lake/workspace-records/v1/seal/<workspace>`) so file text is never plaintext in the Zetta. Lake
loads a workspace's segments on its first search and evicts idle workspaces under a memory bound.
The `customer` search input becomes `workspace_id`; a search spans one workspace. The manifest is
replaced by `put` (atomic), and its writes follow the store's lease, so the lost-update row in
[store-volume.md](../deployment/store-volume.md) no longer applies.

**Migration.** None to copy on dev. After #380's switch, `reindex_artifacts` and a re-publication
of each branch tip rebuild every workspace's index from its store.

### lake#382: transparency log, TUF and timestamp evidence

**Today.** `LAKE_STORE_ROOT/<instance>/log/`: tiles and `tree.json` (appends under
`append.lock`), signed checkpoints, `tuf/<version>.<role>.json`, and `timestamps/*.ers`.

- **Writers:** lake, for `append_transparency_entries`, `initialize_tuf`, `publish_tuf_metadata`,
  `rotate_tuf_root`, checkpoint publication and timestamp evidence (all `admin` on the instance
  log); the attestation of each agent session minted; central `dynamic_secret` audit events; and
  replication batches from peer instances.
- **Readers:** anyone, anonymously: monitors (`transparency_identity_scan`, `/transparency`),
  relying parties verifying inclusion and consistency proofs and proof bundles (liquid clients),
  TUF clients, and peer instances replicating the log.

**Classification.** Instance. The log is one append-only Merkle tree whose value is that every
observer sees the same tree; its checkpoint is signed by the instance, and its entries are public
statements by design. TUF metadata and timestamp evidence protect that one log.

**Target (S7).** Stays at `LAKE_STORE_ROOT/<instance>/log/` on lake's store volume, under the
root lock. The umbrella rule names it as an exception: the instance's public commitment.
**Done** ([lake#382](https://github.com/oramasearch/lake/issues/382), [PR #519](https://github.com/oramasearch/lake/pull/519)): the audit row in
[workspace-data-plane.md](workspace-data-plane.md#storage-audit) classifies it as instance exception
S7. The two checks that belonged with it:

- the TUF and timestamp writers are unsafe between two processes (in-process mutex, per-process
  temporary names). The store root lock (`.lake-owner.lock`, [store-volume.md](../deployment/store-volume.md))
  admits one writing process at a time, which covers them; they are unchanged.
- `dynamic_secret` events still reach the log. `lease_dynamic_secret`, `renew_dynamic_secret` and
  `revoke_dynamic_secret` are in the central list of decision 52's audit routing
  (`src/api/audit_routing.cpp`), although the lease is a record of its mount's workspace, so
  `finish_call` signs each into the central chain and appends that event as the instance log leaf
  the identity-exit scenario expects. No leaf was lost. The leaf is the central event's body: the
  lease, mount and workspace ids, the principal and the operation, never the credential, the role
  or the mount's name. `WorkspaceRecordStores.DynamicSecretsInAWorkspaceKeepTheirInstanceLogLeaf`
  proves it with a workspace audit route available, and fails if the three capabilities move to
  the workspace list. Moving them there would need the log to name a workspace chain's event
  first.

**Migration.** None.

### lake#383: registry storage

**Today.** `LAKE_STORE_ROOT/<instance>/registry/`: `blobs/` (content-addressed), `manifests/`
(per repository and tag), `uploads/`, `npm/`, `python/`, `debug/`.

- **Writers:** build pods with a build push token (`<space>/<name>`, `<space>/cache/<name>`); the
  administrator for npm, Python, debug and legacy one-component OCI repositories.
- **Readers:** the cluster's kubelet pulling app images with an app pull token or a
  `registry_pull` pull secret (`<in-cluster-registry-host>/<space>/<app>`), builds reading their
  cache, npm and pip clients, debuginfod clients (public), and `/ship/packages`.

**Classification.** Split (S4). Tenant images are workspace data. npm, Python, debuginfod and
legacy OCI repositories are one instance-wide namespace with public reads; they stay with the
instance on the store volume under the root lock, and their read-modify-write indexes keep that
lock's protection.

**Target for tenant images (S5, S6).**

- **Blobs** are objects in the workspace's liquid store, written with SHA-256 identity so the
  digest a client sends finds them. With S3 (encrypted store), a record in
  `lake_oci_blobs` maps the OCI digest to the stored identity. Layers deduplicate with build
  outputs and with each other.
- **Manifests and tags** are records: `lake_oci_manifests` (keyed by digest, written `if_absent`)
  and `lake_oci_tags` (keyed by repository and tag, moved `if_revision`), which removes today's
  last-writer-wins on tags.
- **Uploads** are staged in the workspace bucket `lake_uploads` and become a store object at
  completion, after the digest check.
- **Routing.** A central, content-free `oci_repository_routes` (space, repository name, workspace,
  created by) is written `if_absent` at a build's first push, from the build row's workspace;
  pulls resolve it before any workspace is read. App images keep their addresses.
- **Workspace deletion** (S12, cascade): a workspace's routes are deleted with it, and its images
  go with its store and records. Until then lake#514 removes the tags and manifests of the
  workspace's tenant repositories from the instance registry (`WorkspaceContent` in
  `include/lake/api/workspace_deletion.hpp`); this item deletes the routes in the cascade
  transaction instead.

**Hot path: image pulls.** The kubelet pulls through lake, so pull latency is a workspace store
read: lake streams the blob from the store with read-ahead and never holds a whole layer in
memory. The window cache of #380 serves repeated pulls of one layer across nodes. Redirecting
pulls straight to the workspace Zetta is not proposed: it would expose each workspace Zetta to the
nodes' network and move the pull-credential check out of lake.

**Migration.** 561 MB on dev, one space. `move_workspace_store` copies each tenant blob referenced
by a tenant manifest into the workspace store, verifies its SHA-256, writes the manifest and tag
records, and writes the route. Cache repositories may be dropped instead (dev data) and rebuilt
by the next build. Blobs no tenant manifest references stay behind. The instance registry has
nothing to move on dev.

**Phase A done** (migration `0068_oci_repository_routes`, [PR #523](https://github.com/oramasearch/lake/pull/523)). The route table is
`oci_repository_routes (customer_id, name, workspace_id, created_at, updated_at)`: no manifest,
digest or tag, and, unlike the target above, no `created by` (`create_image_repository` records
who in its audit event; a build's first push is the build's). A build's first
push of its own target (`<image>`, `cache/<image>`) writes it from the build row's workspace;
anything else needs `create_image_repository` with an
explicit `workspace_id` (no default; missing or unknown is `invalid_input` at `/workspace_id`). A
trigger refuses any change of a route, the `0065` cascade deletes a workspace's routes in its
transaction, and the operator's `unroute_image_repository` removes a wrong one. The migration routes each image name the apps and builds of exactly one live workspace
use. Manifests are `lake_oci_manifests` records keyed by the blind index of the digest, written
`if_absent`; tags are `lake_oci_tags` records keyed by repository and tag and moved at the revision
read, each carrying its manifest's references with their sizes, so blob reads and the usage count
read only tags (one `list_records` per repository). A tag move is conditional, so never torn, but
concurrent pushes of one tag still end last-writer-wins. Blobs stay in `blobs/`, uploads in
`uploads/`; each blob uploaded into a workspace's repository is recorded in its `lake_oci_blobs`,
and a manifest may reference only blobs recorded in its workspace.
The copy is `copy_workspace_records` (collection `oci_tags`), not `move_workspace_store`: `copy`
(never `plan` or `verify`) routes what is unambiguous (route, then the one workspace whose apps or builds use the name, then the
space's only live workspace), reports the rest, and writes and verifies each manifest and tag.
Phase B moves the blobs into the workspace store (`lake_oci_blobs` then maps each digest to the
stored identity; uploads in `lake_uploads`) after lake#380. Not done
in phase A: counting registry requests in a workspace's `lake_requests` series (#384).

### lake#384: Prometheus metrics and request logs

**Today.** `GET /metrics` exposes process-local counters, gauges and histograms whose labels are
bounded (method, route, status, store operation; never a raw path, customer id or secret). Each
request writes one JSON line to stdout (`path` without its query, `route`, `status`,
`duration_ms`, `node`). Nothing on the dev cluster scrapes or keeps either.

- **Writers:** the lake process.
- **Readers:** a Prometheus scraper with the bearer (none on dev) and `kubectl logs`.

**Classification (S9).** Instance operations. The series describe the lake process and carry no
customer identity, by the rule in [telemetry.md](../serving/telemetry.md). The request line's raw
`path` can carry repository and package names, so it is replaced by `route` plus the request's
ids, after which the log carries no customer content.

**Target.** The process telemetry stays as it is. New: a counter `lake_requests` in each workspace
Zetta's labelled time series (labels: route, capability, status class), recorded by a bounded
in-process recorder that the owner loop flushes every few seconds with `record_metric`, never on
the request path (as `MeteringRecorder` does). It is what a workspace's own pages can show about
its traffic. Metering stays in records (exact), never in this series.

**Migration.** None; the series starts empty.

**Done** ([PR #521](https://github.com/oramasearch/lake/pull/521), no migration). The request log line and the `handler_exception` line dropped
`path`; a line names `route`, `request_id`, the `capability` of a REST or MCP call, each captured
`*_id` that is a UUID, and the routed `workspace_id`. `RequestRecorder` aggregates per (workspace,
minute, route, capability, status class); the owner loop flushes it every ten seconds with
`create_metric` once per workspace and `record_metric` (at most 1000 points a call). A request is
counted in the first workspace one of its handled capability calls acted on (`acted_workspace`, the
lookup that routes workspace audit), never a default; the instance registry's routes are counted
nowhere yet: #383 phase A gave tenant images a workspace route, and counting their requests
through it is a follow-up. Drops are counted in
`lake_request_series_dropped_total{reason}` (`bound`, `workspace_deleted`, `refused`). See
[telemetry.md](../serving/telemetry.md#the-workspace-request-series).

### lake#315 part 2: repository metadata

**Today.** `repositories`: id, `customer_id`, `workspace_id` (immutable since `0051`), name
(unique among a space's live repositories), description, visibility, `deleted_at`,
`retention_until`, timestamps. Its policy object lives in `objects`.

- **Writers:** lake (create, import, rename, visibility, delete), from people, agents and imports.
- **Readers:** every repository route, the sync and registry authorizers, listings, anonymous public
  reads, and the retention collector.

**Classification.** Split (S10). Name, visibility and lifecycle are what lake must know before it
can pick a workspace or authorize (a lookup by name, an anonymous read of a public repository, the
collector finding expired tombstones); they are routing and policy. The description is content.

**Target.** The row stays as the content-free routing row, as `auth_applications` did (lake#351).
`description` and any later repository settings (default branch, topics) move to a sealed
`lake_repositories` record in the repository's workspace, keyed by the repository id, written
`if_absent` at create and changed `if_revision`. Repository content (refs, history, objects) moves
with #380, and forge records already moved (lake#341).

**Migration.** The standard [workspace records](workspace-records.md#migration-and-cutover)
sequence: a migration records owners (dev: 9 assigned, 1 unassigned, S14), the release reads and
writes the record, `copy_workspace_records` copies and verifies, and a later migration drops the
`description` column.

**Done** in migration `0064_repository_records`: the record is keyed by the repository and found by
its space (`customer:<id>`) rather than by the repository, so a listing reads one `list_records` per
workspace. See [repositories.md](../api/repositories.md#where-the-description-lives-lake315-part-2)
and the [cutover runbook](workspace-records.md#cutover-runbook-repository-descriptions-dev-cluster).

### lake#319: `repository_refs`

**Today.** `(repository_id, name, head, updated_at)`. Lake writes it after a push, a replication
and a `refs` read (`set_repository_ref`), copying what liquid's repository reports. Nothing reads
it: the `refs` and `repository_refs` capabilities read the liquid repository. It never removes a
ref liquid deleted. Dev has 0 rows.

**Classification.** Workspace, but redundant: the liquid repository is the authority, and it moves
with #380.

**Target (S11).** Drop it. One migration drops the table; lake removes `set_repository_ref` and
`list_repository_refs` from the store interface and returns refs from liquid alone. No copy.

**Done** in migration `0064_repository_records`. `refs` answers `{repository_id, name, head}` from
liquid; `updated_at` is gone.

## Work, in dependency order

| # | Issue | Change | Waits on |
|---|---|---|---|
| 1 | [zetta#1005](https://github.com/oramasearch/zetta/issues/1005) | The object capability above, with put-if-absent, tests and a latency budget (the decided specification is its 2026-10-03 comment) | nothing |
| 2 | [liquid#326](https://github.com/oramasearch/liquid/issues/326) | Repository state (`LIQUID_REPO`, journal, checkpoint, replication cursor) through `store_backend` | nothing |
| 3 | [liquid#327](https://github.com/oramasearch/liquid/issues/327) | An injected owner lock with fencing for backends without a local root | nothing |
| 4 | [liquid#328](https://github.com/oramasearch/liquid/issues/328) | Copy a whole repository between two open stores (the copy step) | liquid#326 |
| 5 | [orama-operator#259](https://github.com/oramasearch/orama-operator/issues/259) | Grow each workspace Zetta's volume from `workspaces.storage_gi` | nothing |
| 6 | [orama-operator#260](https://github.com/oramasearch/orama-operator/issues/260) | Scheduled CSI snapshots of workspace volumes and the store volume (dev has the snapshot CRDs but no `VolumeSnapshotClass`) | nothing |
| 7 | [lake#513](https://github.com/oramasearch/lake/issues/513) | A workspace for the `liquid` repository's space, and its assignment (S14). The workspace is created on dev; migration `0066_assign_unrouted_repositories` assigns the repository | nothing |
| 8 | [lake#319](https://github.com/oramasearch/lake/issues/319) | Drop `repository_refs` (S11). **Done** (migration `0064`) | nothing |
| 9 | [lake#315](https://github.com/oramasearch/lake/issues/315) | Part 2: `description` to `lake_repositories` (S10). **Done** (migration `0064`); the copy of the one unassigned row waits | lake#513 for the one unassigned row |
| 10 | [lake#514](https://github.com/oramasearch/lake/issues/514) | Cascade workspace deletion (S12). Shipped before #380 and #383 (migration `0065`); the store and route steps are extension points | nothing; its store step follows #380 |
| 11 | [lake#384](https://github.com/oramasearch/lake/issues/384) | Route patterns in request logs; the per-workspace `lake_requests` series (S9). **Done** ([PR #521](https://github.com/oramasearch/lake/pull/521)) | nothing |
| 12 | [lake#382](https://github.com/oramasearch/lake/issues/382) | Stays with the instance (S7): the audit row, and the dynamic-secret leaf check. **Done** ([PR #519](https://github.com/oramasearch/lake/pull/519); no migration) | nothing |
| 13 | [lake#383](https://github.com/oramasearch/lake/issues/383) | Phase A: the route table, manifests and tags as records. **Done** (migration `0068`, [PR #523](https://github.com/oramasearch/lake/pull/523)). Phase B: blobs in the workspace store | A: nothing; B: lake#380 |
| 14 | [lake#380](https://github.com/oramasearch/lake/issues/380) | `ZettaObjectBackend`, per-workspace leases and lazy open, encryption, `move_workspace_store`. **Implemented** (migration `0067`); dev's switch waits on the snapshots | 1–7, [liquid#332](https://github.com/oramasearch/liquid/issues/332) (merged) |
| 15 | [lake#381](https://github.com/oramasearch/lake/issues/381) | Search beside the workspace store, rebuilt (S8) | lake#380 |

The umbrella rule's exceptions for S7 and S9 are already in the bridge `CLAUDE.md`.

## Changing this plan

When an item ships, mark its row in [workspace-data-plane.md](workspace-data-plane.md#storage-audit)
as moved (or as an instance exception for #382 and #384) and link its change, as the records moves
do. A change to a decision above is a new liquid-bridge decision, not an edit of this table.
