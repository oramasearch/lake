# Offline authorization

**Status: implemented by lake #22.** A signed offline grant permits a named device to read
already-cached repository content for a bounded period. It never permits publication. Push checks
the publishing credential, the grant's current revocation and expiry state, and current repository
policy for every incoming change before liquid imports the atomic batch.

## Grant contract

`issue_offline_grant` is a self-management action for a row-resolved user session. Its input is a
registered `device_id`, one or more scopes, and `duration_seconds`. A scope has the exact form
`repository:<repository-id>:read`; issuance calls `authorize` for every repository. Durations are
positive and cannot exceed 604800 seconds (seven days).

The returned `credential` is a lake-signed TAAP `offline_grant/1` attestation. It binds the grant
UUID, user TAAP identity, device UUID, scopes, issue time, and Unix expiry to the lake instance
identity. A cache verifies the signature, requested scope, and expiry with `check_cached_read`.
`list_offline_grants` returns the user's rows and `revoke_offline_grant` records revocation.

Revocation prevents online use and publication immediately, but cannot recall bytes that are
already on a disconnected device. A valid signed credential therefore continues to permit its
named cached reads until its embedded expiry. The cache must not extend that expiry, add a scope,
or treat the credential as permission to fetch new content.

## Publication reauthorization

An offline client sends the grant UUID on push:

```text
Liquid-Offline-Grant: <grant-uuid>
```

The ordinary bearer or session credential is still required. Lake binds the grant to that
credential's user, TAAP identity, and device, verifies the stored attestation signature and signed
fields, and requires the pushed repository's read scope. It then calls `authorize(..., "write")`
against the current repository object for each incoming change. A missing, mismatched,
wrong-scope, revoked, or expired grant refuses publication even when an older cached policy
allowed local work.

The response carries a base64url-encoded JSON array in `Liquid-Reauthorization`. Every checked
entry has:

```json
{"change":"<change-id>","allowed":false,"reason":"offline grant was revoked before publication"}
```

Liquid validates the batch first and invokes authorization in frame order. The first refusal
aborts the atomic import, so the array identifies every change checked through that refusal. The
binary error also puts the refused change ID in `context` and the same explanation in `message`.
No repository ref advances.

Issuance, listing, cache checks, revocation, the push call, and each per-change decision are signed
audit events through `Registry::call` and `audit::record`. The push audit details retain grant ID,
change ID, stable error code, and explanation.

## Verify

`OfflineGrant.*` covers signature, scope, expiry, revocation, and cached reads.
`SyncRegistry.RevokedOfflineGrantRefusesPublicationWithChangeAndReason` proves publication refusal
and its per-change result. `PostgresOfflineRecovery.*` repeats the grant lifecycle against the
numbered Postgres schema.
