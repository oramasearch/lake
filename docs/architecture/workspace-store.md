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
completed in place, so when the appends are sent it is written whole to its own object
`K.lake-tail-<block>-<salt prefix>` (an atomic replacing put), and the next send appends the
completed block to `K` and replaces the tail. A tail whose block number is below `K`'s count of
appended blocks, or whose salt is not `K`'s, is stale: readers ignore it and the backend removes it
with the sync after the one that made its replacement durable. Every offset is therefore computable from the header and `K`'s size alone:
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

Since lake#527 the backend sends every mutation in the order liquid issued it, one at a time (each
answered before the next is sent), and holds small puts and buffered appends until a later write
needs them sent first. The bucket's log therefore holds a prefix of liquid's issue order, and a
crash keeps such a prefix (Zetta truncates its log at the first torn frame), never a later write
without an earlier one. That is the order liquid relies on between barriers; its contract does not
state it yet ([liquid#344](https://github.com/oramasearch/liquid/issues/344)).

Since lake#535 no write asks Zetta to fsync. Liquid's `barrier()` sends nothing, and the store is
made durable once per store operation, by lake, before it answers: `make_durable()` sends what is
held, in order, and then one Zetta `:barrier`. Before lake#535 every write went with
`?durable=true`, so a one-file commit on dev paid about 100 fsyncs (about 33 ms each on its Azure
disk, 3.1 to 3.4 s per commit); it now pays one.

What "a crash keeps a prefix" covers: a crash of the lake process or of the Zetta process on a
one-node workspace Zetta (every workspace today). A power loss of the workspace Zetta's node does
not, yet: across a segment roll a later segment's pages can reach the disk while an earlier
segment's unsynced tail does not, so a later write can survive without an earlier one
([zetta#1235](https://github.com/oramasearch/zetta/issues/1235)). With `rf` above 1 each key has
its own primary, so Zetta gives no order across keys; the `:barrier`'s durability still holds.

**Deploy gate: zetta#1235.** Before lake#535 every write was fsynced before the next was sent, so
only the writes sent without durability (large puts, threshold flushes) were exposed to that
window. Since lake#535 every write of an operation is, until its `:barrier`, and that loses
acknowledged data, not only the operation in flight. For example, a checkpoint puts `LIQUID_REPO`
and later removes `LIQUID_JOURNAL`; if the segment rolls between the two and the node loses power
before the roll is synced, the removal can survive without the put, and the reopened repository
falls back to the previous `LIQUID_REPO` with no journal, losing up to a checkpoint interval (64)
of acknowledged commits. A collection that writes a new pack and then removes the old ones can
lose objects the same way. The window is short (a roll happens once per 64 MiB written, and Zetta's
maintenance fsyncs a rolled segment within 100 ms), but it is real, so the order is:
1. zetta#1235 fixed (a roll fsyncs the previous segment, or a durable request syncs every earlier
   unsynced segment) and rolled out to every workspace Zetta;
2. then lake with lake#535.
With zetta#1235 in place a power loss keeps a prefix of the bucket's log, and every argument below
holds for it too.

**Not detected: a Zetta restart inside an operation.** If the workspace Zetta restarts (a power
loss of its node) between two writes of one operation, lake sees only failed or slow requests,
then answers; the writes before the restart may be gone while the ones after it landed, and
neither lake nor the final `:barrier` can tell. Before lake#535 each write was durable before the
next, so a restart lost nothing acknowledged. A boot or incarnation id from Zetta in each answer,
checked by the backend, would close this: a change within one operation fails it.

Why liquid's barriers need not be durable: a crash keeps a prefix of the issue order, and every
prefix is a state liquid could have been left in by a crash at the moment that prefix ends, on a
backend whose every barrier was durable (every write before that moment applied, none after).
Liquid recovers from such a crash by its own contract. What a crash may take back is therefore only
what no one outside the process has been told about, since lake's sync runs before lake answers or
records anything about the operation elsewhere (a repository row, a switch, an image blob record).

- `put` of at most 1 MiB is held: reads are answered from memory, and the bucket keeps the previous
  version until the held put is sent as one PUT, which Zetta commits atomically. A larger put is
  sent at once, after what is held.
- `put_if_absent` (`?if_absent=true`, whose `409` is `already_exists`) is sent at once, after what
  is held: its answer decides the create.
- `remove` is sent at once, after what is held. A removal of a key that was only held, never sent,
  and that is followed by at most one held key which goes as a single PUT, sends nothing and drops
  the held put (lake#535): liquid stages its object map as `packs/objects.meta.tmp`, publishes it
  as `packs/objects.meta` and removes the staging, and every prefix of the PUT of `objects.meta`
  alone is a prefix of those three. The key's tails go after the next sync.
- **Liquid's journal.** A checkpoint ends with the removal of `LIQUID_JOURNAL` and no barrier after
  it ([liquid#343](https://github.com/oramasearch/liquid/issues/343)), and a reopen prefers the
  journal while it exists. The removal is sent at once and made durable by the sync before lake
  answers the commit, so an acknowledged commit is never reverted by a journal that comes back.
  Liquid's next barrier would not do it: it sends nothing, and the next operation may never come.
- `put_range` appends to a buffer. Complete blocks are sent once at least 1 MiB is held, as PATCHes
  at the object's exact size of at most one Zetta slice (4 MiB), each applied by Zetta as one
  frame, so an object never ends inside a block. An object not in Zetta yet (a held put appended
  to, or one created by appends) is sent whole, as one PUT of its header and base blocks, so a put
  is never seen in part and the object needs no tail.
- **Order.** Held writes are kept per key in the order of each key's earliest held write. A write
  about a key that is held before another key's held write sends what is held first, so coalescing
  never moves a later write ahead of an earlier one. Holding spans liquid's barriers.
- **Durability.** Writes go without `?durable=true`. `ZettaObjectBackend::sync()` sends what one
  view holds and, when anything was sent since the last `:barrier`, one `:barrier`; the tails made
  stale by an earlier sync are removed beside it. `make_durable()` syncs the backend and every view
  of it; a sync with nothing sent and no stale tail costs no request. Only the data key's creation
  is sent `?durable=true` (the bucket's first object).
- **A new view starts unsynced.** What the bucket holds when a store is opened may include writes
  a predecessor (or this process before a reopen) sent but never made durable: its last `:barrier`
  failed, or it stopped first. They are served from then on, so every fenced view starts as needing
  a `:barrier`, and lake syncs right after it opens a workspace store, before serving anything; a
  store whose `:barrier` fails then is closed and not served. That is one fsync per open.
  Opening a repository syncs too: liquid's open may recover its state (replay a journal, finish a
  checkpoint), and the pre-open (lake#536) opens repositories with no request after it.
- **Where lake syncs.** Every repository call on a workspace store (`enter_repository`) syncs when
  it leaves the store's lane, while the request's `Server-Timing` still counts it. The calls that
  write (commit, push, chunk upload, LFS staging, conflict resolution, restore, provenance,
  replication, portable and git import) sync explicitly before they answer, so a failed `:barrier`
  fails the call; so do a repository's creation in a workspace store, the move's copy, and an image
  blob's put and rooting. A sync that fails on the way out of the lane (a call that does not write,
  or one that already failed) marks the backend failed, and lake reopens the store before its next
  use.
- **Bounds.** Held writes past 8 MiB (`held_bytes_limit`) or 256 held puts (`held_puts_limit`) are
  sent at once. The count is exact: a put replacing a held put, a held key removed, and a threshold
  flush leave only what is still held. A view also keeps, to skip an identical rewrite, the
  plaintext of small objects it sent whole: at most 16 KiB each and 1 MiB in all.
- A put of the bytes the bucket already holds for that key (sent whole by this view, nothing
  appended since) sends nothing: liquid publishes its index manifest twice in a row, with barriers
  between, and the second is the same bytes. The skip leaves every prefix as it was, since the
  bucket's log holds that version before anything issued after it.
- **Search's objects.** The search index's sealed objects (`search/`, lake#381) are written with
  the store's client, outside this backend; the backend leaves them out of its directory and
  refuses liquid keys under that prefix. A search commit sends its new objects and then its
  manifest, one at a time, and one `:barrier` after the manifest (lake#535 dropped the `:barrier`
  before it: the bucket's log keeps the order).
- **Why the guarantees hold.** Everything issued before a sync is durable when it answers: the
  writes it sends are applied before its `:barrier`, which fsyncs every segment written since the
  last one, and the writes sent earlier are too. A crash keeps a prefix of the issue order that
  includes everything the last answered sync covered. A tail is removed only after the sync that
  made its replacement durable, so a crash never leaves an object shorter than acknowledged.
- **A lost answer.** A PUT, PATCH or DELETE whose answer is lost is retried, and the retry changes
  nothing more (a PUT of the same bytes, a PATCH that repeats the bytes there, a DELETE answered
  `404`). A conditional create whose answer is lost is not retried: it answers `unavailable` and
  fails the backend, and the reopened store finds whatever landed.
- **Quota.** A held put meets the bucket's quota when it is sent, so a write past the quota
  (`507`, liquid's `busy`) can fail at the sync, or at the next write sent at once, rather than at
  the `put` that went over.
- A range that overlaps written bytes must repeat them (a retry, which changes nothing). Different
  bytes are refused `conflict`, as Zetta refuses them (zetta `docs/data/object-store.md`); liquid
  since liquid#332 never writes so, and the store is reopened before its next use.
- Every mutation carries the lease's fence (`Zetta-Fence`).
- One sender at a time: a buffered-append flush and a sync wait for each other rather than send
  the same blocks twice.
- **Closing.** The owner syncs the store's backend before it releases the lease
  (`ZettaObjectClient::before_release`, set by the backend; liquid's close releases without a last
  barrier), so a store closed cleanly leaves nothing unsent or undurable. Liquid's close lets go of
  the backend and revokes the store's copy of the grant before it calls the release, so the root
  backend keeps a view that still holds writes for the release, and the release settles unless its
  own renewal lost the lease (lake#535; before, the revoked grant read as a lost lease and the
  settling never ran on liquid's close, which cost nothing while liquid's barriers sent
  everything). After a takeover the settling's writes are refused as fenced and land nothing; a
  lease lost to the renewal lets go of the views without sending. The views are kept only while an
  owner is attached to the client, and the owner lets go of them when it is destroyed without a
  release, so a view and the backend's state never keep each other alive. This settling is bounded
  (10 s by default): each of its requests gets one attempt, cut to the time left, so a drain with
  the workspace Zetta unreachable does not stall. Only the settling's requests are cut; other users
  of the client (search) keep their timeouts. A lost lease or a failed store is closed without it;
  what was held then is lost, as liquid allows for writes no barrier acknowledged.
- A conditional create that Zetta refuses while the directory said the key was absent (another
  writer, which the fence should exclude) marks the backend failed and keeps the directory and
  buffered appends as they are.
- A failed mutation or sync marks the backend failed: liquid may already have advanced its memory
  of the store (an index flush clears its memtable before the write that fails), and liquid's
  contract is to recover on the next open. Lake closes such a store and opens it again before the
  next request uses it.

## Reads

The backend keeps a directory of the bucket from one paged listing, maintained by its own writes;
that is exact because the fence makes it the only writer, and `list` and `exists` cost no request.
Headers are read with the first window of each object, on first use; a tail that is not the
object's current one is then removed by the next sync. Ranges are read through a
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
  like any other. The wait is Zetta's, not a poll: `acquire_object_lease` registers a lease watch
  before its attempt and waits on it or the holder's expiry (zetta `docs/data/object-store.md`,
  "Registry capabilities"), so a release wakes the waiting acquire at once and the successor takes
  the lease within one request, whatever is left of its piece or of the holder's TTL.
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

A running switched workspace's store is opened ahead of its first request, in the background from
start (the pre-open, [below](#rollout-hand-off-lake536)); any other store, and one the pre-open
could not open, opens on the first request that needs it.

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

**Shutdown.** `ApiServer::begin_drain` (at SIGTERM) hands the workspace stores over first
([below](#rollout-hand-off-lake536)). `ApiServer::stop` then drains requests, stops the maintenance
thread, closes every store still open,
then gives repository-store work still running on the blocking pool the rest of the drain deadline.
After that it closes its io_context to the pool (`close_execution_context`): a task still running is
abandoned and posts nothing into the destroyed context. The request that awaited an abandoned task
is never resumed, so its response and its audit event are lost; the count is logged
(`abandoned_store_tasks`) and counted (`lake_abandoned_store_tasks_total`).

**Clients and 503.** liquid's sync client does not retry a 503, nor read `Retry-After`:
[liquid#340](https://github.com/oramasearch/liquid/issues/340). Until it does, a `liquid push` or
`pull` refused during a move or a lease hand-off must be rerun.

`.lake-owner.lock` still covers the volume's instance data and the space stores.

## Rollout hand-off (lake#536)

In an overlapping rollout (decision 49, `spec.rollout.overlapOnSharedStore`) the successor starts
beside its predecessor on the same volume and the same workspace Zettas, and every workspace store
passes from one to the other through its bucket lease.

**The predecessor, at SIGTERM** (`ApiServer::begin_drain`, called by a watcher thread the moment the
signal arrives, before anything else winds down):

1. It stops taking new work as before (`/readyz` 503, connections closed after each response,
   requests held for the root answered).
2. A workspace's lane admits no new request: `admit("w:<workspace>")` and a new image pull answer
   `503 store_unavailable` with `Retry-After: 1` at once ("lake is stopping and hands the workspace
   store to its successor; retry shortly"), and no workspace store is opened again, so a released
   lease stays released. A request admitted to the lane before the drain began (holding it or
   waiting for it) finishes. An in-flight request that had not reached the lane yet is refused like
   a new one; the retry reaches the successor. The blob operations that take the lane without
   admission (`workspace_blob_present`, `root_workspace_blob`) are refused the same way.
3. It announces the drain to a successor once no open of its own is in progress
   (`wait_for_opens`: `open_workspace` calls are counted, and none starts after the drain began),
   bounded by the drain deadline; a deadline passed with one still in progress announces nothing,
   and the successor then waits for the root. The announcement is its own lock record written to
   `<root>/.lake-owner.draining` (`StoreOwnerLock::announce_draining`). The record carries a random
   token, so the marker names this lock and never an earlier or later one; an owner whose record
   cannot be written refuses the root at start instead of keeping its predecessor's record.
4. Lake's own work leaves the workspace lanes (`pause_lake_work`): search stops (a rebuild stops at
   its next repository), the search and maintenance queues take no new job, and a lane refuses
   lake work (`lake_work` borrows) and the operator's copy, verify and switch (`503`) from now on.
   Nothing is joined, so a release waits for the lane's current holder only. The index work
   dropped here (a rebuild queued or running, repositories pending) is not lost: before such a
   workspace's store is released its `search/manifest.json` is removed under the lease, and the
   next process rebuilds the index ([search.md](../api/search.md#where-the-index-lives-lake381-decision-54-s8)).
5. Each open workspace store is closed as soon as its lane is idle (no admitted request, no image
   pull), eight stores at a time (`release_workspace_stores`, sweeping again until none is left, so
   a store an open already in progress inserts later is released too): the owner settles what the
   backend holds (bounded, 10 s) and releases the lease. This does not wait for the process's
   other requests (a long poll, a space store's push) or for the drain deadline; a store whose lane
   is still in use at the deadline is closed with the rest by `stop()`. Each store logs
   `{"event":"store_owner","scope":"workspace","state":"released","waited_ms":…,"closing_ms":…}`.
6. `stop()` then drains the remaining requests, closes the space stores and releases the root, as
   before ([store-volume.md](../deployment/store-volume.md#the-hand-off)).

**The successor, from start** (`preopen_workspace_stores`, `LAKE_WORKSPACE_STORE_PREOPEN`, default
on): a background pass lists every space's switched workspaces that are running and opens their
stores, at most eight at once.

- **It never takes a lease from a predecessor that still serves.** Before this process owns the
  root, a free lease may still be the predecessor's to take (a workspace it never opened, a store
  it reopens after a failed write), so the pass attempts no open until the root's owner has
  announced its drain (the marker above matches the lock file's record) or this process owns the
  root; one poller reads the marker every 250 ms for all the openers. A marker left by an earlier
  owner, or a copy of an older record, never matches. A predecessor from before lake#536 never
  announces, so then the opens start with the root.
  A workspace store is guarded by its lease and fence, not by `.lake-owner.lock`, and liquid writes
  nothing under the root for a store on a backend, so once announced the opens need not wait for
  the root.
- Each attempt waits up to 5 s for the lease, so the open happens the moment the predecessor
  releases it (Zetta's acquire wakes on the release), which is before the predecessor releases the
  root. Before each attempt, under the lane, the pass checks again that the workspace is still
  switched and running; one deleted or stopped meanwhile is not opened.
- It holds the lane as lake's own work, so a request that arrives meanwhile waits for the open and
  then uses it. Attempts go on until `LAKE_STORE_OWNER_WAIT_SECONDS`, the drain, or a failed root;
  anything but a held lease or an unready workspace Zetta is left to the first request. Each store
  logs `{"event":"store_owner","scope":"workspace","state":"preopened","attempts":…,"store_ms":…}`.
- Once this process owns the root (and has removed the marker, logging a removal that fails),
  eight workers open the live repositories of each opened store, each repository under the lane on
  its own (a repository is named by its path under the root): the stores opened before then in
  turn, and a store that opens later as soon as it opens, so one workspace slow to open (an unready
  workspace Zetta, retried until `LAKE_STORE_OWNER_WAIT_SECONDS`) holds up no other. Each logs
  `"state":"repositories_preopened"`.
- An `ApiServer` is started once; after `stop()` its `start()` refuses, and a restart is a new
  server (as `main` and the tests do).

**Fencing stays the guarantee.** Nothing here lets two processes write one store: the predecessor
closes the store before it releases the lease, and every write it might still send carries its
fence, which Zetta refuses as `fenced` once the successor's acquire raised it.

**What the 73 s on dev was.** The log lines in lake#536 (`store_owner … waiting … wait_ms 90000`,
then `handoff … waited_ms 23676`) are the store root's hand-off (`.lake-owner.lock`, lake#479), not a
workspace lease: the successor waited 23.7 s for the root, which includes the predecessor's 15 s
preStop. The 73 s commit began after that, at 17:51:22, when the predecessor had already released
the workspace leases (its renewals on the workspace Zettas stopped at about 17:51:19). The workspace
Zetta's audit counter shows what the commit did instead: 2,647 requests in 74 s (about 28 ms each,
one after another), against about 0.2 a second before; the next commits took about 3 s. So the
time was the cold store: opening it and its repository and reading what the first commit needed into
an empty window cache. The pre-open moves the store and repository opens off the first request and
overlaps them with the rest of the root hand-off; the commit's own cold reads remain. With 10 ms a
request (`WorkspaceStoreHandoff.*`), a pre-opened first commit sent 75 to 101 requests against 55
to 57 for the next one: the extra are window reads and liquid's first writes after an open.

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
assumed to be 5 to 10 ms; lake#544 measured it on dev at under 0.2 ms, see
[Per-request cost](#per-request-cost-lake544)). It counts the requests of each kind one commit sends and
prints them in order. Apple M3 Max, Zetta `f6a82675`, liquid `8955519`; the fourth commit (the
store and repository open, directory and headers known):

| | Requests | PUT | PATCH | `:barrier` | DELETE | GET / list | Sequential rounds | 0 ms RTT | 5 ms | 10 ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Before lake#527 | 180 | 77 | 0 | 68 | 35 | 0 | 180 | 169 ms | not measured | 2,537 ms |
| An interval sent concurrently (#532 `0362c6c`, withdrawn) | 80 | 55 | 0 | 0 | 25 | 0 | 57 | 75 ms | 421-429 ms | 753-771 ms |
| In issue order (#532 now) | 80 | 55 | 0 | 0 | 25 | 0 | 80 | 63-106 ms | 671-707 ms | 1,023-1,228 ms |

The two #532 rows were measured on the same machine under the same load (a load average of 5 to 10,
from other builds), twice each; the first row's 10 ms figure is from an earlier, quieter run. The
sequential rounds are counted by the test's transport: a request sent while none is in flight
starts a round.

Before, every request was its own round trip. Per liquid object a commit stores (about eight for
one file: the blob, its directory documents, the change, the journal), liquid wrote its pack tail,
staged and published the object map, wrote an index run and published the index manifest twice,
with a barrier after each step: about fifteen requests, six of them `:barrier`. Through a running
lake, `WorkspaceStoreMove.ACommitNamesItsStoreRequestsInServerTimingAndMetrics` checks that the
`Server-Timing` count of a commit equals what the workspace Zetta received. Metering and audit do
not reach the store, and the store is not reopened. Since lake#381 the commit's search reindex
writes its index objects to the same bucket, outside liquid's backend: in that test 6 writes and
2 `:barrier`s of the commit's 70 requests (the other 62 are liquid's). Those writes keep their own
order and barrier (`SealedIndexObjects`); the held writes and ordering here do not apply to them.

What changed: no `:barrier` (each write durable as it is sent); an object created in one interval
is one PUT with no tail (an index run was a PUT, a tail PUT and later two DELETEs); the repeated
identical manifest is not sent; stale tails are removed beside the next barrier's writes. Sending
an interval's writes concurrently saved a further 23 round trips, but a crash could then keep any
subset of them, which liquid does not promise to survive
([liquid#344](https://github.com/oramasearch/liquid/issues/344)); they are sent in order, and
that costs about 300 ms per one-file commit at 10 ms.

**The floor is one round trip per write.** Every write must be answered before the next is sent
to keep the issue order, so a one-file commit costs its 80 writes: about 800 ms at a 10 ms round
trip plus Zetta's fsync per write, measured about 1.05 s through the proxy. The 500 ms target at
10 ms is not met. Getting under it needs one of:
- fewer writes and barriers from liquid: an object batch around a repository operation's object
  writes, one manifest publication per index flush, or an index flush per operation instead of
  per object;
- order without a round trip per write: HTTP pipelining on one connection (lake's Zetta HTTP
  client does not pipeline), or an ordered batch route in Zetta (it has none);
- liquid stating that an interval's writes may land in any subset (liquid#344), which would allow
  concurrent sends again.

The process metrics `lake_workspace_store_requests_total{kind,outcome}` and
`lake_workspace_store_request_duration_seconds{kind}` and each response's `store` Server-Timing
entries give the same counts on dev; a deployed measurement belongs in the deployment record.

### One fsync per operation (lake#535)

**On dev** (lake `20261006d`, `d86f1ca`, 2026-10-06, before lake#535), a one-file commit to
`test1` in workspace `6bb777ba` took 3.1 to 3.4 s, with this `Server-Timing`:

```
store;dur=3445.630;desc="104 requests"
store_put;dur=2119.637;desc="59"
store_put_if_absent;dur=44.872;desc="3"
store_barrier;dur=57.233;desc="2"
store_delete;dur=1223.888;desc="40"
```

About 33 ms per request against an in-cluster round trip of about 2 ms: each write was sent
`?durable=true`, and an fsync on the workspace Zetta's Azure disk costs about 30 ms. The fsync is
what the proxy models with `LAKE_TEST_ZETTA_FSYNC_MS=30`: it holds every request that asks for one
30 ms longer. The model reproduces dev's figure (80 requests, 3.4 s at a 2 ms round trip).

**Where dev's 104 requests come from**, against the benchmark's 80:
- **The search index (lake#381, PR #530): 8.** The commit's reindex writes one new segment, one
  artifacts object and one blob run (write-once: the 3 `put_if_absent`), the manifest (a PUT), and
  removes the replaced segment and artifacts object (2 DELETEs); one `:barrier` before the manifest
  and one after (the 2 `:barrier`s).
- **Image blobs: none.** Only a registry push writes them (`put_workspace_blob`), never a commit.
- **Liquid: the other 96** (about 58 PUTs and 38 DELETEs). Per object the commit stores (about
  nine for one file) 4 writes: the pack's tail, the object map, an index run, the index manifest
  (plus, before lake#535, the object map's staging PUT and its DELETE). On top of that, two things
  that grow with the repository, which the benchmark's young five-directory repository barely has:
  - **Directory leases.** Each checkpoint retains every directory of the repository, and each
    retention renews the directory's lease: a PUT of the next generation and a DELETE of the last,
    2 requests per directory per commit.
  - **Index merges.** Liquid merges its index runs as they accumulate: each merge is a PUT of the
    merged run and a DELETE per run merged.
  `LAKE_TEST_ZETTA_SEED_COMMITS=40` (40 earlier commits of 8 files over 16 directories) reproduces
  dev's mix: 116 requests before lake#535 (72 PUT, 1 PATCH, 43 DELETE). In one such commit after
  lake#535 (107 requests), 44 are lease writes (34 of them renewals) and 27 are index runs (15 of
  them DELETEs of merged runs); the count moves by about 10 from commit to commit with the merges.

**What lake#535 changed** (Apple M3 Max, real Zetta `f6a82675`, liquid `8955519`, the steady-state
fourth commit including lake's sync, a load average of 5 to 8 from other builds; the lake#535
ranges are two runs):

| | Requests | durable (fsync) | 0 ms RTT | 5 ms | 10 ms | 2 ms + 30 ms fsync | 10 ms + 30 ms fsync |
|---|---:|---:|---:|---:|---:|---:|---:|
| #532 (before) | 80 | 80 | 71 ms | 607 ms | 1,068 ms | 3,371 ms | 4,261 ms |
| lake#535 | 63 | 1 | 56-60 ms | 473-603 ms | 844-979 ms | 264-369 ms | 895-1,028 ms |

With the dev-like repository (`LAKE_TEST_ZETTA_SEED_COMMITS=40`, 2 ms round trip, 30 ms fsync):
116 requests and 115 fsyncs, 5.3 s in requests, before; 113 requests and 1 fsync, 0.6 s in
requests, after. That run also spends about 1.5 s outside requests (2.6 s before), in liquid and
the backend's own work for a repository of 320 files on a loaded machine; dev's `Server-Timing`
shows no such share (its store time is the whole commit).

Opening costs one fsync more: a workspace store's open sends one `:barrier` (its view starts
unsynced), and a repository's open one more when liquid's open wrote something (it rewrites the
repository's entry point). At dev's figures that is about 32 ms per open, once per process for
each workspace store and repository, and lake#536's pre-open pays it before the first request.

Through a running lake (`WorkspaceStoreMove.ACommitNamesItsStoreRequestsInServerTimingAndMetrics`,
fake Zetta) a commit is 56 requests, 70 before: liquid's 48 writes (62 before) and one `:barrier`,
and the search index's 6 writes and one `:barrier` (two before). The test checks that a commit asks
for one or two fsyncs.

What was cut, and why it is safe:
- **The fsync per write.** Writes go without durability and one `:barrier` per operation makes them
  durable before lake answers ([Writes](#writes)).
- **The object map's staging** (`packs/objects.meta.tmp`, a PUT and a DELETE per object): dropped
  while it is held and only the publication follows it, which leaves every prefix a prefix of the
  issue order.
- **The search index's first `:barrier`**, before its manifest: the bucket's log keeps the order.

What is left is liquid's: 4 writes per object stored and, on a repository with history, the
directory lease renewals and index merges. Fewer of them need liquid: an object batch per
operation and one manifest publication per index flush
([liquid#341](https://github.com/oramasearch/liquid/issues/341)), and renewing a directory's lease
only when its retention changes or nears expiry rather than at every checkpoint. At dev's 2 ms round
trip a one-file commit should now cost about its request count times the round trip plus one fsync:
about 0.25 to 0.3 s for dev's 104 requests. The measurement on dev belongs in the deployment record
once deployed.

## Per-request cost (lake#544)

**On dev after lake#535** (lake `20261007a`, `777e6e3`, Zetta `dev-5f70b306`), a one-file commit was
1.7 s for 77 requests, about 20 ms each, although none of them asked for an fsync:

```
total;dur=1712  store;dur=1665;desc="77 requests"  store_put;dur=944;desc="49"  store_barrier;dur=127;desc="2"  store_delete;dur=522;desc="23"
```

**Where the 20 ms went** (measured on dev on 2026-10-07, read-only, from a pod in the `lake`
namespace and from lake's own pod):

| Step | Cost on dev |
|---|---:|
| Resolving `prod-ws-6bb777ba498a-zetta.prod-ws-6bb777ba498a.svc.cluster.local` with glibc in lake's pod | 5 to 32 ms, about 18 ms on average (10 samples) |
| The same name with a trailing dot (no search domains) | 1.7 to 6.8 ms, including the process start of `getent` |
| TCP connect to a Zetta Service address | 0.08 ms |
| A whole request on a fresh connection (connect, send, Zetta's answer) | 0.15 ms |
| A request on a kept connection | 0.05 ms |

The pod's resolver has `options ndots:5` and four search domains. The workspace Zetta's name has
four dots, so every lookup first tries it under each search domain (`lake.svc.cluster.local`,
`svc.cluster.local`, `cluster.local` and the Azure host domain, which is forwarded off the cluster),
A and AAAA each, before the absolute name. Lake sent every request through Zetta's connector client
(`zetta::connect`, `src/core/api/native_http_transport.cpp` of the pinned Zetta), which resolves,
dials, sends `Connection: close` and closes for each request: so each request paid one
resolution, about 18 ms of its 20. Connection setup and Zetta's own work were well under 1 ms. The
workspace Zetta is reached over plain HTTP inside the cluster, so there is no TLS handshake. The
probe pod could not reach the workspace Zetta itself (its NetworkPolicy admits only lake's pods),
so the connect and request figures are from the bus Zetta in the `lake` namespace, the same binary
on the same network; the probe used no credential.

**What changed.** Lake has its own client for the object routes, `make_pooled_object_transport`
(`src/control/object_http.cpp`, its own library `lake-object-http`), and every workspace store
uses it (`make_zetta_object_transport`):
- **HTTP/1.1 keep-alive.** Connections are kept per origin (host and port; up to 32 idle), one
  request at a time each, and a request in flight while the others are busy opens another. Zetta's
  HTTP server (Beast2's `http_worker`) keeps a connection open between requests and closes none
  for being idle.
- **One resolution per origin** while it is younger than 30 s, shared by dials that need it at
  once, and only when a new connection is dialed. An address that refuses is resolved again once.
  When the request that started a shared resolution is cancelled (its timeout), the others waiting
  on it resolve again instead of failing with its cancellation.
- **`TCP_NODELAY`** on every connection, so a request's header and body are not held apart by
  Nagle's algorithm on a kept connection.
- **What is not reused.** A kept connection is reused only when it is still open and silent (a
  non-blocking peek), and one idle longer than 30 s is closed. A connection whose request failed or
  timed out, or that Zetta answered `Connection: close` on, is closed.
- **A peer that went silent.** When a request on a kept connection fails or times out, every other
  connection kept to that origin is closed too, so the retry dials afresh rather than waiting on
  another connection to the same dead peer. A dial gets 2 s (`connect_timeout`); one that gets no
  answer fails then, not at the request's timeout, and its address is resolved again next time.
  Every connection has TCP keepalive probes (after 10 s idle, every 5 s, 3 probes where the
  platform lets lake set them), so a kept connection to a vanished peer is found dead.
- **Nothing lingers.** Every 10 s (`sweep_interval`) the client closes, for every origin, kept
  connections idle past 30 s or found closed, and forgets resolutions past 30 s: an origin no
  longer used (a deleted workspace's Zetta) keeps no socket. Connections are kept oldest first and
  the most recent is reused, so the sweep takes the old ones. Connections closed after their answer
  because the origin already keeps 32 are counted (`overflow`).
- **A request that is not retried gets a new connection.** `ZettaObjectClient` sends the requests
  it makes once (a conditional create, a lease acquisition or renewal, a usage read) with
  `fresh_connection`, so a kept connection Zetta closed just as the request went out cannot fail
  them. Its connection is kept afterwards like any other.
- **Nothing is sent that lake did not ask for.** An unknown method, or a header that cannot be set
  (a value with a line break, a name with a space), fails the request before anything is sent; a
  header is never dropped. The error names the header, never its value.
- **Framing.** Content-Length and chunked answers are read to their end and leave the connection
  kept; a 204 too. A body shorter than its Content-Length is a transport failure, and a body that
  runs to the close leaves nothing to keep. Bytes after an answer are never served to the next
  request. An interim (1xx) answer, which lake never asks for, is a transport failure and its
  connection is not kept. IPv6 addresses connect (the socket opens for the endpoint's family).
- **Stopping.** `shutdown()` (the destructor calls it) answers every request still waiting as a
  transport failure, and later requests fail at once.
- **No request is sent twice by the transport.** A request on a kept connection that fails is a
  transport failure like any other; the retry of an idempotent request stays
  `ZettaObjectClient::send`'s. So the ordering, fencing and durability of lake#527, lake#535 and
  zetta#1235 are unchanged: the same requests are sent in the same order, one at a time where they
  were, with the same queries and headers.
- An `https` endpoint (none today) still goes through Zetta's client, unpooled.

**Measured** (`WorkspaceStoreZetta.AOneFileCommitsRequestsAreCountedAndTimed`, Apple M3 Max, real
Zetta built from zetta#1235's branch at `c4a62169` (merged into Zetta dev as `5f70b306`), liquid `8955519`, `LAKE_TEST_ZETTA_SEED_COMMITS=20`,
the endpoint named `localhost` so that a dial resolves; `LAKE_TEST_ZETTA_TRANSPORT=unpooled`
measures the client before lake#544):

| | 0 ms round trip | 2 ms round trip |
|---|---:|---:|
| Steady one-file commit, before | 567 to 7,514 ms (one commit 0.3 s to 13.8 s in requests) | 679 to 802 ms |
| Steady one-file commit, after | 101 to 108 ms, 33 to 35 ms in about 100 requests | 349 to 394 ms |
| First commit after the store is opened again, before | 1,215 ms | 920 ms |
| First commit after the store is opened again, after | 122 ms | 521 ms |

After, about 8,500 requests of that run went over 16 connections and one resolution. Before, each
request opened a connection, and on a Mac the loopback churn alone made the figures vary by an
order of magnitude. Locally a request costs about 0.33 ms in all; the proxy's round trip comes on
top.

On dev the resolution was the cost, so a commit should come down to its request count times Zetta's
answer time, plus the one `:barrier`'s fsync (about 30 ms): about 0.1 to 0.2 s for 77 requests,
against 1.7 s. The cold first commit of lake#538 (2,647 requests at about 28 ms) should fall by the
same factor, to a few seconds. Both belong in the deployment record once measured on dev.

**What was not done for lake#538.** Its reads are liquid's, one at a time: each is a separate small
object (an index run, the object map, a repository document), so the backend's window is the whole
object and there is no next window to read ahead. Reading them concurrently needs liquid to issue
them together; prefetching them needs to know which objects the first commit reads, which the
counts by kind in `Server-Timing` give once the transport cost is gone. A read across several
missing 4 MiB windows of one large object (a Drive download) is still fetched window by window;
at about 1 ms a request that costs a few ms per 100 MiB.

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
| `WorkspaceStoreBackend.TailRollbackDeletionTruncationAndACrashBeforeTheTailAreAsDocumented` | tail rollback and deletion read as the shorter object, truncation inside a block is `corrupt`, a crash between the PATCH and the tail keeps what the last sync acknowledged |
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
| `WorkspaceStoreZetta.AOneFileCommitsRequestsAreCountedAndTimed` | lake#527: one-file commits on a real Zetta behind a round trip, requests by kind, rounds, the request trace; lake#535: each commit with lake's sync, an fsync cost per durable request (`LAKE_TEST_ZETTA_FSYNC_MS`), a repository with history (`LAKE_TEST_ZETTA_SEED_COMMITS`), and one durable request per commit; lake#544: the transport before it (`LAKE_TEST_ZETTA_TRANSPORT=unpooled`) or lake's, with its connections and resolutions (fewer than one connection per ten requests); lake#538: the first commit after the store is opened again |
| `ObjectHttp.*` (`tests/test_object_http.cpp`) | lake#544: against a loopback server, sequential requests share one connection and one resolution; overlapping requests each get a connection and keep it; a connection the server closed, one answered `Connection: close`, one idle too long and one whose request timed out are not reused; no listener is a transport failure; the bearer never appears in an error; a request that times out on a kept connection drops the origin's other kept connections; a dial to a silent peer fails within `connect_timeout` and resolves again; a client over a silent Zetta fails `unavailable` within a bounded time; the sweep closes an origin no longer used and only the old connections of one in use; a connection past the pool's bound is closed and counted; stopping the transport answers a waiting caller; `fresh_connection` dials, and the client asks for it for exactly its single-attempt requests; an unknown method and a header that cannot be set fail with nothing sent; an interim answer leaves its connection unkept; an IPv6 address connects; chunked, 204, extra-bytes, short and run-to-close answers are framed and nothing passes to the next request; a shared resolution cancelled with its request is done again for the others waiting on it (`resolve_delay`, tests only); 200 rounds of a request right after the server closes its connections: a fresh-connection request never fails, any other answers or fails as a transport failure and never gets another request's answer |
| `WorkspaceStoreZetta.ALostAnswerOverThePooledTransportIsRetriedOrFailsTheBackend` | lake#544 end to end on a real Zetta through the pooled transport and a proxy that loses answers: a PUT and a DELETE whose answers were lost are retried harmlessly on a new connection; a conditional create whose answer was lost answers `unavailable`, fails the backend, and a reopened backend holds the object |
| `WorkspaceStoreBackend.ACrashAnywhereKeepsAPrefixOfTheIssueOrderPastTheLastBarrier` | lake#527: 200 random sequences of held puts, immediate puts, appends past the threshold, conditional creates, removes, repeated bytes and crossed bounds, with liquid's barriers and lake's syncs between (lake#535), ending in a sync that is answered, stopped part way, or loses requests or answers at random, then a crash keeping a prefix of the bucket's log: what survives is a prefix of the issue order at least as long as the last answered sync |
| `WorkspaceStoreBackend.AnIntervalsWritesAreSentOneAtATimeInIssueOrder` | lake#527: a marker, a pack, an index run with appends, a removal and a manifest reach Zetta in that order, never two at once; lake#535: liquid's barriers between them send nothing, and the sync sends one `:barrier` |
| `WorkspaceStoreBackend.AWriteWhoseAnswerWasLostIsRetriedOrFailsTheBackend` | lake#527: a durable PUT and a DELETE whose answers were lost after they landed are retried harmlessly; a conditional create that landed answers `unavailable`, fails the backend, and the reopened store holds it |
| `WorkspaceStoreBackend.HeldWritesPastTheirBoundAreSentEarly` | lake#527: past the held-put count or byte bound, held writes are sent at once, and the next sync sends `:barrier` |
| `WorkspaceStoreBackend.ConcurrentRequestsStopAtTheFirstFailureAndReportALostLeaseFirst` | lake#527: no request starts after a failure; `locked` is reported before other errors |
| `WorkspaceStoreBackend.ACrashMidCommitKeepsEveryAcknowledgedCommit` | lake#527: liquid one-file commits with checkpoints at several intervals, each acknowledged once lake's sync after it answers (lake#535), one interrupted (stopped, requests lost, or answers lost), a crash, a successor's reopen: every acknowledged commit reads back and the head is the last acknowledged change (a stale `LIQUID_JOURNAL` would revert it), or the interrupted one when liquid answered it and only the sync failed |
| `WorkspaceStoreOwner.ReleaseSettlesWithinItsBudgetWhenZettaDoesNotAnswer` | lake#527: release with Zetta not answering the held writes returns within its budget and still releases the lease |
| `WorkspaceStoreOwner.TheSettleBudgetCutsOnlyTheSettlingRequests` | lake#527: a request beside the settling keeps its own timeout |
| `WorkspaceStoreBackend.HeldBytesAndPutsStayExact` | lake#527: a replaced held put, a removed held key and a threshold flush leave the held count exact |
| `WorkspaceStoreBackend.ABarrierNeverRemovesATailItJustWrote` | lake#527: a stale tail whose name the sync writes again survives the sync |
| `WorkspaceStoreBackend.TheSearchIndexsObjectsAreNotLiquids` | lake#527: `search/` objects stay out of liquid's directory, and liquid cannot write there |
| `WorkspaceStoreMove.ACommitNamesItsStoreRequestsInServerTimingAndMetrics` | lake#527: a commit through lake names its store requests in `Server-Timing`, equal to what the workspace Zetta received, and the metrics count them by kind without a workspace label; lake#535: the commit asks the workspace Zetta for one or two fsyncs |
| `WorkspaceStoreOwner.AWaitingAcquireTakesTheLeaseAsSoonAsItIsReleased` | lake#536: an acquire waiting on a held lease answers within 1 s of the release, with a higher fence; a write with the released fence is refused |
| `WorkspaceStoreHandoff.ASuccessorTakesEachLeaseBeforeTheRootAndItsFirstRequestsNeedNoOpen` | lake#536: two lake servers on one volume and one fake Zetta (10 ms a request, 30 s TTL): the successor's pre-open waits while the predecessor holds both workspaces; once the predecessor begins to drain (still owning the root) the successor takes both leases with higher fences, before the root; the first commit to each workspace answers in under 5 s and takes no lease (no open); a write with the predecessor's fence is refused |
| `WorkspaceStoreHandoff.AServingPredecessorKeepsAWorkspaceItNeverOpenedWhileTheSuccessorPreopens` | lake#536: before the predecessor drains, the successor takes no free lease, and the predecessor serves a workspace it had never opened |
| `WorkspaceStoreHandoff.MoreWorkspacesThanWorkersAreAllPreopenedBeforeTheRoot` | lake#536: ten workspaces (more than the eight pre-open workers) are all pre-opened while the predecessor still owns the root |
| `WorkspaceStoreHandoff.AWorkspaceStoppedOrDeletedDuringThePreopenIsNotOpened` | lake#536: a workspace stopped, and one deleted, while the pre-open waits for their leases are not opened once the leases are free |
| `WorkspaceStoreHandoff.ADrainingLakeRefusesNewStoreRequestsFinishesAdmittedOnesAndReleasesTheLease` | lake#536: after `begin_drain` a new request to the workspace answers `503` with `Retry-After: 1` at once, the commit admitted before finishes `200`, and the lease is released before `stop()`; a search, a commit and the operator's copy, verify and switch after the release are refused and take no lease again; the commit reads back after a restart |
| `WorkspaceStoreHandoff.ASearchRebuildDoesNotHoldOffTheRelease` | lake#536: a search index rebuild reading eight repositories from a slow workspace Zetta stops at the drain, and the lease is released in under 2.5 s (about 8.5 s when the rebuild runs on) |
| `RegistryStore.ADrainingLakeRefusesBlobWorkAndNeverRetakesTheLease` | lake#536: after `begin_drain` a blob `HEAD` and `GET` answer `503` with `Retry-After: 1` (stream admission), the operator's copy of an unrooted blob is refused by `root_workspace_blob`, and the released lease is not taken again |
| `WorkspaceStoreHandoff.TheDrainIsAnnouncedOnlyOnceAnOpenInProgressHasFinished` | lake#536: with an open in progress at the drain, the announcement waits for it (written at once without the count) |
| `WorkspaceStoreHandoff.AnUnreadyWorkspaceDoesNotHoldUpTheOthersRepositories` | lake#536: with one workspace's Zetta not ready, another's repositories are opened within 3 s of the root; lake#535: nothing their open wrote is left unsynced |
| `WorkspaceStoreHandoff.AStaleDrainAnnouncementIsIgnored` | lake#536: an earlier owner's announcement and a copy of the record with another token are ignored; the successor takes no free lease, and removes the marker once it owns the root |
| `WorkspaceStoreHandoff.IndexWorkTheDrainDropsIsRebuiltByTheSuccessor` | lake#536: a rebuild and a pending commit dropped by the drain: the manifest is removed, and the successor's rebuilt index holds every commit |
| `WorkspaceStoreBackend.ACommitAsksForOneDurableRequestAndKeepsItsJournalRemoval` | lake#535: liquid one-file commits that each checkpoint send no fsync until lake's sync, which sends one `:barrier`; an idle sync sends nothing; after a crash `LIQUID_JOURNAL` stays removed and the reopened head is the last commit |
| `WorkspaceStoreBackend.ANewViewsFirstSyncMakesWhatItServesDurable` | lake#535: a write a predecessor sent but never made durable is served by the next view, whose first sync sends `:barrier` (and only the first); a crash after it keeps the write |
| `WorkspaceStoreMove.AStoreWhoseOpeningBarrierFailsIsNotServed` | lake#535: after a restart, a workspace store whose opening `:barrier` fails serves no read; once Zetta answers, it opens and serves |
| `WorkspaceStoreOwner.ViewsKeptForAReleaseThatNeverComesAreLetGo` | lake#535: views kept for the owner's release are let go of when the owner goes without one |
| `WorkspaceStoreOwner.ALiquidCloseSyncsWhatTheBackendHolds` | lake#535: liquid's close (backend let go of, grant revoked) still syncs what the backend holds with one `:barrier`; a crash after it keeps every object |
| `WorkspaceStoreBackend.AStagedObjectRemovedAfterItsPublicationSendsOnlyThePublication` | lake#535: a staged object published and then removed sends only the publication; with two held keys after the staging, everything is sent; a crash keeps a prefix |
