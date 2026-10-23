# Transparency log and TUF metadata

## What

Lake serves the four top-level roles from The Update Framework (TUF) 1.0: `root`, `targets`,
`snapshot`, and `timestamp`. The roles protect repository clients from key compromise, stale
metadata, rollback, and combinations of metadata from different publication states. A signed
checkpoint binds the current root and timestamp metadata together with the tiled attestation log's
Merkle root and tree size. Its configured mode determines whether Lake may serve it immediately,
must first obtain witness cosignatures, or must also retain an OpenTimestamps-style aggregation
proof. RFC 3161 trusted timestamps and RFC 4998 evidence renewal are implemented by lake #36.
Asynchronous replication is implemented by lake #38, and logged durable-agent delegation is
implemented by lake #39 under the [agent-session contract](agents.md).

The log accepts batches of canonical-CBOR TAAP attestations encoded as base64url. Lake decodes each
entry through libliquid, verifies its registered profile and its TAAP signature under the key the
attestation carries, hashes it as an RFC
6962-style leaf, and advances the append-only SHA-256 Merkle tree. Entry objects and interior
Merkle hashes are grouped into immutable tiles of 1,024 values. Partial tiles are versioned by
their exact width, so a tile object that a monitor has fetched is never replaced.

The log records statements; it does not attribute them (liquid#383). Like an audit entry, an
attestation entry proves only that it is well formed and was signed by the key it names. Whether that
key belongs to the named issuer is for whoever reads the entry to decide, against the issuer's
identity chain (`liquid identity trust`, or a chain carried with the statement). An identity scan
therefore lists every statement made in a name, including a forged one, which is what lets the
owner of that name monitor it.

Append validates the signed envelope and registered profile at the attestation's stated time. It
does not discard a historically valid statement because its freshness window later elapsed or its
status later changed; status evidence remains a consumer policy input and revocation statements
must themselves remain visible in the append-only history.

Lake never accepts a private TUF key. Root and targets signing can remain offline, and the online
snapshot and timestamp signers can live in a separate operator-owned process. The API accepts
complete signed metadata, verifies it, and appends it only after the role thresholds and cross-role
bindings pass.

The implementation follows the [TUF specification](https://theupdateframework.github.io/specification/latest/).
Lake consumes liquid's public transparency client, checkpoint-trust, tile, inclusion, and
consistency verification APIs from the pinned submodule.
RFC 3161 and RFC 4998 evidence verification is tracked in
[`oramasearch/liquid#210`](https://github.com/oramasearch/liquid/issues/210).

## Storage

For server `node_name = <instance>`, immutable JSON objects are stored at:

```text
LAKE_STORE_ROOT/<instance>/log/tuf/<version>.root.json
LAKE_STORE_ROOT/<instance>/log/tuf/<version>.targets.json
LAKE_STORE_ROOT/<instance>/log/tuf/<version>.snapshot.json
LAKE_STORE_ROOT/<instance>/log/tuf/<version>.timestamp.json
LAKE_STORE_ROOT/<instance>/log/append.lock
LAKE_STORE_ROOT/<instance>/log/tree.json
LAKE_STORE_ROOT/<instance>/log/tiles/entries/<tile-index>/<width>.tile
LAKE_STORE_ROOT/<instance>/log/tiles/hashes/<level>/<tile-index>/<width>.tile
LAKE_STORE_ROOT/<instance>/log/timestamps/<checkpoint-sha256>.ers
```

The instance name must be one safe path component. A write uses a sibling temporary file and an
atomic rename. An existing version is never replaced. Reading without a version selects the
highest stored version for that role; reading with a version returns that immutable object.

`tree.json` is the atomically replaced publication state: `tree_size`, `root_hash`, tile width, and
the Merkle frontier needed for the next append. `append.lock` serializes writers across Lake
processes. A complete tile is stored as `1024.tile`; the final partial tile uses its value count.
Tile publication never replaces an existing path. When a partial tile grows, Lake writes a new
width path and retains every earlier path for checkpoints at prior tree sizes. A log created before
hash tiles existed is backfilled from its immutable entry tiles on the first hash-tile or proof
read, after the recomputed root matches `tree.json`. When an older checkpoint needs a narrower
partial hash tile, Lake publishes the immutable prefix under that checkpoint's exact width.

Checkpoints are immutable objects beside TUF metadata:

```text
LAKE_STORE_ROOT/<instance>/log/checkpoints/<sequence>.checkpoint.json
LAKE_STORE_ROOT/<instance>/log/checkpoints/<sequence>.ots.json       # anchored only
LAKE_STORE_ROOT/<instance>/log/checkpoints/current.json
LAKE_STORE_ROOT/<instance>/log/checkpoints/publish.lock
```

`current.json` is atomically replaced only after its configured requirements succeed. A checkpoint
contains the exact current root and timestamp metadata, the attestation `tree_size`, `root_hash`,
`tile_width`, `hash_tile_width`, and `hash_algorithm`, its SHA-256 hash, Lake's TAAP identity
signature, and valid witness signatures.
An anchored checkpoint also records the aggregation endpoint and its opaque proof; the same proof
is retained in the adjacent `.ots.json` object.

### Authenticated signing time

Every newly signed `lake_checkpoint/1` body contains `issued_at`, a non-negative integer counting
seconds since the Unix epoch in UTC. Lake samples its system clock immediately before invoking
the existing checkpoint signer; callers cannot supply or override the value. Lake's existing
TAAP/1 Ed25519 signature covers the complete body, including `issued_at`: the signing input is
`TAAP/1 lake_checkpoint/1`, a zero byte, and the body's existing CBOR encoding. The checkpoint
SHA-256 and witness signatures also cover the body containing the field. The signature format,
identity, and Liquid signature-verification path are unchanged.

Publication holds `publish.lock` across reading the last immutable checkpoint, sampling the clock,
signing, witness/anchor verification, and updating `current.json`. This serializes separate Lake
processes sharing the instance directory. Times may be equal within one second. If the clock is
behind the last persisted `issued_at`, publication fails with `conflict` before signing and retains
the current checkpoint. Lake does not clamp or invent a later time. The check survives restart and
an interrupted publication that wrote an immutable record before replacing `current.json`.
Replication preserves the source's signed bytes, rejects malformed issuance times, and refuses a
successor whose time moves backwards or disappears after an authenticated time was recorded.

**The log attests its own time.** This allows a consumer to reject a stale or replayed checkpoint;
it does not defend against a log that lies about when it signed. This deliberate limit is
[decision 37](https://github.com/oramasearch/liquid-bridge/blob/main/docs/decisions.md#37-authenticated-time-on-a-transparency-checkpoint--decided-steve-2026-09-07-a-signed-issued_at-in-the-checkpoint-body).
No second key, signature, credential, or timestamp authority is added. RFC 3161 binding remains
available for a later stronger freshness policy; air-gapped publication requires no authority.

Historical checkpoints remain byte-for-byte readable and replicable. They have no `issued_at`
and cannot satisfy a finite `max_checkpoint_age_seconds`. Liquid admission enforcement is owned
by [liquid#290](https://github.com/oramasearch/liquid/issues/290): an absent bound disables freshness;
a positive bound rejects missing or over-age time; zero is invalid policy; times over 60 seconds
in the future are refused. The Lake change supplies authenticated time, not admission policy.

The API returns both the parsed `metadata` object and `encoded`, the exact compact JSON bytes that
were signed, hashed, and stored. Clients use `encoded` for length and digest checks rather than
serializing the parsed object with their own JSON settings.

Timestamp evidence is binary DER. Each `.ers` object is an RFC 4998 `EvidenceRecord` version 1
with one SHA-256 digest algorithm and one archive-timestamp chain. The first RFC 3161 token's
message imprint is SHA-256 over the decoded checkpoint **signature value**, not the checkpoint
document. A renewal token's imprint is SHA-256 over the complete DER encoding of the preceding
`ArchiveTimeStamp`. That preserves the prior token, embedded TSA certificate, and signed
attributes. A single checkpoint needs no reduced hash tree.

Lake verifies the TSA response status, request nonce, message imprint, policy, CMS signature,
timestamp-signing certificate purpose, and certificate chain before publishing the evidence
record. Reads reparse the DER, verify every CMS signature without applying today's certificate
expiry to a historical token, and recompute every predecessor imprint. Malformed tokens and broken
renewal links are refused as stored-data corruption.

## RFC 3161 endpoint and renewal

`LAKE_TSA_URL` configures the RFC 3161 HTTP endpoint. Operators should use HTTPS outside controlled
test networks. Requests use `application/timestamp-query`, responses use
`application/timestamp-reply`, and both are DER. `LAKE_TSA_TRUST_CERT` may contain one or more PEM
trust anchors for a private TSA; when it is empty, OpenSSL's system trust roots are used.

The owner loop checks timestamp evidence each hour. It renews a record at the earlier of:

- the last token's generation time plus `LAKE_TSA_RENEWAL_SECONDS` (default 31,536,000); or
- the TSA signing certificate's expiry minus `LAKE_TSA_RENEW_BEFORE_SECONDS` (default 2,592,000).

Each due renewal calls `renew_timestamp_evidence` as the operator principal through
`Registry::call`. It consequently uses the same authorization metadata and creates the same signed
audit event as a manual renewal. A failed TSA exchange leaves the prior immutable evidence record
in place and writes a structured operational error. Operators must alert on that error and restore
the TSA before `renew_after`.

Air-gapped lakes leave `LAKE_TSA_URL` empty. They retain signed checkpoints but do not claim trusted
UTC or long-term timestamp evidence.

After `publish_transparency_checkpoint` succeeds, call `timestamp_checkpoint`. The capability reads
the current immutable checkpoint hash and Lake signature from the checkpoint store; it does not
accept caller-supplied signature bytes. This binds the evidence object to a checkpoint already
accepted by the configured witness and anchoring mode.

Complete client verification of the DER record, TSA trust, signature imprint, and renewal links is
tracked in [`oramasearch/liquid#210`](https://github.com/oramasearch/liquid/issues/210).

## Metadata contract

Every metadata document is the TUF envelope:

```json
{
  "signed": {
    "_type": "root",
    "spec_version": "1.0.36",
    "version": 1,
    "expires": "2099-01-01T00:00:00Z"
  },
  "signatures": [
    {"keyid": "<sha256-of-key-object>", "sig": "<hex-ed25519-signature>"}
  ]
}
```

Lake currently accepts Ed25519 keys and SHA-256 metadata hashes. A key id is the lowercase SHA-256
digest of the compact canonical JSON key object. Each counted signature must have a distinct key
id; repeating one signature cannot satisfy a threshold.

The root must define all four roles, each role's key ids, and a positive threshold. Initial root
metadata is version 1 and meets its own root threshold. A replacement root advances by exactly one
version and meets both the currently trusted root threshold and its own new root threshold.

Targets, snapshot, and timestamp metadata must have future UTC expiry values and versions greater
than the stored versions. Snapshot metadata binds `targets.json` by version, byte length, and
SHA-256 digest. Timestamp metadata binds `snapshot.json` the same way. These checks reject:

- a freeze attempt through expired timestamp metadata;
- a rollback through a non-increasing role version;
- a mixed publication through a mismatched version, length, or hash;
- a compromised or duplicated signer that does not meet the configured threshold.

TUF protects metadata selection. Each targets entry still names the length and digest of a TAAP
attestation object; complete attestation and target verification belongs to the liquid client.

## Tile and proof format

The Merkle tree uses the RFC 6962 domain separation rules. All integer indexes and sizes in the API
are JSON integers. All hashes in JSON are 32-byte values encoded as 64 lowercase hexadecimal
characters:

```text
MTH({}) = SHA256("")
MTH({entry}) = SHA256(0x00 || entry)
MTH(entries[0:n]) = SHA256(0x01 || MTH(entries[0:k]) || MTH(entries[k:n]))
```

For the last rule, `k` is the largest power of two strictly less than `n`. `entry` is the exact
decoded canonical-CBOR byte string, not its base64url text. Concatenation has no length prefix.

`transparency_tile` returns one exact tile object as unpadded base64url plus the lowercase SHA-256
digest of the complete decoded tile, including its header. `index` and `level` start at zero;
`level` defaults to zero for compatibility. Level zero is the entry stream. Positive level `L`
contains hashes for complete subtrees of `2^L` consecutive leaves. Hash number `j` at that level is
stored at tile index `floor(j / 1024)`, offset `j % 1024`.

For checkpoint tree size `N`, the level-zero value count is `N`. The value count at positive level
`L` is `floor(N / 2^L)`. For either kind, tile `i` exists when `i * 1024` is less than that value
count, and its required width is:

```text
min(1024, value_count - i * 1024)
```

Callers must request that exact width. A partial width names an immutable checkpoint-era version;
later appends publish a new width without replacing it. Width 1,024 is final and is never replaced.
There is no tile for an empty level. The capability, checkpoints, proofs, and log configuration are
public because independent monitoring cannot depend on a Lake account.

An entry tile is byte-for-byte:

```text
12 bytes: ASCII "LAKE-TILE-1\n"
repeated width times:
  4 bytes: unsigned entry length, network byte order; must be greater than zero
  length bytes: exact canonical-CBOR attestation
EOF: no padding, count field, or trailer
```

Alongside canonical liquid attestations, Lake may append an internal `LAKE-AUDIT-EVENT-1\n`
record followed by canonical CBOR `audit_event/1`. Admission recomputes the event digest before
the leaf is accepted. The event already carries Lake's signed audit-chain fields; secret values are
never included. These entries replicate and scan through the same immutable tile and Merkle path.

A hash tile is byte-for-byte:

```text
17 bytes: ASCII "LAKE-HASH-TILE-1\n"
repeated width times:
  32 bytes: raw SHA-256 interior-node hash
EOF: no padding, count field, length field, or trailer
```

Level zero returns `version: "lake_tile/1"`, `kind: "entries"`, `entries: width`, `level: 0`,
`index`, `encoding: "base64url"`, `tile`, and `sha256`. A positive level returns
`version: "lake_hash_tile/1"`, `kind: "hashes"`, `hashes: width`, `level`, `index`, `encoding`,
`tile`, and `sha256`.

### Checkpoint-bound inclusion proofs

`transparency_inclusion_proof` takes `leaf_index` and positive `tree_size`. Lake selects the
highest-sequence immutable checkpoint whose embedded checkpoint has that exact tree size. Selecting
by size is intentional: a metadata-only publication can create more than one checkpoint for the
same log root. If no checkpoint has the requested size, Lake returns `not_found`; if its root does
not match the log's proof root, Lake returns `unrecoverable`.

The result has `version: "lake_inclusion_proof/1"`, `hash_algorithm: "sha256"`, `leaf_index`,
`tree_size`, `root_hash`, `audit_path`, and `checkpoint`. `checkpoint` is the complete immutable
checkpoint record, including `sequence`, `checkpoint_hash_sha256`, Lake `signature`, and
`witness_signatures`. Each audit-path member is one lowercase hexadecimal hash. Members are ordered
from the leaf toward the root. Recursively, append the right-subtree root when the leaf is left of
the split; append the left-subtree root when the leaf is right of the split. The path is empty for a
one-leaf tree.

### Checkpoint-bound consistency proofs

`transparency_consistency_proof` takes non-negative `old_tree_size` and `new_tree_size`, with old no
greater than new. Lake selects and embeds the highest-sequence checkpoint for the exact new size.
The result has `version: "lake_consistency_proof/1"`, `hash_algorithm: "sha256"`, both sizes,
`old_root_hash`, `new_root_hash`, `audit_path`, and the complete new checkpoint record. A verifier
must compare `old_root_hash` with its independently trusted old checkpoint and `new_root_hash` with
the embedded signed checkpoint before evaluating the path. The path is empty for an old size of
zero or equal sizes.

For `0 < m < n`, Lake emits the RFC 6962 path below. `MTH(start,count)` applies the rules above to
that entry range; `k` is the largest power of two strictly less than `n`; `||` joins path arrays:

```text
SUBPROOF(m, n, start, complete):
  if m = n: [] when complete, otherwise [MTH(start, n)]
  if m <= k: SUBPROOF(m, k, start, complete) || [MTH(start + k, n - k)]
  otherwise: SUBPROOF(m - k, n - k, start + k, false) || [MTH(start, k)]
```

The exact proof routes are `GET /v1/transparency_inclusion_proof` and
`GET /v1/transparency_consistency_proof`. The registry also exposes both operations as MCP tools,
and its OpenAPI document describes their query parameters and response envelopes.

### Offline proof bundles and split-view detection

`transparency_proof_bundle` takes one `leaf_index` in the current checkpoint. It returns
`version: "lake_transparency_proof_bundle/1"` with Lake's public identity, effective witness
configuration, the complete signed checkpoint, the exact-width entry and interior-hash tiles
committed by that checkpoint, and the checkpoint-bound inclusion proof for the selected leaf.
Once all its pages are put together, the bundle is self-contained: a verifier does not need to
resolve a Lake URL after export.

The tiles come a page at a time, so one anonymous call's work does not grow with the log
(lake#941). They are numbered in a fixed order: every entry tile from index 0, then each
interior-hash level's tiles from level 1 up. `tile_offset` (default 0) names the first tile of a
page and `tile_limit` (1 to 16, default 16) how many it carries; a larger `tile_limit` is
`invalid_input`, and so is a `tile_offset` at or past the end. Each page answers `tile_offset`,
`tile_count` (the checkpoint's tiles in all) and, while tiles remain, `next_tile_offset`. A
later page passes the first page's `tree_size`, which selects that signed checkpoint
(`not_found` without one), so every page is of one checkpoint even after the log grows. A log of up
to 4,095 entries (16 tiles) is still one page. The verifier puts the pages' `tiles` arrays
together and verifies them as one bundle.

The liquid verifier first checks the checkpoint signature and configured witness threshold, then
checks each tile digest and reconstructs the committed root. It locates the canonical attestation
in the verified entry stream, checks the compact inclusion path, and persists the trusted
checkpoint under the log identity. A later equal-size view with a different root or checkpoint
hash is a conflict. Clients exchange these retained checkpoint observations between witnesses;
two observations for the same log and size that disagree are evidence of a split view.

## What the scenario proves

`TransparencyScenario.ExportedBundleVerifiesOfflineAndSplitViewIsDetected` appends canonical
signed attestations, publishes a checkpoint, and exports a bundle. The test loads only that JSON
into an in-memory `liquid::transparency::log_source`, which has no HTTP implementation or other
network path, and runs liquid's production `log_client::verify_inclusion`. The verifier validates
the checkpoint, all entry and parent-hash tiles, the attestation, and its inclusion proof.

The same test models two witness observations of an equal-size checkpoint. It retains the first
through liquid's checkpoint-trust API, changes both the root and checkpoint hash in the second,
and requires liquid to return a conflict. This proves that a client retaining witness observations
does not accept incompatible histories at the same tree size.

## Hosted monitoring

`transparency_identity_scan` is Lake's hosted monitor for every customer and every other observer.
It reads the current signed checkpoint, fetches its exact tile versions, decodes and verifies every
TAAP attestation, and recomputes every leaf and parent hash through the committed Merkle root. It
returns an entry when the requested TAAP identifier is the issuer or an exact text/list attribute,
which covers authorship, review, membership, delegation, and registered profile identity fields.
The response reports tree size, root, the tiles and entries this call read and verified, matching
indexes, profiles, issuers, subjects, exact base64url attestations, and measured scan duration.

Lake keeps what its scans have verified (lake#941): the Merkle frontier, each complete tile's root,
the leaf hashes of the last, incomplete tile, and which entries name each identity. A scan verifies
only the entries appended since the last one, from the frontier to the checkpoint's root, so the
first scan after a start verifies the whole log and later ones only what is new. A checkpoint older
than what was verified starts again from the first entry. The kept index takes memory in
proportion to the log: one 8-byte index for each identity an entry names.

The scan reads immutable tile files for the checkpoint's tree size and never takes the append lock,
so appends, and with them dynamic-secret operations, go on while it runs. One scan runs at a time
per Lake process; another answers `busy` (503), to be retried. The registry runs it off the server's
thread, on the pool repository-store calls use.

Matches come 100 a page, in index order. `start_index` (default 0) names the first entry a page
looks at, and a page with matches left answers `next_index`, the next page's `start_index`.

The root check happens after the new tile bytes are read. A modified, reordered or missing new
entry, substituted partial tile, malformed record, invalid attestation, or changed tile header
causes `unrecoverable`; the monitor returns no partial results. A tile the scan reads again, to
return entries an earlier scan verified or to continue the incomplete last tile, must reach the
tile root or leaf hashes kept for it, or the scan is `unrecoverable`. A tile already verified that
holds none of a page's entries is not read again; liquid's verifier, which checks every tile it
fetches, is the check for those. Earlier partial tile versions stay available, so a monitor can
verify an older checkpoint even after later appends fill that tile.

Lake's monitor verifies inclusion in the selected checkpoint by recomputing the complete root.
Lake serves the compact proofs and hash tiles described above. The identity-exit scenario invokes
the pinned libliquid inclusion verifier directly over a dynamic-secret audit leaf and Lake's proof.
The registry-exit scenario also verifies an exported bundle offline and exercises retained
checkpoint trust to reject conflicting roots at the same tree size.

The deterministic conformance vector is
[`../../tests/fixtures/transparency/fixture.json`](../../tests/fixtures/transparency/fixture.json).
It contains one signed, witnessed checkpoint, every tile for seven entries, inclusion proofs for
all leaves, and a size-3 to size-7 consistency proof. Run
`./tests/fixtures/transparency/regenerate.py --check` to prove the checked-in bytes are current.
That fixed vector remains a legacy checkpoint without `issued_at`; newly published, timed
checkpoints are covered by the clock and live HTTP tests below.

### Measured monitoring cost

The Release acceptance test on the arm64 Apple M3 Max development machine on 2026-09-05 appended
100,000 entries in 100 batches in 969.419 ms, then scanned and verified the 98 bulk tiles in
289.977 ms. The scan found 100 matching entries and read each logged entry once. The executable
test requires scan duration to remain below measured log-growth duration; it also changes one tile
byte and requires the next scan to fail as `unrecoverable`.

## Runtime configuration and checkpoint protocol

When `LAKE_STORE_ROOT` is configured, Lake reads these runtime values at startup:

| Variable | Required value |
|---|---|
| `LAKE_LOG_MODE` | `air-gapped`, `witnessed` (the default), or `anchored` |
| `LAKE_LOG_WITNESSES` | JSON array of `{endpoint, public_key}` records; `public_key` is a 32-byte Ed25519 hexadecimal public key |
| `LAKE_LOG_WITNESS_THRESHOLD` | Positive integer no greater than the number of distinct witnesses in witnessed or anchored mode |
| `LAKE_LOG_AGGREGATOR` | HTTPS aggregation endpoint, required only in anchored mode |

Endpoints must use HTTPS; loopback HTTP is accepted only for local integration tests. An invalid
mode, an insufficient witness set, duplicate endpoint or key, or anchored mode without an
aggregator makes startup fail with the exact configuration error. Air-gapped mode accepts no
witness or aggregator configuration and never makes an outbound request.

### Publish on a running instance

Starting the server does not publish a checkpoint. Provision a persistent store and control-store
identity, initialize TUF with operator-signed root metadata through `initialize_tuf`, and publish
operator-signed targets, snapshot, and timestamp metadata through `publish_tuf_metadata`. Append
canonical attestations through `append_transparency_entries`, then call the existing registry action:

```sh
curl --fail-with-body -H "Authorization: Bearer $LAKE_ADMIN_TOKEN" \
  -H 'Content-Type: application/json' -d '{}' \
  "${LAKE_URL}/v1/publish_transparency_checkpoint"
curl --fail-with-body "${LAKE_URL}/v1/transparency_checkpoint"
```

The POST must succeed and both responses must contain the signed body at `value.checkpoint`,
including `issued_at`. `not_found` means the instance has no checkpoint or is missing its TUF
metadata; witness/anchor failures retain the previously current checkpoint. Lake does not generate
TUF keys or bypass configured requirements. Provisioning only a server and a log directory is
insufficient. The Postgres-backed HTTP proof below exercises publication and fetch on a running
instance.

Freshness concerns the time of explicit publication. An unchanged checkpoint can age beyond a
consumer's bound. Automatic refresh is not introduced: re-signing an unchanged tree at a later
time changes its checkpoint hash, which retained Liquid trust compares at equal tree sizes.

For each checkpoint Lake POSTs this JSON to every configured witness:

```json
{"checkpoint": {"version": "lake_checkpoint/1"}, "checkpoint_hash_sha256": "<hex>"}
```

A witness returns `{"signature":"<hex-ed25519-signature>"}` over the exact compact JSON
checkpoint body. Lake verifies each response against that witness's configured public key and only
publishes `current.json` after the configured threshold of distinct signatures verifies. In
anchored mode, after witness verification Lake POSTs
`{"hash":"<checkpoint SHA-256>","hash_algorithm":"sha256"}` to `LAKE_LOG_AGGREGATOR`; the
aggregator must return `{"proof": ...}`. Lake stores that opaque proof without interpreting it.

Anchoring proves only that the checkpoint hash existed before the aggregation proof's confirmed
chain event. It does not prove authorship, correctness, ownership, truth, an authoritative UTC
time, or agreement on a canonical history. Witnessing is the control against split views; an
anchor supplements it and does not replace its quorum.

## Capabilities

All transparency capabilities use the single registry and record one signed `transparency` audit
event through `Registry::call` and `audit::record`. Administrative publication uses the instance
authorization object and `authorize`. Public monitoring reads are deliberately unauthenticated.

| Capability | Kind | Input | Result |
|---|---|---|---|
| `initialize_tuf` | action | `{root}` | stored root role, version, exact bytes, and parsed metadata |
| `publish_tuf_metadata` | action | `{targets, snapshot, timestamp}` | the root and three appended versions |
| `rotate_tuf_root` | action | `{root}` | the verified successor root |
| `tuf_metadata` | read | `role`, optional positive `version` | latest or named immutable role object |
| `append_transparency_entries` | action | `{attestations: [base64url canonical CBOR]}` | first index, resulting tree size/root, tile width, and entry/hash tile objects published |
| `transparency_log_configuration` | read | `{}` | effective mode, public witness set, quorum, and anchored aggregation endpoint when configured |
| `publish_transparency_checkpoint` | action | `{}` | the signed current checkpoint after its mode's requirements complete |
| `transparency_checkpoint` | read | `{}` | current signed checkpoint, or `not_found` before one has met the requirements |
| `transparency_tile` | read | non-negative `index`, optional `level` from 0 through 63, exact `width` from 1 through 1,024 | immutable entry or interior-hash tile as base64url with SHA-256 |
| `transparency_inclusion_proof` | read | non-negative `leaf_index`, positive checkpoint `tree_size` | compact audit path and complete named checkpoint record |
| `transparency_proof_bundle` | read | non-negative `leaf_index` in the checkpoint; optional checkpoint `tree_size` (default the current one), `tile_offset` (default 0) and `tile_limit` (1 to 16, default 16) | public identity, witness configuration, checkpoint, one inclusion proof, and one page of the checkpoint's tiles with `tile_offset`, `tile_count` and, while tiles remain, `next_tile_offset` |
| `transparency_consistency_proof` | read | non-negative `old_tree_size` and `new_tree_size`, old no greater than new | compact audit path and complete named new-checkpoint record |
| `transparency_identity_scan` | read | TAAP `identity`, optional `start_index` (default 0) | verified checkpoint/root, scan measurements, up to 100 matching attestations from `start_index` on, and `next_index` while matches remain; `busy` while another scan runs |
| `timestamp_checkpoint` | action | `{}` | new RFC 4998 evidence record for the current checkpoint signature |
| `renew_timestamp_evidence` | action | `checkpoint_digest` | evidence record with one appended archive timestamp |
| `timestamp_evidence` | read | `checkpoint_digest` | verified DER evidence record, token summaries, signature digest, and `renew_after` |

TUF initialization/publication/rotation, attestation append, checkpoint publication, timestamp
creation, and timestamp renewal require `admin` on the instance transparency log. TUF metadata and
timestamp-evidence reads require `read`. The operator credential follows the registry's existing
explicit administrative bypass; row-resolved credentials go through `authorize`. Configuration,
checkpoint, tile, proof, proof-bundle, and identity-scan reads are public.

`GET /audit` displays the effective mode and witness quorum through
`transparency_log_configuration`; it does not read configuration outside the registry. Every
configuration read and checkpoint publication records a signed `transparency` audit event.
`GET /transparency?identity=<taap-id>` is the public server-rendered monitor. It calls
`transparency_identity_scan` and does not read tile storage directly.

## Key-compromise recovery

The recovery drill is the production procedure, not a separate emergency code path:

1. Stop metadata publication and identify every exposed key id and role.
2. Generate replacement keys outside lake. Keep the root threshold offline and under distinct
   custodians.
3. Create root version `N+1`. Remove each exposed role key, add its replacement, and set the new
   thresholds. Sign the candidate with the trusted version `N` root threshold and with the
   candidate version `N+1` root threshold.
4. Call `rotate_tuf_root`. Lake refuses a skipped version or either missing threshold.
5. Recreate targets, snapshot, and timestamp metadata with increasing versions, future expiries,
   the replacement role signatures, and freshly computed version/length/hash bindings. Call
   `publish_tuf_metadata`.
6. Fetch every role through `tuf_metadata`, compare the returned exact bytes and versions, verify
   that the removed key can no longer satisfy its role, and retain the signed audit events with
   the incident record.
7. Destroy or revoke the exposed private-key material in its custody system.

If fewer root keys are exposed than the root threshold, the remaining trusted threshold performs
the procedure above. If an attacker controls the complete trusted root threshold, TUF cannot
distinguish the attacker from the repository owners. Recovery then requires distributing a new
trusted root through an authenticated out-of-band channel and explicitly replacing each client's
trust anchor.

`TransparencyTuf.RootRotationReplacesCompromisedRoleKeys` rehearses the procedure. It proves that
old-root-only and new-root-only candidates are refused, a candidate carrying both thresholds is
accepted, removed targets keys are refused, and replacement targets keys publish successfully.

## How to verify

```sh
cmake --build build -j 6
./build/tests/lake-tests --gtest_filter='Transparency*'
```

`TransparencyCheckpoint.IssuedAtUsesSigningClockAndRefusesRollbackAfterRestart` proves that the
signer receives the server's sampled time, equal seconds are allowed, and rollback refuses signing
without changing the current checkpoint. The concurrent-publisher test verifies sequence/time
ordering across two service instances sharing a directory. The replication test preserves legacy
records and rejects backwards or removed issuance time after an authenticated checkpoint.
`PostgresTransparency.LiveCheckpointCarriesAuthenticatedMonotonicTime` starts real loopback HTTP
servers backed by Postgres, publishes over HTTP after two successive appends, fetches both
through Liquid's unchanged HTTP client and signature verifier, and prints the complete records.
It checks that each issuance time lies between the before/after system-clock samples, that the
second is non-decreasing, and that changing only the time (even with a recomputed body hash) fails
signature verification. Run it with the database configured as in
[`../build/local.md`](../build/local.md); the complete static and shared CTest suites remain the
acceptance proof. Fresh/expired admission enforcement tests belong to liquid#290.

`TransparencyTuf.AppendsThresholdSignedMetadataAndRejectsRepositoryAttacks` covers append, exact
fetch, unique-key thresholds, expired timestamps, rollback, and mixed metadata.
`TransparencyLog.BulkTilesDetectTamperingAndScanOneHundredThousandEntries` measures 100,000-entry
growth and identity scanning, fetches full and partial tiles, proves the scan is cheaper than log
growth, and detects a modified tile against the checkpoint root.
`TransparencyLog.ConcurrentWritersPreserveEveryEntry` verifies that two Lake writers sharing an
instance serialize their appends without losing entries.
`TransparencyLog.ScanDoesNotHoldUpAppendsAndVerifiesOnlyNewEntries` appends while a 65,536-entry
scan runs, gets `busy` from a second scan meanwhile, and requires the next scans to verify only the
entries appended since. `TransparencyLog.ScanPagesMatchesAndChecksTilesItReadsAgain` pages matches
and refuses a republished last tile whose verified entries changed.
`TransparencyScenario.ProofBundleIsPagedByTileRangeAndCapped` exports a 30-tile bundle in two pages
pinned to one checkpoint, verifies them together with liquid's offline verifier, refuses a
`tile_limit` over 16, and pages the identity scan through the registry.
`TransparencyCheckpoint.ValidatesRuntimeConfiguration` covers startup configuration refusal.
`TransparencyCheckpoint.AirGappedSignsAndServesWithoutOutboundCalls` proves air-gapped checkpoint
publication does not invoke its outbound client. The witnessed and anchored test verifies a
threshold of valid Ed25519 witness cosignatures, refuses a checkpoint below that threshold, and
retains an aggregation proof beside the anchored checkpoint. The registry test proves configuration
and checkpoint capability dispatch with signed audit.
`TransparencyScenario.ExportedBundleVerifiesOfflineAndSplitViewIsDetected` runs the liquid client
against an exported in-memory bundle without a network transport, then proves conflicting
equal-size witness observations are rejected. `PostgresTransparency` repeats TUF, append,
tile fetch, identity scan, configuration, checkpoint, and timestamp capabilities with the Postgres
control store and verifies the resulting audit chain.
`TransparencyTimestamp.CreatesAndRenewsRfc4998EvidenceFromStubTsa` creates a signed RFC 3161 token
from a stub TSA, verifies the signature-value imprint, finds the due record, appends a renewal that
covers the prior archive timestamp, reloads the DER evidence record, and detects tampering.
`TransparencyTimestampRegistry` proves all three timestamp capabilities and their audit events.
`TransparencyTimestamp.OwnerLoopRenewsDueEvidenceThroughRegistry` proves a due record is renewed by
the owner loop through the audited registry path.

## How to change

Keep private signing keys outside lake. A new key or hash algorithm updates the root validation,
attack fixtures, tile hashing, schema above, and liquid verifier issue together. A tile format or
width change gets a new format version and retains readers for published tile objects. A new
capability belongs in the single registry and this page. Do not add an unsigned recovery path or
overwrite a stored metadata or tile version. The current evidence policy accepts SHA-256 only. A
future digest-policy change must add an RFC 4998 hash-renewal chain before the old algorithm is
outside policy; it must never reinterpret or discard an existing archive timestamp.
