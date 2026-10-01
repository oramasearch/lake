# Control-plane contracts

[streams.md](streams.md) describes creator ownership bound to a workspace incarnation,
server-side shared-bus isolation and migration `0037_streams` with `streams_guard`.

[schema.md](schema.md) describes persisted tables, migrations and the operator handoff, including
active-only repository-name uniqueness, token ancestry and the one-use invalidation of legacy
API/static tokens. See
[postgres.md](postgres.md) for the backing store and deployment contract.
[web-apps.md](web-apps.md) defines the `apps` desired row, protected-app authorization-code state,
operator status fields, notification, quotas, namespaces, Secret-reference boundary and the four
purpose-hash credentials including exact-repository image pulls.
[builds.md](builds.md) defines the `builds` desired row, configured Ship App runtime base, operator
status loop, cancellation, notifications and purpose-hash credential boundary.

Verify changes with the full PostgreSQL-backed static and shared CTest suites. Keep the identity
contract in `docs/api/identity.md` updated with changes to token derivation.

[schema.md](schema.md#session-lookup-migration) documents the indexed session selector, stored digest,
legacy-session invalidation and required operator handoff for `0022_session_lookup`.
