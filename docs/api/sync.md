# Repository sync protocol

**What.** The HTTP/1.1 protocol used by libliquid clients and lake to exchange
repository metadata and content. This page mirrors liquid's
`docs/library/sync.md` at the pinned libliquid revision.

**Status.** liquid #160 publishes the client contract. Lake consumes that
revision, but cannot register its server endpoints until libliquid exposes the
server-side repository/store binding and frame codec needed to uphold the
customer-store contract. See **Change**.

## Repository URL and requests

The URL passed to `liquid push` or `liquid pull` identifies one repository.
Clients append these paths. Requests and responses use
`Content-Type: application/vnd.liquid.sync-v1`; they may use `Content-Length`
or HTTP/1.1 chunked transfer coding, and frame boundaries do not depend on HTTP
read boundaries.

| Method and path | Request body | Successful response |
|---|---|---|
| `POST <repository>/sync/push` | client metadata stream | acknowledgement header, optionally followed by metadata the client imports |
| `POST <repository>/sync/pull` | header frame containing client cursor and refs | metadata stream exported after that cursor |
| `POST <repository>/sync/find-missing` | header frame containing `objects` | header frame containing the missing subset |
| `POST <repository>/sync/chunks` | zero or more pack-entry frames | empty body |
| `GET <repository>/sync/chunks/<digest>?offset=<n>&length=<n>` | none | raw verified logical bytes for the requested range |

The metadata header is a canonical-CBOR map with `v: 1`, `cursor`, sorted
`refs`, and sorted `objects`. A push header cursor is the end of the exported
client journal; a pull cursor is the last server cursor the client observed; a
response cursor is the next opaque cursor to persist for that peer. `objects`
contains roots referenced by the metadata stream. Missing-object negotiation
walks their content-addressed graph and sends only objects the receiver reports
absent. `Authorization: Bearer <token>` is optional and the CLI obtains it from
`LIQUID_TOKEN`; credentials never appear in a URL or output.

## Metadata stream

Every frame has a one-byte type, a four-byte unsigned big-endian payload length,
and that many bytes. Receivers reject unknown types, a length above 16 MiB,
truncated frames, bytes after the end frame, or a stream without an end frame.

| Type | Name | Canonical-CBOR payload |
|---:|---|---|
| 1 | header | version, cursor, refs, and object roots |
| 2 | drip | `{author, body, signature}`; `body` is the exact signed canonical drip body |
| 3 | change | `{author, id, body}`; signed change bytes, digest, and verifiable TAAP key |
| 4 | directory | `{repository, node, root, message}`; two 16-byte IDs, root marker, automerge v1 sync message |
| 5 | ref | `{name, change}` |
| 255 | end | empty |

Drips retain journal order; a change follows the drips it cuts; directory
messages use the checked CRDT receive path; and refs apply last. An empty
repository adopts incoming repository and root IDs. A non-empty repository with
a different identity rejects the batch. Duplicate signed records and observed
CRDT messages are valid, so retry is idempotent.

Before import changes state, the receiver parses every frame, verifies every
actor identifier and TAAP signature, checks every change digest and referenced
object, applies each directory message to candidate documents through the
checked patch-returning path, and validates refs. A failure rejects the whole
batch. The error `item` identifies the zero-based metadata frame that failed.

## Chunk stream

Chunk uploads concatenate unencrypted physical pack-entry frames, without a
pack-file header, table, or footer:

```text
8 bytes  "LQFRAME1"
4 bytes  uncompressed length, little-endian
4 bytes  compressed length, little-endian
1 byte   digest algorithm
32 bytes digest
N bytes  zstd payload
```

The receiver bounds both lengths, decompresses the payload, verifies the
algorithm-tagged digest, and writes logical bytes through the ordinary store
path. Trust-domain encryption, if configured, is applied by the receiving
store. Encrypted physical frames are never transferred. A range response is
logical content, verified as part of the complete object read.

## Errors

Non-success responses use `Content-Type: application/vnd.liquid.error-v1` and
canonical CBOR `{v: 1, code, message, context, item?}`. `code` is the stable
`liquid::errc` name.

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

An interrupted request has no success response and retries from the last
acknowledged cursor. Servers do not advance that cursor until metadata import
and required chunk publication both succeed.

## libliquid contract

`repository::export_drips` writes `sync_item` values into a caller span and
returns the next cursor and end flag. `repository::import_drips` accepts a span,
verifies it as one atomic batch, and reports imported count. `sync_session`
sends or receives an object graph through `remote_blob_source` after
`find_missing`. `sync_endpoint` implements that source and the push/pull
requests over vendored `oramasearch/http`; `sync_endpoint_options` takes a last
cursor and the report returns the next cursor. Waiting operations are Capy tasks.

## Verify

libliquid tests exchange two on-disk repositories through an in-process server
double, count bytes, prove a second exchange transfers only new chunks, corrupt
a TAAP signature and require atomic rejection, and run the CLI against that
double. Lake will repeat that proof against its own server when the required
server-facing libliquid surface is available.

## Change

Lake requires one `liquid::store` per customer at
`LAKE_STORE_ROOT/<customer id>/store`. The current public
`repository::create/open` API instead creates or opens
`<repository path>/.liquid-store`, with no overload accepting an existing
customer `store`. `sync_session` can operate on a `store`, but metadata import
and export require a `repository` connected to that same store. The CBOR
metadata and pack-frame codec is private implementation detail, so lake cannot
reach the public `sync_item` boundary without depending on it.

libliquid must expose repository creation/opening over a supplied `store` and
protocol encode/decode helpers (or a server endpoint abstraction). Lake can then
add registry capabilities and streaming adapter routes without a second store or
duplicating a private codec.
