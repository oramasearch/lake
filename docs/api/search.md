# Search

`GET /v1/search` is the authenticated repository-search capability. It is registered once and is
therefore also present in `GET /v1/openapi.json` and `POST /mcp` as the `search` tool. The
server-rendered page is `GET /search`; it accepts the capability's scope values and never exposes
a result that the capability would reject.

## Request and result

| Input | Meaning |
|---|---|
| `query` | Required. Plain text is ranked BM25F; quoted text is an exact substring; `/expression/` is an ECMAScript regex. |
| `customer` | Optional space id. Searches the space's index on lake's volume and the index of each of its switched workspaces. Empty searches every space. |
| `workspace_id` | Optional. Searches only that switched workspace's index (lake#381). An unswitched or deleted workspace is `invalid_input`. |
| `repository` | Optional repository id. |
| `ref` | Optional branch or tag name. An immutable tag snapshot takes precedence over a branch with the same name. |
| `change` | Optional exact change digest. Requires `customer` and `repository`, cannot be combined with `ref` or `history`, and uses scan mode. |
| `history` | Optional boolean. Walk every change in one `customer` and `repository`; cannot be combined with `ref` or `change`. |
| `limit` | Optional result count, default 20 and maximum 1000. |
| artifact scope | Append `in:artifact:<name>[@<version>]` to `query`. The name is a build output path and the version is its immutable result digest. This scope cannot be combined with `ref`, `change`, or `history`. |

The value is
`{mode,scanned_changes,skipped_workspaces,items:[{customer,repository,path,change,snippet,score,artifact,artifact_version,digest,line,workspace_id}]}`.
`workspace_id` names the workspace whose index answered a hit (empty for the space's index on lake's
volume). `skipped_workspaces` lists the switched workspaces a space-wide search could not read
(stopped, its store unavailable or moving); a search that names a workspace or a repository fails
instead. `mode` explains which corpus answered the request:

| Mode | Corpus |
|---|---|
| `hot` | Current branch-tip segments. This is the default when neither `ref`, `change`, nor `history` selects another corpus. |
| `snapshot` | The immutable segment built once for the requested release or tag. |
| `scan` | An exact change or the repository's complete change log. `scanned_changes` reports how many snapshots were examined. |
| `artifact` | Members of the selected published output. Hits name the artifact, result version, logical member digest, and matching one-based line. |

Hits are score-descending and then repository/path ordered for stable ties. A quoted query and
regex query return score `1`; ranked text returns the combined content and path BM25F score.

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
| A switched workspace's store (`workspaces.store_moved_at` set) | Objects `search/<file>` in bucket `lake_store` of the workspace Zetta, beside its liquid store | Sealed with the workspace's data key by the workspace store's backend (below) |
| Its space's store on lake's volume | `<root>/<space>/index/` | Plain files (below) |

**Beside the workspace store.** A switched workspace's catalogue, segments, artifact members and
manifest are the same documents as below, kept as the objects `search/blobs.json`,
`search/segment-….json`, `search/artifacts.json` and `search/manifest.json` in the workspace store's
own bucket. They are written through a view of the store's `ZettaObjectBackend`
(`search::make_store_index_objects`), so each is encrypted with the workspace's data key exactly as
the store's objects are ([workspace-store.md](../architecture/workspace-store.md#encryption-below-liquid-s3);
the associated data binds the object's key, so a segment cannot be moved under another name), and
every write carries the store's lease fence: the lake that owns the store is the only writer of its
index, and the manifest is replaced with one atomic `put`. Liquid never lists or writes under
`search/`. A prefix in the store's bucket rather than a bucket of its own: one lease and fence cover
the store and its index (a second bucket would need its own lease, or would take writes from any
process), the index needs no key of its own, it counts in the workspace's usage and quota, and it
goes with the bucket. No index content of a switched workspace is written to lake's volume or to
PostgreSQL.

