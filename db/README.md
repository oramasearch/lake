# Database schema

`schema.sql` is the idempotent PostgreSQL source of truth. Migration `0022_session_lookup`
adds a unique public session selector and revokes legacy sessions without a selector once.
New session rows store SHA-256 of a random bearer. Apply it before the new binary. The operator
must vendor the same bytes and use `session_lookup_guard`; that handoff is a separate change.
See `docs/control-plane/schema.md` and `docs/api/auth.md` for rollout and replacement credentials.

Migrations `0034_web_apps` and `0035_web_apps_operator_contract` add the App desired/observed row,
runtime mode, optional source entrypoint and operator-written token hashes. The latest guard is
`web_apps_operator_contract_guard`. Apply before exposing Apps; the upgrade removes the obsolete
credential table. Lake never stores app tokens. See `docs/control-plane/web-apps.md`.

Verify changes with the full PostgreSQL-backed static and shared CTest suites. Keep the identity
contract in `docs/api/identity.md` updated with changes to token derivation.
