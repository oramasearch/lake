# How the operator applies a lake version

**What:** a lake image carries everything the operator needs to run that version: the schema
(`lake migrate` applies it), the Stripe webhook events it handles, its Stripe products, and every
environment variable it reads (`lake manifest` prints them).
**Why:** Steve, 2026-10-24: *"why do we need to release a new operator to push out a new version
of lake?"*. Until this, the operator vendored `db/schema.sql` and listed lake's Stripe events in Go,
so a lake release with a new migration or event needed an operator release first.
**How:** the operator runs the commands below from the image a `Lake` names (`spec.image`).
Releasing lake is then only the `image:` in a set file.

Issues: [lake#1119](https://github.com/oramasearch/lake/issues/1119) (this side) and
[orama-operator#936](https://github.com/oramasearch/orama-operator/issues/936) (the operator side,
not done here).

## Rolling a version

1. **Read the image's manifest:** run the image with arguments `["manifest"]` (the image's
   `ENTRYPOINT` is `lake`). It needs no environment, no network and no database, and exits 0.
2. **Migrate before the new image serves:** run a Job from the same image with arguments
   `["migrate", "--wait-seconds", "600"]` and `LAKE_CONTROL_DB_URI` from the CNPG Secret
   `lake-<name>-controldb-app` (key `uri`), once per image digest. Keep the new Deployment's image
   held until the Job succeeds, as the `PostgresSchema` gate does today.
3. **Subscribe Stripe to the manifest's events:** set the Lake's webhook endpoint's enabled events
   to `stripe.webhook_events`, and create the `stripe.products` in the account.
4. **Render the environment the manifest declares:** every `required` variable must be set; a
   `secret` one comes from a Secret.
5. **Roll the Deployment.** The server never migrates.

A lake that predates `lake migrate` (v0.6.1 and earlier) answers `lake manifest` (and `lake migrate`)
with "unknown argument", its usage text on stdout and exit status 2, never JSON with
`manifest_version`. Detect support by the manifest, and keep the vendored path for those images
until none runs.

## `lake migrate`

```
lake migrate [--control-db URI] [--wait-seconds N]
```

- Applies the `db/schema.sql` built into the binary to `LAKE_CONTROL_DB_URI` (or `--control-db`).
  It reads no other setting, so a setting the server would refuse cannot stop it.
- **The same result as the operator's `PostgresSchema` Job**, checked in the same order: connect
  (retrying every 2 s for up to `--wait-seconds`, default 0), then, in **one transaction under
  `pg_advisory_xact_lock(0x6c616b656d6967)`**, skip when the newest guard table (`schema.guard_table`)
  exists and every migration the file records is in `schema_migrations`; otherwise run the whole
  file and commit. Objects are owned by the connecting role, `lake` with the CNPG app Secret, as
  with the Job. A second run started at the same time waits for the lock, then finds the database
  current.
- Tests: `LakeMigrate.*` in `tests/test_migrate.cpp` (an empty database gets the same tables,
  columns, constraints, indexes, functions, triggers, owners, migration rows and row counts as
  `psql --single-transaction -f db/schema.sql`; a current database is left unchanged; a missing
  migration row re-applies; four concurrent runs apply once; a failed statement leaves an empty
  database empty; an unreachable server is `connect`).

**Exit codes.**

| Code | Meaning | What the operator does |
|---|---|---|
| 0 | applied, or already current | roll the image |
| 1 | a statement failed; the transaction rolled back, nothing changed | keep the image held; the Job's log has the error |
| 2 | bad arguments, or no database URI | fix the Job spec |
| 3 | the database could not be reached within `--wait-seconds` | retry (Job backoff) |

**Output.** One JSON line on stdout, success or failure; a one-line summary of a failure on stderr.

```json
{"command":"migrate","ok":true,"outcome":"applied","version":"0.6.1","revision":"<commit>",
 "schema":{"sha256":"<64 hex>","version":"sha256:<12 hex>","latest_migration":"0099_account_balances",
           "guard_table":"account_balances_guard","migrations":97},
 "migrations_missing":["0099_account_balances"],"migrations_applied":["0099_account_balances"],
 "guard_tables_created":["account_balances_guard"],"migrations_recorded":97,"elapsed_ms":812}
```

