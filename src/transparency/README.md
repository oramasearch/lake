# Transparency log

`log.cpp` stores immutable entry and hash tiles and supplies Merkle proofs, replication and
identity scans. The public entry contract is in [transparency.md](../../docs/api/transparency.md).

Successful dynamic-secret audit events enter through `TiledLog::append_audit_event`: it validates
the private event's digest and writes only that digest under `LAKE-AUDIT-COMMITMENT-1\n` (lake#953). The registry keeps denied and failed events private; this method also refuses them.
Commitment admission accepts exactly one canonical CBOR digest field containing 64 lowercase
hexadecimal characters. Commitments contribute no identities to scans.

`validated_entries` is shared by raw append and replication and refuses legacy full audit records.
Identity scans also refuse existing `LAKE-AUDIT-EVENT-1\n` tiles (lake#974): a
self-digest authenticates neither signer nor principal. Existing tiles and Merkle proofs stay
readable; public records cannot be retracted. No path attributes these legacy events to identities.
Remote replication authenticates each head against a trusted signed checkpoint before calling
`apply_replication_batch`; that low-level method validates entries and Merkle consistency only.

Run `SecretsCapabilities.DynamicSecretTransparencyPublishesOnlySuccessfulCommitments`,
`TransparencyLog.Audit*`, `IdentityExit.*` and `Transparency*` with CTest before changing admission,
publication or proof handling. `TransparencyLog.AuditLegacyForgedSignatureCannotClaimVerifiedIdentities`
covers valid-self-digest forgeries and historical scan refusal. Full suites run on the integration
branch.
