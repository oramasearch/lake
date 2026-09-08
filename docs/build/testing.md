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

## HTTP schema conformance

Test builds require Python 3 and its `jsonschema` package; configure verifies the import.
A local virtual environment keeps this dependency scoped to the checkout:

```sh
python3 -m venv .cache/test-python
.cache/test-python/bin/python -m pip install jsonschema
cmake -S . -B build -DPython3_EXECUTABLE="$PWD/.cache/test-python/bin/python" \
  -DLAKE_ZETTA_SOURCE_DIR=/path/to/zetta-bridge/modules/zetta
```

`SchemaConformance.EveryToolAndRequiredOutputUsesHttpContract` starts the production HTTP server,
captures MCP and REST responses, and validates them with that independent JSON Schema
implementation. Its control store is private to the test and in memory; the repository and
transparency log use real Liquid data. This prevents the registry discovery pass from filling
bounded audit pages used by unrelated Postgres fixtures. All transport, registry, authorization,
audit, and value handlers remain production implementations. Run the full static and shared
suites with `LAKE_TEST_DB_URI` set to cover the production Postgres store as well.

Use a fresh database for each full suite, including consecutive static and shared runs: existing
audit assertions inspect bounded pages, so accumulated diagnostic runs can hide later fixture
events beyond that page. Keep manual server reproductions in a separate fresh database so they
cannot initialize the suite's persisted identity with another key.

For focused diagnostics after the full proof, retain payloads with
`LAKE_SCHEMA_EVIDENCE=<absolute path>` and run `ctest --test-dir build -V -R SchemaConformance`.
The fixture initializes TUF from the signed `tests/fixtures/schema-tuf.json` role set, then
appends real TAAP attestations and publishes a checkpoint before requesting proof tools. Its
fixture README describes replacement and signature-binding requirements.

The report distinguishes all tools called from successful tool responses validated, and requires
a successful call for every advertised value schema with required fields. Add fixtures when
introducing new required output fields. The envelope decision is in
[`../api/README.md`](../api/README.md).
