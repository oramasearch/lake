# Tests

`PagePerformance.EveryRegisteredGetPageKeepsItsWorkspaceCallBudget` discovers HTML pages from the
production GET router, resets the workspace definition cache before each render, and checks every
route's exact `Server-Timing` call and sequential-round budget. Unlisted routes must make zero
workspace calls; Project's bounded run details bring its populated fixture to nine calls in two
rounds, and Solution uses three calls in one round.
The loose wall guard is p95 below 500 ms over 20 renders at 30 ms of workspace latency. Its
populated fixtures include both 50- and 19-dataset Data cases and a first-request Metering case
with four repositories and 128 files. The first Metering request checks populated-byte correctness;
the measured Metering route receives the same p95 guard as every other page. `Workspaces.Registry*` and
`Workspaces.PipelineDefinitionCache*` cover expiry, change invalidation, input isolation,
readiness/credential changes, errors and stale in-flight replies. Run with PostgreSQL in both
native builds; [performance.md](../docs/ui/performance.md) explains the evidence rows.
It requires `LAKE_TEST_DB_URI`, uses a regular authenticated user, and rejects redirects or errors
for canonical `/w/:workspace_id/...` pages and `/workspaces`.
Non-workspace discovery also tries operator and signed-out profiles to include protected operator
pages and public sign-in forms.

`Workspaces.SolutionProjectShowsRunFeedbackAndReadableOperatingState` pins a failed run's stage and
error, an active run's status and three-second refresh, the full-run link, the lifecycle header,
policy tiers, experiment allocation, monitor guardrails, the ROI reason and verdict-first grouped
certificate. `Workspaces.EveryNewFeatureIsInOpenApiAndWorksOverMcp` calls the direct filtered
`list_runs` capability and checks its completed status and stage.

`test_experiments.cpp` covers all eleven training operations, space permissions, confirmation,
workspace alias checks, audit, REST/MCP parity and actual calls, hashed assets, navigation and
control mappings, plus the editable ExperimentsAgent proposal. `experiment_fixture.hpp` supplies
a stateful fake caller against the authorized engine contract and an opt-in browser fixture.
`experiment_charts.cjs` tests chart transformations and cursors without a browser. Independent
response validation also uses these fixtures. See
[API](../docs/api/experiments.md) and [UI](../docs/ui/experiments.md) contracts.

`test_streams.cpp` covers Streams space/system/creator permissions, non-consuming browse,
retention, revisions, REST/MCP parity with one call per operation, UI control mappings and
selected navigation. Its 1,000-topic case checks the 50-row default page, cursor and prefix
filtering, sub-500 ms response, one list call per Zetta and no per-topic detail calls.
`stream_fixture.hpp` is a stateful fake workspace caller using the exact Zetta CRUD wire shape;
bus tests use `MemoryAgentBus`. PostgreSQL tests reopen ownership across space/workspace
boundaries. The schema conformance fixture exercises all nine successful outputs. See
`docs/api/streams.md` and `docs/ui/streams.md`.

`test_instance_origin.cpp` checks external-origin precedence, startup validation and repository
command rendering. `InstanceOrigin.BrowserToTerminal` runs `instance_origin.mjs`: a native Chromium page
supplies the actual clone/push text executed by the pinned Liquid CLI against the shared fixture's
fresh database, non-default loopback port, store and credential. It refuses off-origin requests and compares
cloned and pushed bytes. Set `LAKE_TEST_DB_URI` and `LAKE_TEST_LIQUID`, plus the native browser
and Puppeteer paths where needed. See [private-install.md](../docs/deployment/private-install.md).

Every filesystem fixture lives below the process-unique directory returned by
`lake::test::process_fixture_root()` in `temporary_directory.hpp`. Use `fixture_path()` for named
children. Fixed child names are safe; fixed names directly below the system temporary root are not.
The helper honors the platform temporary-root selection, including `TMPDIR`, reports an explicit
error when that root is not writable, and removes the process directory at exit.

Postgres-backed tests obtain connections through `postgres_fixture.hpp`. `test_main.cpp` creates
and schema-loads a fresh database on first use in each test and drops it after fixture destruction.
`LAKE_TEST_DB_URI` supplies connection settings and a role with `CREATEDB`; its database remains
untouched. Each test receives a random `LAKE_INSTANCE_KEK`. See
[`testing.md`](../docs/build/testing.md) for ownership, assertion patterns, and the sweep.
`audit_test_support.hpp` exhausts append-order pages; use exact request filters for specific calls.

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

`HttpServer.WorkspacePrefixServesRestOpenApiMcpAndAppSourceReads` covers the workspace API base
URL used by apps, event publishers, builds, and agents. It proves REST parity and path-derived
`workspace_id`, mismatch and unknown-workspace errors, identical OpenAPI, an app-source token
reading a real repository tree, MCP reading the same tree, and refusal on a different workspace.

