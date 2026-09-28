# Control stores

Memory and PostgreSQL stores implement the public Store contract together. Token issuance retains
its parent and caps expiry; authentication checks the complete session/device ancestry. PostgreSQL
DDL lives in `db/schema.sql` and must be applied before the containing binary.

Verify changes with the full PostgreSQL-backed static and shared CTest suites. Keep the identity
contract in `docs/api/identity.md` updated with changes to token derivation.

Password sign-up provisions a personal customer and ownership grant in the same PostgreSQL
transaction as the account and session. The memory store stages fallible credential derivation
before publishing account rows. See `docs/api/provisioning.md`.

Customer lists optionally filter by user ID before the limit. Audit lists apply exact request
filters and event-ID cursors before the limit in both stores. The test regressions and fixture
rules are in `docs/build/testing.md`; the public query contract is in `docs/api/audit.md`.

PostgreSQL session authentication uses a unique public lookup UUID and constant-time SHA-256
verification of one random bearer. Legacy sessions are invalidated by migration `0022_session_lookup`.
Password Argon2 policy remains unchanged. See `docs/api/auth.md` and `docs/control-plane/schema.md`.

Agent tasks (lake#269) are claimed with one insert into `agent_task_status`, so exactly one claim
wins. A finished task keeps its phase and loses its credential digest. `agent_bus.cpp` publishes
tasks on Zetta pub/sub and applies what agents publish; `stale_agent_tasks` is what its loop
re-publishes or fails. See `docs/api/agent-bus.md`.

`web_apps.cpp` defines the Ship App JSON and deterministic namespace contract. Memory and
PostgreSQL stores enforce live name/host uniqueness and app/replica quotas; desired writes advance
generation and PostgreSQL notifies `lake_apps`. The operator writes token hashes only; Lake checks
purpose, current binding and target, and purges rows after Deleted acknowledgement. See
`docs/control-plane/web-apps.md`.

`builds.cpp` defines build JSON plus source, push and event token purposes. Memory and PostgreSQL
stores insert queued rows, retain operator observations and notify `lake_builds` on create/cancel.
See `docs/control-plane/builds.md`.
