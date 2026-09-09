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
