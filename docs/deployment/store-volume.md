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
`10001:10001`. One lake process owns a store root at a time (next section), so a Lake whose store
is one volume runs one replica and rolls with `Recreate`.

## One process per store root

**What.** `ApiServer::start` takes an exclusive `flock` on `$LAKE_STORE_ROOT/.lake-owner.lock`
before it opens anything under the root, and writes the owner's pid, host, node and start time
into the file. A second lake process on the same root refuses to start and exits 1 with:

```text
lake: cannot start: the store root <root> is in use by another lake process: pid <pid> on host
<host> (node <node>, since <time>). One lake process owns a store root at a time, ...
```

The kernel releases the lock when the owner exits, however it exits; `ApiServer::stop` releases
it after in-flight requests finish. The lock file is opened close-on-exec, so no child process
can keep it. The lock needs a filesystem where `flock` is honored between the processes that
mount it: a local block filesystem such as the AKS ReadWriteOnce Azure Disk is. A network
filesystem (ReadWriteMany) must provide `flock` between its clients before it is used.

**Why (lake#467).** The libliquid customer stores are single-process by design. Each process
keeps the store's pack append position, its object map, its chunk index and its delta candidates
in memory, appends to the current pack at the offset it remembers, and publishes
`packs/objects.meta` and the index manifest by overwriting them; opening a store also deletes
the `*.tmp` files another process may be writing. Liquid's own `flock` in `src/data.cpp` guards
only data-branch (PostgreSQL) operations, not the object store. Two lake processes that write one
customer store overwrite each other's frames and object maps. With the lock removed, two lake
processes serving repository pushes and drive writes on one store for three seconds produced
`chunk header digest differs from index`, `invalid index run size` and `chunk is not present`
errors, and afterwards the customer store no longer opened: acknowledged writes were lost.

What is on the store volume, and how each part is written:

| Path | Written by | Two processes |
|---|---|---|
| `<customer>/store/` (packs, `objects.meta`, `remotes.meta`, index, leases, gc) | libliquid store, in-process mutation mutex, state cached in memory | **unsafe**: corrupted in the test above |
| `<customer>/repositories/<id>/` (refs, journal, checkpoint, replication cursor) | libliquid repository, opened once and cached; cursor via a fixed `.tmp` name | **unsafe**: cached state, shared temporary name |
| Drive content | a liquid repository in the customer store | **unsafe**, as the store |
| Models and datasets | liquid sync into the customer store | **unsafe**, as the store |
| `<customer>/index/` (search segments and manifest) | `search::Service`, per-customer state loaded once and republished from memory | **unsafe**: a publication drops the other process's segments |
| `<node>/registry/` OCI blobs | uploads under unique ids, blobs renamed into content-addressed paths | safe in the test (444 manifests, 888 blobs, no loss) |
| `<node>/registry/` manifests, tags, npm and Python indexes | write-then-rename; npm and Python indexes are read-modify-write under an in-process mutex | tags: last writer wins; **package indexes: lost update** |
| `<node>/log/` transparency tiles and `tree.json` | `flock` on `append.lock`, state re-read under the lock | safe in the test (1,692 appends, tree rebuilt from its entries) |
| `<node>/log/checkpoints/` | `flock` publication lock, re-read under it | safe by design; temporary names are per-process counters |
| `<node>/log/tuf/`, `<node>/log/timestamps/` | in-process mutex, existence check then rename, per-process temporary counters | **unsafe**: two processes can write one version or one temporary file |
| Process memory | open liquid stores and repositories, search segments, metering warm-up | **stale** once another process writes |

The control plane is PostgreSQL, which the processes share safely (the audit chain is serialized
by an advisory lock).

**What it costs.** An overlapping rollout (orama-operator `spec.rollout.overlapOnSharedStore`)
cannot help a Lake with one store volume: its surge pod exits with the message above until the
old pod is gone, the old pod is not stopped until the surge pod is ready, and the rollout stalls
at its progress deadline while the old pod keeps serving. Leave the opt-in off; deploys keep
the short `Recreate` window. Removing that window needs either a multi-process libliquid store
or a hand-off in which the new process serves non-store routes until it can take the lock.

**Verify.** `StoreOwner.OverlappingProcessRefusesAndAcknowledgedWritesSurviveTheDrain`
(`tests/test_shared_store.cpp`, needs `LAKE_TEST_DB_URI`) starts the real `lake` binary on one
store root and one PostgreSQL database, runs concurrent repository pushes, drive writes, registry
uploads and transparency appends, starts a second process (which must refuse with the message),
sends the owner SIGTERM under load, then starts a successor. Every acknowledged push, drive file,
manifest, blob and log leaf must read back from the successor; the customer store must reopen and
pass a libliquid dry-run collection (a full reachability walk); and the transparency log's
entries must rebuild its recorded tree.

The operator also vendors the byte-identical `db/schema.sql` asset. Migration
`0006_repository_lifecycle` changes its `PostgresSchema` guard table to
`repository_lifecycle_events`.

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
