# Workspace Store

Refs [lake#380](https://github.com/oramasearch/lake/issues/380), liquid-bridge decision 54 (S1, S2,
S3 and its 2026-10-03 implementation note), [plan](workspace-storage-plan.md#lake380-the-liquid-store).

**Status: implemented in the source tree; not deployed; no workspace on dev is switched.** A switched
workspace's repositories (and with them its Drive, models and datasets) are served from one liquid
store in its workspace Zetta, on Zetta's object capability
([zetta#1005](https://github.com/oramasearch/zetta/issues/1005), zetta `docs/data/object-store.md`,
live on dev Zetta). Writing such a store needs
[liquid#332](https://github.com/oramasearch/liquid/issues/332) (write-once index runs): until it lands,
liquid's index merge is refused, which every store reaches after a few writes (below).

## What lives where

| Piece | Where |
|---|---|
| Packs, index runs, object map, leases, collector state, each repository's `LIQUID_REPO`, journal, sessions and replication cursors | Bucket `lake_store` of the workspace Zetta, one object per liquid key, encrypted |
| The workspace's data key, wrapped | Object `keys/workspace-v1.wrapped` of the same bucket |
| Whether the workspace is switched | `workspaces.store_moved_at` (migration `0067`), content-free |
| The space's store from before the switch | `LAKE_STORE_ROOT/<space>/store`, unchanged and read-only from the switch on; removing it needs Steve's approval |

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
block's index and its length. A block altered, moved within the object, moved to another key or
object version, or read under another workspace's key fails authentication (`corrupt`).

**Appending without rewriting.** Zetta objects only grow. A partial block of appended bytes cannot be
completed in place, so at each barrier it is written whole to its own object
`K.lake-tail-<block>-<salt prefix>` (an atomic replacing put), and the next barrier appends the
completed block to `K` and replaces the tail. A tail whose block number is below `K`'s count of
appended blocks, or whose salt is not `K`'s, is stale: readers ignore it and the backend removes it
after the next barrier. Every offset is therefore computable from the header and `K`'s size alone:
plaintext size = base length + appended blocks × block size + the current tail's length.

**What it does not hide or prove.** Object names (`packs/pack-3.pack`, `repositories/<hash>/…`),
sizes to the block, and timing are visible to the workspace Zetta. Dropping whole trailing blocks of
`K` is not detected by the cipher; liquid's own pack scan and digests detect it as a torn tail. A
random 96-bit nonce per block keeps the collision bound under 2⁻³² for up to 2³² blocks (256 TiB of
writes) per data key; the header's key version leaves room for rotation, which is not built.

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
  bytes are refused `conflict`: liquid's index merge rewrites a finished run's header and bloom filter
  at offset 0, which Zetta refuses too (zetta `docs/data/object-store.md`, "One liquid write the
  contract refuses"). The message names liquid#332, and the store is reopened before its next use.
- Every mutation carries the lease's fence (`Zetta-Fence`).
- A failed mutation marks the backend failed: liquid may already have advanced its memory of the
  store (an index flush clears its memtable before the barrier that fails), and liquid's contract is
  to recover on the next open. Lake closes such a store and opens it again before the next request
  uses it.

## Reads

The backend keeps a directory of the bucket from one paged listing, maintained by its own writes;
that is exact because the fence makes it the only writer, and `list` and `exists` cost no request.
Headers are read with the first window of each object, on first use. Ranges are read through a
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
- A thread renews every third of the TTL (`LAKE_WORKSPACE_STORE_LEASE_SECONDS`, default 30). A
  renewal refused as fenced, or none accepted before the lease would run out, revokes the grant.
  Zetta refusing any write as `fenced` revokes it at once (liquid's wrapper).
- A lost lease closes the store; that workspace's requests answer `503 store_unavailable` while
  another process holds the lease, and the next request after it is free owns the store again with a
  higher fence. Other workspaces are not affected.
- `release` stops renewing and calls `:release`; lake does it when it drains (`ApiServer::stop`),
  when a workspace is deleted, and when it reopens a failed store.

A workspace store opens on the first request that needs it, never at start. The opening request
gives up lake's store mutex while it waits for the lease, so requests for other workspaces proceed;
requests for the same workspace wait for that open. `.lake-owner.lock` still covers the volume's
instance data and the space stores.

## Moving a workspace

`move_workspace_store` ([docs/api/workspace-store.md](../api/workspace-store.md)) plans, copies,
verifies and switches one workspace at a time with liquid#328's `repository::transfer`. A copy writes
nothing to the space store; a rerun copies only what is missing; an interrupted copy leaves no
`LIQUID_REPO` in the workspace store, and the store is reopened before the rerun, as liquid's
contract asks.

## Metering, quota and deletion

- Storage for a space is its space store (measured from its packs as before) while any of its
  workspaces is not switched, plus each switched workspace's bucket `usage`, which counts stored
  ciphertext. During a move the moved repositories are counted in both until every workspace of the
  space is switched.
- The bucket is created with the space's storage quota (`create_bucket {quota_bytes}`); Zetta answers
  a write past it `507`, which liquid sees as `busy`. An existing bucket keeps its quota.
  `workspaces.storage_gi` ([orama-operator#259](https://github.com/oramasearch/orama-operator/issues/259)) is not built.
- Deleting a workspace closes its store and releases its lease before the deletion is reported
  (`WorkspaceContent::close_workspace_store`); the workspace Zetta's deletion destroys the bytes.

## Latency

`WorkspaceStoreZetta.TheBackendOnARealZettaHoldsOnlyCiphertextAndItsLatencyIsMeasured`, 2026-10-03,
Apple M3 Max, one process: a `zetta` built from Zetta `f6a82675` (its object code is identical to
Zetta dev `b82a5e94`, which carries #1234) on loopback HTTP, beside a disk store on APFS in the
same run, both with liquid main `5fa3b50` plus liquid#332 (PR #334) applied, since without it the
measurement stops at the first index merge. Each row is the same work on both stores.

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

- Writes need liquid#332; reads, copies and verification work without it.
- The copy's precondition of fresh snapshots of the workspace volume and of lake's store volume
  ([orama-operator#260](https://github.com/oramasearch/orama-operator/issues/260)) is not enforced:
  lake has no snapshot desired state yet. The move never writes the space store.
- Search ([lake#381](https://github.com/oramasearch/lake/issues/381)) still indexes from the space
  store, so after a switch it does not see new content until #381 moves the index beside the
  workspace store.
- Data-key rotation; quota resize of an existing bucket.

## Tests

`tests/test_workspace_store.cpp` runs against `FakeZettaObjects` (`tests/zetta_object_fixture.hpp`),
Zetta's object routes as specified, including the refusal of in-place rewrites; the move tests run
a real lake server. `WorkspaceStoreZetta.*` run against a real Zetta binary when
`LAKE_TEST_ZETTA_BINARY` names one and print the latency table below.

| Test | Covers |
|---|---|
| `WorkspaceStoreBackend.RangesRoundTripAcrossBlocksTailsAndReopen` | ranges across blocks, buffered reads, tails, one current tail, a fresh backend reading and appending, put then append, ranges outside, conditional create, remove, reserved keys, no plaintext |
| `WorkspaceStoreBackend.ARewriteWithOtherBytesIsRefusedAsLiquid332` | a retried range changes nothing; a rewrite and a skip are `conflict` naming liquid#332 |
| `WorkspaceStoreBackend.TamperingMovedBlocksAndAnotherWorkspacesKeyAreRefused` | a flipped bit, swapped blocks, an object under another key, another workspace's bucket and key document, another KEK, no KEK |
| `WorkspaceStoreBackend.ALiquidStoreHoldsOnlyCiphertextAndReopens` | a liquid store on the backend: no plaintext in any object or request, reopen, collector dry run, nothing written locally |
| `WorkspaceStoreBackend.AStoreThatMergesIndexRunsNeedsLiquid332AndSaysSo` | the merge refusal and its message without liquid#332; every acknowledged object kept |
| `WorkspaceStoreBackend.ATransientFailureIsRetriedAndAPersistentOneIsUnavailable` | retries, then `unavailable` |
| `WorkspaceStoreOwner.ASuccessorWaitsForTheHolderAndTakesOverWithAHigherFence` | refusal naming the holder, a bounded wait, the hand-off, a higher fence |
| `WorkspaceStoreOwner.ALostLeaseRefusesWritesAndTheSuccessorKeepsEveryAcknowledgedObject` | renewal finds the takeover, writes refused, the successor reads everything; a fenced write revokes at once |
| `WorkspaceStoreMove.PlanCopyInterruptedVerifySwitchServeFromTheWorkspaceAndKeepWorkspacesApart` | plan, an interrupted copy, rerun, verify, an idempotent copy, switch, reads and writes after it, the space store untouched, two workspaces of one space in separate buckets with separate keys |
| `WorkspaceStoreMove.ALostLeaseAnswers503UntilReacquiredAndDeletionReleasesTheLease` | `503 store_unavailable` for the workspace only, reacquisition with a higher fence, deletion releasing the lease |
| `WorkspaceStoreZetta.TheBackendOnARealZettaHoldsOnlyCiphertextAndItsLatencyIsMeasured` | the same on a real Zetta, and the latency table |
