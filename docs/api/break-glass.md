# Break-glass recovery API

## What

`begin_root_recovery` creates Shamir shares for a caller-selected quorum and returns them only in
that response. `complete_root_recovery` accepts the quorum, validates that it restores the
customer root, and writes a signed audit event. One share fewer than the threshold fails. Shares
are not retained by Lake.

## Degraded operation

When the control plane is unavailable, Lake cannot authenticate new callers, issue or renew
credentials, create ACME orders, or begin a recovery ceremony. Existing X.509, SSH, client, and
device credentials continue until their expiry; systems must renew before their normal operating
margin. JWT SVID verification continues from a cached issuer key until the token expiry. The CA
root is never made available as a fallback credential.

## How to verify

The recovery suite reconstructs a root with the configured quorum, proves a one-share-short
attempt fails, and verifies the corresponding audit events.

## How to change

Keep shares out of durable storage and logs. Rehearsals use the same capability path as an
emergency recovery.
