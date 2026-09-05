# Test fixture isolation

## What exists

Lake test processes create one randomly suffixed directory below the platform temporary root. Every
filesystem fixture is a named child of that directory, and the process removes the directory at
exit. The helper uses `std::filesystem::temp_directory_path`, so platform selection and `TMPDIR`
remain effective. Failure to create the process directory reports the selected root and the
filesystem error.

Postgres-backed tests connect only through `LAKE_TEST_DB_URI`. They do not create or drop the named
database. Concurrent suites therefore receive distinct, freshly created databases with
`db/schema.sql` applied and distinct random `LAKE_INSTANCE_KEK` values.

## Why

Fixed paths below a shared temporary root let concurrent test processes delete or rewrite each
other's repository stores and search manifests. Shared Postgres databases similarly mix durable
rows between processes. Process-scoped filesystem and database names let independent worktrees run
the full suite concurrently.

## How to verify

Configure two build directories from the same checkout. Create two uniquely named Postgres
databases, apply `db/schema.sql` to each, then start both CTest suites concurrently with a distinct
`LAKE_TEST_DB_URI` and random `LAKE_INSTANCE_KEK` for each process. Both suites must report 100%.

Also verify that test code has no direct system-temporary paths:

```sh
rg -n 'temp_directory_path|/tmp/lake-' tests
```

Only `tests/temporary_directory.hpp` should match.

## How to change

Use `lake::test::fixture_path()` for every new filesystem fixture. Fixed child names are allowed
inside the process root. A test that must provision Postgres itself must derive a unique database
name per process and must not replace the caller-provided `LAKE_TEST_DB_URI` path.
