# Control-plane contracts

[schema.md](schema.md) describes persisted tables, migrations and the operator handoff, including
token ancestry and the one-use invalidation of legacy API/static tokens. See
[postgres.md](postgres.md) for the backing store and deployment contract.
[web-apps.md](web-apps.md) defines the `apps` desired row, operator status fields, notification,
quotas, namespaces and Secret-reference boundary.

Verify changes with the full PostgreSQL-backed static and shared CTest suites. Keep the identity
contract in `docs/api/identity.md` updated with changes to token derivation.

[schema.md](schema.md#session-lookup-migration) documents the indexed session selector, stored digest,
legacy-session invalidation and required operator handoff for `0022_session_lookup`.