`outcome` is `applied` or `current` (then the three lists are empty). On failure:

```json
{"command":"migrate","ok":false,"outcome":"failed","version":"0.6.1","revision":"<commit>",
 "schema":{...},"error":{"code":"apply","message":"ERROR: ..."}}
```

`error.code` is `usage` (exit 2), `apply` (exit 1) or `connect` (exit 3). The message never carries
the URI's password.

## `lake manifest`

```
lake manifest
```

Prints one JSON document, `manifest_version` 1:

| Field | What |
|---|---|
| `name`, `version`, `revision` | `lake`, the release, the source commit the image was built from |
| `schema.sha256`, `schema.version` | the embedded `db/schema.sql`'s SHA-256, and `sha256:` + its first 12 hex digits |
| `schema.latest_migration` | the greatest `NNNN_name` the file records in `schema_migrations` |
| `schema.guard_table` | the guard table the newest migration creates |
| `schema.guard_tables` | every `*_guard` table the file creates, in file order: what a restore rehearsal counts, replacing the operator's hard-coded `postgresGuardCountQuery` |
| `schema.migrations` | every migration the file records, in file order |
| `migrate` | `args` (`["migrate"]`), `wait_flag`, `database_env` (`LAKE_CONTROL_DB_URI`), `lock_key`, and `exit_codes` as above |
| `stripe.webhook_events` | the Stripe events this version handles, sorted (18 today), replacing the operator's `lakeStripeWebhookEvents` |
| `stripe.products` | `{id, name}` of each Stripe product lake bills under (`lake_usage`, `lake_private_warehouse`, `lake_dedicated_cluster`), replacing `lakePaymentProducts` |
| `environment` | `{name, required, secret, description}` for every variable lake reads, sorted by name |
| `features` | what this image supports that the operator switches on, additive under version 1. `agent_bus_workspace_credentials`: a `LAKE_AGENT_BUS_DIR` file `<agent>.<workspace id>` is a bus credential bound to that workspace, and lake re-reads the directory when it changes, so adding or removing one needs no restart (lake#871; orama-operator#1176 runs per-workspace agents only for an image that lists it). `LakeManifest.ListsTheFeaturesTheOperatorSwitchesOn` checks it |

`required` marks what a Lake run by the operator cannot work without: `LAKE_CONTROL_DB_URI`,
`LAKE_ADMIN_TOKEN`, `LAKE_INSTANCE_KEK`, `LAKE_STORE_ROOT`, `LAKE_PUBLIC_HOST` and
`ZETTA_EGRESS_ALLOW`. The rest are optional, each turning on what its description says (Clerk,
Stripe, the agent bus, regions, the order sheet).

**Why the lists cannot drift.**

- `stripe.webhook_events` is `stripe_webhook_events` in `include/lake/api/stripe.hpp`;
  `StripeWebhookEvents.AreExactlyTheEventsTheHandlerDispatchesOn` fails when
  `src/api/stripe.cpp` compares an event type not listed, or a listed one is compared nowhere.
  `server_info` publishes the same list as `stripe_webhook_events`.
- `environment` is `environment_variables()` in `src/api/manifest.cpp`;
  `LakeManifest.DeclaresExactlyTheEnvironmentLakeReads` fails when `src/main.cpp` reads a variable
  not declared there, or one declared is no longer read.
- `schema` is read from the embedded file; `LakeManifest.CarriesTheSchemaFileByteForByte` checks the
  embedded bytes equal `db/schema.sql`, and `LakeManifest.TheGuardTableIsTheNewestMigrations` that
  the newest migration creates the guard.

## What changes for a release

From the release that carries these commands, a lake release that adds a migration, a Stripe event
or an environment variable needs no operator release once orama-operator#936 consumes them: the
operator reads them from the image. Until then the operator keeps vendoring the schema
(`internal/controller/assets/lake-schema.sql`) and the steps in [aks.md](aks.md) still apply.
