# Break-glass recovery API

## What

Each customer CA root has a configured Shamir threshold and share count. A recovery ceremony is
an explicit, time-bounded capability. It accepts the configured quorum, validates each submitted
share against its stored digest, restores the root only in process, and writes a signed audit event
for every attempt. One share fewer than the threshold fails.

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

Changing a threshold creates a new ceremony; it does not alter an active ceremony. Keep shares
out of durable storage and logs. Rehearsals use the same capability path as an emergency recovery.
