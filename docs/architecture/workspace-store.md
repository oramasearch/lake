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
| The workspace's data key, wrapped | Object `keys/workspace-v1.wrapped` of the same bucket |
| Whether the workspace is switched | `workspaces.store_moved_at` (migration `0067`), content-free |
| The space's store from before the switch | `LAKE_STORE_ROOT/<space>/store` and `<space>/repositories`, unchanged and read-only from the switch on, until the operator's `remove_space_store` removes both once every workspace of the space is switched ([below](#removing-the-spaces-store)) |

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

- `put` and `put_if_absent` send the whole encrypted object in one PUT (`?if_absent=true` for the
  conditional create, whose `409` is `already_exists`). Zetta commits a PUT atomically.
- `put_range` appends to a buffer. Complete blocks are sent once at least 1 MiB is held, and always
  before `barrier()` answers, as PATCHes at the object's exact size of at most one Zetta slice
  (4 MiB), each applied by Zetta as one frame, so an object never ends inside a block.
- `barrier()` sends what is buffered, writes each object's tail, calls Zetta's `:barrier` when
  anything was written since the last one (a barrier with nothing new costs no request), then
  removes stale tails.
- A range that overlaps written bytes must repeat them (a retry, which changes nothing). Different
  bytes are refused `conflict`, as Zetta refuses them (zetta `docs/data/object-store.md`); liquid
  since liquid#332 never writes so, and the store is reopened before its next use.
- Every mutation carries the lease's fence (`Zetta-Fence`).
- One flush per object at a time: a buffered-append flush and a barrier on the same object wait for
  each other rather than send its blocks twice.
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
- `release` stops renewing and calls `:release`; lake does it when it drains (`ApiServer::stop`),
  when a workspace is deleted, and when it reopens a failed store.

A workspace store opens on the first request that needs it, never at start.

**Isolation.** Each store has its own lane (`s:<space>` for a space's store, `w:<workspace>` for a
workspace's), and a request holds only its store's lane for its call; the lock that guards lake's
maps of open stores is never held across store I/O, a network call or a wait.

- **Off the server thread.** Synchronous capabilities that wait on a repository store (repository,
  Drive, model and dataset capabilities, search, `replicate_repository`, `move_workspace_store`,
  `delete_workspace`) run on a pool of 32 threads instead of lake's single server thread
  (`Registry::call_async`), and asynchronous handlers run each repository-store call there too
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
- **Search skips switched workspaces until [lake#381](https://github.com/oramasearch/lake/issues/381).**
  The index files are on lake's volume, so indexing a switched workspace's repository there would
  put its content back on lake's volume. Until lake#381 moves the index beside the workspace store,
  publish and reindex of such a repository write nothing (they drop what the index held for it),
  a history scan of it answers no occurrence, and hot and artifact searches leave it out. A
  repository whose route cannot be read is treated as switched.
- **Zetta's HTTP client** (`install_native_http_transport`) is safe from many threads at once: one
  dedicated thread runs every exchange, callers post to it and wait on an event, and its shared TLS
  context cache is locked.

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
- Deleting a workspace closes its store and releases its lease before the deletion is reported
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

## Not yet

- The copy's precondition of fresh snapshots of the workspace volume and of lake's store volume
  ([orama-operator#260](https://github.com/oramasearch/orama-operator/issues/260)) is not enforced.
  Migration `0069` added the snapshot desired state (`volume_snapshot_policies`) and the operator's
  `volume_snapshot_status`, but `move_workspace_store` does not request a snapshot or wait for one
  to be ready yet. The move never writes the space store.
- The search index files stay on lake's volume until [lake#381](https://github.com/oramasearch/lake/issues/381)
  moves them beside the workspace store; search already reads a switched workspace's repositories
  from its store.
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
| `WorkspaceStoreMove.ASwitchedWorkspacesRepositoryIsNotIndexedOnTheVolumeOrReturned` | no segment or blob of a switched repository in the space index; not returned; the space's other repository still is |
| `WorkspaceStoreMove.ARowLeftWithoutItsRepositoryIsCleanedUpByTheNextCreate` | an orphaned row is tombstoned by the next creation of its name |
| `WorkspaceStoreMove.ARestartCountsSwitchedWorkspacesUsageBeforeAnyWrite` | usage primed at start |
| `WorkspaceStoreMove.ASearchWaitingOnOneSpacesStoreDoesNotHoldSearchInAnother` | search borrows before its lock |
| `WorkspaceStoreSearch.OnceTheRepositoryStoreLentRepositoriesSearchNeverOpensAStoreItself` | the fallback refuses once a repository store was set |
| `WorkspaceStoreRemoval.RemovesOnlyTheSpacesStoreOnceEveryWorkspaceIsSwitched` | `remove_space_store`: invalid and unknown spaces refused; refused (dry run too) while a workspace is not switched; a dry run's paths and bytes with nothing removed; the removal of exactly the two directories; the instance's directory and another space untouched; switched workspaces read and written afterwards and after a restart; a repeated removal; central audit; a later unswitched workspace served from a new space store and refusing the removal until it is switched |
| `WorkspaceStoreRemoval.ALinkedSpaceOrStoreDirectoryIsRefused` | a symbolic link at `<space>/store` and at `<space>` itself is refused with `conflict` (dry run too) and nothing behind it is touched; put back, the removal proceeds |
| `WorkspaceStoreRemoval.ALiveRepositoryOfADeletedWorkspaceOrAPersonIsRefused` | a live repository of a deleted workspace refuses the removal (dry run too); no bearer and a person are refused |
| `WorkspaceStoreRemoval.AnUnassignedRepositoryIsRefused` | on PostgreSQL, a repository without a workspace refuses the removal although the space's only workspace is switched |
| `WorkspaceStoreZetta.TheBackendOnARealZettaHoldsOnlyCiphertextAndItsLatencyIsMeasured` | the same on a real Zetta, and the latency table |
