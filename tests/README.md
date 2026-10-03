# Tests

`test_main.cpp` is the shared GoogleTest runner. `lake::test::postgres_uri()` gives each
Postgres-backed test a private database copied from a `lake_fixture_template_<digest>` template
loaded once per `db/schema.sql` version, and drops it afterwards; a start-up sweep removes only
stale `lake_fixture_*` databases. Fixtures use that database directly, with no nested schema. See
[testing.md](../docs/build/testing.md#databases-the-runner-owns).

A test's own socket clients (`liquid::http1::request`, liquid sync pushes and pulls) run on one
thread through `one_thread_io.hpp`, never `liquid::runtime::block_on`, whose several runner threads
hit a corosio close race (lake#510). The runner's terminate handler prints the uncaught exception
and its stack. See [testing.md](../docs/build/testing.md#test-clients-run-their-sockets-on-one-thread).

`lake-ui-fixture-pages` (lake#497) starts `lake-ui-fixture` and requests every page it seeds and
every registered GET route; a fixture that cannot start, a seeded page that is not 200 HTML, or a
5xx anywhere fails the suite. See [tools/ui/README.md](../tools/ui/README.md).

`test_shared_store.cpp` (lake#467, lake#479) checks that one lake process owns a store root and
hands it over. The PostgreSQL cases start real `lake` binaries on one root: a successor started
beside a serving owner must become Ready while it waits, hold a drive write sent to it until the
owner has had SIGTERM, and serve it then; with writers routed first to both processes and then to
the successor, no response may be a 503 and no request may fail, every acknowledged write must read
back from the successor, the customer store must reopen and pass a libliquid dry-run collection,
and the log must rebuild its tree. A successor whose wait expires must exit 1 naming the owner. See
[store-volume.md](../docs/deployment/store-volume.md#the-hand-off).
Its writer threads run liquid clients and are `lake::Thread`s, so they keep lake's 8 MiB stack in a
Debug build on macOS, and the hand-off case waits (up to 60 s) for one acknowledged write of each
kind before starting the successor, because a Debug push under the parallel suite can outlast the
fixed warm-up (lake#478).
`test_thread.cpp` (lake#478) runs a 2 MiB frame on a `lake::Thread`, which a `std::thread` on
macOS cannot hold, and checks that a moved thread joins once.
lake#452: `WebUi.PhoneWidthGivesTheWorkspacePickerItsOwnRow` checks the top bar's rendered order
and the 40rem rules; `WorkspaceNavigationHttp.SelectorRedirectsRememberedWorkspaceAndValidatesForms`
covers workspace labels and the `/w/<workspace>/` home redirect;
`Workspaces.CreateProjectSaysWhetherTheNameIsMissingOrTaken`,
`Workspaces.ServiceStatusIsTheSameInListServicesAndOnThePage`,
`Workspaces.ADeletedWorkspacesZettaLeavesTheListWithItsWorkspace` and the PostgreSQL services test
cover the rest; `AgentRuns.ApplicationAgentWritesAppsOnlyAfterTheProposalIsConfirmed` and
`HttpResponseIsolation.ShipAppsCreatesOperatesAndFollowsAnApp` cover the public acknowledgement.

`test_agent_leases.cpp` runs the same claim, activity, expiry, re-claim, three-attempt exhaustion,
workspace notice recovery and racing-terminal-write cases on memory and PostgreSQL stores.
It checks Apps/Experiments/Project rendering, legacy migration adoption, schema reapplication,
the partial expiry index, and duration bounds without clock sleeps. The Streams task-credential
fixture takes a real claim before making calls. See [agent-bus.md](../docs/api/agent-bus.md).

`HttpResponseIsolation.AskOramaOffersPeoplesAgentsAndSaysWhoIsWorkingOnALongTurn` (lake#451) runs a
real Ask Orama claim, a delegation with its turn key, and the subagent's progress and result, and
checks *Talk to*, the working notice and the asking turn's `agent-events`. The HTTP fixture runs
`agent-eval-generator` too, so the test proves a running internal agent is not offered.
`Workspaces.TalkToOffersOnlyRunningAgentsAPersonUses` checks the list builder.

Lease regressions also cover delayed notice acknowledgements and stale waiting-deadline
observations: neither may discard the final failure notice or fail a fresh retry.

`PagePerformance.EveryRegisteredGetPageKeepsItsWorkspaceCallBudget` discovers HTML pages from the
production GET router, resets the workspace definition cache before each render, and checks every
route's exact `Server-Timing` call and sequential-round budget. Unlisted routes must make zero
workspace calls; Project's bounded run details bring its populated fixture to nine calls in two
rounds, Solution uses three calls in one round, and a dataset page three in one round (its
metadata, first rows and `analyze_dataset` column types). The fake workspace answers the Data
list with Zetta's real `system.datasets` column, `dataset`.
The loose wall guard is p95 below 500 ms over 20 renders at 30 ms of workspace latency. Its
populated fixtures include both 50- and 19-dataset Data cases and a first-request Metering case
with four repositories and 128 files. The first Metering request checks populated-byte correctness;
the measured Metering route receives the same p95 guard as every other page and a budget of four
workspace calls in one round (its usage summary reads datasets, runs, and the repository transfer
and build records of lake#348 and lake#349). `Workspaces.Registry*` and
`Workspaces.PipelineDefinitionCache*` cover expiry, change invalidation, input isolation,
readiness/credential changes, errors and stale in-flight replies. Run with PostgreSQL in both
native builds; [performance.md](../docs/ui/performance.md) explains the evidence rows.
It requires `LAKE_TEST_DB_URI`, uses a regular authenticated user, and rejects redirects or errors
for canonical `/w/:workspace_id/...` pages and `/workspaces`.
Since lake#446 it also fixes the exact Zetta capabilities of Ask Orama, Agents, Search Indexes,
Models and Experiments, requires every response's `zetta_<capability>` `Server-Timing` entries to
match what the fake observed, holds the three deferred panel routes to their own JSON budgets, and
refuses any page that waits for `list_agents`, `get_agent`, `agent_training_status` or the full
`list_datasets` outside the documented exceptions.
`HttpServer/HttpResponseIsolation.SlowZettaPanelsLoadAfterTheirPage/*` proves in both stores that
Ask Orama renders without `list_agents` and loads the space's chat agents from `/ask/agents`, and
that Agents and Indexes defer their panels (and `?inline=1` waits).
Non-workspace discovery also tries operator and signed-out profiles to include protected operator
pages and public sign-in forms.

`Workspaces.SolutionProjectShowsRunFeedbackAndReadableOperatingState` pins a failed run's stage and
error, an active run's status and three-second refresh, the full-run link, the lifecycle header,
policy tiers, the engine's `balance.allocations` shape, scope-filtered ledger counts and actions,
held and pending A1 rows, the no-holdout-outcomes ROI reason, monitor guardrails and the
verdict-first grouped certificate. `Workspaces.EveryNewFeatureIsInOpenApiAndWorksOverMcp` calls the direct filtered
`list_runs` capability and checks its completed status and stage.

`test_examples.cpp` (lake#439) pulls every copy-paste example out of what a customer reads: the
Ship → Models `curl`, the search index page's `curl` and Python calls, every `/docs` page, and the
request bodies in `docs/api/*.md`. It names each example's capability from `/v1/<name>`, a code span
of the name, or a JSON-RPC `tools/call`, and validates the body or query string with the registry's
`validate_input`. A missing `workspace_id` therefore fails the build's tests rather than a first
paste. Pages must fill in their own workspace; static Docs show `<workspace id>`.

`test_experiments.cpp` covers all eleven training operations, space permissions, confirmation,
workspace alias checks, audit, REST/MCP parity and actual calls, hashed assets, navigation and
control mappings, plus the editable ExperimentsAgent proposal, `start_study` with `from_defaults`
and Zetta's error `path`, canonical metric labels, and the study page's server-rendered analysis
tables and trial curves (lake#449, lake#463). `test_workspaces.cpp` covers trial runs and study
models named by their study, the promoted marker, the run report's held-out score lead, and
`ZettaWorkspaceCaller` keeping `error.path` behind a fake Zetta HTTP transport. `experiment_fixture.hpp` supplies
a stateful fake caller against the authorized engine contract and an opt-in browser fixture.
`experiment_charts.cjs` tests chart transformations and cursors without a browser. Independent
response validation also uses these fixtures. See
[API](../docs/api/experiments.md) and [UI](../docs/ui/experiments.md) contracts.

`test_audit_isolation.cpp` also runs `ActivityIsNewestFirstNamedAndQuiet` on both stores:
`my_audit` ordering, principal names (an acting agent included), a workspace call's inner
capability and the hidden successful reads (lake#445).

`test_streams.cpp` covers Streams space/system/creator permissions, non-consuming browse,
retention, revisions, REST/MCP parity with one call per operation, UI control mappings and
selected navigation. Its 1,000-topic case checks the 50-row default page, cursor and prefix
filtering, sub-500 ms response, one list call per Zetta and no per-topic detail calls.
`stream_fixture.hpp` is a stateful fake workspace caller using the exact Zetta CRUD wire shape;
bus tests use `MemoryAgentBus`. `Streams.CreatorOwnershipLivesInTheTopicsWorkspaceZetta` keeps
each topic's creator at its own workspace endpoint across two spaces and two workspaces, and covers
missing and unknown workspace ids, a workspace that is not ready, workspace deletion and the
immutable `if_absent` claim (lake#379). The schema conformance fixture exercises all nine successful outputs. See
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
`HttpServer.AHandlerThatThrowsIsAnswered500AndLogged` runs `guard_route` handlers on a Beast2
server and checks the HTML and JSON `500` answers, the request id, the logged route and message,
and that the connection then serves a `200`. `lake-tests` adds `src/` to its include path for this
private header. `HttpServer/HttpResponseIsolation.ShipPackagesListsABuildThatHasNoDigestYet/*` and
`Workspaces.PackagesPageRendersABuildThatHasNoDigestYet` cover the Ship → Packages page with a
queued build whose digest is null (lake#440).

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
The same fixture uploads a generated layer larger than 64 MiB through the real server, proves
ranged patches followed by an empty final put, checks the configured registry limit and response,
and verifies that an oversized capability JSON request still receives HTTP 413 at 64 MiB.
The transparency scenario exports a self-contained bundle, verifies it with liquid from an
in-memory source that has no network transport, and rejects conflicting equal-size checkpoint
views retained under one log identity.

Platform coverage uses an append-only fake Zetta control source and fake Loki source against
`InMemoryStore`, with the repository in a Ready workspace of a `RecordWorkspaces` (lake#358,
lake#359). It proves latest-wins sync, environment policy-object creation, signed phase audit,
tombstones, mutation rows, secret detection, role checks, stale reads, degraded health state, log
cursors, renderer strings, OpenAPI, and MCP discovery. A Beast2 Loki double verifies the real
coroutine HTTP client, selector request, secret bearer, line conversion, and cursor.
`test_workspace_records.cpp`'s `PlatformEnvironmentsLiveInTheirRepositorysWorkspace` covers two
spaces with two workspaces each, sealed records only at the repository's workspace endpoint, a quiet
sync writing nothing, an older row never replacing a newer one, a stale revision, one environment
created once by two writers, least privilege without a workspace call, missing, malformed and
unknown ids, a workspace that is not ready (only that repository stale), a repository without a
workspace and workspace deletion; `TheOneShotCopyMovesPlatformEnvironmentsAndTheMirror` the copy of
both collections in both stores (the memory store seeds legacy rows with
`seed_legacy_platform_environment` and `seed_legacy_platform_mirror_row`), including a grant on an
old policy object still deciding and no phase audited twice; and
`PostgresPlatform.MigrationRecordsEachRowsOwnerForTheCopy` migration `0060_platform_workspaces`.

`test_web_apps.cpp` proves Ship App policy isolation, validation, desired-state generations,
reserved platform and configured names, direct-zone default hosts, app/replica quotas,
fake-operator status, Secret references, service binding, Loki namespace selection, purpose-scoped
operator hashes, including the pull purpose, and lifecycle event delivery with the memory store.
`WebAppSessions.*` checks app-session claims, tamper rejection and expiry.
`HttpServer/HttpResponseIsolation.ProtectedAppAuthorizationHandsOffAHostOnlySession/*` covers
anonymous refusal, normal sign-in return, exact redirect-host validation, one-use code exchange,
host-only cookie issuance, code reuse, cross-app and expired cookies, and membership revocation in
both stores. `PagePerformance.EveryRegisteredGetPageKeepsItsWorkspaceCallBudget` includes the
workspace-prefixed forward-auth route with no Zetta calls and a 100 ms p95 bound.
`test_api.cpp` proves successful private repository tree/file reads with a source token.
`PostgresWebApps.*` proves the production desired row, hash registration/rotation, customer
locking, `lake_apps` notification, operator status and physical deletion. Both stores exercise
cross-app/customer/revision refusal and reject other capabilities.
`test_builds.cpp` proves asynchronous build intent, public digest references after success, fake
operator status, cancellation, configured App runtime-image arguments, missing-setting refusal,
generated App Dockerfiles, purpose-scoped hashes and retained events in memory. `PostgresBuilds.*`
proves the same row shape, `lake_builds` notifications and credential revocation in Postgres.
`RegistryProtocols.BuildPushCredentialIsLimitedToItsCustomerPrefix` pins the private OCI boundary;
`RegistryProtocols.AppPullCredentialReadsOnlyItsOwnImage` pins Basic user `app`, exact app
repository reads, and cross-app, cache and write refusal. The Web UI renderer check pins
repository/App actions and live build followers.
Authorization assertions use well-formed input; malformed authenticated calls separately assert
`invalid_input`. Postgres registry contexts supply the fixture's audit encryption key.
`HttpServer/HttpResponseIsolation.ShipAppsCreatesOperatesAndFollowsAnApp/*` creates a repository
app through the form routes, replies to an ApplicationAgent question in the same conversation,
rejects a changed proposal digest, confirms the stored proposal, renders its deployed URL and the
operating page, queues an app build from the runtime image supplied through real `ServerOptions`,
publishes a deploy event, and uses restart through its POST adapter. `AgentRuns`
proves app tasks use their own topic and credential, Ask Orama delegates to them, and App writes are
absent before confirmation but usable after it. Memory variants add fake operator Ready status;
renderer coverage verifies question replies, secret masking, rollback inputs, and live app/build
polling markup.
`HttpServer/HttpResponseIsolation.ExperimentsAgentRestMcpAndPageUseTheSameConfirmedTurn/*`
compares the `agent_turn` OpenAPI/MCP schemas, continues one conversation across those transports,
then replies to a page question, edits and confirms the proposal.
`AgentRuns.ExperimentsAgentGrantsEachTurnOnlyItsStudyOperations` verifies the
three kinds, independent topic/credential, workspace URL and confirmation-limited study actions.
`AgentRuns.AProjectAgentProposalIsCheckedThenAppliedApprovedAndRun` covers lake#438: a pipeline
change turn's credential runs `compile_intent` confined to lake's check graph on its own project
(and reads it back on a repeat), is refused every other workspace action, and the person then
applies the proposal with `edit_pipeline`, approves it and runs it with the proposal's settings.
It also renders the project page and pins that the check graph is not listed as a pipeline.
`AgentRuns.AProjectAgentBuildChecksItsCompileAndThePickIgnoresTheCheckGraph` gives a build turn
the same confined check and proves the pick after the build compiles the project's own pipeline
rather than taking the check graph.
`Workspaces.PipelinePageShowsTheGraphInspectorEditsGovernanceAndRuns` also pins that raw agent
failures render as plain sentences with a next step and that the working panel reloads itself;
`AgentLeases.RetryNoticeRendersOnAppsExperimentsAndProject` pins the same reload on the
ExperimentsAgent working panel.
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

`Mcp.ToolSchemasAreByteIdenticalToOpenApi` (`test_api.cpp`) compares every MCP tool's input and
output schema with its OpenAPI operation, byte for byte, for callers and agent-task credentials;
`Mcp.SessionsNameTheToolListTheyBeganOn` and `HttpServer.McpSessionsLearnThatTheToolListChanged`
(`test_http_server.cpp`) prove `tools.listChanged`, the tool-list session identifier and the
`notifications/tools/list_changed` notice after an upgrade; `Registry.EnforcesEnumsAndNumericBoundsOnEveryTransport`
proves the published bounds are enforced. See [`../docs/api/mcp.md`](../docs/api/mcp.md) (lake#441).

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
proof is in [front-door.md](../docs/ui/front-door.md).
`HttpServer/HttpResponseIsolation.PersonCreatesAnOrganizationInvitesAndAColleagueJoins` and
`.PersonalSpaceBecomesAnOrganizationAfterConfirmation` drive the Organizations page forms as two
signed-up people against both stores; `Organizations.*` and
`PostgresCapabilities.OrganizationSpaceMembershipsCarryTheirGrantToTheSpace` cover the capabilities
and the organization-space grants ([organizations.md](../docs/api/organizations.md)).
`SchemaConformance.EveryToolAndRequiredOutputUsesHttpContract` calls `create_organization_space`
and `convert_personal_space` with a signed-up person's session, since they refuse the operator.
`PostgresCapabilities.OrganizationAndPolicyRoundTrip`
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

`Workspaces.DatasetRulesNameCsvIdAndEngineResults`, `Workspaces.DatasetWritesAreCheckedAndIdsAgree`
and `Workspaces.DataPageNamesResultsColumnTypesAndUploadNotice` pin lake#447: SQL-queryable
names, malformed CSV refused with its line before any upload call, a taken name refused or
reported as renamed, one `<database>/<name>` id across list and get, engine results kept out of
pickers and grouped under Results, and typed columns. See
[workspaces.md](../docs/api/workspaces.md).
`AgentRuns.LaunchesAndSchedulesBelongToTheirWorkspace`,
`AgentRuns.AWaitingLaunchSaysWhyAndTimesOutWithAReason` and
`AgentRuns.CreateAgentRefusesDefinitionFieldsZettaWouldDrop` cover lake#442 in memory;
`PostgresAgentLaunches` covers the `0043_agent_launch_workspaces` routing trigger, workspace
filters and the start-timeout withdrawal. See [agent-launches.md](../docs/api/agent-launches.md).
`test_docs_status.cpp` runs `tools/docs/check-status.py` on `docs/design/status.md` against the
newest snapshot in `docs/deployment/observed/` and the in-tree registry's OpenAPI, and proves the
script refuses each contradiction it looks for (lake#448). `WebUi.FrontDoorDocQuotesTheRenderedHomePage`
holds `docs/ui/front-door.md` to the rendered homepage, and
`Clerk.VisitorsAreNotOfferedAnAccountTheSignUpPageRefuses` keeps the homepage and `/sign-up`
agreeing under Clerk.
`OperatorContract.EveryStatementTheOperatorRunsPreparesAgainstTheSchema` and
`OperatorContract.RowsLakeWritesAppearInTheOperatorsPolls` (`test_operator_contract.cpp`, in
`lake-postgres-tests`) run a copy of every statement in orama-operator's
`internal/controller/lake_*_pgx.go` against a fresh schema: a column the operator selects that lake
lacks (lake#442, lake#465) fails here. `PostgresStores.MigrationAssignsTheSoleWorkspaceAndRecordsTheRest`
covers `0044_store_workspaces`. See [storage.md](../docs/control-plane/storage.md).

Repository workspaces (lake#315): `Repositories.BelongToOneWorkspaceOfTheirSpace` covers two spaces
and two workspaces in one, missing, empty, malformed, unknown, deleted and foreign `workspace_id`,
reads and the workspace filter, the refused transfer, both imports, and workspace deletion.
`PostgresRepositories.MigrationAssignsTheSoleWorkspaceAndRoutingStaysFixed` covers
`0051_repository_workspaces` (sole, several, none, a tombstoned repository, a rerun) and the
trigger's refusals. `repository_test_support.hpp` (`repository_workspace`) gives other tests the
space's first live workspace for the repositories they create.

`test_workspace_records.cpp` covers workspace records (lake#370,
[workspace-records.md](../docs/architecture/workspace-records.md)): sealed records and blind
indexes, paged listing, records that open only in their own workspace and lookup, conditional
writes with conflict retries and `busy` retries, two spaces with two workspaces each over the
memory and PostgreSQL stores, workspace deletion, the allow-list keeping the record store away from
`workspace_query`/`workspace_act`, and the operator's one-shot copy. `record_fixture.hpp`
(`RecordTables`) is the keyed record store (zetta#1231) as records use it; the test workspace
callers route record capabilities to it, one store per endpoint.
`WorkspaceRecordsZetta.TheEngineStoresAndReturnsSealedRecords` runs against a real Zetta node when
`LAKE_TEST_ZETTA_URL` and `LAKE_TEST_ZETTA_TOKEN` are set: build Zetta dev (with zetta#1233) in a
scratch directory and run `zetta --headless --mcp-listen 127.0.0.1:48712 --admin-token <token>`.
`PostgresAskMessages.MigrationDropsTheTableAndKeepsItsAssignments` covers `0050_drop_ask_messages`
(lake#483): a pre-0050 table with a row is dropped, its `workspace_assignments` row stays, and two
reruns of the whole schema leave it dropped. The `ask_messages` copy test and the 0045 test went
with the table.
Design → Opportunities (lake#376): `Stores/WorkspaceRecordStores.OpportunitiesStayInTheWorkspaceTheirScanRanIn`
(two spaces with two workspaces each, the scan's workspace only, a second settle writing nothing,
foreign and missing/malformed/unknown ids, a workspace that is not ready, workspace deletion),
`WorkspaceRecords.AnOpportunityStatusChangeRereadsAfterAConcurrentWrite` (derived keys and a raced
conditional write), `Stores/WorkspaceRecordStores.TheOneShotCopyMovesEachOpportunityToItsScansWorkspace`
and `PostgresOpportunities.MigrationRecordsEachRowsOwnerForTheCopy` (`0046_opportunity_workspaces`).
`Stores/WorkspaceRecordStores.TheOneShotCopyMovesEachTopicCreatorIntoItsWorkspace` copies the
retired `stream_owners` rows (lake#379), and `PostgresStreamOwners.MigrationRecordsEachRowsWorkspaceForTheCopy`
covers `0047_stream_owner_workspaces`.
`Stores/WorkspaceRecordStores.TheOneShotCopyMovesDriveShareLinksAndRoutesTheirTokens` copies retired
drive share links and checks their routes (lake#350), and
`PostgresDriveShareLinks.MigrationRecordsOwnersAndContentFreeRoutes` covers
`0048_drive_share_link_workspaces`. `Drive.ShareLinksLiveOnlyInTheWorkspaceThatHoldsThem` covers
two spaces with two workspaces each, missing/unknown/foreign `workspace_id`, least-privilege
callers, forged and moved tokens, a workspace that is not ready, revocation and workspace deletion.
`workspace_fixture.hpp` (`RecordWorkspaces`) gives a test store Ready workspaces whose records live
in `RecordTables`, over the memory or PostgreSQL store.
Application events and delivery attempts (lake#356, lake#357): `test_apps.cpp` covers two spaces,
two workspaces in one, missing, malformed, unknown and foreign `workspace_id`, least privilege, a
workspace that is not ready, deletion, and an attempt claimed by another caller not being sent;
`WorkspaceRecordStores.TheOneShotCopyMovesApplicationEventsAndTheirDeliveries` the copy in both
stores; `PostgresApplicationWebhooks.MigrationRecordsEachEventsOwnerForTheCopy` migration
`0049_application_webhook_workspaces`. `stream_fixture.hpp`'s `ready_workspace` makes a Ready
workspace over either store for tests that sign into an application pool.
Application users and organizations (lake#353, lake#354):
`ApplicationAuth.UsersAndOrganizationsLiveInTheApplicationsWorkspace` covers two spaces, two
workspaces in one, sealed records and claims at the application's endpoint only, duplicate emails,
TAAP identities and names, fresh and abandoned claims, a sign-up completed at the next sign-in, an
account that is not a user, suspension, deletion, renames, missing, malformed, unknown and foreign
`workspace_id`, least privilege, a workspace that is not ready and workspace deletion;
`WorkspaceRecordStores.TheOneShotCopyMovesApplicationUsersAndOrganizations` the copy in both
stores; `PostgresApplicationUsers.MigrationRecordsEachRowsOwnerForTheCopy` migration
`0055_application_user_workspaces`. The identity capabilities are asynchronous, so tests call them
through `call_async`.

Repository usage in workspace Zettas (lake#348, lake#349): `test_workspace_records.cpp`'s
`RepositoryUsageStaysInEachRepositorysWorkspace` covers `MeteringRecorder` across two spaces with
two workspaces each (nothing written on the request path, merged counters, a second replica's
conditional increment, not-ready and deleted workspaces, a repository without a workspace, the
bound, build records once), and `TheOneShotCopyMovesMeteringIntoEachSpacesWorkspace` the copy.
`PostgresMetering.MigrationRecordsEachRowsOwnerForTheCopy` covers `0053_metering_workspaces`, and
`PostgresSync.PushAndPullUsePostgresControlPlane` a real push and pull flushed into the
repository's workspace, nothing in PostgreSQL, and the flush on stop.

The audit split by subject (decision 52, lake#317): `test_workspace_records.cpp`'s
`AuditEventsAboutAWorkspaceLiveInItsOwnChain` covers the `AuditRecorder` and the readers across two
spaces with two workspaces each (a call writes nothing until the flush, control-plane calls,
refusals and missing, malformed and unknown `workspace_id` central, sealed records at the
workspace's endpoint only, a second replica's appends never forking the chain, verify, export,
`audit` and `my_audit`, a workspace that is not ready, workspace deletion, the bound, tampering,
a lake without workspaces), and `TheOneShotCopyMovesWorkspaceAuditEvents` the copy in both stores.
`AuditRouting.EveryRegisteredCapabilityIsClassified` (`test_audit_isolation.cpp`) holds every
registered capability to one of the two tables. `PostgresAudit.MigrationRecordsEachWorkspaceEventsOwnerForTheCopy`
covers `0063_audit_workspaces`. `PostgresSync.*` and `PlatformFixture.*` read pushes, branch
protection decisions and deployment phases from the repository's workspace audit;
`TokenAttenuation` finds an application's events there through `audit`; and
`WebUi.AuditPagesSayWhichLogHoldsEachEvent` the pages.

Forge records in workspace Zettas (lake#341): `test_workspace_records.cpp`'s
`ForgeRecordsLiveInTheirRepositorysWorkspace` covers two spaces with two workspaces each, records
only at their own endpoint, other repositories' and workspaces' ids, a record copied elsewhere,
conditional updates, least privilege, missing and unknown `repository_id`, a repository without a
workspace, a workspace that is not ready and workspace deletion, for the capabilities and for
`load_branch_protection`; `TheOneShotCopyMovesForgeRecordsIntoTheirRepositorysWorkspace` the copy
in both stores; `PostgresForgeRecords.MigrationRecordsEachRowsOwnerForTheCopy` migration
`0054_forge_record_workspaces`. The forge, export, GitHub import and branch protection tests in
`test_api.cpp` give `Surface` a Ready workspace (`Surface::workspace`), and the sync and forge
scenario servers a `RecordWorkspaces`; `RecordWorkspaces::forge` writes a forge record the way lake
does. `Surface`'s KEK is hexadecimal, as `LAKE_INSTANCE_KEK` is.
Application signing keys and webhooks (lake#352, lake#355): `test_apps.cpp`
(`KeysAndWebhooksLiveInTheApplicationsWorkspace`, `AConcurrentRotationIsRereadNotLost`) covers two
spaces with two workspaces each, the anonymous JWKS, missing, malformed, unknown and foreign
`workspace_id`, least privilege, rotation and a raced rotation, not ready and deletion;
`WorkspaceRecordStores.TheOneShotCopyMovesApplicationKeysAndWebhooks` the copy in both stores;
`PostgresApplicationCredentials.MigrationRecordsEachRowsOwnerForTheCopy` migration
`0056_application_credential_workspaces`. `stream_fixture.hpp`'s `seed_application_signing_key`
writes the first key ring of an application a test creates directly in a store.

Secrets in workspace Zettas (lake#328-#332): `test_secrets.cpp` runs the capability contract on a
`RecordWorkspaces` (history, number reuse, a stopped put completed by the next, transit, a KEK
rotation finished after a workspace was not ready, leases, denial). `test_workspace_records.cpp`'s
`SecretsLiveInTheirMountsWorkspace` covers two spaces with two workspaces each, sealed records only
at their own endpoint and none in PostgreSQL, missing, malformed, unknown and foreign ids, another
space's person, a record copied elsewhere, a workspace not ready, KEK rotation across workspaces and
workspace deletion; `TheOneShotCopyMovesSecretsIntoTheirMountsWorkspace` the copy of the five
collections in both stores (the memory store seeds legacy rows with `seed_legacy_secret_*`);
`PostgresSecrets.MigrationRecordsEachMountsOwnerAndRoute` migration `0057_secret_workspaces`. The
identity exit scenario and the Postgres secrets round trip give their mount a Ready workspace.
Dynamic leases (lake#362): `test_secrets.cpp` covers a lease's sealed record and content-free
route, renewal and revocation, a renewal racing a revocation or an expiry (the record fixture's
`before_put` lands the other writer's change between the read and the conditional write), the
expiry pass reading only the workspaces of due leases, holds, not-ready and deleted workspaces, and
the server tick. `test_workspace_records.cpp`'s `DynamicLeasesLiveInTheirMountsWorkspace` and
`TheOneShotCopyMovesDynamicLeasesIntoTheirMountsWorkspace` cover two spaces with two workspaces
each and the copy in both stores (`seed_legacy_dynamic_secret_lease`);
`PostgresSecrets.LeaseRoutesAreAContentFreeDueIndex` migration `0058_secret_lease_workspaces`.
Repository descriptions and refs (lake#315 part 2, lake#319): `test_workspace_records.cpp`
(`RepositoryDescriptionsLiveInTheirWorkspace`, `TheOneShotCopyMovesRepositoryDescriptions`) covers
two spaces with two workspaces each, sealed records and empty routing rows, reads and one
`list_records` per workspace, missing, malformed, unknown and foreign `workspace_id`, a refused
create leaving nothing, least privilege, a raced change, a moved record, not ready, deletion and the
copy in both stores; `PostgresRepositories.MigrationDropsRefsAndRecordsEachDescriptionsOwner`
migration `0064_repository_records`. The sync tests in `test_api.cpp` and the forge scenario read
refs through the `refs` capability (`served_refs`, `ref_head`), since lake keeps no copy, and
`PagePerformance` holds `/repositories` at one `list_records`.
Application settings (lake#351): `test_apps.cpp`
(`SettingsLiveInTheApplicationsWorkspace`) covers two spaces with two workspaces each, sealed
records and empty routing rows, reads and listings from the workspace, the theme with missing,
malformed, unknown and foreign `workspace_id` and a raced change, origins checked from the record,
least privilege, a moved record not opening, not ready and deletion;
`WorkspaceRecordStores.TheOneShotCopyMovesApplicationSettings` the copy in both stores;
`PostgresApplicationSettings.MigrationRecordsEachRowsOwnerForTheCopy` migration
`0059_application_settings_workspaces`. `stream_fixture.hpp`'s `seed_application_settings` writes
the settings of an application a test creates directly in a store, and `test_webauthn.cpp` calls
the passkey ceremonies asynchronously.
Application authorization codes (lake#363): `test_apps.cpp`
(`AuthorizationCodesLiveInTheApplicationsWorkspace`) covers two spaces with two workspaces each,
sealed records under blind keys, retention, one redemption, racing exchanges (through
`record_fixture.hpp`'s `before_delete`, a delete landing between a read and a conditional delete),
expiry, missing, malformed and unknown `application_id`, least privilege, not ready and deletion;
`WorkspaceRecordStores.TheOneShotCopyMovesLiveApplicationAuthorizationCodes` the copy in both
stores (`seed_legacy_application_authorization_code`);
`PostgresApplicationAuthorizationCodes.MigrationRecordsEachLiveCodesOwnerForTheCopy` migration
`0061_application_authorization_code_workspaces`.
Agent tasks (lake#374, lake#375): `test_workspace_records.cpp`'s
`AnAgentTaskKeepsItsFirstAnswerAndRereadsARacedEvent` covers the record (sealed, written once, a raced
write reread, the first answer kept, another key or space not opening, a request filled in), and
`AgentTasksKeepWhatWasWrittenInTheirOwnWorkspace` and
`TheOneShotCopyMovesAgentTaskContentIntoEachTasksWorkspace` cover two spaces with two workspaces
each and the copy in both stores (`seed_legacy_agent_task`);
`PostgresAgentTasks.MigrationRecordsEachTasksOwnerAndKeepsDispatchContentFree` migration
`0062_agent_task_workspaces`. `stream_fixture.hpp`'s `seed_agent_task_request`,
`seed_agent_task_answer` and `agent_task_content` write and read a task's record for tests that
create or finish tasks directly in a store; `AgentRuns::task` and `HttpResponseIsolation`'s
`assembled_task` read a task from both halves, and `test_instance_kek` gives memory-store tests a
KEK. `Stores/AgentLeases.ReclaimedTaskUsesFirstTerminalWriteAndRejectsLaterEvents` also checks that
the workspace keeps only the winning answer.
