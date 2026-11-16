# Repository sync protocol

**What.** The HTTP/1.1 protocol used by libliquid clients and lake to exchange repository
metadata and content. This page mirrors liquid's `docs/library/sync.md` at the pinned revision and
records lake's adapter-specific authorization, storage, and audit behavior.

**Status: implemented.** liquid #160, #165, and #192 provide the bounded client, public
`sync_server`, and server-derived per-reference authorization and outcome contracts. Lake
registers their operations through its capability registry, streams the binary bodies in its one
HTTP adapter translation unit, and stores all repositories owned by a customer in that customer's
caller-owned liquid store.

**Where a repository's content is (lake#380).** A repository of a switched workspace
(`workspaces.store_moved_at` set by `move_workspace_store`) is served from that workspace's own
encrypted store in its workspace Zetta ([architecture/workspace-store.md](../architecture/workspace-store.md));
every other repository from its account's store on lake's volume. The protocol and the client do not
change. A workspace store opens on the first sync that needs it; while another process holds its
bucket lease (a hand-off, or after this lake lost the lease) its repositories answer
`503 store_unavailable`, and repositories of other workspaces are unaffected. Pushes into a
workspace store need liquid `8955519` or later, which writes each index run once (liquid#332).

## Repository URL and requests

The URL passed to `liquid clone`, `liquid push` or `liquid pull` identifies one repository. For
lake it is:

```text
<configured-external-origin>/v1/repositories/<repository-id>
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
`objects`. A push request may add `atomic: true`. A successful push reply may add `outcomes`, an
array in request-ref order. Each entry is `{name, outcome: "applied"}` or
`{name, outcome: "refused", reason}`. A push header cursor is the end of the exported client
journal. A pull request cursor is the last server cursor observed by that client. A response
cursor is the next opaque cursor to persist for that peer. `objects` contains roots referenced by
the metadata stream. Missing-object negotiation walks their content-addressed graph and sends only
objects the receiver reports absent.

An optional `Authorization: Bearer <token>` header carries lake authentication. The CLI reads the
token from `LIQUID_TOKEN`; credentials never appear in the URL or output.
The repository page uses `LAKE_EXTERNAL_ORIGIN`, or `https://` plus `LAKE_PUBLIC_HOST` when the
explicit origin is unset, for its examples; see
[`../serving/configuration.md`](../serving/configuration.md). Obtain a token from that same instance.
Use `liquid clone <url> ./checkout` for an initial materialized checkout, then `cd ./checkout`.
`liquid pull` and `liquid push` require an existing local Liquid repository in the current
directory. The native browser-to-terminal proof and private runbook are in
[`../deployment/private-install.md`](../deployment/private-install.md).

A client may send its liquid release as `Liquid-Version: <MAJOR.MINOR.PATCH>`. One outside the
range `server_info` publishes under `liquid` is refused with HTTP 400 before the sync starts, in
this protocol's error body; see [liquid-clients.md](liquid-clients.md) (lake#1324).

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

Each change is authorized against the pushing credential and recorded as a `push_change` audit
event. Its details name the change's signer (`author`, the TAAP identity in the change), the
account that identity acts for (`author_account`: an account's own identifier, or the account an
own identity is linked to, [identity.md](identity.md#your-own-identity)) and whether that is the
pushing account (`author_is_caller`). A change signed by someone else is still accepted when policy
allows the pusher to write; the event says so (lake#762).

Before import changes state, the receiver parses every frame, verifies each actor identifier and
TAAP signature, checks each change digest and referenced object, applies every selected directory
message to candidate documents through the checked patch-returning path, and validates accepted
refs. A validation failure rejects the selected import. The error `item` is the zero-based
metadata-frame index that failed.

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
succeed. A reply that reports refused references is still successful and acknowledges the batch;
the refused refs are offered again on later pushes.

## Public server and client contract

`sync_server(repository&, store&, options)` requires the repository to use that store. The
repository-only constructor uses the repository's existing store and is intended for adapters such
as the CLI. Its task-returning handlers accept or produce byte spans for push, pull, missing-object
queries, pack-frame uploads, and logical range reads; the canonical codecs remain private.
`sync_server::encode_error` produces the error body described above so an adapter need only choose
the transport status.

`sync_server_options::authorize` is an optional asynchronous callback for changes and reference
updates. The callback receives the metadata item plus the affected `sync_ref_update` values and
returns exactly one named `sync_ref_verdict` for each update. Each update names the ref, supplies
its server-derived prior and proposed heads, and reports whether the old head is reachable from the
new head. Push computes this context before invoking callbacks in frame order and before repository
import. A refused verdict applies only to that ref and preserves its reason string; a callback task
error refuses every affected ref. Object uploads that arrived earlier remain content-addressed data
eligible for collection. Embedders can perform authorization and audit work without reimplementing
the wire format.

The server imports shared metadata with accepted ref frames and leaves refused refs at their
pre-push heads. With `atomic: true`, any refusal prevents metadata import. Originally refused refs
keep their reasons, while otherwise accepted refs report
`atomic push refused because another reference was refused`.

`repository::export_drips` writes as many `sync_item` values as fit in the supplied span and returns
the next cursor plus an end flag. `repository::import_drips` accepts a span, verifies it as one
atomic batch, and reports the number imported. `sync_session` sends or receives an object graph
through a `remote_blob_source` after `find_missing`. `sync_endpoint` implements that source and the
push/pull requests over vendored `oramasearch/http`. A caller may put its last observed cursor and
the push's `atomic` choice in `sync_endpoint_options`. The returned `sync_report::references`
contains every reference outcome and reason, and the report supplies the next cursor to retain. All
waiting operations are Capy tasks.

## Lake adapter

The registry entries are `refs`, `find_missing`, `push`, `pull`, `upload_chunks`, and `read_chunk`.
`refs` is the ordinary JSON read capability. The five binary operations use the repository-scoped
routes above. OpenAPI exposes their binary media types; the push operation also publishes
`x-liquid-sync-header-schema` for the `atomic` request field and the response's `outcomes` entries.
The binary operations are omitted from MCP because MCP tool arguments cannot carry the request and
response streams; MCP clients use the repository-scoped REST route and receive the outcome shape
described above. Every route still calls `Registry::call`, so credential resolution, repository
policy, and signed outcome audit use the same path as JSON capabilities. Request buffering is
bounded at 64 MiB and accepts fixed-length or HTTP chunked bodies before passing one byte span to
`sync_server`.

Push additionally calls the policy engine with the repository object and `write` action once for
each change through `sync_server_options::authorize`. Before the push, lake reads the
repository's branch protection rules (and the reviews or releases they ask for) from the
repository's workspace Zetta, where forge records live (lake#341); if they cannot be read, every
reference update of the push is refused with that reason (rule `branch_protection_unavailable`).
On each reference callback, lake evaluates the server-derived update against that branch protection
with the authenticated caller. The callback records `forge.branch_protection.allowed` or
`forge.branch_protection.refused` for protected refs; the audit detail has the repository, ref, caller, rule, and reason but never a credential. The
registry rejects an anonymous or unauthorized request before callbacks run. Lake keeps no copy of
the refs a push lands: the liquid repository is their only record, and `refs` reads it (the
`repository_refs` table was dropped by migration `0064`, lake#319). Pull and chunk reads
require `read`; push and chunk upload require `write`.

The five binary paths add accepted request and returned response byte counts to the customer's
current UTC-hour metering row. Chunk writes also sample the store's exact unique raw bytes.
`LAKE_STORAGE_QUOTA_BYTES` limits that gauge: a write that would cross it returns `busy` (HTTP
429), while reads continue and existing bytes stay complete. See [metering.md](metering.md).

Lake returns one verdict per reference from branch-protection evaluation. A normal push applies
accepted refs, leaves refused refs unchanged, and returns every outcome with the precise
reference-and-rule reason. An atomic push with any refusal changes no ref and still reports every
outcome. Shared metadata imports only when at least one ref can apply; uploaded objects remain in
the content-addressed store regardless of the outcomes.

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
applies an unprotected ref beside a protected refusal, preserves the exact refusal reason, and
keeps both refs unchanged for the same atomic push while reporting both outcomes. `PostgresSync.*`
repeats push and pull with the production Postgres control store. The full macOS suite and Linux
image build run with the rest of the proof documented in [`../build/local.md`](../build/local.md).
