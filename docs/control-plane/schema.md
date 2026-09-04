# Control-plane schema

**Source of truth: `db/schema.sql` in this repository.** The operator normally carries a
byte-identical vendored copy at `internal/controller/assets/lake-schema.sql` and applies it to
every Lake's Postgres. The Phase 6 audit waiver defers that copy and its guard update to the
separate operator pull request opened once the wave is integrated; the operator's test checks the
copy carries the source-of-truth header.

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

**Audit** (migration `0005_audit`):

| Table | Columns and invariants |
|---|---|
| `lake_identity` | Every historical lake TAAP/1 identity: UUID, TAAP ID, public key, wrapped generated Ed25519 seed, creation timestamp, optional dual-signed rotation document, and a partial-unique `current` row. Operator-supplied seeds remain outside this table. |
| `audit_events` | Ordered sequence, UUID, timestamp, optional customer, kind, principal, object, action, outcome, request ID, optional method/device, JSON facts, predecessor digest, signer, signature, and digest. `audit_events` rejects UPDATE and DELETE through triggers. |
| `audit_retention` | One customer retention setting, constrained to at least 400 days and timestamped on update. |

`audit_events` is the new migration guard table. The application takes a transaction advisory lock
before it reads the latest digest and appends the next event. The signature and digest chain make
a missing or reordered row detectable to `verify_audit`.

This mirrors the Orama Cloud v1 tenant model (user and organization are identity, customer is
the tenant, one handle namespace) and fixes its two known gotchas at the constraint level:
case-sensitivity and unvalidated handle format.

## How it is applied

With `spec.postgres` on, the operator owns, per Lake, a ConfigMap `lake-<name>-controldb-migrations`
(the SQL) and a `PostgresSchema` `lake-<name>-controldb-schema` whose controller runs an idempotent
Job: wait for the DB, skip if the guard table exists, apply the whole file in **one transaction**
with `psql --single-transaction`. **The guard is the table the newest migration creates**
(`audit_events` for the current source), so a new migration means a new guard and the next Job
re-runs the idempotent file, adding only what is missing. The version is a content hash of the SQL
(`sha256:<12 hex>`); `kubectl get lake` shows it as `SCHEMA`.

**Migrations never run in lake pods**, so the replica count cannot duplicate or race them.

## How to change it

Today the mechanism is single-shot: once the guard table exists, a changed version is a no-op
(operator issue #126 tracks incremental migrations). Until that lands:

1. Edit `db/schema.sql` idempotently (`IF NOT EXISTS`, `CREATE OR REPLACE`, `DROP … IF EXISTS`
   before `CREATE TRIGGER`), append a new `INSERT INTO schema_migrations`.
2. The operator's byte-identical copy and `GuardTable` update are waived during this Phase 6 wave;
   they land together in the separate operator PR opened once the wave is integrated. Do not copy
   a partial schema between waves.
3. For an existing Lake, delete its `PostgresSchema` (`kubectl delete postgresschema
   lake-<name>-controldb-schema -n <ns>`); it is owned, so the operator recreates it within a
   reconcile and the new Job applies the file.

## Verify

```sh
kubectl get postgresschema -n <namespace>                              # APPLIED = VERSION, PHASE Applied
kubectl exec -n <namespace> lake-<name>-controldb-1 -c postgres -- psql -U postgres -d lake \
  -c "SELECT version FROM schema_migrations" \
  -c "SELECT conname FROM pg_constraint WHERE conrelid='customers'::regclass"
```
