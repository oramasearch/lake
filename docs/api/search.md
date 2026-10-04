# Search

`GET /v1/search` is the authenticated repository-search capability. It is registered once and is
therefore also present in `GET /v1/openapi.json` and `POST /mcp` as the `search` tool. The
server-rendered page is `GET /search`; it accepts the capability's scope values and never exposes
a result that the capability would reject.

## Request and result

| Input | Meaning |
|---|---|
| `query` | Required. Plain text is ranked BM25F; quoted text is an exact substring; `/expression/` is an ECMAScript regex. |
| `customer` | Optional customer-store directory name. Empty searches all opened customer stores. |
| `repository` | Optional repository id. |
| `ref` | Optional branch or tag name. An immutable tag snapshot takes precedence over a branch with the same name. |
| `change` | Optional exact change digest. Requires `customer` and `repository`, cannot be combined with `ref` or `history`, and uses scan mode. |
| `history` | Optional boolean. Walk every change in one `customer` and `repository`; cannot be combined with `ref` or `change`. |
| `limit` | Optional result count, default 20 and maximum 1000. |
| artifact scope | Append `in:artifact:<name>[@<version>]` to `query`. The name is a build output path and the version is its immutable result digest. This scope cannot be combined with `ref`, `change`, or `history`. |

The value is
`{mode,scanned_changes,items:[{customer,repository,path,change,snippet,score,artifact,artifact_version,digest,line}]}`. `mode` explains
which corpus answered the request:

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

## Index and publication format

The store root is `LAKE_STORE_ROOT` (or `--store-root`). A customer owns a liquid store and a
single deduplicated blob catalogue:

```text
<root>/<customer>/
  repositories/<repository-id>/       liquid repository, opened through libliquid
  index/blobs.json                    blob digest → UTF-8 content, indexed once
  index/segment-<repository>-<change>-<kind>-<hex-ref>.json
  index/manifest.json                 active {repository,ref,change,kind,segment} entries
```

`blobs.json` is append-only. A change segment contains its `{path,blob}` occurrences, not another
copy of blob text. On startup lake reloads the catalogue and the manifest-selected segments into
oramacore-cpp's in-memory BM25F adapters. Content is indexed once per customer. Path scores are
built from the occurrence mappings selected for each query, so a tag or historical change cannot
inherit paths from a different snapshot.

Sync publication replaces the segment for a branch tip. Release creation publishes a
`TagSnapshot` before recording the release: the same tag and change is idempotent, while attempting
to move a tag to another change returns `conflict`. A request for that ref reads only its cold
snapshot, even when a branch has the same name. The version 2 manifest and segments record the
publication kind and collision-safe filename. The loader retains support for version 1 documents,
which are interpreted as branch-tip publications until existing stores have migrated.

Scan mode does not add every historical path mapping to the hot manifest. It asks libliquid for the
requested change or log, walks each snapshot's tree, and uses the change-to-blob mapping as the
query corpus. New blob text is added to the customer catalogue once by digest; later scans and
repositories reuse it. The response names the originating change for every matching path.

The manifest is written to a sibling temporary file, flushed, and renamed over `manifest.json`.
Segments and the append-only blob catalogue are written before that replacement. A query holds one
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
service without a repository store opens the space store itself. The index files stay on lake's
volume until lake#381.

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
history fields. It labels every result set with its query mode and reports the number of examined
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
filtering, substring/regex matching, and ranked order. The Postgres fixture invokes the registered
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

## Change

Keep the manifest replacement as the only operation that makes a segment visible. If the blob or
segment schema changes, version it in the JSON document and retain a reader for the preceding
version until existing customer stores have been migrated. A future persistent BM25F export from
oramacore-cpp can replace the adapter rebuild without changing segment or manifest semantics;
the needed engine interface is tracked in oramacore-cpp #1.
