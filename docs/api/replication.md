# Asynchronous multi-region replication

Lake replicates customer repositories and the instance transparency log between Lake instances.
The protocol is pull-based: an operator reconciler invokes the destination, and the destination
authenticates to the source with `LAKE_REPLICATION_TOKEN`. If that variable is absent, the
executable uses `LAKE_ADMIN_TOKEN`. Tokens are never returned or written to audit details.

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
- the current signed checkpoint, when one exists.

`POST /v1/replicate_transparency_log` accepts `source_url`. The destination requests a batch from
its current tree size, verifies every TAAP attestation, and reconstructs the proposed Merkle head
in memory. It publishes tiles and `tree.json` only when its current root exactly equals the
source's base root and the reconstructed head exactly equals the declared head. A base mismatch,
invalid attestation, modified entry, or false head is an `unrecoverable` failure and leaves the
destination head unchanged.

When the batch includes a checkpoint at or below the applied head, Lake also:

1. reconstructs the checkpoint's root from the local tiles;
2. verifies the Lake identity signature against the control-plane identity record;
3. verifies the configured witness quorum and anchoring shape; and
4. adopts the immutable checkpoint sequence without allowing rollback or conflicting reuse.

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

## Authorization and audit

All three actions are guarded registry capabilities and pass through `authorize`. They emit one
signed audit event through `audit::record`, with `kind: replication`. Repository events use the
customer and `repository/<id>` audit object. Log export and apply use `transparency/log`. Audit
details include only source identity or URL, high-water marks, counts, and byte totals.

Production peer URLs use HTTPS. Plain HTTP is accepted only for loopback integration tests.
`source_instance` is a path-safe stable name because it namespaces the durable liquid cursor.

The operator mapping, promotion rules, and two-region desired state are in
[`../control-plane/replication.md`](../control-plane/replication.md).
