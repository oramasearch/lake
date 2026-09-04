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

Migration `0003` is reserved for the identity-core work developed in parallel.

| Table | Purpose |
|---|---|
| `objects` | the authorization graph: type, stable external id, optional parent and `inherit` cut flag |
| `teams` | named teams within an organization |
| `memberships` | user or workload membership in an organization or team, its `owner` / `admin` / `member` role, TAAP attestation digest and granting identity |
| `grants` | principal, policy object and role, with the TAAP attestation digest and granting identity |
| `invitations` | email or handle invitations, expiry, state and accepted membership |
| `audit_events` | the provisional Phase 6 event shape used by these mutations until issue #18 formalizes the stream |

The migration backfills policy objects for existing customers, organizations and repositories.
New rows add their policy objects in the same store transaction. Parent topology and the role
matrix are validated by the stores; graph traversal and action evaluation are documented in
[`../api/policy.md`](../api/policy.md).

This mirrors the Orama Cloud v1 tenant model (user and organization are identity, customer is
the tenant, one handle namespace) and fixes its two known gotchas at the constraint level:
case-sensitivity and unvalidated handle format.

## How it is applied

With `spec.postgres` on, the operator owns, per Lake, a ConfigMap `lake-<name>-controldb-migrations`
(the SQL) and a `PostgresSchema` `lake-<name>-controldb-schema` whose controller runs an idempotent
Job: wait for the DB, skip if the guard table exists, apply the whole file in **one transaction**
with `psql --single-transaction`. **The guard is the table the newest migration creates**
(`invitations` today), so a new migration means a new guard and the next Job re-runs the
idempotent file, adding only what is missing. The version is a content hash of the SQL
(`sha256:<12 hex>`); `kubectl get lake` shows it as `SCHEMA`.

**Migrations never run in lake pods**, so the replica count cannot duplicate or race them.

## How to change it

Today the mechanism is single-shot: once the guard table exists, a changed version is a no-op
(operator issue #126 tracks incremental migrations). Until that lands:

1. Edit `db/schema.sql` idempotently (`IF NOT EXISTS`, `CREATE OR REPLACE`, `DROP … IF EXISTS`
   before `CREATE TRIGGER`), append a new `INSERT INTO schema_migrations`.
2. Copy to the operator's `assets/lake-schema.sql`; the version hash changes automatically.
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
  -c "SELECT version FROM schema_migrations WHERE version='0004_organizations_policy'"
```
