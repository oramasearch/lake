# Search

`GET /v1/search` is the authenticated repository-search capability. It is registered once and is
therefore also present in `GET /v1/openapi.json` and `POST /mcp` as the `search` tool. The
server-rendered page is `GET /search`; it accepts the same `query` value and never exposes a
result that the capability would reject.

## Request and result

| Input | Meaning |
|---|---|
| `query` | Required. Plain text is ranked BM25F; quoted text is an exact substring; `/expression/` is an ECMAScript regex. |
| `customer` | Optional customer-store directory name. Empty searches all opened customer stores. |
| `repository` | Optional repository id. |
| `ref` | Optional branch or tag name; the hot manifest supplies branch tips. |
| `limit` | Optional result count, default 20 and maximum 1000. |

The value is `{items:[{customer,repository,path,change,snippet,score}]}`. Hits are score-descending
and then repository/path ordered for stable ties. A quoted query and regex query return score `1`;
ranked text returns the combined content and path BM25F score.

```sh
curl -H "Authorization: Bearer $LAKE_ADMIN_TOKEN" \
  --get --data-urlencode 'query="connection refused"' \
  http://127.0.0.1:4900/v1/search
```

## Index and publication format

The store root is `LAKE_STORE_ROOT` (or `--store-root`). A customer owns a liquid store and a
single deduplicated blob catalogue:

```text
<root>/<customer>/
  repositories/<repository-id>/       liquid repository, opened through libliquid
  index/blobs.json                    blob digest → UTF-8 content, indexed once
  index/segment-<repository>-<change>.json
  index/manifest.json                 active {repository,ref,change,segment} entries
```

`blobs.json` is append-only. A change segment contains its `{path,blob}` occurrences, not another
copy of blob text. On startup lake reloads the catalogue and the manifest-selected segments into
oramacore-cpp's in-memory BM25F adapters. Content is indexed once per customer; paths are rebuilt
from the active occurrence mappings so path scores stay current without copying blob content.

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
Anonymous search, including search of public repositories, is intentionally unavailable: the
registry rejects an unauthenticated request before the query executes. Search therefore satisfies
#40's query-time filtering and anonymous-public-read decision.

Search does not yet write `audit_events`: the table and control-store writer are owned by #18 and
are absent from the current schema. When #18 lands, `search` calls must write its documented event
shape for query execution and publication.

## Verify

```sh
cmake -S . -B build -G Ninja -DLAKE_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

`Search.*` covers deduplication across repositories, reload from the persisted manifest, atomic
publication, batched filtering, substring/regex matching, and ranked order.

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
