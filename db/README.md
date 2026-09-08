# Database schema

`schema.sql` is the idempotent PostgreSQL source of truth. Migration `0021_token_attenuation`
records token parents and revokes pre-migration API/static tokens once. Apply it before the new
binary. The operator must vendor the same bytes and use `token_attenuation_guard`.

Verify changes with the full PostgreSQL-backed static and shared CTest suites. Keep the identity
contract in `docs/api/identity.md` updated with changes to token derivation.