The four `HttpServer/HttpResponseIsolation` cases sign in a user and check anonymous `/`,
`/sign-up`, `/sign-in`, `/v1/server_info`, and `/v1/openapi.json` responses for stale cookies and
redirects. Independent client cookies
prove the anonymous client cannot open the signing user's sessions, while the original cookie
still works. One worker forces response reuse on both the same socket and a new socket, with
in-memory and production Postgres variants. Each production variant creates a UUID-named schema
inside the supplied test database, loads the schema there, and removes only that schema after the
case. Postgres fixtures generate valid Liquid identity documents. The contract and reproduction
are in [`../docs/serving/http.md`](../docs/serving/http.md).
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

`test_web_apps.cpp` proves Ship App policy isolation, validation, desired-state generations,
reserved platform and configured names, direct-zone default hosts, app/replica quotas,
fake-operator status, Secret references, service binding, Loki namespace selection, purpose-scoped
operator hashes and lifecycle event delivery with the memory store.
`test_api.cpp` proves successful private repository tree/file reads with a source token.
`PostgresWebApps.*` proves the production desired row, hash registration/rotation, customer
locking, `lake_apps` notification, operator status and physical deletion. Both stores exercise
cross-app/customer/revision refusal and reject other capabilities.
`test_builds.cpp` proves asynchronous build intent, fake operator status, cancellation, generated
App Dockerfiles, purpose-scoped hashes and retained events in memory. `PostgresBuilds.*` proves the
same row shape, `lake_builds` notifications and credential revocation in Postgres.
`RegistryProtocols.BuildPushCredentialIsLimitedToItsCustomerPrefix` pins the private OCI boundary;
the Web UI renderer check pins repository/App actions and live build followers.
Authorization assertions use well-formed input; malformed authenticated calls separately assert
`invalid_input`. Postgres registry contexts supply the fixture's audit encryption key.
`HttpServer/HttpResponseIsolation.ShipAppsCreatesOperatesAndFollowsAnApp/*` creates a repository
app through the form routes, replies to an ApplicationAgent question in the same conversation,
rejects a changed proposal digest, confirms the stored proposal, renders its deployed URL and the
operating page, publishes a deploy event, and uses restart through its POST adapter. `AgentRuns`
proves app tasks use their own topic and credential, Ask Orama delegates to them, and App writes are
absent before confirmation but usable after it. Memory variants add fake operator Ready status;
renderer coverage verifies question replies, secret masking, rollback inputs, and live app/build
polling markup.
`HttpServer/HttpResponseIsolation.ExperimentsAgentRestMcpAndPageUseTheSameConfirmedTurn/*`
compares the `agent_turn` OpenAPI/MCP schemas, continues one conversation across those transports,
then replies to a page question, edits and confirms the proposal.
`AgentRuns.ExperimentsAgentGrantsEachTurnOnlyItsStudyOperations` verifies the
three kinds, independent topic/credential, workspace URL and confirmation-limited study actions.
`HttpServer/HttpResponseIsolation.LibraryAgentsFinishEveryTurnKindWithoutAWorkspaceEcho/*`
claims and completes `app`, `solution:` and `experiment` tasks through the HTTP bus without a
workspace field, publishes progress before the result, and exercises task-credential workspace
inference through both Lake and Zetta-shaped routes. The lower-level `AgentRuns` bus case also
pins optional filtering and rejects mismatched workspace IDs.

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
`PostgresProvisioning.*` proves personal ownership, retry/concurrency uniqueness, malformed
document recovery, and transaction rollback after session failure. The HTTP provisioning cases
check configured quota inheritance, zero initial storage and cost with nonzero prices, and a first
repository in both stores. `personal_customer_test_support.hpp` resolves the tenant already
created by sign-up; fixtures must not create a second one. See
[`../docs/api/provisioning.md`](../docs/api/provisioning.md).

Front-door renderer checks cover the landing journey and visitor/account/operator navigation.
`HttpServer/HttpResponseIsolation.NavigationUsesTheResolvedViewerOnEveryPage` runs with both
stores, valid and invalid cookies, and operator credentials; each advertised global link must
return HTTP 200 for that same viewer. It also pins customer redirects to `/sign-in`. The browser
proof is in [front-door.md](../docs/ui/front-door.md). `PostgresCapabilities.OrganizationAndPolicyRoundTrip`
filters its deletion audit assertion by the exact request ID and `repository/<id>` object.
`Stores/AuditIsolation.*` proves lookup after 1,105 same-object events and complete cursor traversal
against memory and PostgreSQL stores. Personal-customer lookup filters by user before limiting.
Repository lifecycle coverage deletes a repository and recreates its name against both stores.
The PostgreSQL case also exercises the live-name unique index through the registry and asserts the
product conflict message and `/name` pointer. `ApiErrors.DatabaseDiagnosticsStayOutOfErrorMessages`
pins the final registry boundary: database diagnostic markers are absent from API messages while
the raw detail is retained in the server log.

`PostgresSessionAuthentication.*` in `test_token_attenuation.cpp` compares 7/17 real sessions,
measures 7,000 rows, checks actual tuple counts, password policy, stored-digest refusal, operator
precedence, migration idempotence, scopes, expiry and next-request bearer/cookie revocation.
See [session authentication proof](../docs/build/session-authentication.md).
