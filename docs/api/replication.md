# Asynchronous multi-region replication

Lake replicates customer repositories and the instance transparency log between Lake instances.
The protocol is pull-based: an operator reconciler invokes the destination, and the destination
authenticates to the source with `LAKE_REPLICATION_TOKEN`. There is no fallback: without that
variable the destination sends no credential, and never the admin token. The credential is sent
only to a source whose origin is listed in `LAKE_REPLICATION_PEERS`
([Peers and the replication credential](#peers-and-the-replication-credential)). Tokens are never
returned or written to audit details.

This is an active-region/standby-region protocol, not multi-writer consensus. The operator routes
repository writes, transparency appends, and checkpoint publication to one active instance. A
standby may serve reads from its most recently applied repository refs and log checkpoint. During
an outage, promotion is an operator action performed only after the old active is fenced.

## Repository protocol

`POST /v1/replicate_repository` accepts:

```json
{
  "repository_id": "<repository UUID>",
  "source_instance": "region-a",
  "source_url": "https://region-a.lake.example"
}
```

Lake resolves the repository through the control store and authorizes `admin` on that repository.
It pulls from `<source_url>/v1/repositories/<repository_id>` using liquid's signed sync protocol.
The source validates its normal repository-read policy. Content-addressed objects are transferred
before their referencing metadata is imported, and liquid verifies object digests and signed
records. The latest returned journal cursor is saved with liquid's replication-cursor API
(`repository::set_replication_cursor`, liquid#326) only after the pull succeeds, as the repository
state key `repositories/<id-hash>/replication/<source>.cursor` of whichever store holds the
repository, so it moves with the repository into its workspace store (lake#380). A cursor an older
lake wrote at `<root>/<customer>/repositories/<repository>/replication/<source>.cursor` is read once
when liquid holds none. It is local replication state and is not added to the liquid history. A retry therefore
resumes at the last committed cursor and transfers no object already present. The response reports
the cursor, metadata items, received objects and bytes, and current ref count.

The source and destination must refer to the same repository and customer identifiers in their
control planes. Control-plane Postgres replication and placement are operator responsibilities;
this data-plane protocol does not copy customer, policy, grant, or repository rows.

## Zetta data-plane placement

The current adapter opens one region-local liquid object store at
`LAKE_STORE_ROOT/<customer>/store`. The target Zetta deployment keeps that placement rule: each
region has a Zetta data plane, objects stay customer-namespaced, and repository replication copies
missing content-addressed objects before advancing refs. A cross-region shared filesystem is not
part of the design. Replacing the filesystem adapter with Zetta changes object transport and
placement, but not the journal cursor, digest verification, or ref commit points exposed here.

Transparency tiles, signed checkpoints, and mutable current-head records remain in the Lake
instance log namespace. With a Zetta-backed adapter, immutable tile versions and checkpoint
sequences become ordinary Zetta objects; the current tree and checkpoint records are the small
publication pointers. The prefix/root checks stay in Lake so the storage service never decides
which log branch is authoritative.

## Transparency-log protocol

The source action `POST /v1/export_transparency_replication_batch` accepts an
`after_tree_size`. It returns at most 100,000 canonical base64url TAAP attestations with:

- `version: lake_log_replication/1`;
- the source instance and current source tree size;
- the Merkle root and size of the requested base prefix;
- the Merkle root and size after the returned suffix; and
- a required signed checkpoint covering the batch head; and
- `checkpoint_consistency`, a compact Merkle consistency path from that head to the checkpoint.

`POST /v1/replicate_transparency_log` accepts `source_url`. Before writing any tiles, the
destination requires a checkpoint signed by a Lake identity already trusted in its control-plane
identity history. A configured peer or a self-consistent Merkle root supplies no identity trust.
The checkpoint must cover the entire batch head; an equal-size checkpoint must name that exact
root, and an earlier partial head needs a valid `checkpoint_consistency` proof under libliquid's
checkpoint-bound verifier. Missing checkpoints, unknown signers, invalid signatures, stale
checkpoints and conflicting roots fail closed before the destination log advances (lake#974).

The destination then validates every entry and reconstructs the proposed Merkle head in memory.
It publishes tiles and `tree.json` only when its current root exactly equals the source's base root
and the reconstructed head exactly equals the authenticated head. Audit commitments contain no
identities; legacy full audit events are refused rather than attributed from their self-digests.
Merkle consistency and event authenticity remain separate checks.

Once the signed checkpoint is reached locally, Lake checks its root against the local tiles,
verifies the configured witness quorum and anchoring shape, and adopts the immutable sequence
without rollback or conflicting reuse. Export requires a current checkpoint covering the source
log; after appends, publish a checkpoint before exporting again. A concurrent append or checkpoint
change can make export fail; retry after checkpoint publication. Older unsigned replication
responses are no longer accepted.

A source more than 100,000 entries ahead sets `caught_up` to false. The reconciler repeats the
action until it is true; a later pass adopts the checkpoint once its committed tree size is local.
An already-applied batch is harmless. Lake never truncates a log and never chooses between
branches.

## Consistency model

Repository and log replication are asynchronous. A successful response gives a destination
high-water mark, not synchronous cross-region durability for the source write. Reads in a standby
are prefix-consistent: repository refs name locally imported objects, and the accepted log is an
exact prefix of the active log. Read-your-writes is available from the active instance or after an
operator observes the relevant repository cursor and log tree size at the destination.

The log has one total order because only the active instance appends and every destination accepts
only the suffix of its current cryptographic prefix. Witnesses remain the defence against a source
presenting incompatible histories. A prefix mismatch is not repaired automatically: operators
must fence both writers, retain the evidence, compare witnessed checkpoints, and select a recovery
procedure explicitly.

## Peers and the replication credential

Lake issue #915 (a security review finding):
`replicate_repository` is authorized as `admin` on the repository, which every sign-up holds on its
own repositories, and it used to send the instance credential to whatever `source_url` the caller
named, with the admin token as the default credential. A repository owner could point it at their
own HTTPS server and read the operator's bearer from the request.

Now:

- **`LAKE_REPLICATION_TOKEN` has no default.** Unset, it is empty and nothing is sent; the
  executable never substitutes `LAKE_ADMIN_TOKEN`. The startup log says `replication_token_set`.
- **`LAKE_REPLICATION_PEERS` lists the sources replication may pull from**: origins
  (`https://region-a.lake.example`, `https://host:8443`) separated by commas or whitespace. Lake
  lower-cases the host and drops a default port. An entry with a path, query or user name, or plain
  `http` to anything but loopback, stops startup with exit code 2. The startup log lists the peers.
- **`replicate_repository` and `replicate_transparency_log` refuse any other source** with
  `invalid_input` at `/source_url`, before any name lookup or connection, for every caller, the
  operator included. `source_url` must be an origin (a trailing `/` is accepted). With no peers
  configured, both refuse every source. So the credential only ever reaches an origin the operator
  configured, never one a tenant chose.
- **A refusal is audited**: the `replication` event has outcome `failure` and
  `details.refused_source_url`, in the repository's customer log for `replicate_repository`.
- **Only HTTPS to public addresses.** Before liquid's sync client connects, Lake resolves the peer's
  name and refuses it if any address is loopback, private, link-local (the cloud metadata address
  included), carrier-grade NAT or otherwise not public, by the rule MCP connections use
  (`mcp_public_address`). The transparency batch request uses `OutboundRequest::public_only`
  (lake#876), which checks each address at connect time. Plain `http` is refused.
  `LAKE_REPLICATION_ALLOW_PRIVATE_ADDRESSES=1` lifts both rules for local development and tests.
- **Bounded time.** One repository pull, the name lookup, connection and every transfer included,
  takes at most `LAKE_REPLICATION_TIMEOUT_SECONDS` (default 600) and then answers
  `store_unavailable` ("did not finish"); the cursor is saved only after a pull succeeds, so the next
  call resumes from the last one. A transparency batch request waits at most 60 seconds, or the
  replication timeout if it is shorter. A repository pull runs on the blocking pool, so a slow peer
  holds only that request.

A tenant-chosen source is therefore not supported. A tenant copying a repository from another Lake
pulls it with their own credential on that Lake (`liquid pull`, or `import_repository`), not through
the instance's replication.

To verify: `RepositoryReplication.ASourceThatIsNotAConfiguredPeerNeverReceivesTheCredential`
(a repository owner and the operator name a recording loopback server; Lake refuses every call,
the server sees no connection, and six refusals are audited),
`RepositoryReplication.APeerOnAPrivateAddressOrPlainHttpIsRefusedBeforeConnecting`,
`RepositoryReplication.APeerThatNeverAnswersTimesOut`,
`RepositoryReplication.PeersAreOriginsAndOnlyAConfiguredOriginMatches`,
`ReplicationStartup.TheReplicationCredentialNeverDefaultsToTheAdminToken`, and the existing
`SyncHttp.TwoLakeInstancesReplicateRepositoryAndResumeFromCursor` and transparency replication
tests, which now configure their source as a peer.

## Authorization and audit

All three actions are guarded registry capabilities and pass through `authorize`. They emit one
signed audit event through `audit::record`, with `kind: replication`. Repository events use the
customer and `repository/<id>` audit object. Log export and apply use `transparency/log`. Audit
details include only source identity or URL, high-water marks, counts, and byte totals.

Peer URLs use HTTPS to public addresses. Plain HTTP and private addresses are accepted only with
`LAKE_REPLICATION_ALLOW_PRIVATE_ADDRESSES=1`, for local development and loopback integration tests.
`source_instance` is a path-safe stable name because it namespaces the durable liquid cursor.

The operator mapping, promotion rules, and two-region desired state are in
[`../control-plane/replication.md`](../control-plane/replication.md).

## Verification and maintenance

`TransparencyRegistry.ReplicationAuthenticatesTheBatchHeadBeforeWriting` covers missing,
malformed, unknown-key and invalid-signature checkpoints, stale and conflicting signed roots,
invalid partial proofs, and honest partial catch-up. Each refusal leaves the log unchanged.
`TransparencyLog.AuditLegacyForgedSignatureCannotClaimVerifiedIdentities` uses a forged signature
with a valid self-digest and Merkle root, tests append and replication refusal, and models a
historical tile to prove identity scans refuse it while tiles and inclusion proofs remain readable.
Run these with the existing `TransparencyLog`, `TransparencyRegistry`, `TransparencyCheckpoint`
and `IdentityExit` tests when changing replication. Keep checkpoint authentication before
`apply_replication_batch`; a Merkle head supplied by the peer is not a trust anchor.
