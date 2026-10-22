# Transparency log

`log.cpp` stores immutable entry and hash tiles and supplies Merkle proofs, replication and
identity scans. The public entry contract is in [transparency.md](../../docs/api/transparency.md).

Successful dynamic-secret audit events enter through `TiledLog::append_audit_event`: it validates
the private event's digest and writes only that digest under `LAKE-AUDIT-COMMITMENT-1\n` (lake#953,
LSR-087). The registry keeps denied and failed events private; this method also refuses them.
Commitment admission accepts exactly one canonical CBOR digest field containing 64 lowercase
hexadecimal characters. Commitments contribute no identities to scans.

`validated_entries` is shared by raw append and replication and refuses legacy full audit records.
The scan verifier still understands existing `LAKE-AUDIT-EVENT-1\n` tiles so their published roots
remain verifiable. Existing public records cannot be retracted, and replication of old full-event
batches is refused. Keep new admissions stricter than historical reads when changing this format.

Run `SecretsCapabilities.DynamicSecretTransparencyPublishesOnlySuccessfulCommitments`,
`TransparencyLog.Audit*`, `IdentityExit.*` and `Transparency*` with CTest before changing admission,
publication or proof handling; use the full suite before handing off a change.
