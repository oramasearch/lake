# Control-plane schema

**Source of truth: `db/schema.sql` in this repository.** The operator carries a byte-identical
vendored copy at `internal/controller/assets/lake-schema.sql` and applies it to every Lake's
Postgres. Change the schema here, then copy it into the operator in the same change; the
operator's test checks the copy carries the source-of-truth header.

## Tables (migration `0001_identity_and_customers`)

`schema_migrations(version, applied_at)` records what has been applied.

**Identity** (who; owns nothing):

| Table | Columns |
|---|---|
| `users` | `id uuid` (v4, `gen_random_uuid()`), `taap_id text UNIQUE` (the person's TAAP identity `taap:1:<alg>:<base32>`, null until they have a key), `email text` (unique on `lower(email)`), `display_name`, `created_at`, `updated_at` |
| `organizations` | `id uuid`, `taap_id text UNIQUE`, `display_name`, `created_at`, `updated_at` |

**Tenant** (the customer owns everything; every owned table will carry `customer_id … ON DELETE
CASCADE`):

| `customers` | |
|---|---|
| `id uuid` | v4 |
| `customer_type text` | `individual` or `organization` |
| `user_id uuid UNIQUE` → `users`, `organization_id uuid UNIQUE` → `organizations` | nullable; the UNIQUEs give exactly one customer per user and per organization (NULLs don't collide); `ON DELETE CASCADE` |
| `customer_type_check` | a customer is one or the other, never both: the type ties to exactly one non-null identity column |
| `name varchar(256) NOT NULL` | the handle: **one global namespace** shared by individuals and organizations (`customer_name_unique`); lowercase letters, digits, inner hyphens, 1–256 chars (`customer_name_format`), so uniqueness is effectively case-insensitive and a handle can never carry spaces or uppercase. No default: the application generates handles (`{adjective}-{noun}-{number}`) and must pre-check uniqueness to answer 409 rather than 500 |

**Repositories** (migration `0002_repositories`):

| `repositories` | |
|---|---|
| `id uuid` | v4 |
| `customer_id uuid NOT NULL` → `customers` | the owner; `ON DELETE CASCADE`; indexed |
| `name varchar(256) NOT NULL` | slug in the handle format (`repository_name_format`); `UNIQUE (customer_id, name)` so the address is `<customer>/<name>` |
| `description text` | default empty |
| `visibility text` | `private` (default) or `public` (`repository_visibility_check`) |

`updated_at` is maintained by a trigger (`set_updated_at()`) on every table.

## Organizations and authorization (migration `0004_organizations_policy`)

| Table | Purpose |
|---|---|
| `objects` | the authorization graph: type, stable external id, optional parent and `inherit` cut flag |
| `teams` | named teams within an organization |
| `memberships` | user or workload membership in an organization or team, its `owner` / `admin` / `member` role, TAAP attestation digest and granting identity |
| `grants` | principal, policy object and role, with the TAAP attestation digest and granting identity |
| `invitations` | email or handle invitations, expiry, state and accepted membership |

The migration backfills policy objects for existing customers, organizations and repositories.
New rows add their policy objects in the same store transaction. Parent topology and the role
matrix are validated by the stores; graph traversal and action evaluation are documented in
[`../api/policy.md`](../api/policy.md).

This mirrors the Orama Cloud v1 tenant model (user and organization are identity, customer is
the tenant, one handle namespace) and fixes its two known gotchas at the constraint level:
case-sensitivity and unvalidated handle format.

## Audit (migration `0005_audit`)

| Table | Columns and invariants |
|---|---|
| `lake_identity` | Every historical lake TAAP/1 identity: UUID, TAAP ID, public key, wrapped generated Ed25519 seed (AES-256-GCM under `LAKE_INSTANCE_KEK`), creation timestamp, optional dual-signed rotation document, and a partial-unique `current` row. An operator-supplied seed (`LAKE_IDENTITY_SEED`) is used in place of a generated one. |
| `audit_events` | Ordered `sequence`, UUID, timestamp, optional customer, kind, principal, object, action, outcome (`success`, `failure`, `denied`, `revoked`), the request id as text (clients may send `x-request-id`), optional method and device, JSON details, predecessor digest, signer, signature and digest. UPDATE and DELETE are rejected by triggers. |
| `audit_retention` | One retention setting per customer, constrained to at least 400 days, timestamped on update; the application refuses to lower it. |

Every capability call, sign-in and session event is one row, written by `audit::record` under a
transaction advisory lock that reads the latest digest and appends the next event; the signature
and digest chain make a missing or reordered row detectable to `verify_audit`. The provisional
`audit_events` shape that migration `0004` carried is gone: the store methods no longer write
their own rows, because `Registry::call` records every outcome.

## Repository lifecycle (migration `0006_repository_lifecycle`)

`repositories` gains nullable `deleted_at` and `retention_until` timestamps. Its visibility
constraint also accepts `organization`, between private and public. `repository_refs` holds the
current repository heads. Lifecycle operations update
the repository and its policy-object parent in one store transaction; the registry records each
attempt in the signed audit stream.

## Identity core (migration `0007_identity_core`)

| Table | Purpose |
|---|---|
| `identity_documents` | Verified TAAP/1 document and SHA-512/256 digest for each user. |
| `credentials` | Pool-scoped password, TOTP, passkey, OAuth, or admin proof; secrets are hashed or encrypted. Passkey public data carries its device link and lake-signed TAAP binding. |
| `sessions` | Browser, API-token, static, or admin bearer hash; scopes, expiry, revocation, and last use. |
| `devices` | Named user device and revocation state; `list_devices` joins passkey binding data from its credential. |
| `keystore` | Purpose-specific data key wrapped by the instance KEK. |
| `oauth_providers` | Pool-scoped provider configuration and encrypted client secret. |
| `identity_oauth_flows` | Five-minute, one-use OAuth state, encrypted PKCE verifier, nonce, and initiating device. |
| `identity_webauthn_challenges` | Five-minute, one-use registration/assertion challenge with pool, RP ID, and origin. |
| `identity_core_guard` | Guard table for the identity-core migration. |

Migration numbers `0004`–`0006` landed first from parallel branches, so identity core follows as
`0007`. It uses the signed `audit_events` and `lake_identity` tables from migration `0005`; it does
not recreate the provisional audit shape from the earlier identity checkpoint.

## MFA recovery (migration `0008_mfa_recovery`)

The `credentials` kind constraint accepts `recovery`. Each MFA enrollment stores its TOTP seed as
an encrypted `totp` credential and eight independently Argon2id-hashed recovery credentials.
Successful recovery-code verification revokes that row immediately. Re-enrollment and verified
disablement revoke all prior recovery rows. `identity_mfa_guard` is the migration guard.

## How it is applied

With `spec.postgres` on, the operator owns, per Lake, a ConfigMap `lake-<name>-controldb-migrations`
(the SQL) and a `PostgresSchema` `lake-<name>-controldb-schema` whose controller runs an idempotent
Job: wait for the DB, skip if the guard table exists, apply the whole file in **one transaction**
with `psql --single-transaction`. **The guard is the table the newest migration creates**
(`identity_mfa_guard` today), so a new migration means a new guard and the next Job re-runs the
idempotent file, adding only what is missing. The version is a content hash of the SQL
(`sha256:<12 hex>`); `kubectl get lake` shows it as `SCHEMA`.

**Migrations never run in lake pods**, so the replica count cannot duplicate or race them.

## How to change it

Today the mechanism is single-shot: once the guard table exists, a changed version is a no-op
(operator issue #126 tracks incremental migrations). Until that lands:

1. Edit `db/schema.sql` idempotently (`IF NOT EXISTS`, `CREATE OR REPLACE`, `DROP … IF EXISTS`
   before `CREATE TRIGGER`), append a new `INSERT INTO schema_migrations`.
2. Copy to the operator's `assets/lake-schema.sql`; the version hash changes automatically. During
   the Phase 6 waves this copy and the `GuardTable` update land once per wave in a separate operator
   pull request (orama-operator #134 carries migration `0006`; identity core still needs its
   operator copy), never
   a partial schema between waves. The `0008_mfa_recovery` copy remains required in the operator.
3. Set the operator's `GuardTable` to the new migration's table.
4. For an existing Lake, delete its `PostgresSchema` (`kubectl delete postgresschema
   lake-<name>-controldb-schema -n <ns>`); it is owned, so the operator recreates it within a
   reconcile and the new Job applies the file.

## Verify

```sh
kubectl get postgresschema -n <namespace>                              # APPLIED = VERSION, PHASE Applied
kubectl exec -n <namespace> lake-<name>-controldb-1 -c postgres -- psql -U postgres -d lake \
  -c "SELECT version FROM schema_migrations" \
  -c "SELECT conname FROM pg_constraint WHERE conrelid='customers'::regclass" \
  -c "SELECT version FROM schema_migrations WHERE version IN ('0007_identity_core', '0008_mfa_recovery')"
```
