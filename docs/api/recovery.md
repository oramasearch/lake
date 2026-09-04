# Account and encrypted-repository recovery

**Status: implemented by lake #23.** Recovery codes, email recovery with a 24-hour cooling period,
and organization recovery approved by two distinct administrators restore account sign-in. They
do not recreate end-to-end encrypted repository keys.

## Account recovery paths

| Capability | Caller and result |
|---|---|
| `recover_with_code` | Public entry. Consumes one of the eight Argon2id-hashed codes created by `enroll_mfa` and issues a browser session. A consumed code cannot be reused. |
| `begin_email_recovery` | Public entry with pool and email. Creates a one-use token, a 24-hour cooling period, a further 24-hour completion window, an operator mail delivery, and a notice for every registered device. |
| `begin_admin_recovery` | An organization owner or administrator opens recovery for a user who is a member of that organization. The requester supplies approval one. |
| `approve_admin_recovery` | A different owner or administrator supplies approval two. Reuse by the requester is refused. Approval queues the one-use completion token and notifies every device. |
| `complete_recovery` | Public entry with recovery UUID and token after the cooling/approval condition. Atomically consumes the attempt, revokes the user's existing sessions, notifies every existing device, and issues a fresh browser session on the named device. |
| `claim_recovery_deliveries` | Operator-only mail-worker action. Atomically claims pending deliveries and clears their plaintext tokens from Postgres. |
| `list_recovery_notifications` | Self read of the recovery notices addressed to the user's device rows. |

Organization start and approval resolve the attempt's organization and call the shared
`authorize(..., "admin")` policy path. The store repeats the membership and distinct-approver
checks inside its transaction before changing recovery state.

Tokens are stored only as password hashes on attempts. Plaintext exists in the delivery outbox
until the operator claims it, and a claimed row is not returned again. Public completion responses
use the ordinary `lake_session` HttpOnly cookie path. All capability outcomes use the registry's
signed audit path.

The email cooling period is a security delay, not a delivery delay: the mail worker may deliver
the link immediately, but `complete_recovery` refuses it until `ready_at`. Operators should use the
device notices to present the start, second approval, and completion events to active clients.

## End-to-end encrypted repositories

Account recovery restores authentication only. Lake never has the repository trust-domain key and
cannot derive it from a password, MFA factor, administrator approval, or recovered session.

Before a device is lost, a repository administrator may call `configure_recovery_contact` with a
designated contact's TAAP identity and a trust-domain key already wrapped by the client to that
contact. Lake stores the opaque wrapped value; ordinary status and configuration responses never
render it. `recover_repository_key` returns it only to the designated TAAP identity (or the audited
operator break-glass identity), so the contact's client can unwrap it.

`repository_recovery_status` states the consequence plainly:

- With a configured contact, the repository is recoverable through that contact's wrapped key.
- Without one, a key held only on the lost device is unrecoverable and the encrypted repository is
  lost. Account recovery does not change that result.

## Verify

`AccountRecovery.*` covers recovery-code consumption, cooling refusal, distinct administrator
approval, session replacement, and device notices. `RepositoryRecovery.*` covers both the
unrecoverable and designated-contact cases. `PostgresOfflineRecovery.*` repeats every successful
account path and both repository-key outcomes on Postgres.
