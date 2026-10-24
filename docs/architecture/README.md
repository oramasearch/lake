# Architecture

Workspace ownership and storage contracts are indexed in the [documentation map](../README.md).
[Workspace data-plane ownership](workspace-data-plane.md#storage-audit) and the
[storage plan](workspace-storage-plan.md#lake382-transparency-log-tuf-and-timestamp-evidence)
classify the instance transparency log separately from private central and workspace audit.

Dynamic-secret lifecycle events stay in private central audit; only successful events publish
public digest commitments (lake#953, LSR-087). Denied and failed events publish nothing. The
[transparency contract](../api/transparency.md) defines admission, historical entries and proofs;
[secrets](../api/secrets.md#dynamic-leases) lists the verification and change rules.
