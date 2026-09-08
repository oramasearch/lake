# Control-plane contracts

[schema.md](schema.md) describes persisted tables, migrations and the operator handoff, including
token ancestry and the one-use invalidation of legacy API/static tokens. See
[postgres.md](postgres.md) for the backing store and deployment contract.

Verify changes with the full PostgreSQL-backed static and shared CTest suites. Keep the identity
contract in `docs/api/identity.md` updated with changes to token derivation.
