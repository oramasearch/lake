# Search

`GET /v1/search` is the authenticated repository-search capability. It is registered once and is
therefore also present in `GET /v1/openapi.json` and `POST /mcp` as the `search` tool. The
server-rendered page is `GET /search`; it accepts the capability's scope values and never exposes
a result that the capability would reject.

## Request and result

| Input | Meaning |
|---|---|
| `query` | Required. Plain text is ranked BM25F; quoted text is an exact substring; `/expression/` is an ECMAScript regex. |
| `customer_id` | Optional account id (`customer` is its deprecated name, lake#591). Searches the account's index on lake's volume and, when the caller may read the account, the index of each of its switched workspaces. Empty: every account for the operator, and for anyone else every account the caller may read. |
| `workspace_id` | Optional. Searches only that switched workspace's index (lake#381). A workspace that does not exist, is not switched, is deleted or is in an account the caller cannot read answers the same `invalid_input` (`workspace_id must name a workspace Zetta in this account`). |
| `repository` | Optional repository id. |
| `ref` | Optional branch or tag name. An immutable tag snapshot takes precedence over a branch with the same name. |
| `change` | Optional exact change digest. Requires `customer_id` and `repository`, cannot be combined with `ref` or `history`, and uses scan mode. |
| `history` | Optional boolean. Walk every change in one `customer_id` and `repository`; cannot be combined with `ref` or `change`. |
| `limit` | Optional result count, default 20 and maximum 1000. |
| artifact scope | Append `in:artifact:<name>[@<version>]` to `query`. The name is a build output path and the version is its immutable result digest. This scope cannot be combined with `ref`, `change`, or `history`. |

The value is
`{mode,scanned_changes,skipped_workspaces,building_workspaces,stale_workspaces,items:[{customer,repository,path,change,snippet,score,artifact,artifact_version,digest,line,workspace_id}]}`.
`workspace_id` names the workspace whose index answered a hit (empty for the account's index on lake's
volume). `skipped_workspaces` lists the switched workspaces an account-wide search could not read
(stopped, its store unavailable or moving); a search that names a workspace or a repository fails
instead. `building_workspaces` lists the workspaces whose index is being built (after a switch, or
because it was lost): they answer nothing yet, and the search does not wait for them or take their
store's lane. `stale_workspaces` lists the workspaces whose index answered but is partly stale: a
write to it failed and left repositories pending (lake#540, [below](#a-failed-index-write-is-retried-lake540)),
so it answers what it held before them until the retry succeeds. `mode` explains which corpus
answered the request:

| Mode | Corpus |
|---|---|
| `hot` | Current branch-tip segments. This is the default when neither `ref`, `change`, nor `history` selects another corpus. |
| `snapshot` | The immutable segment built once for the requested release or tag. |
| `scan` | An exact change or the repository's complete change log. `scanned_changes` reports how many snapshots were examined. |
| `artifact` | Members of the selected published output. Hits name the artifact, result version, logical member digest, and matching one-based line. |

Hits are score-descending and then repository/path ordered for stable ties. A quoted query and
regex query return score `1`; ranked text returns the combined content and path BM25F score.

**A regex query is bounded** (LSR-117, lake#984): the pattern is at most 256 characters and may not
repeat a group that itself repeats (`(a+)+`, `(x*y)*`), only the first 1 MiB of each file is
searched, and a query stops scanning after two seconds, answering what matched so far.

```sh
curl -H "Authorization: Bearer $LAKE_ADMIN_TOKEN" \
  --get --data-urlencode 'query="connection refused"' \
  http://127.0.0.1:4900/v1/search

curl -H "Authorization: Bearer $LAKE_ADMIN_TOKEN" \
  --get --data-urlencode 'query=connection' \
  --data-urlencode 'customer=<customer-id>' \
  --data-urlencode 'repository=<repository-id>' \
  --data-urlencode 'history=true' \
  http://127.0.0.1:4900/v1/search
```

## Where the index lives (lake#381, decision 54 S8)

A repository's index is kept beside the store that serves it:

| Repository served by | Index | Format |
|---|---|---|
| A switched workspace's store (`workspaces.store_moved_at` set) | Objects `search/<name>` in bucket `lake_store` of the workspace Zetta, beside its liquid store | Sealed with the workspace's data key (below); blobs in write-once runs |
| Its account's store on lake's volume | `<root>/<space>/index/` | Plain files (below) |

**Beside the workspace store.** A switched workspace's index is the objects `search/manifest.json`
(replaced atomically), `search/runs/<id>.json` (blob text, each run written once with
`if_absent`), `search/segments/<id>.json` and `search/artifacts/<id>.json` (written once, named by the
manifest). A prefix in the store's bucket rather than a bucket of its own: one lease and fence cover
the store and its index (a second bucket would need its own lease, or would take writes from any
process), the index needs no key of its own, and it goes with the bucket. Liquid never lists or
writes under `search/`. No index content of a switched workspace is written to lake's volume or to
PostgreSQL.

**Across a rollout (lake#536).** What search knows beyond the stored index is in memory only: a
rebuild running or due, and repositories pending until it is done or waiting for a retry
(`Service::unsettled_workspaces`). A draining lake drops those jobs, so before it releases such a
workspace's store it removes `search/manifest.json` under the store's lease, and the next process
rebuilds the index from the repositories on its first use.

**Sealed, fenced, and apart from the store's failures.** Search writes its objects through its own
`ZettaObjectClient` calls (`search::make_sealed_index_objects`), not through liquid's backend: each
object is `LKSIDX1\0` followed by AES-256-GCM under the workspace's data key (the store's
`WorkspaceStoreCipher`, unwrapped once per open store from `keys/workspace-v1.wrapped`), its
associated data binding the workspace and the object's name, so an object cannot be moved under
another name or read under another workspace's key. Objects carry no key version: once data-key
rotation exists, a rotated key makes them unreadable and the index is rebuilt. Every write carries
the store's lease fence. A failed index write is the search's failure only: it never marks the
workspace store failed, so the store is not reopened; only Zetta refusing an index write as
`fenced` (the lease is another holder's) marks the store unusable, as a lost lease does. A
write-once put retried after Zetta took its first attempt (`already_exists` with the same content)
counts as written.

**Writes once, not whole.** A commit writes the new segments and one new run holding only the blobs
no run has, then replaces the manifest; runs, segments and artifact objects the manifest no longer
names are removed after it. When more than half of the runs' bytes are unreferenced (and they hold
over 1 MiB), the live blobs are written into one new run and the old runs removed. A push therefore
writes the text it added, not the workspace's whole text.

**Room for the repositories.** Index bytes count in the account's storage quota with the
repositories. Before a commit, search checks that what it is about to write fits in the quota less
the account's stored bytes as lake's push quota check counts them (the account's store on lake's volume
while any workspace is not switched, plus every switched workspace's last bucket usage), less what
search indexes wrote since those readings, less a tenth of the quota kept for liquid. A usage not
read yet counts as no room. Otherwise it writes nothing and answers `busy` ("indexing is stopped"),
which a push or commit records in its audit details and never fails on.

**Who can make search open a workspace.** The `search` capability decides the scope before search
lends, opens or rebuilds anything (`search_scope`). The operator reads every account. Anyone else
names a `customer_id`, a `workspace_id` or a `repository`, or is limited to the accounts they may read:
every account filtered by `read` through the policy engine, as the dashboard lists the caller's own, so
ownership, membership, grants and organization roles count, for a person, an agent or a service
token alike. An account's switched workspaces are opened only when the caller may read the account
(`read` on its `customer` policy object), a named workspace only then too, and a repository's workspace only when the caller
may read the repository. An account the caller cannot read is still searched on lake's volume, hit by
hit through the policy filter, without opening its workspaces. `skipped_workspaces` and
`building_workspaces` only ever name workspaces in the scope.

**Lent under the lane.** Search reaches a workspace's index through lake's repository store
(`search::WorkspaceAccess`): it lends the index objects under the workspace store's lane, opening the
store on first use, and lends a repository together with its workspace's index. Publish,
`reindex_artifacts` (run by every push and every `commit_repository_files`, including the Hugging
Face upload route, which runs it off the server thread) and history scans of a workspace repository
update its index under the same lane as the repository's own writes. A release is published to the
index before its record is written and again after, so a build that read the releases before the
record existed applies it afterwards. Deleting a repository removes it from the index. Lock
order: a store's lane, then an index's lock, then search's map lock, which is only held briefly and
never across I/O or a lane wait.

### A failed index write is retried (lake#540)

A publication, a `reindex_artifacts` (every push and commit) or a deletion whose write to a
workspace's index fails (a Zetta error or timeout, the index's room spent) leaves the repository
pending in that workspace's index (`pending`, or `removed` for a deletion), in memory, and the
caller still sees the failure (a push or commit records it and never fails on it). Search's worker
retries the workspace after a delay that starts at 15 s (`Service::set_pending_retry`) and doubles on
each retry that fails, up to 10 minutes; it is reset once nothing is pending. A retry loads the
stored index if it was dropped from memory by the failure, rather than rebuilding it, and refreshes
each pending repository (branch tips, release snapshots, artifacts) under its store's lane as lake's
own work. Until then the index answers what it held before the failed write, and every search that
reads it names the workspace in `stale_workspaces`. What is pending is in memory only: a lake that
stops drops it, and removes that workspace's manifest before it releases the store (lake#536), so
the next process rebuilds the index.

**Loaded on first use, evicted when idle.** A workspace's index is loaded into memory on its first
use, and again when the store is owned under another lease fence. Loaded workspace indexes are
bounded by `LAKE_SEARCH_MEMORY_BYTES` (default 256 MiB, estimated from their text and occurrences):
after each use the least recently used idle ones are evicted and loaded again from their objects
when next used. The one used last, one in use, one being built and one with writes waiting are kept.

**Rebuilt off the request path, never copied.** An index with no manifest (never written, or wiped),
one missing a run, segment or artifact object its manifest names, one naming a blob it holds no text
for, or one that does not authenticate or parse is never served: it is marked as building and its
rebuild is queued on search's own worker thread, never on lake's maintenance thread (a switch
queues it at once), so the usage refresh and the deletion pass never wait on a rebuild. Objects of
an index its manifest does not name (left by a commit that stopped before its manifest) are removed
whenever the index is loaded. Meanwhile searches list
the workspace in `building_workspaces` without taking its lane, and publish, reindex, push and
commit only record the repository as pending.
- The rebuild reads each live repository of the workspace under the store's lane, one repository at
  a time (so a push waits at most one repository's read), as lake's own work: it waits for the lane
  rather than being refused admission when requests fill it. its branch tips as its references name
  them, its release snapshots from its release records (`create_release`'s forge records, lake#341),
  and its published artifacts. It writes the index without holding any lane, in one pass that also
  removes every other `search/` object, then indexes the pending repositories again and removes the
  ones deleted meanwhile (they are also left out of what it writes). A pending repository that
  cannot be read or a deletion that cannot reach the index is kept and tried again after 15 s on the
  worker.
- A failed rebuild is not retried on every request: the workspace answers `building` for a delay that
  grows from 15 s to 10 min, and the next use after it queues the rebuild again. The delay is kept in
  memory only, by decision: a restarted lake tries again at once, which a restart is usually for.
- The repository list has no cursor in the control store; a workspace with 100,000 repositories or
  more is refused rather than rebuilt from a partial list, so a rebuild never sweeps the objects of
  repositories past a limit.
Nothing is copied from the account's index.

**The account's index keeps nothing of a switched workspace.** A switch drops from the account's index on
lake's volume every repository the switched workspace's store now serves, and the history-scan blobs
no segment references; lake does the same for every account at start, removes leftover `.tmp-*`
files, and removes an account's index directory once it holds nothing. The `move_workspace_store`
switch reports the count as `space_index_repositories_dropped`, or `space_index_error` when the
pruning write failed. A hit from the account's index for a repository a workspace store serves is
never returned.

**Deletion.** Deleting a repository removes it from its workspace's index and its account's
(`forget_repository`; a failure is recorded in the deletion's audit details). Deleting a workspace
(lake#514) forgets its index and removes its `search/` objects while lake has the workspace's store
open; otherwise they go with the workspace Zetta, as its store does. A failure to remove them is
logged and never holds the deletion up.

## Index and publication format

The store root is `LAKE_STORE_ROOT` (or `--store-root`). An account's index on lake's volume holds the
repositories of its unswitched workspaces, with one deduplicated blob catalogue:

```text
<root>/<customer>/
  repositories/<repository-id>/       liquid repository, opened through libliquid
  index/blobs.json                    blob digest → UTF-8 content, indexed once
  index/segment-<repository>-<change>-<kind>-<hex-ref>.json
  index/manifest.json                 active {repository,ref,change,kind,segment} entries
```

A change segment contains its `{path,blob}` occurrences, not another copy of blob text. Blobs no
segment or artifact references any more are dropped from `blobs.json` after the manifest that last
named them is replaced. Files that are not UTF-8 text are not indexed. An account's index is loaded on
first use into oramacore-cpp's in-memory BM25F adapters. Content is indexed once per index. Path scores are
built from the occurrence mappings selected for each query, so a tag or historical change cannot
inherit paths from a different snapshot.

Publication replaces the segment for a branch tip. Every push, every `commit_repository_files`
and every drive write (`upload_drive_files`, `create_drive_folder`, `move_drive_path`,
`delete_drive_path`, `restore_drive_version`, `resolve_conflict`) runs `reindex_artifacts`, which
publishes the repository's branch tips again as its references name them now, in an account's index
on lake's volume as in a workspace's (lake#584). Before lake#584 only a workspace's index did: a
account's hot index held a branch only once something published it explicitly, so a file pushed or
saved through a drive was found only by a history scan, and `stale_workspaces` and
`building_workspaces` said nothing. Release creation publishes a
`TagSnapshot` before recording the release: the same tag and change is idempotent, while attempting
to move a tag to another change returns `conflict`. A request for that ref reads only its cold
snapshot, even when a branch has the same name. The version 2 manifest and segments record the
publication kind and collision-safe filename. The loader retains support for version 1 documents,
which are interpreted as branch-tip publications until existing stores have migrated.

Scan mode does not add every historical path mapping to the hot manifest. It asks libliquid for the
requested change or log, walks each snapshot's tree, and uses the change-to-blob mapping as the
query corpus. New blob text is added to the customer catalogue once by digest; later scans and
repositories reuse it. The response names the originating change for every matching path.

On lake's volume the manifest is written to a sibling temporary file, flushed, and renamed over
`manifest.json`; in a workspace it is one atomic `put`, sent after the objects it names were each
answered and followed by one `:barrier` (the bucket's log keeps the order they were sent in, so no
barrier is needed before it, lake#535). New segments and the blobs they add are written before that
replacement. Without that `:barrier`, the index shares the workspace store's power-loss window
until zetta#1235 is fixed ([workspace-store.md](../architecture/workspace-store.md#writes)): the
manifest could survive without an object it names. The index is rebuilt then (a manifest naming a
missing object is not served), so nothing is lost but the time to rebuild it. A query holds one
in-memory manifest state while it gathers candidates; publication swaps in the rebuilt state only
after the replacement succeeds. It consequently observes the previous complete change or the next
complete change, never a mixture.

Lake owns the positional trigram component. For every byte trigram it stores posting lists of
`{blob digest, byte positions}`. A substring intersects trigram postings and validates adjacent
positions before checking the content. Regex queries use the longest literal run of three or more
bytes as the same prefilter, then evaluate the regex; patterns without such a run scan the blob
catalogue. This is intentionally byte-oriented so snippets and reported offsets agree with stored
content. Non-text/binary format extraction belongs to the format registry work in #42.

## Access control and audit

The query path resolves each candidate repository to its policy object, batches the object ids
through `control::authorize_batch(principal, objects, "read", cache)`, and emits only allowed
hits. This is the shared attestation-backed policy engine; `admin` is its operator principal.
**Where search reads repositories (lake#380).** Search reads a repository only through lake's
repository store, which lends it from the store that serves it (the account's store, or a switched
workspace's own store) under that store's lane and on its one open instance
(`search::Service::set_repository_access`). It never opens a second instance of a store: a liquid
open removes the store's temporary files, which another writer may be about to publish. A search
service that never had a repository store opens the account store itself; once one was set, search
refuses instead. Search borrows a repository before it takes its own lock. Every candidate is
authorized in one batch after every index and lane is let go.

Anonymous search, including search of public repositories, is intentionally unavailable: the
registry rejects an unauthenticated request before the query executes. Search therefore satisfies
#40's query-time filtering and anonymous-public-read decision.

Every REST, MCP, and server-rendered-page search call passes through `Registry::call`, which writes
one signed `audit_events` record for the query outcome. The audit details record `mode` and
`scanned_changes`. Sync publication and search open the same repository identity through
`repository_options.store` over
`LAKE_STORE_ROOT/<customer-id>/store`. Push records its route and per-change authorization outcomes;
index publication remains a separate search lifecycle hook.

## Published artifacts

After a repository sync publication, Lake walks retained build outputs through
`liquid::format_registry::global()`. The same audited `reindex_artifacts` action rebuilds records
for artifacts that predate the hook. Recognised Debian packages, wheels, JARs, and OCI tar layers
contribute text and source members to the existing deduplicated index; binary members retain only
their name and logical digest in `index/artifacts.json`.

The registry depth limit is two: a container inside an artifact is opened, while any deeper
container remains a digest-bearing member record without indexed contents. Each output has a 256
MiB decoded-byte budget and 100,000-entry limit. `reindex_artifacts` requires repository `write`
access and returns `{artifacts,members,indexed,binary,truncated}`; OpenAPI and MCP expose that
registry capability automatically.

The `/search` page exposes query, customer, repository, branch or tag, exact change, and complete
history fields, and passes `workspace_id` through. It runs the capability on the asynchronous
registry path, so a workspace's lane or a rebuild never holds lake's server thread. It labels every result set with its query mode and reports the number of examined
changes for a scan.

## Verify

```sh
cmake -S . -B build -G Ninja -DLAKE_BUILD_TESTS=ON \
  -DLAKE_ZETTA_SOURCE_DIR=/path/to/zetta-bridge/modules/zetta
cmake --build build
ctest --test-dir build --output-on-failure
```

`DrivesHttp.ARepositoryDriveWriteIsSearchedDownloadedAndLoaded` (lake#584) writes a file through a
repository drive of an account that is not switched and finds it with hot `search` at once, then
finds it at its new path after `move_drive_path` and not at all after `delete_drive_path`.

`Search.*` covers deduplication across repositories, reload from the persisted manifest, atomic
publication, immutable tag-only results, a matching change among 1,000 scanned changes, batched
filtering, substring/regex matching, and ranked order. `SearchWorkspaces.*` cover workspace indexes
on in-memory objects: two workspaces with separate indexes, a first search answering `building`
without rebuilding inline, a reopened service loading them without writing, an account-wide search that
never lends a workspace outside its permitted accounts, a wiped index and a manifest whose runs,
segments or artifact members are gone rebuilt (without a restart), a failed rebuild backing off, a
rebuild republishing release snapshots, runs written once and not rewritten by a push, reindex
following a moved branch, eviction and reload, a deleted repository leaving the index, a history scan
naming another workspace refused, a rebuild that waits for a busy lane rather than being refused,
writes and deletions during a rebuild applied after it (a refused re-index kept and retried), a
deletion that cannot reach the index retried, orphan objects swept on load, a dropped workspace
keeping no state, and dropping a workspace. `WorkspaceStoreSearch.*`
(`tests/test_workspace_store.cpp`) run through a lake server on the fake Zetta object routes: sealed
indexes of two workspaces with no term in the clear in either bucket or on lake's volume, the switch
dropping the account index's copy, a restart reloading without rewriting, a deleted repository leaving
the index; a non-operator caller in two accounts who never makes search touch the other account's
workspace and gets one answer for another account's workspace and an unknown one; a wiped, partly
lost or tampered index rebuilt with and without a restart; a failed index write leaving the store's
lease and holder as they were; a commit whose index writes are all dropped
(`FakeZettaObjects::drop_matching`) leaving its workspace in `stale_workspaces` until search's worker
retries it by itself and the change is found (lake#540); the first push after a switch not waiting for a slow rebuild; pushes,
commits and searches side by side with 503s bounded; and a deletion removing the index objects and
not held up when their removal fails; the index's room counted against the whole account and what it
wrote since the last usage reading; a usage refresh not waiting on a slow rebuild; a Hugging Face
upload not holding the server thread; and a write-once put retried with the same content.
`WorkspaceStoreZetta.TheSearchIndexOnARealZettaIsSealedReloadsAndIsRebuilt` does the sealing, reload
and rebuild against a real Zetta when `LAKE_TEST_ZETTA_BINARY` names one. The Postgres fixture invokes the registered
capability for hot and exact-change scan requests and proves that a real policy-store batch hides a
denied repository from search results.

## 10,000-file measurement

On an arm64 macOS 14.6.1 host using Homebrew Clang 22.1.8 and the Release configuration generated
by CMake, the direct-libliquid fixture reported the following from the committed benchmark:

| Corpus | Index build | Mean ranked query (100 runs) |
|---|---:|---:|
| 10,000 UTF-8 files | 9,546.94 ms | 6,598.50 us |

The fixture creates the repository through libliquid, cuts one change, measures `publish`, then
measures 100 `benchmark` queries. Run it explicitly because CTest leaves performance measurements
disabled:

```sh
build/tests/lake-tests --gtest_also_run_disabled_tests \
  --gtest_filter='SearchBenchmark.DISABLED_BuildAndQueryTenThousandFiles'
```

## Not yet

- `customer_id` (formerly `customer`) stays an input; the decided API (a required `workspace_id`, `/w/<id>/search`) is a
  later change. `rebuild_search_index` as its own capability is not built; `reindex_artifacts` of
  each repository, or a wiped index, rebuilds.
- Rollback of a workspace's index objects by the workspace Zetta is not detected, as for the store
  ([lake#522](https://github.com/oramasearch/lake/issues/522)); a rollback reads as an older index.

## Change

Keep the manifest replacement as the only operation that makes a segment visible. If the blob or
segment schema changes, version it in the JSON document and retain a reader for the preceding
version until existing customer stores have been migrated. A future persistent BM25F export from
oramacore-cpp can replace the adapter rebuild without changing segment or manifest semantics;
the needed engine interface is tracked in oramacore-cpp #1.
