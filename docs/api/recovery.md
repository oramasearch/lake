# Account and encrypted-repository recovery

**Status: implemented by lake #23; email abuse protections and cancellation by lake#968 (LSR-102), not deployed.** Recovery codes, email recovery with a 24-hour cooling period,
and organization recovery approved by two distinct administrators restore account sign-in. They
do not recreate end-to-end encrypted repository keys.

## Account recovery paths

| Capability | Caller and result |
|---|---|
| `recover_with_code` | Public entry. Consumes one of the eight Argon2id-hashed codes created by `enroll_mfa` and issues a browser session. A consumed code cannot be reused. |
| `begin_email_recovery` | Public entry with pool and email. Reuses an unexpired email attempt; otherwise, within the budgets below, creates a one-use token, a 24-hour cooling period, a further 24-hour completion window, an operator mail delivery, and a notice for every registered device. **It answers the same for every address** (lake#774): `{"status": "requested", "message": "If an account uses this address, a recovery link is on its way."}`, whether or not the address has an account; the attempt (its id, the account's user id, its timing) reaches only the address's owner, in the recovery email. It used to answer 200 with the attempt for a known address and 404 for an unknown one. |
| `begin_admin_recovery` | An organization owner or administrator opens recovery for a user who is a member of that organization. The requester supplies approval one. |
| `approve_admin_recovery` | A different owner or administrator supplies approval two. Reuse by the requester is refused. Approval queues the one-use completion token and notifies every device. |
| `cancel_recovery` | Signed-in account holder only (Lake or application pool), with `recovery_id`. Cancels their own pending email or administrator recovery, invalidates its token and removes unclaimed mail. Another account, an anonymous caller or a completed/expired attempt is refused. The operation is linked from each device notice as `cancel_action` (`POST /v1/cancel_recovery` with its input). |
| `complete_recovery` | Public entry with recovery UUID and token after the cooling/approval condition. Atomically consumes the attempt, revokes the user's existing sessions, notifies every existing device, and issues a fresh browser session on the named device. |
| `claim_recovery_deliveries` | Operator-only mail-worker action. Atomically claims pending deliveries and clears their plaintext tokens from Postgres. |
| `list_recovery_notifications` | Self read of the recovery notices addressed to the user's device rows. |

Organization start and approval resolve the attempt's organization and call the shared
`authorize(..., "admin")` policy path. The store repeats the membership and distinct-approver
checks inside its transaction before changing recovery state.

Tokens are stored only as password hashes on attempts. Plaintext exists in the delivery outbox
until the operator claims it, and a claimed row is not returned again. Public completion responses
use the ordinary `lake_session` HttpOnly cookie path. All capability outcomes use the registry's
signed audit path; cancellation is classified in the central identity log.

The email cooling period is a security delay, not a delivery delay: the mail worker may deliver
the link immediately, but `complete_recovery` refuses it until `ready_at`. Operators should use the
device notices to present the start, second approval, and completion events to active clients.

## Email recovery budgets

`begin_email_recovery` always returns the accepted body above, including for unknown addresses,
case variants, duplicate requests and exhausted budgets. Store failures still fail the request;
they never enqueue mail through a fallback. An accepted response promises no delivery.

Both stores enforce these limits before creating a token, delivery or notice:

- Reuse an existing unexpired `cooling` email attempt for the account, across pools and address
  case variants. Preserve its original token, ready time and expiry; claim does not requeue mail.
- Create at most one email attempt per account per 24 hours, counting cancelled and completed
  attempts. Cancellation cannot restart the account's budget.
- Admit at most 30 requests per client address per hour and 300 requests globally per hour.
  Unknown addresses and duplicates spend these budgets too. A window starts with its first
  admitted request and resets after an hour; denied requests do not extend it.

The HTTP adapter supplies the existing ingress-derived address (`order_client_address`) to
REST and MCP; capability inputs cannot supply it. Deployments must keep ingress-nginx's
sanitizing of `X-Real-IP`/the final forwarded hop and prevent direct access around that ingress,
as for the order form. No address means a shared direct-client bucket. PostgreSQL retains only
the SHA-256 address digest and expires idle client buckets after an hour; the global cap bounds
new rows even when a caller varies addresses. Budgets persist across store instances and replicas.

PostgreSQL serializes account starts by locking the user row; budget upserts are atomic.
The memory store uses its mutex. Cancellation and completion serialize on the attempt, so a
cancelled token cannot complete or gain a later administrator approval. A mail delivery already
claimed by a worker cannot be recalled, but cancellation invalidates its token immediately.

Migration `0096_email_recovery_limits` adds durable budgets and the cancelled state. Before
rollout, the operator must vendor `db/schema.sql` byte for byte and select
`email_recovery_limits_guard`. To change a budget, update both stores, this contract and
`EmailRecoveryLimits.*` together.

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

`Stores/EmailRecoveryLimits.*` runs the same contract against memory and PostgreSQL: concurrent
first starts and address case variants create one delivery and device notice; duplicates preserve
attempt timing after mail is claimed; unknown addresses spend client/global budgets; a new store
instance keeps the budget and an elapsed window resets it; owner cancellation refuses another
account, invalidates already delivered tokens, removes pending mail and cannot be reversed by
administrator approval. PostgreSQL moves a cancelled attempt beyond its cooling period before
trying its original token.
