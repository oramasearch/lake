# Repository sync protocol

**What.** The HTTP/1.1 protocol used by libliquid clients and lake to exchange repository
metadata and content. This page mirrors liquid's `docs/library/sync.md` at the pinned revision and
records lake's adapter-specific authorization, storage, and audit behavior.

**Status: implemented.** liquid #160, #165, and #184 provide the bounded client, public
`sync_server`, and server-derived reference-update authorization contracts. Lake registers their
operations through its capability registry, streams the binary bodies in its one HTTP adapter
translation unit, and stores all repositories owned by a customer in that customer's caller-owned
liquid store.

## Repository URL and requests

The URL passed to `liquid push` or `liquid pull` identifies one repository. For lake it is:

```text
https://<lake>/v1/repositories/<repository-id>
```

Clients append the paths below. Requests and responses use
`Content-Type: application/vnd.liquid.sync-v1` and may use either `Content-Length` or HTTP/1.1
chunked transfer coding. Frame boundaries are independent of HTTP read boundaries.

| Method and path | Request body | Successful response |
|---|---|---|
| `POST <repository>/sync/push` | client metadata stream | acknowledgement header, optionally followed by server metadata the client must import |
| `POST <repository>/sync/pull` | one header frame containing the client cursor and refs | metadata stream exported after that cursor |
| `POST <repository>/sync/find-missing` | one header frame containing `objects` | one header frame containing the missing subset |
| `POST <repository>/sync/chunks` | zero or more pack-entry frames | empty body |
| `GET <repository>/sync/chunks/<digest>?offset=<n>&length=<n>` | none | raw verified logical bytes for the requested range |

The metadata header is a canonical-CBOR map with `v: 1`, `cursor`, sorted `refs`, and sorted
`objects`. A push header cursor is the end of the exported client journal. A pull request cursor is
the last server cursor observed by that client. A response cursor is the next opaque cursor to
persist for that peer. `objects` contains roots referenced by the metadata stream. Missing-object
negotiation walks their content-addressed graph and sends only objects the receiver reports absent.

An optional `Authorization: Bearer <token>` header carries lake authentication. The CLI reads the
token from `LIQUID_TOKEN`; credentials never appear in the URL or output.

An offline publisher also sends `Liquid-Offline-Grant: <grant-uuid>`. The grant is context for
publication reauthorization, never a replacement for the ordinary credential. Push verifies its
current ownership, device binding, expiry, and revocation state, then checks current write policy
per change. `Liquid-Reauthorization` returns a base64url JSON array of checked change IDs, allowed
flags, and exact explanations on both success and refusal. See [`offline.md`](offline.md).

## Metadata stream

Every metadata frame has a one-byte type, a four-byte unsigned big-endian payload length, and that
many payload bytes. Receivers reject unknown types, a length above 16 MiB, truncated frames, bytes
after the end frame, or a stream without an end frame.

| Type | Name | Canonical-CBOR payload |
|---:|---|---|
| 1 | header | the version, cursor, refs, and object roots described above |
| 2 | drip | `{author, body, signature}`; `body` is the exact signed canonical drip body |
| 3 | change | `{author, id, body}`; signed change bytes, digest, and verifiable TAAP key |
| 4 | directory | `{repository, node, root, message}`; two 16-byte IDs, a root marker, and an automerge v1 sync message |
| 5 | ref | `{name, change}` |
| 255 | end | empty |

Drips retain journal order. A change follows the drips it cuts. Directory messages use the
existing checked CRDT receive path and are associated with their directory by `node`. Ref frames
apply last. An empty repository adopts the incoming repository and root IDs; a non-empty repository
with a different identity rejects the batch. Duplicate signed records and already-observed CRDT
messages are valid, making retry idempotent.

Repository checkpoints keep compact bytes that can be opened either as the shallow active document
or as its full-history archive. Import opens a candidate from the full archive before receiving
peer messages. A retry can therefore name a dependency older than the active checkpoint heads
without contradicting the shallow document's retained actor sequence.

Before import changes state, the receiver parses every frame, verifies each actor identifier and
TAAP signature, checks each change digest and referenced object, applies every directory message to
candidate documents through the checked patch-returning path, and validates all refs. Any failure
rejects the whole batch. The error `item` is the zero-based metadata-frame index that failed.

## Chunk stream

Chunk uploads concatenate the store's unencrypted physical pack-entry frames without a pack file
header, table, or footer:

```text
8 bytes  "LQFRAME1"
4 bytes  uncompressed length, little-endian
4 bytes  compressed length, little-endian
1 byte   digest algorithm
32 bytes digest
N bytes  zstd payload
```

The receiver bounds both lengths, decompresses the payload, verifies its algorithm-tagged digest,
and stores the logical bytes through the ordinary store path. Trust-domain encryption, when
configured, is applied by the receiving store; encrypted physical frames are never sent. A chunk
range response is logical content rather than a partial compressed frame and is verified as part
of the complete object read.

