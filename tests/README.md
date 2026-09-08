# Tests

Every filesystem fixture lives below the process-unique directory returned by
`lake::test::process_fixture_root()` in `temporary_directory.hpp`. Use `fixture_path()` for named
children. Fixed child names are safe; fixed names directly below the system temporary root are not.
The helper honors the platform temporary-root selection, including `TMPDIR`, reports an explicit
error when that root is not writable, and removes the process directory at exit.

Postgres-backed tests use `LAKE_TEST_DB_URI` and do not create or drop its database. A caller that
starts concurrent suites must provide a freshly created, schema-loaded database with a unique name
to each test process.

The `scenarios/` cases combine those production Postgres stores with an in-process Lake, public
HTTP routes, and public client libraries. The drive scenario signs users up, synchronizes and
shares a folder, checks API and rendered history, preserves both named results of concurrent
offline edits, pulls the same repository through Liquid, and builds the project it contains. See
`scenarios/README.md` for the named coverage.

The default `lake-tests` target contains the portable suite plus Postgres-backed cases that skip
when `LAKE_TEST_DB_URI` is absent. `lake-postgres-tests` contains the focused control-store cases.
Run both through CTest after configuring the repository with CMake.

`HttpServer.IdleKeepAliveCloseWritesNothingToStderr` starts Lake with one Beast2 worker, captures
the process stderr file descriptor through a test-owned pipe, completes a keep-alive request, and
closes that connection while it is idle. A request on a second connection proves the worker
processed the close before the test stops the server and asserts that the pipe stayed empty.

The four `HttpServer/HttpResponseIsolation` cases sign in a user and check anonymous `/`,
`/sign-up`, `/sign-in`, `/v1/server_info`, and `/v1/openapi.json` responses for stale cookies and
redirects. Independent client cookies
prove the anonymous client cannot open the signing user's sessions, while the original cookie
still works. One worker forces response reuse on both the same socket and a new socket, with
in-memory and production Postgres variants. Postgres fixtures generate valid Liquid identity
documents. The contract and reproduction are in [`../docs/serving/http.md`](../docs/serving/http.md).
`HttpServer.ResponseResetPreservesCurrentProtocolAndCors` checks preflight header isolation and
the current response's HTTP version, connection policy, status and CORS headers on a reused socket.

Web UI string tests cover the content-hashed stylesheet link shared by the standard and hosted
page heads, SHA-256 URL changes for different stylesheet bytes, content-hashed font references,
and the immutable versus `no-cache` route policy. These checks use embedded or in-memory bytes and
perform no filesystem or network I/O.

Conflict coverage pins the complete Liquid-derived kind and region, plain explanation, reasons,
proposal visibility, generated OpenAPI schema, MCP resolution tool, preserved drive comparison
values, and every advertised resolution input. Renderer checks require the forge and drive pages to
show those words before their write-authorized forms.

`test_forge_scenario.cpp` is the Postgres-backed forge lifecycle proof. It drives repository sync
and review operations through a real Lake server with Liquid's sync and forge clients, opens the
candidate and landed HTML pages, maps a line comment after an inserted line, and lands the approved
target. The same case records a graph action with signed provenance in Lake's repository, rejects
automation YAML in the source tree, and asserts the result identity, output path, and verified
profile in the rendered change page.

`fixtures/transparency/fixture.json` is the deterministic transparency-log interoperability
fixture. Its Python generator and independent C++ verifier cover entry and parent-hash tile bytes,
checkpoint-bound inclusion proofs for every leaf, consistency between two tree sizes, and changed
tile or proof bytes. See `fixtures/transparency/README.md` for regeneration commands.

Agent-session coverage uses an injected clock and a real Ed25519 TAAP signature. The portable and
Postgres cases mint a response-only key, verify its current-profile `delegation/1` attestation,
find it by dispatcher identity, attribute a signed drip to the durable agent, reject a modified
body, and reject the key after expiry.

`IdentityExit.PostgresProvesIdentityPkiAndDynamicSecrets` is the issue #4 acceptance scenario. It
uses a real pinned-libliquid identity and Postgres rows to prove replay-resistant TAAP sign-in, an
application PKCE authorization-code flow with relying-party JWKS verification, agent certificate
issue/rotation/revocation checks, dynamic-secret lease/renew/revoke, signed audit visibility, and a
libliquid-verified transparency inclusion proof.

Registry scenario coverage starts Lake on loopback with process-unique storage. A Distribution API
client pushes and pulls a two-layer OCI image byte-for-byte; the installed `npm` and `pip3` clients
publish or install native fixture packages with protocol fallbacks when either binary is absent;
and the debuginfod paths return a CMake-built executable, debug information, and source exactly.
The transparency scenario exports a self-contained bundle, verifies it with liquid from an
in-memory source that has no network transport, and rejects conflicting equal-size checkpoint
views retained under one log identity.

Platform coverage uses an append-only fake Zetta control source and fake Loki source against
`InMemoryStore`. It proves latest-wins sync, environment policy-object creation, signed phase audit,
tombstones, mutation rows, secret detection, role checks, stale reads, degraded health state, log
cursors, renderer strings, OpenAPI, and MCP discovery. A Beast2 Loki double verifies the real
coroutine HTTP client, selector request, secret bearer, line conversion, and cursor. The focused
Postgres suite round-trips the same environment binding and JSON mirror through the production
store.

Checkpoint-time tests cover clock rollback after restart, concurrent publication, and replicated
time rollback. `PostgresTransparency.LiveCheckpointCarriesAuthenticatedMonotonicTime` starts
loopback HTTP servers, publishes through the HTTP capability, and prints two checkpoints accepted by
Liquid's existing signature verifier with non-decreasing `issued_at` values. Changing the time
without re-signing is refused even after recomputing the checkpoint hash. The contract and full
verification commands are in [`../docs/api/transparency.md`](../docs/api/transparency.md).
Share-link delivery tests cover signed-out folder traversal, registered previews, exact downloads,
policy-restricted descendants, stable-node moves, and sibling/parent, expiry, and revocation
refusals. The registry checks include OpenAPI/MCP discovery and MCP byte delivery; the Postgres
scenario uses a regular user's private repository and text, binary, and empty files.
`test_token_attenuation.cpp` drives REST and MCP against both stores. It proves refused scope,
static and lifetime amplification, permitted descendants, recursive session/device revocation,
expired PostgreSQL ancestors, application-pool issuance without an additional JWT, and one signed audit event per call without bearer values. Each
PostgreSQL case owns an isolated schema in `LAKE_TEST_DB_URI` and removes it afterward. Run the
full static and shared CTest suites with PostgreSQL configured; see `docs/api/identity.md`.

`test_schema_conformance.cpp` records real MCP and REST responses from the production HTTP server
with an isolated in-memory control store and real Liquid data. `validate_api_schema.py` validates those payloads with Python
`jsonschema`, checks every advertised tool was called, and requires success coverage for every
value schema with required fields. Set `LAKE_SCHEMA_EVIDENCE` to retain the captured JSON outside
the process fixture directory. The full contract is in [`../docs/api/README.md`](../docs/api/README.md).
