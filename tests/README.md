# Tests

Every filesystem fixture lives below the process-unique directory returned by
`lake::test::process_fixture_root()` in `temporary_directory.hpp`. Use `fixture_path()` for named
children. Fixed child names are safe; fixed names directly below the system temporary root are not.
The helper honors the platform temporary-root selection, including `TMPDIR`, reports an explicit
error when that root is not writable, and removes the process directory at exit.

Postgres-backed tests use `LAKE_TEST_DB_URI` and do not create or drop its database. A caller that
starts concurrent suites must provide a freshly created, schema-loaded database with a unique name
to each test process.

The default `lake-tests` target contains the portable suite plus Postgres-backed cases that skip
when `LAKE_TEST_DB_URI` is absent. `lake-postgres-tests` contains the focused control-store cases.
Run both through CTest after configuring the repository with CMake.

Web UI string tests cover the content-hashed stylesheet link shared by the standard and hosted
page heads, SHA-256 URL changes for different stylesheet bytes, content-hashed font references,
and the immutable versus `no-cache` route policy. These checks use embedded or in-memory bytes and
perform no filesystem or network I/O.

`fixtures/transparency/fixture.json` is the deterministic transparency-log interoperability
fixture. Its Python generator and independent C++ verifier cover entry and parent-hash tile bytes,
checkpoint-bound inclusion proofs for every leaf, consistency between two tree sizes, and changed
tile or proof bytes. See `fixtures/transparency/README.md` for regeneration commands.

Agent-session coverage uses an injected clock and a real Ed25519 TAAP signature. The portable and
Postgres cases mint a response-only key, verify its logged `delegation/1` attestation, attribute a
signed drip to the durable agent, reject a modified body, and reject the key after expiry.

Platform coverage uses an append-only fake Zetta control source and fake Loki source against
`InMemoryStore`. It proves latest-wins sync, environment policy-object creation, signed phase audit,
tombstones, mutation rows, secret detection, role checks, stale reads, degraded health state, log
cursors, renderer strings, OpenAPI, and MCP discovery. A Beast2 Loki double verifies the real
coroutine HTTP client, selector request, secret bearer, line conversion, and cursor. The focused
Postgres suite round-trips the same environment binding and JSON mirror through the production
store.