## Errors

Non-success responses use `Content-Type: application/vnd.liquid.error-v1` and a canonical-CBOR map
`{v: 1, code, message, context, item?}`. `code` is the stable `liquid::errc` name.

| HTTP status | liquid error |
|---:|---|
| 400 | `invalid_argument`, `corrupt` |
| 401 | `unauthorized` |
| 404 | `not_found` |
| 409 | `already_exists`, `conflict` |
| 423 | `locked` |
| 429 | `busy` |
| 503 | `unavailable` |
| 500 | `io`, `internal` |

An interrupted request has no success response and is retried from the last acknowledged cursor.
Servers do not advance that cursor until metadata import and required chunk publication both
succeed.

## Public server and client contract

`sync_server(repository&, store&, options)` requires the repository to use that store. The
repository-only constructor uses the repository's existing store and is intended for adapters such
as the CLI. Its task-returning handlers accept or produce byte spans for push, pull, missing-object
queries, pack-frame uploads, and logical range reads; the canonical codecs remain private.
`sync_server::encode_error` produces the error body described above so an adapter need only choose
the transport status.

`sync_server_options::authorize` is an optional asynchronous callback for changes and reference
updates. The callback receives the metadata item plus the affected `sync_ref_update` values. Each
update names the ref, supplies its server-derived prior and proposed heads, and reports whether the
old head is reachable from the new head. Push computes this context before invoking callbacks in
frame order and before repository import. A callback failure returns that error and leaves the
complete metadata batch unchanged; object uploads that arrived earlier remain unreferenced
content-addressed data eligible for collection. Embedders can perform authorization and audit work
without reimplementing the wire format.

`repository::export_drips` writes as many `sync_item` values as fit in the supplied span and returns
the next cursor plus an end flag. `repository::import_drips` accepts a span, verifies it as one
atomic batch, and reports the number imported. `sync_session` sends or receives an object graph
through a `remote_blob_source` after `find_missing`. `sync_endpoint` implements that source and the
push/pull requests over vendored `oramasearch/http`. A caller may put its last observed cursor in
`sync_endpoint_options`; the returned report supplies the next cursor to retain. All waiting
operations are Capy tasks.

## Lake adapter

The registry entries are `refs`, `find_missing`, `push`, `pull`, `upload_chunks`, and `read_chunk`.
`refs` is the ordinary JSON read capability. The five binary operations use the repository-scoped
routes above, appear in OpenAPI with their binary media types, and are omitted from MCP because MCP
tools accept JSON input rather than streaming bodies. Every route still calls `Registry::call`, so
credential resolution, repository policy, and signed outcome audit use the same path as JSON
capabilities. Request buffering is bounded at 64 MiB and accepts fixed-length or HTTP chunked
bodies before passing one byte span to `sync_server`.

Push additionally calls the policy engine with the repository object and `write` action once for
each change through `sync_server_options::authorize`. On each reference callback, lake evaluates
the server-derived update against branch protection with the authenticated caller. The callback
records `forge.branch_protection.allowed` or `forge.branch_protection.refused` for protected refs;
the audit detail has the repository, ref, caller, rule, and reason but never a credential. The
registry rejects an anonymous or unauthorized request before callbacks run. Successful push
refreshes the control plane's `repository_refs` rows from the live repository. Pull and chunk reads
require `read`; push and chunk upload require `write`.

Liquid #184 rejects the complete metadata batch when a reference callback refuses it. Lake therefore
returns the precise reference-and-rule reason to the sync client and leaves every ref in that push
unchanged. Liquid #189 will make reference application independent by default, with `atomic` for a
client that requires all-or-nothing application.

Lake opens one caller-owned `liquid::store` at
`LAKE_STORE_ROOT/<customer-id>/store` and passes it through `repository_options.store` when opening
every repository for that customer. Repository identity markers live beneath
`LAKE_STORE_ROOT/<customer-id>/repositories/`; liquid-owned metadata, packs, and indexes remain in
the shared customer store. The adapter does not create repository-local `.liquid-store` folders.

## Local server command

`liquid serve sync <path>` opens an existing repository and serves the five routes above with the
vendored `oramasearch/http` router and serializer. It binds `127.0.0.1:8080` by default;
`--listen <address>` and `--port <port>` select another listener. The command deliberately supplies
no authentication policy. Keep the loopback default for local exchange, or embed `sync_server`
with an authorization callback when exposing a service boundary.

## How to verify

`SyncHttp.*` starts lake, exchanges two on-disk repositories through a real `sync_endpoint`, proves
that a second push transfers only newly introduced chunks and an unchanged push transfers none,
and compares the pulled tree and file bytes. It also corrupts a TAAP signature, denies a writer,
and verifies atomic rejection plus signed audit outcomes. `PostgresSync.*` repeats push and pull
with the production Postgres control store. The full macOS suite and Linux image build run with the
rest of the proof documented in [`../build/ci.md`](../build/ci.md).
