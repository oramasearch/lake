# Break-glass recovery API

## What

A root recovery ceremony splits the customer root private key into Shamir shares among named
people, so that a quorum of them, and no one alone, can restore it (lake#934).

- `begin_root_recovery {customer_id, threshold, participants}` names two to 255 distinct TAAP
  identities, each holding `admin` on the customer, and a `threshold` from 2 to their number.
  Participant *i* holds share *i*. The answer is the ceremony id, the threshold, the share count,
  each participant's index and the expiry (15 minutes). It carries no share. Any customer
  administrator or the operator may begin one; beginning it gives the caller nothing to recover.
- `claim_root_recovery_share {customer_id, ceremony_id}` answers the caller's own share
  (`index`, hex `value`), once. The caller must be a participant signed in as themselves: a
  browser session, account token or TAAP sign-in of that person, holding `admin` on the customer.
  The operator, agents and workloads are refused (`forbidden`); a second claim is `conflict`.
- `complete_root_recovery {customer_id, ceremony_id, share}` is each participant presenting their
  own share, under the same rule. Lake checks the value against the ceremony; a wrong value is
  `unauthorized` and can be presented again, someone else's share is `forbidden`, and presenting
  twice is `conflict`. The answer is `{recovered, submitted, threshold}`; the ceremony is
  recovered, and records `recovered_at`, when `threshold` distinct participants have presented
  theirs. A recovered or expired ceremony issues and accepts nothing more.

Lake stores no share. A ceremony's polynomial coefficients are derived from the root key itself
(HMAC-SHA256 keyed with the root private key over a label, the ceremony id and a block counter, as
RFC 6979 derives a nonce), so whoever can compute a share already holds the root. Lake derives a
participant's share when they claim it and again to check what they present, and at the start
proves the derivation restores the root. `pki_recovery_ceremonies` keeps the quorum and expiry,
`pki_recovery_participants` (migration `0093_root_recovery_participants`) who holds each index and
when it was issued, and `pki_recovery_shares` a SHA-256 evidence digest of each presented share.
Every call is audited centrally as kind `recovery`, object `root-recovery/<ceremony>`; the details
name the threshold and participants, or the share index, never a share value.

Before lake#934, `begin_root_recovery {threshold, share_count}` answered every share to the one
caller, who could therefore restore the root alone, and `complete_root_recovery` took the whole
quorum in one call. Both forms are refused now (`invalid_input`), and a ceremony begun before
migration `0093` has no participants, so it cannot be completed.

What this does not prevent: an administrator can make other accounts administrators and name
them. The quorum separates people only as far as the customer's grants do; review who holds
`admin` before a ceremony.

## Degraded operation

When the control plane is unavailable, Lake cannot authenticate new callers, issue or renew
credentials, create ACME orders, or begin a recovery ceremony. Existing X.509, SSH, client, and
device credentials continue until their expiry; systems must renew before their normal operating
margin. JWT SVID verification continues from a cached issuer key until the token expiry. The CA
root is never made available as a fallback credential.

## How to verify

`Pki.ShamirRecoveryRequiresConfiguredQuorum` reconstructs a secret with the quorum and proves a
one-share-short attempt fails. `Pki.RootRecoveryBeginsWithoutHandingOutAnyShare` checks that
`begin_root_recovery` answers no share value and refuses a threshold of one, repeated or
non-administrator participants and the old `share_count` form.
`Pki.RootRecoveryNeedsSharesPresentedByDistinctParticipants` checks that only each participant,
signed in as themselves, claims their share once, that one person holding two shares cannot
present both, that the ceremony recovers only at the second distinct participant, and that the
audit names the presenter but not the share. `PostgresPki.PersistsAndRewrapsCustomerAuthority`
runs the claims and presentations across PKI service processes against PostgreSQL.

## How to change

Keep shares out of durable storage, logs, audit details and any answer but a participant's own
claim. Never answer more than one share to one caller, and keep each check on the caller's own
identity in the store's single statement or locked transaction, so concurrent calls cannot claim
or present a share twice. Rehearsals use the same capability path as an emergency recovery.
