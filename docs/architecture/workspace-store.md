# Workspace Store

Refs [lake#380](https://github.com/oramasearch/lake/issues/380), liquid-bridge decision 54 (S1, S2,
S3 and its 2026-10-03 implementation note), [plan](workspace-storage-plan.md#lake380-the-liquid-store).

**Status: implemented in the source tree; not deployed; no workspace on dev is switched.** A switched
workspace's repositories (and with them its Drive, models and datasets) are served from one liquid
store in its workspace Zetta, on Zetta's object capability
([zetta#1005](https://github.com/oramasearch/zetta/issues/1005), zetta `docs/data/object-store.md`,
live on dev Zetta). It needs liquid at or after `8955519`, which writes each index run once
([liquid#332](https://github.com/oramasearch/liquid/issues/332)); an earlier liquid's index merge
rewrote a finished run in place, which Zetta refuses.

## What lives where

| Piece | Where |
|---|---|
| Packs, index runs, object map, leases, collector state, each repository's `LIQUID_REPO`, journal, sessions and replication cursors | Bucket `lake_store` of the workspace Zetta, one object per liquid key, encrypted |
| Tenant image blobs (lake#383 phase B) | Objects of the same liquid store, not in any repository; each found through its `lake_oci_blobs` record (SHA-256 digest to liquid digest), [registry.md](../api/registry.md#where-a-tenant-image-lives) |
| The workspace's data key, wrapped | Object `keys/workspace-v1.wrapped` of the same bucket |
| The workspace's search index (lake#381): manifest, write-once blob runs, segments, artifact members | Objects `search/<name>` of the same bucket, sealed with the workspace's data key and fenced by the store's lease through search's own client calls, never by liquid ([search.md](../api/search.md#where-the-index-lives-lake381-decision-54-s8)) |
| Whether the workspace is switched | `workspaces.store_moved_at` (migration `0067`), content-free |
| The space's store from before the switch | `LAKE_STORE_ROOT/<space>/store` and `<space>/repositories`, unchanged and read-only from the switch on, until the operator's `remove_space_store` removes both once every workspace of the space is switched ([below](#removing-the-spaces-store)) |

A switch creates the workspace's store (bucket and data key) even when the workspace has no
repository, since its tenant images live there (lake#383 phase B). A workspace an earlier lake
switched without one is repaired by `move_workspace_store` mode `provision`, which creates the store
only for a workspace that never held anything (no repository row but a refused creation's, no
stored image blob, an absent or empty bucket) and otherwise refuses with `conflict`.

A repository's store is chosen by its `workspace_id`: its workspace's store once `store_moved_at` is
set, the space's store before. The choice is cached per repository and cleared by the switch.

The repository's liquid identifier stays `<LAKE_STORE_ROOT>/<space>/repositories/<repository>` in the
workspace store too. It is a name, not a path (nothing is written there): liquid derives the state
keys `repositories/<blake3 of the identifier>/…` from it, and liquid#328's transfer copies those keys
under the same name. `LAKE_STORE_ROOT` must therefore not change once a workspace is switched.

## Encryption below liquid (S3)

Liquid writes plaintext through `ZettaObjectBackend` (`include/lake/control/workspace_store.hpp`),
which encrypts every byte it sends. Liquid's object identities, deduplication and chunk deltas are
unchanged, so a repository moves between a space store and a workspace store byte for byte
(liquid#328) without re-encryption.

**Keys.** Each workspace has one random 32-byte data key. It is wrapped with AES-256-GCM under
`HKDF-SHA256(LAKE_INSTANCE_KEK, "lake/workspace-store/v1/<workspace id>")`, with associated data
`lake/workspace-store/v1/<workspace id>/data-key/<version>`, and stored as a JSON document
(`format: lake-workspace-store-key/1`, `workspace_id`, `version`, `wrapped`) at
`keys/workspace-v1.wrapped`. The first open of a workspace store creates it with a conditional put.
A document naming another workspace, or one that does not unwrap under this lake's KEK, is refused
`unauthorized`. Liquid writes `keys/` only for a keyring, which a workspace store never has; the
backend refuses liquid keys under `keys/` and keys containing `.lake-tail-`.

**Object layout.** Liquid key `K` is the object `K`:

```text
header (48 bytes)  "LKSTENC1" | format u32 = 1 | key version u32 | block size u32 | 0 u32
                   | base length u64 | salt (16 random bytes)
base blocks        the bytes K was created with (by put), in blocks of `block size` (64 KiB);
                   the last may be short
appended blocks    each later complete block of appended bytes, `block size` each
```

Each block is `nonce (12) || ciphertext || tag (16)`, AES-256-GCM with a random nonce, so a block
of 64 KiB stores 65,564 bytes (0.04 % overhead). Its associated data binds the workspace id, `K`, the
whole header (so the salt and the base length), the region (`b` base, `a` appended, `t` tail), the
block's index and its length. A block altered, moved within the object, moved to another key, or
read under another workspace's key fails authentication (`corrupt`).

**Appending without rewriting.** Zetta objects only grow. A partial block of appended bytes cannot be
completed in place, so at each barrier it is written whole to its own object
`K.lake-tail-<block>-<salt prefix>` (an atomic replacing put), and the next barrier appends the
completed block to `K` and replaces the tail. A tail whose block number is below `K`'s count of
appended blocks, or whose salt is not `K`'s, is stale: readers ignore it and the backend removes it
after the next barrier. Every offset is therefore computable from the header and `K`'s size alone:
plaintext size = base length + appended blocks × block size + the current tail's length.

**What it does not hide or prove.** Object names (`packs/pack-3.pack`, `repositories/<hash>/…`),
sizes to the block, and timing are visible to the workspace Zetta. A hostile or buggy workspace
Zetta can also, without the cipher noticing:

- roll a whole object back to an older version (its old header and blocks are consistent with
  each other);
- roll a tail back to an earlier, shorter version of the same block;
- delete a tail, or trailing whole blocks of an object;
- bring back an object lake removed.

Liquid notices some of these (its pack scan stops at a torn tail, and every chunk is checked against
its digest), but an older `packs/objects.meta`, `index/MANIFEST` or `LIQUID_REPO` reads as an older,
valid store. Detecting them needs a manifest anchored outside the workspace Zetta, a follow-up:
[lake#522](https://github.com/oramasearch/lake/issues/522). Truncation inside a block is detected
(`corrupt`).

A random 96-bit nonce per block keeps the collision bound under 2⁻³² for up to 2³² blocks (256 TiB of
writes) per data key; the header's key version leaves room for rotation, which is not built.

**Every workspace store depends on `LAKE_INSTANCE_KEK`.** Each data key is wrapped under a key
derived from it, and nothing else can unwrap it: losing the KEK (the Secret `lake-prod-instance-kek`)
loses every workspace store. The KEK has no rotation and no backup outside that Secret today.

**A key is created only for a new store.** A missing data key is created only while lake creates a
workspace's store (`move_workspace_store` `copy` or `switch`, before the workspace is switched to
it), and only in a bucket that holds no object. A switched workspace whose bucket is gone, whose key
is gone, or whose bucket holds objects but no key (a wiped or recreated Zetta, a wrong endpoint)
answers `corrupt` and is never served as an empty store. A repository missing from a switched
workspace's store answers `corrupt` too; repositories in a workspace store are created only
explicitly: by the switch (for one the space store never held) and by repository creation, which
runs under the workspace's lane (`SyncStore::create_repository_content`), never by a read or a push.
When the store refuses the creation, the new row is tombstoned with its retention ended, which
frees its name, and the caller is told to retry. If that tombstone fails too, the error says so,
and the next creation of the same name in the workspace tombstones the row whose repository is
missing from the workspace store before it creates the new one. A
repository the space store lost (lake opened it there, so its directory exists beside the space
store, but its entry point is gone) is refused by the move as `corrupt`, never created empty.

## Writes

Since lake#527 the backend holds a barrier interval's writes and sends them when liquid calls
`barrier()`: together, and each with `?durable=true` (Zetta fsyncs it before it answers), so a
barrier costs one round trip and no `:barrier` request in the usual case.

- `put` of at most 1 MiB is held: reads are answered from memory, and the bucket keeps the previous
  version until the barrier sends the new one as one PUT, which Zetta commits atomically. A larger
  put is sent at once, without durability, as one PUT.
- `put_if_absent` (`?if_absent=true`, whose `409` is `already_exists`) is sent at once and durably:
  its answer decides the create. When liquid removed the key since the last barrier, that removal
  is sent first.
- `put_range` appends to a buffer. Complete blocks are sent once at least 1 MiB is held, without
  durability, and the rest at the barrier, as PATCHes at the object's exact size of at most one
  Zetta slice (4 MiB), each applied by Zetta as one frame, so an object never ends inside a block.
  An object that is not in Zetta yet (created by a held put or by appends since the last barrier)
  is sent at the barrier as one PUT of its header and base blocks, so it needs no tail.
- `remove` is held, and sent durably at the barrier; the object's tails go after that barrier.
- A put of the bytes the bucket already holds durably for that key (written whole by this view,
  at most 16 KiB, nothing appended since) sends nothing: liquid publishes its index manifest twice
  in a row, with barriers between, and the second is the same bytes.
- `barrier()` sends, all at once (at most 16 in flight): each held put and removal, each object's
  complete blocks and tail, all durably; Zetta's `:barrier` beside them only when a write was
  acknowledged without durability since the last barrier (a large put or a threshold flush); and
  the removal, without durability, of the tails made stale before this barrier. It answers when
  all of them have. A barrier with nothing held costs no request.
- **Why the guarantees hold.** Everything acknowledged before a barrier is durable when it answers:
  each write it sends is fsynced before Zetta answers it, and a `:barrier` covers the writes
  acknowledged without durability before it. The writes one barrier sends belong to one interval,
  between two of liquid's barriers, where liquid assumes no order; a crash part way keeps any
  subset of them (Zetta keeps each key's writes in order) and everything the previous barrier
  acknowledged. A tail is removed only after the barrier that made its replacement durable (so a
  crash never leaves an object shorter than acknowledged); a tail removal brought back by a crash
  leaves a stale tail, which readers ignore. A stale tail the same barrier writes again is not
  removed.
- A range that overlaps written bytes must repeat them (a retry, which changes nothing). Different
  bytes are refused `conflict`, as Zetta refuses them (zetta `docs/data/object-store.md`); liquid
  since liquid#332 never writes so, and the store is reopened before its next use.
- Every mutation carries the lease's fence (`Zetta-Fence`).
- One flush per object at a time: a buffered-append flush and a barrier on the same object wait for
  each other rather than send its blocks twice.
- **Closing.** The owner sends what the store's backend holds before it releases the lease
  (`ZettaObjectClient::before_release`, set by the backend; liquid's close releases without a last
  barrier), so a store closed cleanly leaves nothing unsent.
- A conditional create that Zetta refuses while the directory said the key was absent (another
  writer, which the fence should exclude) marks the backend failed and keeps the directory and
  buffered appends as they are.
- A failed mutation marks the backend failed: liquid may already have advanced its memory of the
  store (an index flush clears its memtable before the barrier that fails), and liquid's contract is
  to recover on the next open. Lake closes such a store and opens it again before the next request
  uses it.

## Reads

The backend keeps a directory of the bucket from one paged listing, maintained by its own writes;
that is exact because the fence makes it the only writer, and `list` and `exists` cost no request.
Headers are read with the first window of each object, on first use; a tail that is not the
object's current one is then removed after the next barrier. Ranges are read through a
bounded per-store cache (64 MiB) of aligned 4 MiB ciphertext windows keyed by object, salt and
window: a version's bytes never change (appends only add complete blocks; a replacement has a new
salt), so nothing is invalidated except on `put` and `remove`. The bytes each put (up to 16 MiB)
and append sends seed the cache, so reading back what was just written (an index run liquid loads
right after writing it) costs no request. Bytes not yet sent are read from the buffer. Liquid loads index runs into memory at open, so a chunk lookup reads nothing.

## Ownership (lake#479, per workspace)

`ZettaStoreOwner` is liquid's `store_owner` (liquid#327) over the bucket's lease:

- `acquire` calls `:acquire {holder, ttl_ms, wait_ms}` in pieces of at most 25 s until the open's
  `owner_wait` (`LAKE_STORE_OWNER_WAIT_SECONDS`) passes, then refuses `locked` naming the holder. The
  holder is liquid's process description plus a random token, so a second open in one process waits
  like any other.
- A thread renews every third of the TTL (`LAKE_WORKSPACE_STORE_LEASE_SECONDS`, default 30). Each
  renewal is one request that gives up after a third of the TTL, and the lease is counted from when
  the renewal was sent. A renewal refused as fenced, or none accepted before the lease would run
  out, revokes the grant.
  Zetta refusing any write as `fenced` revokes it at once (liquid's wrapper).
- A lost lease closes the store; that workspace's requests answer `503 store_unavailable` while
  another process holds the lease, and the next request after it is free owns the store again with a
  higher fence. Other workspaces are not affected.
- `release` first sends what the backend holds (above), then stops renewing and calls `:release`;
  lake does it when it drains (`ApiServer::stop`), when a workspace is deleted, and when it reopens
  a failed store.

A workspace store opens on the first request that needs it, never at start.

**Isolation.** Each store has its own lane (`s:<space>` for a space's store, `w:<workspace>` for a
workspace's), and a request holds only its store's lane for its call; the lock that guards lake's
maps of open stores is never held across store I/O, a network call or a wait.

- **Off the server thread.** Synchronous capabilities that wait on a repository store (repository,
  Drive, model and dataset capabilities, search, `replicate_repository`, `move_workspace_store`,
  `delete_workspace`) run on a pool of 32 threads instead of lake's single server thread
  (`Registry::call_async`; the `/search` page too), and asynchronous handlers run each repository-store call there too
  (`run_blocking`, `blocking_call`: push and its artifact reindex, repository creation, export,
  import, GitHub import, `get_model`/`get_dataset` with a revision, `metering_usage`). Pages that
  call those capabilities synchronously still run on the server thread.
- **Admission.** A request enters a lane only while fewer than four requests are in it, holding or
  waiting, and never while the lane is being moved (a copy or verify marks the workspace's lane; a
  switch marks the workspace's and its space's). Otherwise it
  is answered `503 store_unavailable` with `Retry-After: 1` at once, so requests for one slow or
  moving store never take more than four of the pool's threads from other stores and spaces.
- **No cross-store waits on writes.** A write's quota check counts each switched workspace's
  usage as last read (every 10 s, `refresh_workspace_usage`, on the maintenance thread), and a write
  in a switched workspace counts the space store's last measurement when another request holds
  that lane; no write waits on another store's lane or Zetta. Both are seeded at start (the space
  store's size when lake opens it, every switched workspace's usage by a priming pass, and a
  workspace's usage again at the first open of its store), so a restart never counts them as 0.
- **Search** reads repositories only through the repository store, under their store's lane and on
  its one open instance (`search::Service::set_repository_access`, `LiquidCustomerStores::lend_repository`);
  it never opens a second instance of a store, whose open would remove another writer's temporary
  files. Once a repository store has lent repositories, search refuses rather than open a store
  itself. Search borrows a repository before it takes its own lock, so a search waiting on one
  store's lane holds up no search in another space.
- **A switched workspace's search index** (lake#381) is kept beside its store as `search/` objects in
  the same bucket. The repository store lends it with a repository, or alone
  (`LiquidCustomerStores::lend_workspace`), under the workspace's lane: objects sealed with the
  store's data key (`WorkspaceStore::search_cipher`, unwrapped once per open store) and written with
  the store's client and lease fence (`search::make_sealed_index_objects`), so publish, reindex and
  history scans write it under the lane and the lease. A failed index write does not mark the store
  failed; Zetta refusing one as `fenced` marks it unusable (`WorkspaceStore::search_fenced`), as a
  lost lease does. Index bytes count in the bucket's quota, and search writes only within the room
  left after a tenth of the quota kept for liquid (`search_index_budget`). Search's own map lock is
  never held across a lane wait or I/O. It is loaded on first use, evicted when idle under
  `LAKE_SEARCH_MEMORY_BYTES`, and rebuilt on the maintenance thread when missing or unreadable,
  reading one repository at a time under the lane and writing without it
  ([search.md](../api/search.md#where-the-index-lives-lake381-decision-54-s8)).
- **Zetta's HTTP client** (`install_native_http_transport`) is safe from many threads at once: one
  dedicated thread runs every exchange, callers post to it and wait on an event, and its shared TLS
  context cache is locked.

**Search index worker.** Search index rebuilds and their retries (lake#381) run on a thread of their
own, never on the maintenance thread, so the usage refresh and the deletion pass never wait on one.

**Maintenance thread.** Work that waits on workspace Zettas or a store's lane runs on its own thread,
never on the owner loop or the server thread: the usage refresh, the start-up usage priming, and
the deletion pass (`resume_workspace_deletions`, which closes workspace stores). The owner loop's
audit, metering and request-series flushes and timestamp renewals are therefore not delayed by a
Zetta timeout or a switch. A job is skipped when one of its name is already queued.

**Shutdown.** `ApiServer::stop` drains requests, stops the maintenance thread, closes every store,
then gives repository-store work still running on the blocking pool the rest of the drain deadline.
After that it closes its io_context to the pool (`close_execution_context`): a task still running is
abandoned and posts nothing into the destroyed context. The request that awaited an abandoned task
is never resumed, so its response and its audit event are lost; the count is logged
(`abandoned_store_tasks`) and counted (`lake_abandoned_store_tasks_total`).

**Clients and 503.** liquid's sync client does not retry a 503, nor read `Retry-After`:
[liquid#340](https://github.com/oramasearch/liquid/issues/340). Until it does, a `liquid push` or
`pull` refused during a move or a lease hand-off must be rerun.

`.lake-owner.lock` still covers the volume's instance data and the space stores.

## Moving a workspace

`copy` and `verify` mark only the workspace's lane as moving and take the space's lane one
repository at a time, so the space's other workspaces wait at most one repository's copy and are
never refused.

The switch marks the workspace's lane and its space's lane as being moved, then holds both for its
whole run and never gives them up: requests for the workspace's repositories (still served by the
space store), every other request for the space store, and repository creation in the workspace are
answered `503 store_unavailable` with `Retry-After` until it is recorded, and a request already
admitted before the switch waits for it. It lists the
workspace's repositories under that hold, copies and verifies each, creates in the workspace store
each one the space store never held (decided from the space store's own entry point, never from a
`not_found` of the workspace store), records `store_moved_at`, and then invalidates every route of
the space, so a request that chose the space store before the switch chooses again. A route is
never answered from the space store when the repository or its workspace cannot be read, nor for a
deleted workspace.

`move_workspace_store` ([docs/api/workspace-store.md](../api/workspace-store.md)) plans, copies,
verifies and switches one workspace at a time with liquid#328's `repository::transfer`. A copy writes
nothing to the space store; a rerun copies only what is missing; an interrupted copy leaves no
`LIQUID_REPO` in the workspace store, and the store is reopened before the rerun, as liquid's
contract asks.

## Metering, quota and deletion

- Storage for a space is its space store (measured from its packs as before) while any of its
  workspaces is not switched, plus each switched workspace's bucket `usage`, which counts stored
  ciphertext, as the owner loop last read it (every 10 s; a workspace not running, or not
  answering within 5 s, keeps its last value), so no write waits on another workspace's Zetta. During a move the moved repositories are counted in both until every workspace of the
  space is switched.
- The bucket is created with the space's storage quota (`create_bucket {quota_bytes}`); Zetta answers
  a write past it `507`, which liquid sees as `busy`. An existing bucket keeps its quota. The quota
  does not yet follow `workspaces.storage_gi` (migration `0069`, the desired size of the workspace
  Zetta's volume); raising `storage_gi` ahead of use is a follow-up.
- Tenant image blobs are objects of the store, so they count in its bucket usage like any other
  write, and an upload checks the space's quota before it writes (`507` past it). Metering reports
  them as `images_in_workspace_stores_bytes`, not again in `images_bytes`.
- Lake runs no liquid collector on workspace stores, but one would keep image blobs: each is
  rooted by a liquid lease for a century when it is stored (a durable pin is
  [liquid#342](https://github.com/oramasearch/liquid/issues/342)).
- Deleting a workspace removes its search index objects (when its store is open) and then closes its
  store and releases its lease before the deletion is reported
  (`WorkspaceContent::close_workspace_store`), waiting for an open in progress on the workspace's
  lane; a deleted workspace's store is never opened again. The workspace Zetta's deletion destroys
  the bytes.

## Latency

`WorkspaceStoreZetta.TheBackendOnARealZettaHoldsOnlyCiphertextAndItsLatencyIsMeasured`, 2026-10-03,
Apple M3 Max, one process: a `zetta` built from Zetta `f6a82675` (its object code is identical to
Zetta dev `b82a5e94`, which carries #1234) on loopback HTTP, beside a disk store on APFS in the
same run, both with liquid `5fa3b50` plus liquid#332 (PR #334, since merged into liquid main
`8955519`). Each row is the same work on both stores.

| Case | Disk | Zetta |
|---|---:|---:|
| 64 × 1 MiB objects put | 787 ms | 870 ms |
| 64 × 1 MiB objects read | 273 ms | 264 ms |
| 100 MiB object put (a Drive upload) | 856 ms | 571 ms |
| 100 MiB object read (a Drive download) | 375 ms | 415 ms |
| 200 × 4 KiB objects, each put on its own | 786 ms | 1,778 ms |
| transfer of a 20-change repository (liquid#328) | 231 ms | 111 ms |

5,240 Zetta requests at a mean of 0.46 ms; the window cache answered 8,991 reads and missed 83.
Bulk transfer is at disk speed. Small writes cost about 5 ms more each: every liquid publication
stages and publishes its metadata with barriers between, and each barrier with new writes is a
Zetta `:barrier` (an fsync), each put and remove a request (per small put about 9 barriers, 10 puts
and 5 removes). Seeding the window cache with the bytes each put and append sends removed the
read-backs (781 reads down to 185). The numbers for the dev repositories through a deployed lake
belong in the deployment record once a workspace is switched.

The same test after lake#527 (2026-10-06, the same machine, the same Zetta binary, liquid
`8955519`): 64 × 1 MiB put 638 ms, read 273 ms; 100 MiB put 645 ms, read 454 ms; 200 × 4 KiB puts
1,164 ms (1,778 before); transfer 111 ms. 2,482 requests (5,240 before): 1,414 puts, 182 appends,
150 gets, 4 lists, 659 removes, 66 `:barrier`s.

## Requests and latency (lake#527)

On dev, a one-file `commit_repository_files` into a switched workspace took 4.5 to 5.8 s while a
read took 3 ms. `WorkspaceStoreZetta.AOneFileCommitsRequestsAreCountedAndTimed` makes the same
commit (remove the path, write it, cut, as `SyncStore::commit_repository_files` does) through a
liquid repository on the backend, against a real Zetta behind a loopback proxy that adds a round
trip (`LAKE_TEST_ZETTA_RTT_MS`, default 10 ms; the cluster's round trip to a workspace Zetta is
assumed to be 5 to 10 ms, not measured). It counts the requests of each kind one commit sends and
prints them in order. Apple M3 Max, Zetta `f6a82675`, liquid `8955519`; the fourth commit (the
store and repository open, directory and headers known):

| | Requests | PUT | PATCH | `:barrier` | DELETE | GET / list | Sequential rounds | 0 ms RTT | 5 ms | 10 ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Before | 180 | 77 | 0 | 68 | 35 | 0 | 180 | 169 ms | not measured | 2,537 ms |
| After | 80 | 55 | 0 | 0 | 25 | 0 | 57 | 75 ms | 505 ms | 835 ms |

Before, every request was its own round trip. Per liquid object a commit stores (about eight for
one file: the blob, its directory documents, the change, the journal), liquid wrote its pack tail,
staged and published the object map, wrote an index run and published the index manifest twice,
with a barrier after each step: about fifteen requests, six of them `:barrier`. The lake commit
path adds no store request of its own:
`WorkspaceStoreMove.ACommitNamesItsStoreRequestsInServerTimingAndMetrics` checks that the
`Server-Timing` count of a commit through a running lake equals what the workspace Zetta received
(the search reindex, metering and audit do not reach the store; the store is not reopened).

What changed: the writes of a barrier interval are held and sent together and durably (no
`:barrier`, one round trip per barrier); an object created in one interval is one PUT with no
tail (an index run was a PUT, a tail PUT and later two DELETEs); the repeated identical manifest is
not sent; and stale tails are removed beside the next barrier's writes instead of in a round of
their own after each barrier.

**The floor is liquid's barrier count.** A barrier must not answer before what it acknowledges is
durable, and the next interval's writes must not be sent before it answers, so each of liquid's
barriers with new writes is one round trip: 57 for a one-file commit, which at a 10 ms round trip
is about 570 ms plus Zetta's fsync per round (about 1.3 ms here), measured 835 ms through the
proxy. Under 500 ms at 10 ms needs fewer barriers from liquid, not a cheaper backend: an object
batch around a repository operation's object writes (liquid already has
`store::begin_object_batch`, used by its sync import, but its writes publish repository state that
names the objects before the batch would be committed), one manifest publication per index flush,
or an index flush per operation instead of per object. That is liquid's to change.

The process metrics `lake_workspace_store_requests_total{kind,outcome}` and
`lake_workspace_store_request_duration_seconds{kind}` and each response's `store` Server-Timing
entries give the same counts on dev; a deployed measurement belongs in the deployment record.

## Image blobs (lake#383 phase B)

`put_workspace_blob`, `admit_workspace_stream`, `workspace_blob_present` and `read_workspace_blob`
(`SyncStore`) are the registry's only way into a workspace store. A put streams a claimed upload
into the store with liquid's `put_file` under the workspace's lane, admitted for a push (refused at
once when the lane is full or moving) and held for the copy; it holds the lane, and liquid's
mutation lock, for the whole put, so the workspace's other requests wait for a large layer, and it
roots the new object with a lease (once per object its record names). A pull takes one of the
workspace's eight stream slots for its whole stream, apart from the lane's four admissions, so
pulls never refuse the workspace's pushes, git or repository requests; it does not hold the lane,
and each window (1 MiB) takes the lane only for its read. At most 24 pulls run in the process,
below the server's 32 workers, and a client that takes no bytes for 60 s loses its stream. Nothing here changes `ZettaObjectBackend`.

## Not yet

- The copy's precondition of fresh snapshots of the workspace volume and of lake's store volume
  ([orama-operator#260](https://github.com/oramasearch/orama-operator/issues/260)) is not enforced.
  Migration `0069` added the snapshot desired state (`volume_snapshot_policies`) and the operator's
  `volume_snapshot_status`, but `move_workspace_store` does not request a snapshot or wait for one
  to be ready yet. The move never writes the space store.
- Data-key rotation; quota resize of an existing bucket.

## Removing the space's store

Once every workspace of a space is switched, nothing is served from the space's store, and the
operator removes it with `remove_space_store` ([API](../api/workspace-store.md#removing-the-spaces-store)).
It is refused (`conflict`) while a live workspace of the space has no `store_moved_at`, a
repository of the space has no workspace, or a live repository belongs to a deleted workspace.
Under the space's lane, marked as moving so new space-store requests answer 503 at once and the
admitted ones finish first, it checks that again, closes the space's liquid store and forgets its
cached repositories and routes, then deletes `<LAKE_STORE_ROOT>/<space>/store` and
`<LAKE_STORE_ROOT>/<space>/repositories` and nothing else: the instance's directory
(`<LAKE_STORE_ROOT>/<node name>`, the registry and the log), `<LAKE_STORE_ROOT>/workspaces`, other
spaces and the space's search index (`<space>/index`, lake#381) stay. The space id must be the UUID
of an existing space; a symbolic link in place of the space's directory or either of the two is
refused. A dry run measures the two directories (regular files only, skipping entries that change
while it counts) and changes nothing.

Switched workspaces keep answering, since a repository identifier under `<space>/repositories` is a
name in the workspace store, not a path. A workspace created in the space afterwards is not switched,
so its repositories open the space's store on first use, and liquid creates it empty, as it does for
a new space; the removal is then refused again until that workspace is moved and switched.

## Tests

`tests/test_workspace_store.cpp` runs against `FakeZettaObjects` (`tests/zetta_object_fixture.hpp`),
Zetta's object routes as specified, including the refusal of in-place rewrites; the move tests run
a real lake server. `WorkspaceStoreZetta.*` run against a real Zetta binary when
`LAKE_TEST_ZETTA_BINARY` names one and print the latency table below.

| Test | Covers |
|---|---|
| `WorkspaceStoreBackend.RangesRoundTripAcrossBlocksTailsAndReopen` | ranges across blocks, buffered reads, tails, one current tail, a fresh backend reading and appending, put then append, ranges outside, conditional create, remove, reserved keys, no plaintext |
| `WorkspaceStoreBackend.ARewriteWithOtherBytesIsRefused` | a retried range changes nothing; a rewrite and a skip are `conflict` |
| `WorkspaceStoreBackend.TamperingMovedBlocksAndAnotherWorkspacesKeyAreRefused` | a flipped bit, swapped blocks, an object under another key, another workspace's bucket and key document, another KEK, no KEK |
| `WorkspaceStoreBackend.ALiquidStoreHoldsOnlyCiphertextAndReopens` | a liquid store on the backend: no plaintext in any object or request, reopen, collector dry run, nothing written locally |
| `WorkspaceStoreBackend.AStoreThatMergesIndexRunsWritesThemOnce` | a store that merges index runs works on the workspace store (liquid#332) |
| `WorkspaceStoreBackend.ATransientFailureIsRetriedAndAPersistentOneIsUnavailable` | retries, then `unavailable` |
| `WorkspaceStoreOwner.ASuccessorWaitsForTheHolderAndTakesOverWithAHigherFence` | refusal naming the holder, a bounded wait, the hand-off, a higher fence |
| `WorkspaceStoreOwner.ALostLeaseRefusesWritesAndTheSuccessorKeepsEveryAcknowledgedObject` | renewal finds the takeover, writes refused, the successor reads everything; a fenced write revokes at once |
| `WorkspaceStoreMove.PlanCopyInterruptedVerifySwitchServeFromTheWorkspaceAndKeepWorkspacesApart` | plan, an interrupted copy, rerun, verify, an idempotent copy, switch, reads and writes after it, the space store untouched, two workspaces of one space in separate buckets with separate keys |
| `WorkspaceStoreMove.ALostLeaseAnswers503UntilReacquiredAndDeletionReleasesTheLease` | `503 store_unavailable` for the workspace only, reacquisition with a higher fence, deletion releasing the lease |
| `WorkspaceStoreBackend.AMissingKeyIsCreatedOnlyForANewStoreInAnEmptyBucket` | no key without `create_key`; never over objects |
| `WorkspaceStoreBackend.ARemoteConflictOnAConditionalCreateFailsTheBackendAndKeepsItsBuffer` | a refused conditional create fails the backend and loses no buffered append |
| `WorkspaceStoreBackend.TailRollbackDeletionTruncationAndACrashBeforeTheTailAreAsDocumented` | tail rollback and deletion read as the shorter object, truncation inside a block is `corrupt`, a crash between the PATCH and the tail keeps what the last barrier acknowledged |
| `WorkspaceStoreBackend.TailsThatAreNotCurrentAreRemovedAfterOpen` | stale tails removed |
| `WorkspaceStoreBackend.AThresholdFlushAndABarrierOnOneKeyDoNotSendItsBlocksTwice` | the flush guard |
| `WorkspaceStoreOwner.ARenewalThatTimesOutLosesTheLeaseInTime` | renewal timeout under the TTL; the grant is lost before the lease runs out |
| `WorkspaceStoreMove.AWipedOrIncompleteWorkspaceStoreIsRefusedNeverServedEmpty` | a missing repository and a wiped bucket answer `corrupt`; nothing is created |
| `WorkspaceStoreMove.ADestinationThatLostObjectsNeverVerifiesOrSwitches` | a destination missing packs never verifies complete |
| `WorkspaceStoreMove.OtherWorkspacesAndSpacesProceedWhileOneSwitchesOrItsZettaHangs` | during a slow switch, forty requests and a creation for the moving workspace are refused at once while another workspace (a push included) and another space answer; with a hung Zetta at most four requests wait on its lane; after the switch a creation and a push land in the workspace store |
| `WorkspaceStoreMove.AWriteDuringTheSwitchIsRefusedAndRetriedIntoTheWorkspaceStore` | a write to a repository the switch already copied is refused without waiting, then retried into the workspace store |
| `WorkspaceStoreMove.ARepositoryTheSpaceStoreLostIsRefusedNotCreatedEmpty` | a space directory without its entry point refuses the copy and the switch |
| `WorkspaceStoreMove.SearchAndReindexRunBesideWritesToTheSameSpaceStore` | search and `reindex_artifacts` beside writes to one space store; the store reopens whole |
| `WorkspaceStoreMove.APushLandsInTheWorkspaceStore` | a liquid push into a switched workspace |
| `WorkspaceStoreMove.ACreationTheWorkspaceStoreRefusesLeavesNoRowAndCanBeRetried` | the row rollback and a retry under the same name |
| `WorkspaceStoreMove.AStoppedSwitchedWorkspaceDoesNotRefuseItsSpacesWrites` | a stopped switched workspace does not block its space's writes |
| `WorkspaceStoreMove.ADeletedWorkspaceIsNeverReopenedAndADeleteWaitsForAnOpen` | a delete racing an open releases the lease; a deleted workspace is not reopened or served from the space store |
| `WorkspaceStoreMove.ACopyDoesNotRefuseTheSpacesOtherWorkspaces` | a copy marks only the workspace's lane |
| `WorkspaceStoreMove.ACopyIntoADroppedBucketFailsInsteadOfReportingAbsent` | a dropped bucket fails the copy instead of answering `absent` |
| `WorkspaceStoreSearch.TwoWorkspacesOfASpaceKeepSealedIndexesBesideTheirStoresAndARestartReloadsThem` | the switch drops the space index's copy; two workspaces' indexes apart, sealed in their buckets, nothing on lake's volume; a restart reloads without rewriting |
| `WorkspaceStoreSearch.AWipedOrTamperedWorkspaceIndexIsRebuiltNotServedEmpty` | a wiped index, lost runs and a tampered run are rebuilt, with and without a restart |
| `WorkspaceStoreSearch.PushesAndSearchesInOneWorkspaceRunSideBySide` | pushes, commits, reindexes and searches in one workspace at once, 503s bounded; every push indexed; a restart reloads whole |
| `WorkspaceStoreSearch.ASearchNeverOpensAWorkspaceItsCallerCannotRead` | after a restart, an owner and a grantee of one space: their space answers, not one request reaches the other space's workspace Zetta, and another space's workspace and an unknown one get one answer |
| `WorkspaceStoreSearch.TheIndexBudgetCountsTheWholeSpaceAndWhatItWroteSinceTheLastReading` | the index's room is the space's, less what it wrote since the last usage reading |
| `WorkspaceStoreSearch.AUsageRefreshDoesNotWaitForARebuild` | the usage refresh runs while a slow rebuild does |
| `WorkspaceStoreSearch.AHubUploadDoesNotHoldTheServerThread` | a Hugging Face upload's commit and index update run off the server thread |
| `WorkspaceStoreSearch.AWriteOnceIndexPutRetriedWithTheSameContentSucceeds` | a retried write-once index put with the same content succeeds; other content is refused |
| `WorkspaceStoreSearch.AFailedSearchWriteDoesNotFailTheWorkspaceStore` | an unanswered index write leaves the store's lease and holder as they were |
| `WorkspaceStoreSearch.TheFirstPushAfterASwitchDoesNotWaitForTheIndex` | a slow rebuild holds up neither the first push nor a search |
| `WorkspaceStoreSearch.DeletingAWorkspaceRemovesItsIndexObjectsAndACleanupFailureDoesNotHoldItUp` | the deletion removes the `search/` objects, and completes when their removal fails |
| `WorkspaceStoreMove.ARowLeftWithoutItsRepositoryIsCleanedUpByTheNextCreate` | an orphaned row is tombstoned by the next creation of its name |
| `WorkspaceStoreMove.ARestartCountsSwitchedWorkspacesUsageBeforeAnyWrite` | usage primed at start |
| `WorkspaceStoreMove.ASearchWaitingOnOneSpacesStoreDoesNotHoldSearchInAnother` | search borrows before its lock |
| `WorkspaceStoreSearch.OnceTheRepositoryStoreLentRepositoriesSearchNeverOpensAStoreItself` | the fallback refuses once a repository store was set |
| `WorkspaceStoreRemoval.RemovesOnlyTheSpacesStoreOnceEveryWorkspaceIsSwitched` | `remove_space_store`: invalid and unknown spaces refused; refused (dry run too) while a workspace is not switched; a dry run's paths and bytes with nothing removed; the removal of exactly the two directories; the instance's directory and another space untouched; switched workspaces read and written afterwards and after a restart; a repeated removal; central audit; a later unswitched workspace served from a new space store and refusing the removal until it is switched |
| `WorkspaceStoreRemoval.ALinkedSpaceOrStoreDirectoryIsRefused` | a symbolic link at `<space>/store` and at `<space>` itself is refused with `conflict` (dry run too) and nothing behind it is touched; put back, the removal proceeds |
| `WorkspaceStoreRemoval.ALiveRepositoryOfADeletedWorkspaceOrAPersonIsRefused` | a live repository of a deleted workspace refuses the removal (dry run too); no bearer and a person are refused |
| `WorkspaceStoreRemoval.AnUnassignedRepositoryIsRefused` | on PostgreSQL, a repository without a workspace refuses the removal although the space's only workspace is switched |
| `WorkspaceStoreZetta.TheBackendOnARealZettaHoldsOnlyCiphertextAndItsLatencyIsMeasured` | the same on a real Zetta, and the latency table |
| `WorkspaceStoreZetta.TheSearchIndexOnARealZettaIsSealedReloadsAndIsRebuilt` | a workspace's search index on a real Zetta: sealed, reloaded without rewriting, rebuilt once wiped |
| `WorkspaceStoreZetta.AOneFileCommitsRequestsAreCountedAndTimed` | lake#527: one-file commits on a real Zetta behind a round trip, requests by kind, rounds, the request trace |
| `WorkspaceStoreBackend.ACrashAnywhereKeepsWhatTheLastBarrierAcknowledged` | lake#527: 200 random sequences of held puts, immediate puts, appends past the threshold, conditional creates, removes and repeated bytes, ending in a barrier that is answered, stopped part way or loses answers at random, then a crash keeping any prefix of each key's writes: every object is what the last answered barrier acknowledged or a later state of it |
| `WorkspaceStoreBackend.ACrashMidCommitKeepsEveryAcknowledgedCommit` | lake#527: liquid one-file commits, one interrupted, a crash, a successor's reopen: every acknowledged commit reads back |
| `WorkspaceStoreMove.ACommitNamesItsStoreRequestsInServerTimingAndMetrics` | lake#527: a commit through lake names its store requests in `Server-Timing`, equal to what the workspace Zetta received, and the metrics count them by kind without a workspace label |
