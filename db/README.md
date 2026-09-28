# Database schema

`schema.sql` is the idempotent PostgreSQL source of truth. Migration `0022_session_lookup`
adds a unique public session selector and revokes legacy sessions without a selector once.
New session rows store SHA-256 of a random bearer. Apply it before the new binary. The operator
must vendor the same bytes and use `session_lookup_guard`; that handoff is a separate change.
See `docs/control-plane/schema.md` and `docs/api/auth.md` for rollout and replacement credentials.

Migration `0034_web_apps` adds the Ship App desired-and-observed row and `web_apps_guard`. Apply it
before a binary that exposes the app capabilities. Its Lake-owned `app_credentials` table retains
scoped values under envelope encryption while the shared `apps` row carries only references. The
operator ownership contract is `docs/control-plane/web-apps.md`.

Verify changes with the full PostgreSQL-backed static and shared CTest suites. Keep the identity
contract in `docs/api/identity.md` updated with changes to token derivation.