**Lent, never opened.** Search reaches a workspace's index only through lake's repository store
(`search::WorkspaceAccess`): it lends the index objects under the workspace store's lane, opening the
store on first use, and lends a repository together with its workspace's index. Publish,
`reindex_artifacts` and history scans of a workspace repository therefore write its index under the
same lane as the repository's own writes. Search takes a lane before any lock of its own and never
holds its map lock across I/O or a lane wait; each index has its own lock.

**Loaded on first use, evicted when idle.** A workspace's index is loaded into memory on its first
use, and again when the store is owned under another lease fence (another lake may have written it).
Loaded workspace indexes are bounded by `LAKE_SEARCH_MEMORY_BYTES` (default 256 MiB, estimated from
their text and occurrences): after each use the least recently used idle ones are evicted, and loaded
again from their objects when next used. The one used last is kept.

**Rebuilt, never copied.** An index with no manifest (never written, or wiped), one missing a segment
its manifest names, or one that does not authenticate or parse is rebuilt from the workspace's
repositories: each live repository's branch tips as its references name them and its published
artifacts, written in one pass that also removes every other `search/` object. It is never served
empty and nothing is copied from the space's index. In a workspace index, `reindex_artifacts`
(which a push runs) also publishes the repository's branch tips again, so the index follows every
push; tag snapshots stay as their release published them. Releases created before an index was wiped
are not republished by a rebuild (see Not yet).

**The space's index keeps nothing of a switched workspace.** A switch drops from the space's index on
lake's volume every repository the switched workspace's store now serves, and the history-scan blobs
no segment references; lake does the same for every space at start, and removes a space's index
directory once it holds nothing. The `move_workspace_store` switch reports the count as
`space_index_repositories_dropped`. A hit from the space's index for a repository a workspace store
serves is never returned.

**Deletion.** Deleting a workspace (lake#514) forgets its index and removes its `search/` objects
while lake has the workspace's store open; otherwise they go with the workspace Zetta, as its store
does.

## Index and publication format

The store root is `LAKE_STORE_ROOT` (or `--store-root`). A space's index on lake's volume holds the
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
named them is replaced. Files that are not UTF-8 text are not indexed. A space's index is loaded on
first use into oramacore-cpp's in-memory BM25F adapters. Content is indexed once per index. Path scores are
built from the occurrence mappings selected for each query, so a tag or historical change cannot
inherit paths from a different snapshot.

Publication replaces the segment for a branch tip. Release creation publishes a
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
`manifest.json`; in a workspace it is one atomic `put` after a barrier. New segments and the blobs
they add are written before that replacement. A query holds one
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
repository store, which lends it from the store that serves it (the space's store, or a switched
workspace's own store) under that store's lane and on its one open instance
(`search::Service::set_repository_access`). It never opens a second instance of a store: a liquid
open removes the store's temporary files, which another writer may be about to publish. A search
service that never had a repository store opens the space store itself; once one was set, search
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

`Search.*` covers deduplication across repositories, reload from the persisted manifest, atomic
publication, immutable tag-only results, a matching change among 1,000 scanned changes, batched
filtering, substring/regex matching, and ranked order. `SearchWorkspaces.*` cover workspace indexes
on in-memory objects: two workspaces with separate indexes, a reopened service loading them without
writing, a wiped index or a missing segment rebuilt, an on-demand rebuild removing stale objects,
reindex following a moved branch, eviction under the memory bound and reload, and dropping a
workspace. `WorkspaceStoreSearch.*` (`tests/test_workspace_store.cpp`) run them through a lake server
on the fake Zetta object routes: two workspaces of one space with sealed indexes and no term in the
clear in either bucket or on lake's volume, the switch dropping the space index's copy, a restart
reloading without rewriting, a wiped, partly lost or tampered index rebuilt, pushes, commits and
searches side by side in one workspace, and a deletion removing the index objects.
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

- A rebuild republishes branch tips and artifacts, not tag snapshots: a release's snapshot lives in
  the index from its creation, and one created before its workspace was switched or its index wiped
  is not republished from the release records.
- `customer` stays an input; the decided API (a required `workspace_id`, `/w/<id>/search`) is a
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
