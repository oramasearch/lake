# Customer liquid-store volume

## What

lake opens one libliquid store per customer at
`$LAKE_STORE_ROOT/<customer id>/store`. The root is an operator-provided read-write PersistentVolume
mounted into every Lake pod; lake does not choose a storage class or create a claim itself.

## Why

The liquid store owns its pack files, index runs, manifests, and collector state. Keeping a store
per customer makes customer ownership explicit and lets the control plane address a repository’s
data through its `customer_id`. It also preserves the library boundary: lake links `liquid::liquid`
with `LIQUID_BUILD_CLI=OFF` and never changes liquid through its lake submodule.

## Operator contract

`orama-operator#132` owns the `Lake` CR volume field, PersistentVolumeClaim reference, mount, and
`LAKE_STORE_ROOT` injection. The required mount must be writable by lake’s numeric runtime user
`10001:10001`. One lake process owns a store root at a time (next section). A Lake whose store is
one ReadWriteOnce volume can roll either way: `Recreate` (the operator default), or an overlapping
surge with `spec.rollout.overlapOnSharedStore`, in which the new process waits for the old one to
hand the root over (see [The hand-off](#the-hand-off)).

## One process per store root

**What.** Lake takes an exclusive `flock` on `$LAKE_STORE_ROOT/.lake-owner.lock` before it opens
anything under the root, and writes the owner's pid, host, node and start time into the file.
Each customer store is also opened with libliquid's `store_options::exclusive_owner`, which holds
liquid's own lock on `<customer>/store/store.lock` until the store is closed (liquid#321). The
root lock is always taken first, so the per-store locks are never contended between lake
processes; they keep any other writer of one store out.

The kernel releases both kinds of lock when the process exits, however it exits; a clean stop
releases them after in-flight requests finish. The lock files are opened close-on-exec, so no
child process can keep them. The locks need a filesystem where `flock` is honored between the
processes that mount it: a local block filesystem such as the AKS ReadWriteOnce Azure Disk is. A
network filesystem (ReadWriteMany) must provide `flock` between its clients before it is used.

**Why the root lock stays (lake#467, lake#479).** liquid#321 made each customer store safe
against a second process by serializing whole processes on the store; it does not make lake's own
state under the root multi-process. The search index, the npm and Python registry indexes, TUF
metadata and timestamp evidence are read once, cached and republished from one process's memory.
Two processes writing them lose updates (table below). The root lock serializes lake processes as
a whole, so every one of those stays single-writer, and the liquid store locks add a second line
of defense for the customer stores.

The libliquid customer stores keep the store's pack append position, its object map, its chunk
index and its delta candidates in memory, append to the current pack at the offset they remember,
and publish `packs/objects.meta` and the index manifest by overwriting them; opening a store also
deletes the `*.tmp` files another process may be writing. Two lake processes that wrote one
customer store without a lock overwrote each other's frames and object maps: in lake#467, two
processes serving repository pushes and drive writes on one store for three seconds produced
`chunk header digest differs from index`, `invalid index run size` and `chunk is not present`
errors, and afterwards the customer store no longer opened.

What is on the store volume, and how each part is written:

| Path | Written by | Two processes |
|---|---|---|
| `<customer>/store/` (packs, `objects.meta`, `remotes.meta`, index, leases, gc) | libliquid store, in-process mutation mutex, state cached in memory; `store.lock` held while open | **unsafe** without the lock: corrupted in lake#467 |
| `<customer>/repositories/<id>/` (refs, journal, checkpoint, replication cursor) | libliquid repository on the customer store handle, opened once and cached; cursor via a fixed `.tmp` name | **unsafe**: cached state, shared temporary name |
| Drive content | a liquid repository in the customer store | **unsafe**, as the store |
| Models and datasets | liquid sync into the customer store | **unsafe**, as the store |
| `<customer>/index/` (search segments and manifest of unswitched workspaces' repositories) | `search::Service`, per-space state loaded once and republished from memory | **unsafe**: a publication drops the other process's segments. A switched workspace's index is not here: it is beside its store in its workspace Zetta, written under the store's lease fence (lake#381, [search.md](../api/search.md#where-the-index-lives-lake381-decision-54-s8)) |
| `<node>/registry/` OCI blobs | uploads under unique ids, blobs renamed into content-addressed paths | safe in the test (444 manifests, 888 blobs, no loss) |
| `<node>/registry/` manifests, tags, npm and Python indexes | write-then-rename; npm and Python indexes are read-modify-write under an in-process mutex | tags: last writer wins; **package indexes: lost update** |
| `<node>/log/` transparency tiles and `tree.json` | `flock` on `append.lock`, state re-read under the lock | safe in the test (1,692 appends, tree rebuilt from its entries) |
| `<node>/log/checkpoints/` | `flock` publication lock, re-read under it | safe by design; temporary names are per-process counters |
| `<node>/log/tuf/`, `<node>/log/timestamps/` | in-process mutex, existence check then rename, per-process temporary counters | **unsafe**: two processes can write one version or one temporary file |
| Process memory | open liquid stores and repositories, search segments, metering warm-up | **stale** once another process writes |

The control plane is PostgreSQL, which the processes share safely (the audit chain is serialized
by an advisory lock).

## The hand-off

**What (lake#479).** A starting lake does not refuse a held root; it waits for it, and it is
reachable while it waits.

1. **Serve first.** `ApiServer::start` binds the listener and starts serving before it touches the
   root. An owner thread then takes `.lake-owner.lock`, retrying (5 ms, doubling to 100 ms) for up
   to `LAKE_STORE_OWNER_WAIT_SECONDS` (default 90). Once it holds the lock it opens every existing
   customer store (with `exclusive_owner` and the same `owner_wait`) and its repository and
   result indexes, then marks the root open.
2. **Hold, never refuse.** Until the root is open, every request except `/healthz`, `/readyz`,
   `/metrics` and the embedded stylesheet, fonts and experiment assets waits for the open. The
   wait is a timer on the server's io thread, not a blocked thread, so probes keep answering. When
   the root opens, the held requests run. They are answered `503 store_unavailable` only if the
   wait expires, the open fails, or the process is stopped before it ever owned the root. All
   other requests wait, not only the ones known to write the store, because a capability can reach
   the root indirectly (the audit mirror into the transparency log, search, registry indexes),
   and a route missed from a list would let two processes write the root at once. The cost is
   that control-plane-only requests sent to the waiting pod are delayed by the hand-off too.
3. **Readiness** (`GET /readyz`), after the existing control-store ping:

   | Store root | `/readyz` | Body |
   |---|---|---|
   | open (or no store root configured) | `200` | `{"ok":true,"store":"postgres","store_root":"open"}` |
   | waiting: another process holds it | `200` | `"store_root":"waiting"`, `"store_root_holder":"pid … on host … (node …, since …)"` |
   | opening, after waiting (taking the lock it waited for, opening the stores) | `200` | `"store_root":"opening"` |
   | opening, uncontended (a cold start: no predecessor) | `503` | `store_unavailable`, "the store root is opening" |
   | failed (wait expired, open failed) | `503` | `store_unavailable` with the reason |
   | stopping (after SIGTERM) | `503` | `store_unavailable`, "lake is stopping" |

   So `/readyz` is not ready before the store is open, with one exception: while a live
   predecessor holds the root. That exception is what lets a surge finish. With
   `maxSurge: 1, maxUnavailable: 0` the Deployment controller stops the old pod only after the
   new pod is Ready, and the old pod is the one holding the lock. If readiness waited for the open,
   neither pod could ever proceed and the rollout would stall at its progress deadline. Reporting
   Ready while waiting is honest about what readiness promises: a request sent to this pod is
   answered, not refused; it waits for the hand-off. Once a pod has reported Ready through a
   wait, it stays Ready through the open that follows, so readiness does not flap.
4. **Stop** (SIGTERM, or `ApiServer::stop`): `/readyz` answers `503`, every response closes its
   connection, and requests still held for the root are answered. Lake then waits up to
   `LAKE_DRAIN_SECONDS` (default 20) for the requests it has accepted to finish, stops the
   listener, closes every customer store (releasing liquid's store locks), releases
   `.lake-owner.lock`, logs `{"event":"store_owner","state":"released",...}` on stderr and exits 0.
5. **Give up.** If the root is still held after `LAKE_STORE_OWNER_WAIT_SECONDS`, lake logs the
   holder and exits 1 with:

   ```text
   lake: cannot start: the store root <root> is still in use by another lake process after
   waiting 90000 ms: pid <pid> on host <host> (node <node>, since <time>). ...
   ```

   The kubelet restarts it and the wait starts again.

**A surge rollout on the operator's settings** (`spec.rollout.overlapOnSharedStore`: RollingUpdate
`maxSurge: 1`, `maxUnavailable: 0`, required pod affinity on `kubernetes.io/hostname`, readiness
`/readyz` every 5 s, startup probe `/healthz`, preStop `sleep 15`, grace period 45 s):

1. B starts on A's node and mounts the same volume. `/healthz` answers at once, so the startup
   probe passes. B finds the root held and reports Ready with `store_root: waiting`.
2. The Service adds B. Requests that reach B wait.
3. The Deployment deletes A. A leaves the endpoints and sleeps 15 s in preStop while it keeps
   serving what was already routed to it.
4. A gets SIGTERM, drains (at most 20 s), closes its stores and releases the root.
5. B takes the root within one retry (at most 100 ms), opens the stores, and answers the requests
   it held.

A request that reaches B in step 2 waits for at most preStop + drain, about 35 s in the worst
case, and is answered. No request is refused. `LAKE_STORE_OWNER_WAIT_SECONDS` (90) outlasts
preStop + `LAKE_DRAIN_SECONDS` (35) and the 45 s grace period after which the kubelet kills A, so
B never gives up on a predecessor that is stopping normally. Raising `LAKE_DRAIN_SECONDS` needs a
longer grace period and owner wait to match.

**Operator.** The design works with the operator as it is on `origin/main` (7b8ecb4): no
operator change is required. Two of its documents describe the old behavior and should be updated
there: `docs/lake/README.md` (Probes and drain: "lake stops its HTTP server at once on SIGTERM"
and the readiness row "200 only when lake's control store answers a ping"; Pod replacement:
"nothing yet proves that two processes can safely share it") and `DESIGN.md` decision 38. The
store-root state is visible in the `/readyz` body and the `store_owner` log events.

**Verify.** `tests/test_shared_store.cpp` (`StoreOwner.*`; the binary cases need
`LAKE_TEST_DB_URI`):

- `WaitingAcquireTakesTheRootWhenItsHolderReleasesIt`: the bounded wait takes the lock once the
  holder releases it, names the holder when it expires, and returns at once when abandoned.
- `SecondServerIsReadyWhileWaitingAndServesAfterTheHandOff`: in one process, a second server on a
  held root answers `/readyz` `200 waiting` and `/healthz`, holds a request until the first
  server stops, then answers it `200`.
- `SuccessorGivesUpAfterItsOwnerWait`: with a 400 ms wait, the held request is answered `503`
  naming the holder and `/readyz` turns `503`.
- `SuccessorTakesOverFromTheDrainingOwnerWithoutRefusingARequest`: the real binary. A serves
  concurrent repository pushes, drive writes, registry uploads and transparency appends. B starts
  on the same root and database and must become Ready while it waits. A drive write is sent to
  B; the writers route to both A and B, then to B alone (A's endpoint removed), then A gets
  SIGTERM. The held write must be answered `200` only after A's SIGTERM. Across the run, no
  response may be a `503`, no request to B or to A may fail, and every acknowledged push, drive
  file, manifest, blob and log leaf must read back from B. After B stops, the customer store must
  reopen and pass a libliquid dry-run collection (a full reachability walk), and the transparency
  log's entries must rebuild its recorded tree.
- `SuccessorBinaryExitsWhenTheOwnerOutlastsItsWait`: with `LAKE_STORE_OWNER_WAIT_SECONDS=1` the
  successor exits 1 naming the holder, and the owner keeps serving.

The operator also vendors the byte-identical `db/schema.sql` asset. Migration
`0006_repository_lifecycle` changes its `PostgresSchema` guard table to
`repository_lifecycle_events`.

## Workspace stores (lake#380)

A switched workspace's repositories no longer live on this volume: they are one liquid store in
bucket `lake_store` of the workspace's own Zetta, encrypted with the workspace's data key
([architecture/workspace-store.md](../architecture/workspace-store.md)). The volume keeps the
space stores (read-only for a switched workspace's repositories, never removed by the move), the
search indexes, the registry and the instance's transparency data, all still under
`.lake-owner.lock`.

The hand-off above becomes per workspace for those stores. A workspace store is opened by the first
request that needs it, not at start, after `.lake-owner.lock` is held. The open acquires the
bucket's lease, waiting up to `LAKE_STORE_OWNER_WAIT_SECONDS` for a predecessor to release it, with
lake's store mutex given up so requests for other workspaces proceed. Lake renews the lease every
third of `LAKE_WORKSPACE_STORE_LEASE_SECONDS` (default 30) and releases it on drain, after the last
request and before it releases the root. A lease lost while running (a successor took it, or no
renewal was accepted in time) closes that store and answers its workspace's requests
`503 store_unavailable` until the lease is free again; Zetta refuses every write carrying the old
fence. `spec.rollout.overlapOnSharedStore` covers workspace stores unchanged: the predecessor's
release lets the successor's waiting acquire answer.

## Verify

Start a local lake with an explicit writable directory:

```sh
LAKE_STORE_ROOT="$PWD/test-store" \
  ./build/lake --listen 127.0.0.1:4901 --memory-store --admin-token t
```

After a sync request for customer `<id>`, confirm that the store path is
`test-store/<id>/store`. Run `ctest --test-dir build --output-on-failure` for the capability and
renderer checks. The operator PR adds the mounted-volume integration test.

## Change

Do not change the root layout, user ID, or volume access assumptions only in the deployment
manifest. Update the `Lake` CR reference, this page, `README.md`, `CLAUDE.md`, and the operator
runbook together. Store-format changes belong to liquid and reach lake through a pinned submodule
commit.
