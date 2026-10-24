# Tests

`test_workspace_records.cpp` runs `DisablingAUserRevokesAgentDelegation` and
`DisabledUsersCannotDriveAgentOrMcpStreams` on memory and PostgreSQL (lake#951). They exercise
retained launch/schedule/task credentials, direct disabled rows, lease renewal and queued claims,
cached-context creation, repeat offboarding, stream-trigger and MCP streaming cleanup, and an
unaffected person's launch. Run them with `LAKE_TEST_DB_URI` and `ctest --timeout 600 -R
'DisablingAUserRevokesAgentDelegation|DisabledUsersCannotDriveAgentOrMcpStreams'`.
`test_api.cpp`'s `Export.*` cases bind manifest signer IDs to their embedded keys (LSR-088,
lake#954), refuse an archive re-signed by another key before any destination write, and accept
valid and rotated instance keys. They check local authors and grant attestations naming the importer,
untrusted provenance on both forge import passes, cleared artifact verification, and release
evidence retained only as untrusted history. See [export.md](../docs/api/export.md).
`test_agents.cpp` runs dispatcher and durable-agent registration checks against memory and
PostgreSQL: an authenticated customer writer cannot name another dispatcher or an agent without
an active certificate for that customer. It also checks direct service calls and non-expiring
keys. See [agent session keys](../docs/api/agents.md#verify-and-change).
`Workspaces.DatasetAndModelMetadataRequireAuthenticatedCustomerRead` in `test_workspaces.cpp`
checks customer authorization before dataset/model workspace exchanges through REST dispatch and
MCP, including URL workspace context and the model alias. `test_api.cpp`'s
`Models.AnonymousDatasetAndModelRevisionsRequirePublicRepositories` retains anonymous public
revision reads and private revision refusals (lake#970, LSR-104; [contract](../docs/api/models.md)).
`test_pki.cpp` rejects SAN configuration injection through ACME, SPIFFE and agent issuance
(lake#946), validates malformed DNS/SPIFFE identifiers, and decodes successful certificates to
check one SAN with exactly the requested type and bytes. See [PKI](../docs/api/pki.md).
`Pipeline.*` checks pipeline budgets, original-index topology order, atomic final validation and
validation of a reversed 50,000-step chain within one second (lake#967).
`Workspaces.EditPipelineReadsValidatesAndSavesTheWholeGraph` checks oversized stored graphs on
edit/render, both save spellings and bounded-pool saturation.

`test_http_server.cpp` covers pre-body capability admission (lake#944): headers declaring 2 MiB
receive 413 on public identity routes and 401 on protected routes before any body bytes are sent.
The tests exercise both URL forms, rejected credentials, workspace Zetta and model gateway
adapters, anonymous MCP's 256 KiB cap, chunked bodies, depth and element limits, duplicate keys,
anonymous discovery and authenticated bulk inputs. See [HTTP limits](../docs/serving/http.md#request-body-limits).

`OidcJwks.*` covers invalid tokens, signature failures, unknown key IDs, cooldowns, rotation, concurrent refreshes and failed transports. `HttpServer.ClerkInvalidTokensNeverFetchAndEachIngressClientHasItsOwnLimit` checks that two ingress-reported addresses behind one socket peer get separate limits and a direct client is still limited; `HttpServer.ClerkJwksFetchDoesNotBlockOtherRequests` keeps a fake fetch blocked while a health request completes; `HttpServer.ClerkBackendStatusDoesNotBlockOtherRequests` covers the uncached account-status request too (lake#956).

`test_platform.cpp`'s `LiquidIdFixture` covers deployment rows that name a repository by the liquid
id `liquid deploy` sends, by lake's UUID, by an unknown id (reported, not skipped) and by a liquid
id two repositories share (lake#805); `*.RecordsAndFindsLiquidIds` runs the store contract on
memory and PostgreSQL, and `test_api.cpp`'s `SyncHttp.PushRecordsTheRepositorysLiquidId` the push.

`test_network_tunnel.cpp` exercises recipient-only CSR signing, trust, validity, refusal cases,
admin grant/revoke and signed audit against memory and PostgreSQL. `tunnel_fixture.hpp` creates
local keys and deliberately overprivileged CSRs. Transport evidence in `test_schema_conformance.cpp`
covers REST/MCP issuance. Network tests also cover 0085 replay and pending/scoped history providers.
See [tunnel credentials](../docs/api/tunnel-grants.md) and [the proof](../docs/build/tunnel-grants-proof.md).

`test_network.cpp` runs shared memory/PostgreSQL store cases for all four secure-networking
records through the telemetry wrapper. PostgreSQL-only cases verify contract columns, defaults,
foreign keys, backfill idempotency, notifications, expired grants, audit rollback and concurrent
updates. Shared validation rejects credentials outside app-to-service connections. See
[network.md](../docs/control-plane/network.md#verify) for the proof command.

`test_tunnel_clients.cpp` covers the tunnel client downloads (lake#745): the folder, the platform
allow-list, links bound to platform, version, expiry and key, `tunnel_client_downloads` and
`GET /tunnel-clients/<platform>` over a live server, refusing expired, tampered and unknown links.

`test_network_capabilities.cpp` covers desired-state lifecycle, scoped credential rotation and
refusal, effective rules, account permissions, OpenAPI and MCP registration, an MCP call, and the
explicit workspace-record boundary.
The schema-conformance case also calls every available network capability successfully and checks
its required output with the independent validator. See [the capability
contract](../docs/api/network.md#verify).

`LAKE_TEST_SKIP_STALE_SWEEP=1` disables cleanup of previous runs' databases when a shared-host
work order permits deleting only databases created by the current run. Current fixtures still
clean up their own copies; see [testing.md](../docs/build/testing.md).

`test_main.cpp` is the shared GoogleTest runner. `lake::test::postgres_uri()` gives each
Postgres-backed test a private database copied from a `lake_fixture_template_<digest>` template
loaded once per `db/schema.sql` version, and drops it afterwards; a start-up sweep removes only
stale `lake_fixture_*` databases. Fixtures use that database directly, with no nested schema. See
[testing.md](../docs/build/testing.md#databases-the-runner-owns).

A test's own socket clients (`liquid::http1::request`, liquid sync pushes and pulls) run on one
thread through `one_thread_io.hpp`, never `liquid::runtime::block_on`, whose several runner threads
hit a corosio close race (lake#510). The runner's terminate handler prints the uncaught exception
and its stack. See [testing.md](../docs/build/testing.md#test-clients-run-their-sockets-on-one-thread).

`test_web_app.cpp` and `WebAppHttp.*` cover what the server answers for the web app in `frontend/`
(lake#561): `index.html` for a browser's page request, the hashed files, the routes that are never
the app, and an identity application's custom sign-in host. `test_web_ui.cpp` covers only what lake
still renders: the hosted pages' stylesheet link, the failure page, the assets and Drive previews.
The web app's own tests are in `frontend/tests/`; see [web-app.md](../docs/ui/web-app.md).

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
lake#452: `Workspaces.CreateProjectSaysWhetherTheNameIsMissingOrTaken`,
`Workspaces.ListServicesSaysEachServicesStatusAndWhy`,
`Workspaces.ADeletedWorkspacesZettaLeavesTheListWithItsWorkspace` and the PostgreSQL services test
cover the capabilities; `AgentRuns.ApplicationAgentWritesAppsOnlyAfterTheProposalIsConfirmed` and
`HttpResponseIsolation.ShipAppsCreatesOperatesAndFollowsAnApp` cover the public acknowledgement.

`test_agent_leases.cpp` runs the same claim, activity, expiry, re-claim, three-attempt exhaustion,
workspace notice recovery and racing-terminal-write cases on memory and PostgreSQL stores.
It checks that a retry notice is the task's message (the web app shows it), legacy migration
adoption, schema reapplication, the partial expiry index, and duration bounds without clock sleeps.
The Streams task-credential fixture takes a real claim before making calls. See
[agent-bus.md](../docs/api/agent-bus.md).

`HttpResponseIsolation.AskOramaSaysWhoIsWorkingOnALongTurn` (lake#451) runs a real Ask Orama
claim, a delegation with its turn key, and the subagent's progress and result, and follows the turn
through `settle_ask_turn`'s task message and the asking turn's `agent-events`.
`Workspaces.TalkToOffersEveryAgentAndMarksTheOnesNotRunning` checks which agents *Talk to* offers, `Workspaces.EveryCatalogueAgentIsInTalkToWhenItRuns` that none of the catalogue is left out, and `Workspaces.ListAgentLibraryListsEveryAgentWhateverIncludeInternalSays` that `include_internal` no longer filters (lake#788).
`AgentNames.*` (`test_agent_names.cpp`, lake#763) keeps the Agent Library the one source of agent
names: every name follows the scheme (title-case words, "Agent" last but for Ask Orama), no old or
run-together spelling appears in the code, the web app, the guide, the docs, the tests or the tools
(release notes and dated records excepted), `docs/api/train-agents.md`'s table is the catalogue, and
no code pairs an agent key with its name outside the catalogue.

Lease regressions also cover delayed notice acknowledgements and stale waiting-deadline
observations: neither may discard the final failure notice or fail a fresh retry.

`Workspaces.Registry*` and `Workspaces.PipelineDefinitionCache*` cover the workspace definition
cache: expiry, change invalidation, input isolation, readiness/credential changes, errors and stale
in-flight replies. The per-page workspace-call budget test went with the server pages (lake#561);
[performance.md](../docs/ui/performance.md) explains the evidence rows.

`Workspaces.EveryNewFeatureIsInOpenApiAndWorksOverMcp` calls the direct filtered `list_runs`
capability and checks its completed status and stage.

`test_examples.cpp` (lake#439) pulls every copy-paste example out of what a customer reads: every
`/docs` page and the request bodies in `docs/api/*.md`. It names each example's capability from
`/v1/<name>`, a code span of the name, or a JSON-RPC `tools/call`, and validates the body or query
string with the registry's `validate_input`. A missing `workspace_id` therefore fails the build's
tests rather than a first paste. The Docs show `<workspace id>`.

`test_experiments.cpp` covers all eleven training operations, account permissions, confirmation,
workspace alias checks, audit, REST/MCP parity and actual calls, `start_experiment` with
`from_defaults` and Zetta's error `path`, the agents' guide naming the live read and the
confirmation (`Experiments.AgentDocsNameTheLiveReadAndTheConfirmation`; lake#449, lake#463), and
lake's experiment names translated to and from the engine's study names
(`Experiments.LakeSaysExperimentAndTheEngineKeepsItsStudyNames`, lake#561).
`Docs.AgentDocsSayLakeAndNeverSpace` (`test_markdown.cpp`, lake#647) renders llms.txt,
llms-full.txt, the skill and every description in OpenAPI and `tools/list` and fails on `space` or
a lowercase "lake" in their prose. `Docs.DeveloperDocsNeverSaySpace` (lake#669) reads every
`docs/api/*.md` and `docs/guide/*.md` file, READMEs included, and fails on `space` or `spaces` as a
word in their prose; code spans and blocks, workspace, namespace and whitespace are allowed.
`Docs.GuidesSpeakOfExperiments` and `frontend/tests/wording.test.ts` keep "study" out of the guides
and the pages. `test_workspaces.cpp` covers trial runs and experiment models named by their
experiment, the promoted marker,
the run report's held-out score lead, and `ZettaWorkspaceCaller` keeping `error.path` behind a fake
Zetta HTTP transport. `experiment_fixture.hpp` supplies a stateful fake caller against the
authorized engine contract and an opt-in browser fixture. `frontend/tests/experiments.test.ts` tests
the chart transformations in `frontend/src/pages/experiments/transforms.ts` without a browser, and
`frontend/tests/experiment-methodologies.test.ts` the Start an Experiment choices for a workspace
methodology (lake#689); the methodology case in `test_workspace_records.cpp` runs such an
experiment's trials end to end.
Independent response validation also uses these fixtures. See
[API](../docs/api/experiments.md) and [UI](../docs/ui/experiments.md) contracts.

`test_audit_isolation.cpp` also runs `ActivityIsNewestFirstNamedAndQuiet` on both stores:
`my_audit` ordering, principal names (an acting agent included), a workspace call's inner
capability and the hidden successful reads (lake#445).

`test_streams.cpp` covers Streams account/system/creator permissions, non-consuming browse,
retention, revisions, and REST/MCP parity with one call per operation
(`Streams.RestAndMcpOperateEveryStreamCapabilityAlike`). Its 1,000-topic case checks the 50-row
default page, cursor and prefix filtering, sub-500 ms response, one list call per Zetta and no
per-topic detail calls. `stream_fixture.hpp` is a stateful fake workspace caller using the exact
Zetta CRUD wire shape; bus tests use `MemoryAgentBus`.
`Streams.CreatorOwnershipLivesInTheTopicsWorkspaceZetta` keeps each topic's creator at its own
workspace endpoint across two accounts and two workspaces, and covers missing and unknown workspace
ids, a workspace that is not ready, workspace deletion and the immutable `if_absent` claim
(lake#379). The schema conformance fixture exercises all nine successful outputs. See
`docs/api/streams.md` and `docs/ui/streams.md`.

`test_instance_origin.cpp` checks external-origin precedence, startup validation, and that a
missing configuration names no origin (`InstanceOrigin.MissingConfigurationNamesNoOrigin`).
`InstanceOrigin.BrowserToTerminal` runs `instance_origin.mjs`: a native Chromium page supplies the
actual clone/push text executed by the pinned Liquid CLI against the shared fixture's fresh
database, non-default loopback port, store and credential. It refuses off-origin requests and
compares cloned and pushed bytes. Set `LAKE_TEST_DB_URI` and `LAKE_TEST_LIQUID`, plus the native
browser and Puppeteer paths where needed. See
[private-install.md](../docs/deployment/private-install.md). It finds the panel by its exact
heading, "Clone and Push" (Title Case since 94afa50); CI runs it (lake#547), which is how the stale
"Clone and push" it still looked for was found.

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
`test_store_providers.cpp` (lake#664) runs one conformance suite against every store provider, each
over a local fake of it in `store_provider_fakes.hpp` (an S3 server and an Azure Blob server that
check every signature, and a liquid repository branch), plus the workspace Zetta fakes; SigV4 is
pinned to the AWS examples and Shared Key to an independently computed signature.
`drive_history_support.hpp` is the Drive overwrite exercise of lake#123 (upload twice, history,
comparison, restore and deletion, through the HTTP API) over any running Lake; the Postgres
scenario and `WorkspaceStoreDrive.*` (account store and switched workspace store) both run it.
It also holds the upload exercise of lake#560 (the first upload creates `main`, an upload into a
missing folder creates it, an upload under a file is refused), which the Postgres scenario,
`DrivesHttp.TheFirstUploadCreatesMainAndAMissingFolder` and `WorkspaceStoreDrive.TheFirstUpload*`
run. The Postgres cases sign in as the repository's owner, so they pass the policy check where the
defect was; the others use the operator token.

The `scenarios/` cases combine those production Postgres stores with an in-process Lake, public
HTTP routes, and public client libraries. The drive scenario signs users up, synchronizes and
shares a folder, checks history through the API, preserves both named results of concurrent
offline edits, pulls the same repository through Liquid, and builds the project it contains. See
`scenarios/README.md` for the named coverage.

The default `lake-tests` target contains the portable suite plus Postgres-backed cases that skip
when `LAKE_TEST_DB_URI` is absent. `lake-postgres-tests` contains the focused control-store cases.
Run both through CTest after configuring the repository with CMake.

`HttpServer.IdleKeepAliveCloseWritesNothingToStderr` starts Lake with one request worker, captures
the process stderr file descriptor through a test-owned pipe, completes a keep-alive request, and
closes that connection while it is idle. A request on a second connection follows before the test
stops the server and asserts that the pipe stayed empty. `OutputPipe` captures stderr by default,
or stdout.

`test_http_connections.cpp` (lake#534) covers connections apart from request workers
(`src/api/http_connections.hpp`) over raw sockets: two hundred idle connections beside a request
answered within 500 ms (with a small router and through `ApiServer`), forty long polls that leave
their workers, the idle keep-alive, header (slowloris) and body/response I/O timeouts, the
connection cap's 503 and its fit to the open-file limit, 503 and 429 closing their connection, a
refusal reaching a client still sending its body, unread bodies never parsed as requests, the wait
for a worker and its refusal, 400, 431 and `100 Continue`, uploads, HEAD and a client's close, a
4 MiB streamed response beside idle connections, the drain, an idle connection's memory, the
transfer rate against trickled bodies and slow readers, the declared long polls and strict writes,
the bounded blocking pool, the long-request caps, and the admission, half-close, continue and
discard cases.
`StoreOwner.EveryRequestDuringTheHandOffIsAnsweredAfterIt` (`test_shared_store.cpp`) holds forty
requests through a store-root hand-off on two workers, and
`StoppingAWaitingSuccessorAnswersItsWaitingRequests` answers the ones still waiting at a stop.
`HttpServer/HttpResponseIsolation.ALongPollOverRestGivesItsWorkerBack` and
`RegistryStore.AStreamingPullGivesItsWorkerBack` cover the REST long poll and the pull. It raises the process's open-file soft limit for its sockets. See
[`../docs/serving/http.md`](../docs/serving/http.md#connections-and-workers).

Request telemetry (decision 54 S9, lake#384, [`../docs/serving/telemetry.md`](../docs/serving/telemetry.md)):
`HttpServer.RequestLogsNameRoutesAndEachWorkspaceGetsItsOwnRequestSeries` captures stdout while it
makes repository, capability, registry (`/:package`, OCI manifest), drive and sync requests,
checks every line names the route and request id and carries no raw path and no repository,
package, image or file name, then reads the `lake_requests` series of two workspaces of one account
and one of another after the flush at `stop()` (a request with no workspace counted nowhere). A
handler sends its response before it logs and counts the request, so the test stops the server,
which waits for every request, before it reads the log or the series (lake#547).
`test_request_series.cpp` covers the log line (`RequestLog.*`), `RequestRecorder` aggregation per
workspace, minute and labels, the bound and its drop count, a failed or not-ready write kept, and
deleted, unknown and conflicting workspaces dropped. `RequestRecorderZetta.QueryMetricReturnsTheFlushedCounts`
runs against a real Zetta node (zetta dev with #1231) when `LAKE_TEST_ZETTA_URL` and
`LAKE_TEST_ZETTA_TOKEN` are set, and reads the counts back with `query_metric`. `record_fixture.hpp`'s
`RecordTables` also answers `create_metric`, `record_metric` and `query_metric`.

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
private header. `HttpServer/HttpResponseIsolation.ShipPackagesListsABuildThatHasNoDigestYet/*` reads
what Ship → Packages shows, `list_packages` and `list_builds`, with a queued build whose digest is
null (lake#440).

Web UI string tests cover the content-hashed stylesheet link in the hosted page heads
(`WebUi.HostedPagesLinkTheSharedStylesheetOnce`), the escaped failure page
(`WebUi.AFailurePageEscapesWhatItSays`), SHA-256 URL changes for different stylesheet bytes,
content-hashed font references, and the immutable versus `no-cache` route policy. These checks use
embedded or in-memory bytes and perform no filesystem or network I/O.

Conflict coverage pins the complete Liquid-derived kind and region, plain explanation, reasons,
proposal visibility, generated OpenAPI schema, MCP resolution tool, preserved drive comparison
values, and every advertised resolution input.

`test_forge_scenario.cpp` is the Postgres-backed forge lifecycle proof. It drives repository sync
and review operations through a real Lake server with Liquid's sync and forge clients, reads the
candidate and landed changes and the open review through `/v1`, maps a line comment after an
inserted line, and lands the approved target. The same case records a graph action with signed
provenance in Lake's repository, rejects automation YAML in the source tree, and asserts the result
identity, output path, and verified profile in `list_artifacts`.

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
cursors, OpenAPI, and MCP discovery (`PlatformFixture.DiscoveryExposesPlatformSurface`). A Beast2
Loki double verifies the real coroutine HTTP client, selector request, secret bearer, line
conversion, and cursor. `test_workspace_records.cpp`'s
`PlatformEnvironmentsLiveInTheirRepositorysWorkspace` covers two accounts with two workspaces each,
sealed records only at the repository's workspace endpoint, a quiet sync writing nothing, an older
row never replacing a newer one, a stale revision, one environment created once by two writers,
least privilege without a workspace call, missing, malformed and unknown ids, a workspace that is
not ready (only that repository stale), a repository without a workspace and workspace deletion. The
copy's test and the `0060` migration test went with the retired tables (migration `0070`).

`test_web_apps.cpp` proves Ship App policy isolation, validation, desired-state generations,
reserved platform and configured names, direct-zone default hosts, app/replica quotas,
fake-operator status, Secret references, service binding, Loki namespace selection, purpose-scoped
operator hashes, including the pull purpose, and lifecycle event delivery with the memory store.
`WebAppSessions.*` checks app-session claims, tamper rejection and expiry.
`HttpServer/HttpResponseIsolation.ProtectedAppAuthorizationHandsOffAHostOnlySession/*` covers
anonymous refusal, normal sign-in return, exact redirect-host validation, one-use code exchange,
host-only cookie issuance, code reuse, cross-app and expired cookies, and membership revocation in
both stores.
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
repository reads, and cross-app, cache and write refusal.
Authorization assertions use well-formed input; malformed authenticated calls separately assert
`invalid_input`. Postgres registry contexts supply the fixture's audit encryption key.
`HttpServer/HttpResponseIsolation.ShipAppsCreatesOperatesAndFollowsAnApp/*` starts an
Application Agent turn through `agent_turn`, replies to its question in the same conversation,
refuses a confirmation without `proposal_task_id`, for a turn that proposed nothing, with another or
a changed `proposal_sha256`, and for an unacknowledged public app, confirms the stored proposal and
reads its deployed URL, then creates a repository app, queues an app build from the runtime image
supplied through real `ServerOptions`, publishes a deploy event and calls `restart_app`, all
through `/v1`. `AgentRuns` proves app tasks use their own topic and credential, Ask Orama delegates
to them, and App writes are absent before confirmation but usable after it. Memory variants add
fake operator Ready status.
`HttpServer/HttpResponseIsolation.ExperimentsAgentRestAndMcpUseTheSameConfirmedTurn/*` compares
the `agent_turn` OpenAPI/MCP schemas, continues one conversation across those transports, then
replies to a question, edits and confirms the proposal, and refuses a stale digest or a proposal
changed beyond its experiment.
`AgentRuns.AskOramaDraftsAMethodologyThatLakeChecksBeforeOffering` covers lake#688: Ask Orama's
`methodology_draft` turn carries its kind and context and no write, and `draft_methodology {task_id}`
offers a draft only when Lake's manifest, entrypoint and lock checks pass, falling back to the
template with the findings, a path outside the folder, a failed turn, or another agent's task.
`AgentRuns.ExperimentsAgentGrantsEachTurnOnlyItsExperimentOperations` verifies the
three kinds, independent topic/credential, workspace URL and confirmation-limited experiment actions.
`AgentRuns.AProjectAgentProposalIsCheckedThenAppliedApprovedAndRun` covers lake#438: a pipeline
change turn's credential runs `compile_intent` confined to lake's check graph on its own project
(and reads it back on a repeat), is refused every other workspace action, and the person then
applies the proposal with `edit_pipeline`, approves it and runs it with the proposal's settings.
It also checks the proposal's `review_url` and that the check graph is not listed as a pipeline.
`AgentRuns.AProjectAgentBuildChecksItsCompileAndThePickIgnoresTheCheckGraph` gives a build turn
the same confined check and proves the pick after the build compiles the project's own pipeline
rather than taking the check graph, titled from the Opportunity's intent (*Accounts churn*,
lake#816); `AgentRuns.AProjectAgentBuildIsTitledFromWhatTheProjectIsFor` titles it from the brief's
words when the Opportunity carries no intent, and with the project's name when the brief says
only *Design the pipeline for the project <name>*. `AgentRuns.APipelineTitleIsShortAndReadable` runs the title cases
in `frontend/tests/pipeline-titles.json`, which the web app's `design.test.ts` runs too, so
`pipeline_title` and `pipelineTitle` agree.
`AgentLeases.RetryNoticeIsTheTasksMessage` pins that a retry notice is the task's message, which
the web app's working panels show.
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
`PostgresSessions.AnExpiredCredentialIsToldApartFromAnUnknownOne` (`test_postgres.cpp`) issues an
hour-long token, proves it live, then moves its `expires_at` into the past with SQL instead of
sleeping past a one-second lifetime, so a loaded host cannot expire it early or late (lake#679).

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

`HttpServer/HttpResponseIsolation.TheViewerSaysWhoIsSignedInAndWhereTheyAre` checks the `viewer`
read the web app's navigation is built from, with both stores, a visitor, a stale cookie, a
signed-in person, a workspace they cannot see, and operator credentials. The browser proof is in
[front-door.md](../docs/ui/front-door.md).
`HttpServer/HttpResponseIsolation.PersonCreatesAnOrganizationInvitesAndAColleagueJoins` and
`.PersonalSpaceBecomesAnOrganization` call the organization capabilities as two signed-up people
against both stores (the type-the-handle confirmation is the web app's); `Organizations.*` and
`PostgresCapabilities.OrganizationSpaceMembershipsCarryTheirGrantToTheSpace` cover the capabilities
and the organization-account grants ([organizations.md](../docs/api/organizations.md)).
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

`Workspaces.DatasetRulesNameCsvIdAndEngineResults` and
`Workspaces.DatasetWritesAreCheckedAndIdsAgree` pin lake#447: SQL-queryable names, malformed CSV
refused with its line before any upload call, a taken name refused or reported as renamed, one
`<database>/<name>` id across list and get, and engine results told apart from datasets. See
[workspaces.md](../docs/api/workspaces.md).
`AgentRuns.LaunchesAndSchedulesBelongToTheirWorkspace`,
`AgentRuns.AWaitingLaunchSaysWhyAndTimesOutWithAReason` and
`AgentRuns.CreateAgentRefusesDefinitionFieldsZettaWouldDrop` cover lake#442 in memory;
`PostgresAgentLaunches` covers the `0043_agent_launch_workspaces` routing trigger, workspace
filters and the start-timeout withdrawal. See [agent-launches.md](../docs/api/agent-launches.md).
`test_docs_status.cpp` runs `tools/docs/check-status.py` on `docs/design/status.md` against the
newest snapshot in `docs/deployment/observed/` and the in-tree registry's OpenAPI, and proves the
script refuses each contradiction it looks for (lake#448). `WebApp.FrontDoorDocQuotesTheHomePage`
holds the bold quotes in `docs/ui/front-door.md` to `frontend/src/pages/home/HomePage.tsx` and
`signIn.ts`. Under Clerk the `viewer` read says `password_sign_up: false`
(`HttpServer.ClerkExchangeRefusesAForgedTokenAndSignOutEndsClerkToo`,
`WebApp.ViewerNamesClerkAndAnOrganizationNamesItsSpace`).
`OperatorContract.EveryStatementTheOperatorRunsPreparesAgainstTheSchema` and
`OperatorContract.RowsLakeWritesAppearInTheOperatorsPolls` (`test_operator_contract.cpp`, in
`lake-postgres-tests`) run a copy of every statement in orama-operator's
`internal/controller/lake_*_pgx.go` against a fresh schema: a column the operator selects that lake
lacks (lake#442, lake#465) fails here. `PostgresStores.MigrationAssignsTheSoleWorkspaceAndRecordsTheRest`
covers `0044_store_workspaces`. See [storage.md](../docs/control-plane/storage.md).

Repository workspaces (lake#315): `Repositories.BelongToOneWorkspaceOfTheirSpace` covers two accounts
and two workspaces in one, missing, empty, malformed, unknown, deleted and foreign `workspace_id`,
reads and the workspace filter, the refused transfer, both imports, and workspace deletion.
`PostgresRepositories.MigrationAssignsTheSoleWorkspaceAndRoutingStaysFixed` covers
`0051_repository_workspaces` (sole, several, none, a tombstoned repository, a rerun) and the
trigger's refusals. `PostgresRepositories.MigrationAssignsUnroutedRepositoriesTheirSoleWorkspace`
covers `0066_assign_unrouted_repositories` (lake#513): a repository without a workspace in an account
that later gets exactly one is assigned, with its routing and description owners recorded; one in a
account with two is left alone and reported once; a rerun changes nothing; and every other change of
`workspace_id` is refused in the migration's transaction, afterwards, and with the setting alone. `repository_test_support.hpp` (`repository_workspace`) gives other tests the
account's first live workspace for the repositories they create.

`test_deletions.cpp` covers deleting and revoking (lake#645, `Stores/Deletions.*` over the memory
and PostgreSQL stores): an organization refused while it owns a workspace and then gone from every
read, a revoked invitation that cannot be accepted, the grants that are kept, a secret mount that
leaves nothing in its workspace, a conversation removed for its person only, the engine deletions'
fields and an older engine's `not_supported`, and the log each is audited in.

`test_transcripts.cpp` covers the pure parts of speech-to-text and transcripts (lake#561):
`.vtt`/`.srt` cleaning, audio decoding and its 25 MiB limit, file names, the OpenAI multipart
request and its errors through a fake `TranscriptPost`, paging, excerpts and the audit
classification. `test_workspace_records.cpp`'s `TranscriptsLiveInTheirWorkspaceAndKeepNoAudio`
runs the six capabilities with a fake `Transcriber` over the memory and PostgreSQL stores; no test
calls OpenAI.

`test_mcp_client.cpp` covers Lake's MCP client (lake#652): the event-stream parser, server URLs,
public addresses and tool bounds, then the client over the server's own transport against
`mcp_test_server.hpp`, a local MCP server (Streamable HTTP and HTTP+SSE, an OAuth authorization
server, and hostile modes: slow, huge, garbage, another id first, a sampling request, endless
events, an unknown version).
`McpStdioConnectionsRunAsAWorkspaceApp` (lake#659) checks a stdio connection's secret, App, address
and lifecycle; `McpClient.AWorkspaceAppIsReachedOnlyAtAClusterServiceName` the address rule, and
`McpClient.DrivesAStdioServerThroughTheRunner` runs Lake's client through a running
`lake-mcp-runner` when `LAKE_MCP_RUNNER_URL` and `LAKE_MCP_RUNNER_TOKEN` are set.
`McpConnectionDispatchNamesExportsAndStreamingWorkspacesOnly` (lake#684, lake#685) checks the
dispatch rows, the workspace endpoint's digest and that the stream worker calls only streaming
workspaces; `HttpServer.WorkspaceMcpSessionsLearnThatExportedToolsChanged` drives the notice over
`/w/<workspace>/mcp`.
`test_workspace_records.cpp`'s `McpConnections*` cases run the MCP connection capabilities against
that server with both stores: the sealed record and its secret, the read-or-change rule for people,
Ask Orama and a user-built agent, auto-approve, the audit, the usage series, re-export through the
workspace's MCP endpoint, OAuth sign-in with registration, PKCE and refresh rotation, and the
stream worker's mirrored notifications, agent jobs and published results.
`AStreamStartsItsAgentOncePerMessageAsThePersonWhoSetIt` (lake#677) runs `set_agent_streams` and
`AgentStreamWorker` with both stores: the refusals, the sealed trigger, one job per new message as
the person who set it with the message as its input, and removal by `delete_agent` and by the worker
when the agent is gone.

`test_workspace_records.cpp` covers workspace records (lake#370,
[workspace-records.md](../docs/architecture/workspace-records.md)): sealed records and blind
indexes, paged listing, records that open only in their own workspace and lookup, conditional
writes with conflict retries and `busy` retries, two accounts with two workspaces each over the
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
with the table. `PostgresMovedTables.MigrationRefusesRowsWithNoRecordedWorkspaceOwner`
covers migration `0070_drop_moved_tables` in a PostgreSQL schema of its own (so the tables it
recreates and the unresolved row it leaves are invisible to tests running in parallel): a fresh
database has none of the 21 tables or 8 columns; an older one with a row in each carrying its migration's owner outcome, plus an opportunity recorded `deleted_workspace` that must not block, keeps
everything while one row is `needs_selection` (the migration raises and names the table) and drops
everything once that row's workspace is deleted; two reruns of the section and two of the whole
schema leave them dropped, and routing rows, routes, `audit_events`, `keystore` and the
`workspace_assignments` record stay. The copy and migration tests of every collection `0070`
dropped went with their tables; the tests that checked a retired table held nothing now check that
it does not exist.
Design → Opportunities (lake#376): `Stores/WorkspaceRecordStores.OpportunitiesStayInTheWorkspaceTheirScanRanIn`
(two accounts with two workspaces each, the scan's workspace only, a second settle writing nothing,
foreign and missing/malformed/unknown ids, a workspace that is not ready, workspace deletion),
`WorkspaceRecords.AnOpportunityStatusChangeRereadsAfterAConcurrentWrite` (derived keys and a raced
conditional write). `Drive.ShareLinksLiveOnlyInTheWorkspaceThatHoldsThem` covers
two accounts with two workspaces each, missing/unknown/foreign `workspace_id`, least-privilege
callers, forged and moved tokens, a workspace that is not ready, revocation and workspace deletion.
`workspace_fixture.hpp` (`RecordWorkspaces`) gives a test store Ready workspaces whose records live
in `RecordTables`, over the memory or PostgreSQL store.
Application events and delivery attempts (lake#356, lake#357): `test_apps.cpp` covers two accounts,
two workspaces in one, missing, malformed, unknown and foreign `workspace_id`, least privilege, a
workspace that is not ready, deletion, and an attempt claimed by another caller not being sent. `stream_fixture.hpp`'s `ready_workspace` makes a Ready
workspace over either store for tests that sign into an application pool.
Application users and organizations (lake#353, lake#354):
`ApplicationAuth.UsersAndOrganizationsLiveInTheApplicationsWorkspace` covers two accounts, two
workspaces in one, sealed records and claims at the application's endpoint only, duplicate emails,
TAAP identities and names, fresh and abandoned claims, a sign-up completed at the next sign-in, an
account that is not a user, suspension, deletion, renames, missing, malformed, unknown and foreign
`workspace_id`, least privilege, a workspace that is not ready and workspace deletion. The identity capabilities are asynchronous, so tests call them
through `call_async`.

Repository usage in workspace Zettas (lake#348, lake#349): `test_workspace_records.cpp`'s
`RepositoryUsageStaysInEachRepositorysWorkspace` covers `MeteringRecorder` across two accounts with
two workspaces each (nothing written on the request path, merged counters, a second replica's
conditional increment, not-ready, deleted and unknown workspaces, a repository without a workspace, the
bound, build records once), and `PostgresSync.PushAndPullUsePostgresControlPlane` a real push and
pull flushed into the repository's workspace, no metering table in PostgreSQL, and the flush on
stop.

Usage over time (lake#564): `test_usage.cpp` covers `UsageRecorder` (bounded, flushed, values added
together dropped together, readings keeping their latest, deleted and refusing workspaces),
`usage_over_time` across workspaces by hour, day and month and its refusals, limits, `record_model_usage`,
the owner loop's `sample_usage` and the speech-to-text seconds and tokens of a transcript, and
(lake#661) each closed hour of an app's ingress traffic charged once as `transfer`
(`UsageSample.ChargesEachClosedHourOfAppTrafficOnce`); `test_postgres.cpp`'s
`PostgresBilling.AppTrafficAddsUpByAppAndHour` covers `app_transfer` in both stores and
`test_operator_contract.cpp` the operator's statements for it.
`record_fixture.hpp`'s time series now answers a gauge's `last` reading per step.

The model gateway (lake#248): `test_model_gateway.cpp` covers the request as forwarded (a stream
asked for its usage), the usage of every answer shape (chat, Responses, embeddings, both streams),
the provider's refusals without echoing Lake's key, `model_completion` counting a task's and a
launch's tokens in the run's workspace with the prompt kept out of the audit, every other caller
refused before the provider, the monthly `model_tokens` limit refusing with `usage_limit_reached`,
and the OpenAI-compatible `/openai/v1/*` route over a real server, all against a fake provider.
LSR-094 (lake#960) checks hosted tools, media, asynchronous work, stored references and unknown
fields on every endpoint before the fake provider is called, including global and workspace HTTP
routes; positive cases preserve text, function history and application schemas. Transport failures
without an HTTP status are counted by estimate against the originating workspace. A chat
completion's assistant message sent back as history, with OpenAI's output-only `annotations`,
`refusal` and null `audio`, is accepted and forwarded without them.
Streaming (lake#665): a stream's usage read piece by piece however the pieces cut its events, a
relayed stream counted from its last event and one without usage audited `usage_reported: false`,
and the route over a real server and its outbound client against a loopback provider that holds its
last event until the caller has read the first.

Billing (lake#564): `test_billing.cpp` covers prices and bill lines, choosing a plan now or next
month, a month's bill from two workspaces' usage, invoices frozen, voided, reissued and paid, an
unread workspace holding an invoice back, a fake `PaymentProvider`, and the `LAKE_BILLING` switch
leaving every billing capability, `viewer.billing` and the Billing guide absent;
`PostgresBilling.*` in `test_postgres.cpp` covers the store on PostgreSQL. Stripe (lake#620):
`test_stripe.cpp` runs lake's Stripe client against a fake Stripe (customers, invoices and items,
finalize, voids, idempotency keys, a retry that bills once, rounding to the cent), the webhook
signature and its window, notices applied once per event id with the central audit, the keys read
from a secret mount with rotation and the manual fallback, and the route only where the lake bills.
The apex order sheet (lake#831): `test_orders.cpp` covers its configuration (the default, the merge
patch, every startup error), the order form's checks, the status token, the rate limit, the emails
(the customer's copy carries nothing the sender typed; support's carries every field), text bounded
on a UTF-8 boundary, and the Resend client against a test double for HTTP, Stripe Checkout and its webhook events, the
`cloud_orders` contract in both stores (on PostgreSQL, orama-operator#635's own list, claim and
finish statements: an unpaid order is never claimed, a paid one is claimed once and finished only
by its claimant), a paid order of more than one refused by both stores, and the pages and routes
over HTTP with a fake Stripe and a fake Resend.

The audit split by subject (decision 52, lake#317): `test_workspace_records.cpp`'s
`AuditEventsAboutAWorkspaceLiveInItsOwnChain` covers the `AuditRecorder` and the readers across two
accounts with two workspaces each (a call writes nothing until the flush, control-plane calls,
refusals and missing, malformed and unknown `workspace_id` central, sealed records at the
workspace's endpoint only, a second replica's appends never forking the chain, verify, export,
`audit` and `my_audit`, a workspace that is not ready, workspace deletion, the bound, tampering,
a lake without workspaces), and `TheOneShotCopyMovesWorkspaceAuditEvents` the copy in both stores.
`AuditQueueIsolatesUnavailableCustomersDuringFlush` and
`AuditQueueOverflowPreservesResultsAndSignedDeliveryEvidence` cover customer/workspace limits,
accepted retries during a flush, signed overflow evidence and preserved operation outcomes (LSR-098),
including direct push/reference recording.
`AuditRouting.EveryRegisteredCapabilityIsClassified` (`test_audit_isolation.cpp`) holds every
registered capability to one of the two tables. `PostgresAudit.MigrationRecordsEachWorkspaceEventsOwnerForTheCopy`
covers `0063_audit_workspaces`. `PostgresSync.*` and `PlatformFixture.*` read pushes, branch
protection decisions and deployment phases from the repository's workspace audit;
and `TokenAttenuation` finds an application's events there through `audit`.

Forge records in workspace Zettas (lake#341): `test_workspace_records.cpp`'s
`ForgeRecordsLiveInTheirRepositorysWorkspace` covers two accounts with two workspaces each, records
only at their own endpoint, other repositories' and workspaces' ids, a record copied elsewhere,
conditional updates, least privilege, missing and unknown `repository_id`, a repository without a
workspace, a workspace that is not ready and workspace deletion, for the capabilities and for
`load_branch_protection`. The forge, export, GitHub import and branch protection tests in
`test_api.cpp` give `Surface` a Ready workspace (`Surface::workspace`), and the sync and forge
scenario servers a `RecordWorkspaces`; `RecordWorkspaces::forge` writes a forge record the way lake
does. `Surface`'s KEK is hexadecimal, as `LAKE_INSTANCE_KEK` is.
Application signing keys and webhooks (lake#352, lake#355): `test_apps.cpp`
(`KeysAndWebhooksLiveInTheApplicationsWorkspace`, `AConcurrentRotationIsRereadNotLost`) covers two
accounts with two workspaces each, the anonymous JWKS, missing, malformed, unknown and foreign
`workspace_id`, least privilege, rotation and a raced rotation, not ready and deletion.
`stream_fixture.hpp`'s `seed_application_signing_key`
writes the first key ring of an application a test creates directly in a store.

Secrets in workspace Zettas (lake#328-#332): `test_secrets.cpp` runs the capability contract on a
`RecordWorkspaces` (history, number reuse, a stopped put completed by the next, transit, a KEK
rotation finished after a workspace was not ready, leases, denial). `test_workspace_records.cpp`'s
`SecretsLiveInTheirMountsWorkspace` covers two accounts with two workspaces each, sealed records only
at their own endpoint and none in PostgreSQL, missing, malformed, unknown and foreign ids, another
account's person, a record copied elsewhere, a workspace not ready, KEK rotation across workspaces and
workspace deletion, and no retired secrets table in PostgreSQL; it looks for its plaintext by a
long marker, since sealed base64url bytes can spell a short one (lake#547). The identity exit scenario and the Postgres secrets round trip give their mount a Ready workspace.
`SecretMountsBelongToAnAccountOrAnOrganization` (lake#561) has a person with only an individual
account create a mount without an organization, put, read and list a secret and use a key; another
person refused every call; an organization inside the account owning a mount of the same name
without a collision; and, on PostgreSQL, migration `0071`'s nullable route column.
Dynamic leases (lake#362): `test_secrets.cpp` covers a lease's sealed record and content-free
route, renewal and revocation, a renewal racing a revocation or an expiry (the record fixture's
`before_put` lands the other writer's change between the read and the conditional write), the
expiry pass reading only the workspaces of due leases, holds, not-ready and deleted workspaces, and
the server tick. `test_workspace_records.cpp`'s `DynamicLeasesLiveInTheirMountsWorkspace` covers
two accounts with two workspaces each; `PostgresSecrets.LeaseRoutesAreAContentFreeDueIndex` migration
`0058_secret_lease_workspaces` rerun and the due index.
Repository descriptions and refs (lake#315 part 2, lake#319): `test_workspace_records.cpp`
(`RepositoryDescriptionsLiveInTheirWorkspace`) covers two accounts with two workspaces each, sealed
records and routing rows without a description column, reads and one `list_records` per workspace,
missing, malformed, unknown and foreign `workspace_id`, a refused create leaving nothing, least
privilege, a raced change, a moved record, not ready and deletion;
`PostgresRepositories.MigrationDropsRefsAndTheRowIsRoutingOnly` migration `0064_repository_records`.
The sync tests in `test_api.cpp` and the forge scenario read refs through the `refs` capability
(`served_refs`, `ref_head`), since lake keeps no copy. Application settings (lake#351):
`test_apps.cpp` (`SettingsLiveInTheApplicationsWorkspace`) covers two accounts with two workspaces
each, sealed records and empty routing rows, reads and listings from the workspace, the theme with
missing, malformed, unknown and foreign `workspace_id` and a raced change, origins checked from the
record, least privilege, a moved record not opening, not ready and deletion. `stream_fixture.hpp`'s
`seed_application_settings` writes the settings of an application a test creates directly in a
store, and `test_webauthn.cpp` calls the passkey ceremonies asynchronously.
Application authorization codes (lake#363): `test_apps.cpp`
(`AuthorizationCodesLiveInTheApplicationsWorkspace`) covers two accounts with two workspaces each,
sealed records under blind keys, retention, one redemption, racing exchanges (through
`record_fixture.hpp`'s `before_delete`, a delete landing between a read and a conditional delete),
expiry, missing, malformed and unknown `application_id`, least privilege, not ready and deletion.
Agent tasks (lake#374, lake#375): `test_workspace_records.cpp`'s
`AnAgentTaskKeepsItsFirstAnswerAndRereadsARacedEvent` covers the record (sealed, written once, a raced
write reread, the first answer kept, another key or account not opening, a task without a record
answered), and `AgentTasksKeepWhatWasWrittenInTheirOwnWorkspace` covers two accounts with two
workspaces each and dispatch rows without content columns. `stream_fixture.hpp`'s `seed_agent_task_request`,
`seed_agent_task_answer` and `agent_task_content` write and read a task's record for tests that
create or finish tasks directly in a store; `AgentRuns::task` and `HttpResponseIsolation`'s
`assembled_task` read a task from both halves, and `test_instance_kek` gives memory-store tests a
KEK. `Stores/AgentLeases.ReclaimedTaskUsesFirstTerminalWriteAndRejectsLaterEvents` also checks that
the workspace keeps only the winning answer.
Projections (lake#625): `test_sealed_delivery.cpp` reproduces the orama-operator's sealed-secret
reference fixture byte for byte, checks `lake-secret://` references, delivers every field of a
secret sealed and never a value (rotation, a pinned version, the audit event), refuses anyone but
the operator and any key but the cluster's, leases a dynamic source, and checks an app's secret
environment against the mount's workspace, the secret and its field.

Workspace deletion (lake#514, decision 54 S12): `test_workspace_deletion.cpp` runs on the memory
store and, with `LAKE_TEST_DB_URI`, on PostgreSQL. `CascadesToEverythingTheWorkspaceHoldsAndNothingElse`
builds two accounts with two workspaces each, a workspace with a live, a retained and an expired
tombstoned repository, an app, two builds and tagged images (one name shared with the other
workspace), checks the dry run's counts, then that the deletion tombstones both repositories with
their retention ended and frees the name, deletes the app, cancels the builds, untags only the
workspace's images, leaves the other workspace and account alone, and records one central event per
object plus the cascade, once. `AnInterruptedCascadeResumes` fails the registry after the
transaction and checks the pending deletion, a failing owner-loop pass, and the pass that completes
it without repeating an event. `OnlyAnAdministratorOfTheSpaceDeletes` refuses a writer, a stranger
and another account's administrator, dry run included. `RegistryProtocols.DeletingAWorkspaceMakesItsImagesUnpullable`
(`test_api.cpp`) pulls through a running lake before and after, and
`Search.DroppingARepositoryRemovesItsSegmentsAndUnsharedBlobs` covers the index.

Tenant images (lake#383 phase A): `test_api.cpp`
(`Stores/TenantImages.LiveInTheirRoutedWorkspaceAndKeepTheirAddresses`,
`Stores/TenantImages.TheOneShotCopyRoutesAndMovesLegacyTags`, on the memory store and PostgreSQL)
covers two accounts with two workspaces each, explicit routing with missing, malformed, unknown,
foreign and deleted `workspace_id`, immutable routes, a build claiming only its own target, an app
moved between workspaces and unrouted by the operator, foreign layers refused, unreadable routes as
503 (`RoutesAreClaimedOnlyByTheirOwnerAndUnroutedOnlyByTheOperator`), a move with the cache name
while the old workspace's build keeps pushing (`AMoveReroutesAnImageAndItsCacheInOneStep`), push and pull through the route, sealed records
only in the routed workspace, builds confined to their workspace, npm and one-component OCI
unaffected, a workspace that is not ready, workspace deletion and the copy; the `RegistryProtocols`
tests run tenant pushes against routed workspaces; `WorkspaceRecords.ImageTagsMoveAtTheRevisionReadAndManifestsAreWrittenOnce`
the codecs and a raced tag move; `PostgresImageRoutes.MigrationRoutesUnambiguousImagesOnly`
migration `0068`.

Tenant image blobs in the workspace store (lake#383 phase B): `test_registry_store.cpp`
(`RegistryStore.*`, a real lake server with workspace stores on `FakeZettaObjects`) covers push and
pull of a multi-layer image through a routed, switched workspace (GET and HEAD, ciphertext only,
nothing in the instance registry, a repeated layer stored once, usage counted once), blob requests and
the copy refused during a drain with the lease never taken again (lake#536), a chunked
upload and a wrong digest, a 304 MiB layer streamed with the pull's resident growth bounded, a
blob of another workspace or account refused, the `oci_blobs` copy (plan, copy, rerun, verify with
SHA-256, missing, corrupt and unswitched blobs unresolved, pulls after the registry files are
removed), instance packages unaffected, workspace deletion, and `move_workspace_store` `provision`
(an empty, storeless switched workspace provisioned, then a repository created and committed; a
refused creation's tombstone not counted; live and retained rows, and a bucket holding objects,
refused; a repeated call a no-op, also after a restart; a workspace whose records name stored image
blobs refused; a person's token refused `403`). The review's cases: a PATCH during a completion
refused and the stored bytes unchanged, a record naming a missing or another object (`500` to GET
and HEAD, repaired by a push, the old object kept in `superseded`), forty stalled pulls holding at
most the workspace's eight stream slots while
another workspace answers, `instance_paths` without a shared instance digest and withheld for an
unreadable workspace or an uncopied legacy tag, the quota counting only new bytes (`507`, nothing
staged left), the upload sweep, usage per workspace, and a liquid collection keeping image blobs.
The second review's: eight pulls in flight while a push to the workspace succeeds, a ninth refused
with `Connection: close`, stalled readers ended by the idle timeout; the global cap of 24 pulls
with another workspace still answering; one root per object and the copy rooting older objects; a
claimed upload kept for a retry after a store error and not swept during its completion.

Workspace volumes (migration `0069`, operator#259, operator#260): `test_volumes.cpp`
(`Stores/Volumes.*`, on the memory store and PostgreSQL) covers growing a workspace's storage, the
refused decrease, unknown and deleted workspaces, callers who are not the operator, central audit,
the default snapshot policies, requests with schedule and keep, bad targets and schedules (a schedule
that never fires is refused), the operator's status read back, and tenants seeing `storage_reason`
but not the operator's message; `VolumeSchedules.*` the cron grammar and the never-fires check;
`VolumeStoreFailures.*` a store that cannot answer being `store_unavailable`; `PostgresVolumes.StorageNeverShrinksAndEveryTargetHasAPolicy` the
trigger, the policy checks and the backfill; `OperatorContract.*` the operator's new workspace and
snapshot statements.
`test_workspace_store.cpp` covers lake#380 (the table is in
`docs/architecture/workspace-store.md#tests`): the encrypted backend, its refusal of a rewrite, tampering, the owner's hand-off and lost lease, the rollout hand-off between two lake servers (`WorkspaceStoreHandoff.*`, lake#536; a workspace stopped or deleted while the pre-open waits in Zetta's acquire has the lease released at once, lake#541), and, through a running lake, the
move with an interrupted copy, the switch, two workspaces of one account and the lost lease's `503`;
`WorkspaceStoreRemoval.*` the removal of an account's old store (refusals, a linked account or store
directory refused, dry run, exactly two directories removed, the instance's directory untouched, switched workspaces still answering, a later
unswitched workspace given a new account store).
move with an interrupted copy, the switch, two workspaces of one account and the lost lease's `503`.
Its `WorkspaceStoreSearch.*` cover lake#381 through a running lake: sealed per-workspace search
indexes beside each store, the switch's drop from the account index, reload after a restart, a
non-operator caller who never makes search open another account's workspace, rebuild of a wiped,
partial or tampered index with and without a restart, a failed index write that leaves the store
alone, a dropped index write retried by search's worker with the workspace reported partly stale
until then (lake#540), a first push that does not wait for the rebuild, pushes beside searches, and deletion;
`test_search.cpp`'s `SearchWorkspaces.*` cover the same index on in-memory objects, with the
background rebuild queue, backoff, release snapshots, write-once runs and eviction.
`zetta_object_fixture.hpp` is `FakeZettaObjects`, Zetta's object routes in memory. Set
`LAKE_TEST_ZETTA_BINARY` to a `zetta` built from Zetta dev to run `WorkspaceStoreZetta.*` against it.
move with an interrupted copy, the switch, two workspaces of one account and the lost lease's `503`.
`zetta_object_fixture.hpp` is `FakeZettaObjects`, Zetta's object routes in memory. It models
durability (lake#527): the bucket's log is one sequence of applied writes, a barrier or a
`durable=true` write makes it durable up to there, and `crash` keeps a prefix of the rest (the
rest is kept as the writes themselves and replayed, so appending a large object costs its bytes
once, not a copy per append: lake#547);
`durable_requests` counts the requests that asked for an fsync (lake#535).
`fail_after` and `fail_randomly` lose requests before they are applied; `lose_next_answers` and
`lose_answers_randomly` apply them and then lose the answer. `applied` and `most_writes_in_flight`
show the order and concurrency of what was applied. `requests_seen` lists every request one endpoint received
(method and path), and `store_requests_to` counts them without the owner loop's `:usage` reads,
which run at start and every ten seconds whoever is calling: a test that a caller never reaches a
workspace counts only those (lake#607, `WorkspaceStoreSearch.ASearchNeverOpensAWorkspaceItsCallerCannotRead`
once counted a quota read that landed in its window under load). `hold(endpoint, fragment)` keeps every
matching request waiting until `let_go`, and `held` counts those waiting, so a test can say "while
this request is in flight" without a sleep (lake#603, lake#606). Set `LAKE_TEST_ZETTA_BINARY` to a `zetta` built from Zetta dev
to run `WorkspaceStoreZetta.*` against it; `latency_proxy.hpp` (`LatencyProxy`) puts a loopback
round trip in front of it, `LAKE_TEST_ZETTA_RTT_MS` (default 10) for
`WorkspaceStoreZetta.AOneFileCommitsRequestsAreCountedAndTimed`, and holds each request that asks
for an fsync (`:barrier`, `durable=true`) `LAKE_TEST_ZETTA_FSYNC_MS` longer (default 0; dev's disk
is about 30, lake#535). `LAKE_TEST_ZETTA_SEED_COMMITS` gives the measured repository that many
earlier commits of 8 files over 16 directories first. `LAKE_TEST_ZETTA_TRANSPORT=unpooled` measures
that test with the transport lake used before lake#544 (Zetta's connector client, a resolution and a
connection per request); otherwise it uses lake's pooled transport, prints its connections and
resolutions, and then times the first commit after the store is opened again (lake#538).
`test_object_http.cpp` (lake#544) runs the pooled transport (`make_pooled_object_transport`)
against a loopback HTTP/1.1 server that counts its connections: reuse, concurrency, a connection
closed by the server, `Connection: close`, the idle limit, a timeout, and no listener; a silent peer,
the sweep, the pool's bound, shutdown, fresh connections, refused methods and headers, framing,
IPv6, a cancelled shared resolution, and the close race (200 rounds). `LatencyProxy::lose_next_answers`
drops the next answers from Zetta and closes their connections, for
`WorkspaceStoreZetta.ALostAnswerOverThePooledTransportIsRetriedOrFailsTheBackend`.

`WorkspaceStoreHandoff.AFreshProcessCreatesOneLocalIdentityForItsConcurrentFirstOpens`
(`test_workspace_store.cpp`) points `LIQUID_HOME` at a fresh directory, starts lake with the
pre-open on, runs 32 first uses of liquid's local identity at once, then reads and commits through
the operator token, waits for the `repositories_preopened` line of each workspace on stderr before
it stops the server (the pre-open runs beside the requests, lake#614), and checks them. It guards the
start-time creation of the identity ([`../docs/deployment/store-volume.md`](../docs/deployment/store-volume.md#liquids-local-identity)).

`test_mcp_client.cpp` covers the 64-container response limit, duplicate keys, every transport,
batches, notifications, old tool records, and recovery after rejection. `test_workspace_records.cpp`
adds `McpOAuthRejectsDeepMetadataRegistrationAndTokens` for both stores: resource/issuer metadata,
registration, authorization-code and refresh tokens. Run
`ctest --test-dir build --timeout 600 -R 'McpClient|McpOAuth' --output-on-failure`. See
[MCP connections](../docs/api/mcp-connections.md#the-client) (lake#958, LSR-092).
`Drive.SharingCannotGrantActionsTheCallerDoesNotHold` (`test_api.cpp`, lake#955) checks role
attenuation for live grants and bearer links, member-only sharing by non-admins, combined grants
and admin sharing to known outsiders. See [sharing](../docs/api/sharing.md).
`test_deletions.cpp` runs `OwnershipRequiresAnOrganizationOwnerOnEveryMembershipPath` and
`OwnerInvitationsRecheckTheInvitersCurrentAuthority` over memory and PostgreSQL (LSR-099,
lake#965): admin self-promotion, owner tenure, both scopes, invitation acceptance and stale
inviter authority, direct admin grants, foreign-organization owners, owner approval, the operator
and deletion refusal. `ConcurrentOwnerRemovalsPreserveOneOwner` covers owner counts across independent PostgreSQL
connections.
Run with `ctest --test-dir build --timeout 600 -R 'OwnershipRequires|OwnerInvitations|ConcurrentOwnerRemovals'` and a
configured `LAKE_TEST_DB_URI`; the [API contract](../docs/api/organizations.md) explains the rule.

The policy and billing fixtures establish the first owner of an operator-created organization
with the reserved operator; subsequent owner grants require an existing organization owner.


`DrivesHttp.NestedMutationsRefuseProtectedDescendantsBeforeChangingMain` (lake#962, LSR-096)
checks direct/nested/mixed uploads, protected overwrites, recursive deletion/move and raw commits
against real repository refs, then permits an allowed sibling.
`Drive.MutationChecksTheLockedTreeAndRestoreChecksMain` checks a changed node at commit time and
historical-ref restore and conflict resolution into protected current main. See [drive.md](../docs/api/drive.md).
Dynamic-secret transparency privacy (lake#953, LSR-087):
`SecretsCapabilities.DynamicSecretTransparencyPublishesOnlySuccessfulCommitments` decodes anonymous
tiles for lease, renew and revoke, compares their digest-only payloads with the private signed events,
and checks cross-tenant denials and failures append nothing. `TransparencyLog.Audit*` covers raw
append/replication rejection of full audit records, malformed commitment payloads, replication of
commitments and identity scans that reveal no principal or signer.

`WorkspaceRecordStores.DynamicSecretsInAWorkspaceKeepTheirInstanceLogLeaf` retains its memory and
PostgreSQL coverage of central routing, private lifecycle events and ordered instance leaves; its
public leaves now contain only the matching event digests and exclude tenant and lease identifiers.
Model entitlement and model/direction billing (LSR-095, lake#961) are documented in
[`model-gateway.md`](../docs/api/model-gateway.md). The configured exact model/provider allowlist applies before dispatch on every transport;
billing additionally requires positive model input and output rates. `model_tokens` classes
preserve raw counts and bill each direction separately. `test_model_gateway.cpp` and
`test_billing.cpp` cover provider-call refusal and invoice amounts, including a missing price.
`ModelCompletion.ASeededInstanceCallsTheDefaultModelsWithBillingOnAtTheSeededPrices` makes a model
call with billing on against a fresh PostgreSQL control plane, priced only by the rates migration
0095 seeds, and checks the bill; `PostgresBilling.ModelTokenPricesAreSeededOnceOnFreshAndExistingInstances`
checks the seed on a fresh database and on a simulated existing one, and that it runs once.

`RecordModelUsage.UnpricedReportsCannotPoisonABilledAccountsLedger` rejects unpriced reports
before recording, permits priced outside reports without granting provider access, and retains
billing-off reporting.

`TransparencyLog.AuditLegacyForgedSignatureCannotClaimVerifiedIdentities` refuses a forged audit
signature with a valid self-digest on append, replication and historical scans.
`TransparencyRegistry.ReplicationAuthenticatesTheBatchHeadBeforeWriting` checks trusted checkpoint
authentication, proof rejection without writes, and honest partial catch-up (lake#974, LSR-107).
`test_authentication_limits.cpp` exercises bounded account/client state, selector-backed recovery,
legacy refusal, concurrent one-use consumption and worker saturation (lake#969).
`HttpServer.RecoveryWorkCannotBlockHealthOrTheSharedStoreConnection` holds a recovery row lock
while probing health and reading an unrelated store row; the client-limit HTTP test alternates
REST/MCP, gives two ingress addresses behind one proxy peer separate budgets and still limits
direct connections. See [the contract](../docs/api/identity.md#authentication-attempt-limits).
`SuspendedUsersLoseAgentDelegation` and `SuspendedUsersCannotDriveAgentOrMcpStreams` run the
offboarding fixtures on both stores with reversible Clerk suspension, including cached contexts,
lease claims, cleanup across worker restarts, and fresh work after an unban (lake#948).
`Forge.ReleaseRejectsArtifactsBoundToAnotherSource` and
`Forge.RequiredVerificationRechecksEveryReleaseArtifactSource` cover LSR-106: wrong repository or
change, unbound legacy/imported snapshots, multiple artifacts, malformed snapshots, disappearing
evidence and repository-store failures. `RepositoryHttp.PublicRepositoriesAreReadableAndPrivateRepositoriesAreHidden`
adds a real signed SLSA action environment and checks matching and mismatched release creation.
See [source binding](../docs/api/forge.md#source-binding-lsr-106).
`UsageLimits.SignedUpAccountAdminCannotChangeStorageThroughEitherSetter` uses a real password
sign-up in the in-memory control store to prove account administrators cannot clear, raise or
lower storage through either setter, using synchronous and asynchronous registry calls, while
non-storage limits remain editable. `UsageLimits.OperatorAndInstanceAdministratorCanChangeBothStorageSetters`
checks both privileged callers can set and clear storage (lake#976, LSR-109).
`Stores/EmailRecoveryLimits.*` in `test_offline_recovery.cpp` repeats the email recovery abuse
and cancellation contract on memory and PostgreSQL: concurrent first starts, case variants,
delivery/notice deduplication, account/client/global budgets, persisted budget reset and owner-only
cancellation, including refusal of later administrator approval. See
[recovery.md](../docs/api/recovery.md#email-recovery-budgets).
`Stores/ClerkRevocation.SuspensionAlsoStopsEmailRecovery` in `test_clerk_webhooks.cpp` checks
that a suspended account's email recovery is accepted but queues no mail until the suspension is
lifted, on both stores.
