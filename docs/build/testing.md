# Test fixture isolation

## What exists

Lake test processes create one randomly suffixed directory below the platform temporary root. Every
filesystem fixture is a named child of that directory, and the process removes the directory at
exit. The helper uses `std::filesystem::temp_directory_path`, so platform selection and `TMPDIR`
remain effective. Failure to create the process directory reports the selected root and the
filesystem error.

Postgres-backed tests call `lake::test::postgres_uri()` from `tests/postgres_fixture.hpp`.
The shared GoogleTest runner (`tests/test_main.cpp`) lazily copies a private database for the
test from a template with `CREATE DATABASE … TEMPLATE`, and drops it after the test fixture is
destroyed. Repeated calls in one test return the same connection string, including when the test
restarts a server or opens a second store. A missing `LAKE_TEST_DB_URI` returns null so existing
skip behavior is preserved.

`LAKE_TEST_DB_URI` supplies the administrative connection settings. Its database is never
modified, copied, or dropped. Its role needs `CREATEDB`. The runner generates a fresh random
`LAKE_INSTANCE_KEK` per test; fixtures with explicit cryptographic test keys may retain those
keys within their private database. There is no cross-test persisted identity. Database creation,
schema loading and cleanup errors fail the test; they do not turn into skips.

### Databases the runner owns

Set `LAKE_TEST_SKIP_STALE_SWEEP=1` when a run may remove only databases it creates itself.
This disables the start-up sweep of earlier runs' databases; each current test still drops its
own private copy. The network control-plane proof uses this on the shared development host.

Every database the runner creates starts with `lake_fixture_`:

| Name | What it is | Lifetime |
|---|---|---|
| `lake_fixture_template_<digest>` | `db/schema.sql` loaded once; `<digest>` is the first 16 hex digits of the file's SHA-256 | Kept and reused by later runs; one per schema version |
| `lake_fixture_build_<epoch>_<hex>` | A template while it is being loaded | Renamed to the template name when loaded |
| `lake_fixture_<epoch>_<hex>` | One test's private copy | Dropped at the end of the test |

`<epoch>` is the PostgreSQL server's clock in seconds when the database was created.

- **Template load.** The first test process that finds no template for the current schema takes a
  PostgreSQL advisory lock on the administrative database, loads `db/schema.sql` into a build
  database, marks it `IS_TEMPLATE true` and `ALLOW_CONNECTIONS false`, and renames it to the
  template name. Concurrent processes wait on the lock and then find the template. A runner on
  another administrative database that publishes the same template first wins the rename; the
  loser drops its build. Changing `db/schema.sql` changes the digest, so a new template is loaded
  and the old one is never used again.
- **Copy.** PostgreSQL refuses to copy a template while any session is connected to it (SQLSTATE
  `55006`). The template refuses connections, so only a transient session such as autovacuum can
  cause that; the runner retries the copy on that error only. If the template has disappeared
  (`3D000`, swept by another runner), it loads it again.
- **Sweep.** Once per test process, before its first copy, the runner drops its own leftovers: a
  copy or build database more than an hour old (a process killed before teardown; CTest's default
  timeout is 1,500 seconds), and a template for another schema digest created more than a day
  ago (the creation time is in the template's comment). A database with a live session fails
  `DROP DATABASE` and is left alone. The sweep matches only the `lake_fixture_` names above;
  it never touches other databases on the server, including `lake_test_<uuid>` databases left by
  runners before this scheme. Remove those only once you have established who owns them.
- **Concurrent sweeps** (lake#1254). Every test process sweeps, so under `ctest -j` several reach
  the same stale database at once. PostgreSQL marks a database invalid (`datconnlimit = -2`) as
  soon as a `DROP DATABASE` starts, and `ALTER DATABASE` on an invalid database is `FATAL`: it ends
  the session. The sweep used the process's shared administrative connection, so the process that
  lost that race failed its first test in `SetUp` with *Lost connection to the database server*
  (on 2026-10-26 the first four `SharedLinkScenario` cases, the first PostgreSQL cases in ctest's
  order, failed that way while templates of earlier schema versions were being swept). Each drop
  now runs on a connection of its own (`lake::test::drop_stale_fixture_database`); an invalid
  database is dropped without the `ALTER`, whatever its age; and any failure, a lost connection
  included, is reported on stderr and left for the next sweep, never failing a test.
  `FixtureSweep.AnInvalidTemplateIsDroppedWithoutLosingTheConnection` (in `lake-postgres-tests`)
  covers it.

We chose per-test databases because isolation covers every append-only table, sequences and
advisory locks automatically. A test cannot accidentally omit a tenant or request filter on a
write and contaminate another case. The implementation also works when several tests run in one
executable, with shuffle or repeat, rather than relying on CTest's usual one-case-per-process
execution.

### Why a template

Loading `db/schema.sql` takes a lock on every table, index, sequence and trigger it creates, in
one transaction. Every Postgres-backed test used to do that for its own database, and
`HttpResponseIsolation`, `TokenAttenuation` and `WorkspaceRecordStores` then loaded it a second
time into a nested schema and dropped that schema with `CASCADE`, another transaction locking
every object (about 1,080 locks to load the schema, about 2,100 to drop it). With `ctest -j 6`
several of those ran at once against one server, whose shared lock table holds
`max_locks_per_transaction × (max_connections + max_prepared_transactions)` entries (6,400 on a
default local PostgreSQL), and the suite failed with `ERROR: out of shared memory` once the schema
reached migration `0063`. Every migration made it worse.

Copying a template takes a handful of locks regardless of schema size, so the suite's lock use no
longer grows with the schema, and no PostgreSQL configuration change is needed. The alternative,
applying and dropping the schema in smaller transactions, would bound each transaction but still
do the schema's full work per test and need the schema split by hand. The nested schemas were
redundant once each test had a private database (lake#140), so those fixtures now use the
runner's database directly.

## Why

Fixed paths below a shared temporary root let concurrent test processes delete or rewrite each
other's repository stores and search manifests. Shared Postgres databases similarly mix durable
rows between processes. Process-scoped filesystem and database names let independent worktrees run
the full suite concurrently.

## How to verify

Run the complete native static and shared CTest suites with `LAKE_TEST_DB_URI` set to a local
PostgreSQL 17 database, one suite at a time and builds limited to six jobs. The base database may
contain unrelated records: the test fixture loads its own schema into a fresh database.
`Stores/AuditIsolation.*` covers both stores, an event beyond the first 1,000 same-object events,
exact request lookup through the registry, cursor exhaustion, signed-chain verification, and a
personal customer older than unrelated customers.

For the contamination demonstration, temporarily register a test before
`PostgresCapabilities.OrganizationAndPolicyRoundTrip` that writes 3,000 signed events into the
caller-provided database. Run the full suite and remove the temporary test. Also run the two
permanent PostgreSQL isolation regressions together in one executable to exercise database
cleanup between cases. Retain the commands and output in the pull request.

Also verify that test code has no direct system-temporary paths:

```sh
rg -n 'temp_directory_path|/tmp/lake-' tests
```

Only `tests/temporary_directory.hpp` should match.

## How to change

Use `lake::test::fixture_path()` for every new filesystem fixture. Fixed child names are allowed
inside the process root. Obtain every PostgreSQL connection from `postgres_uri()`, including raw
SQL setup and server options. Do not connect directly to `LAKE_TEST_DB_URI` in a test: it belongs
to the proof runner. The fixture returns libpq connection parameters, not necessarily a URL.

For audit assertions, capture the request ID immediately after the operation and query with
`audit::Filter::request_id` (or the registry's `request_id` input). Object and outcome filters can
further constrain it. Assertions about a whole signed chain or a set of outcomes use
`lake::test::audit_events(store, filter)` from `audit_test_support.hpp`, which exhausts pages by
`after_id`; increasing the first-page limit is never a fix. A test deliberately checking page
size may call `list_audit_events` directly, but must assert the page boundary itself.

For other tables, filter by the fixture's exact identifier before limiting. The personal-customer
helper queries by user ID and requires exactly one result. For a consumable queue without an ID
filter, consume successive pages until the requested record is found or the queue is empty;
`claim_recovery_delivery` demonstrates this. Keep negative assertions and complete-history
verification: isolation is not a reason to remove writes or weaken coverage.

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
transparency log use real Liquid data. The discovery fixture owns its in-memory history independently of the PostgreSQL cases. All transport, registry, authorization,
audit, and value handlers remain production implementations. Run the full static and shared
suites with `LAKE_TEST_DB_URI` set to cover the production Postgres store as well.

The shared PostgreSQL fixture isolates consecutive static/shared and diagnostic runs. Manual
server reproductions should still use a separately owned database.

For focused diagnostics after the full proof, retain payloads with
`LAKE_SCHEMA_EVIDENCE=<absolute path>` and run `ctest --test-dir build -V -R SchemaConformance`.
The fixture initializes TUF from the signed `tests/fixtures/schema-tuf.json` role set, then
appends real TAAP attestations and publishes a checkpoint before requesting proof tools. Its
fixture README describes replacement and signature-binding requirements.

The report distinguishes all tools called from successful tool responses validated, and requires
a successful call for every advertised value schema with required fields. Add fixtures when
introducing new required output fields. The envelope decision is in
[`../api/README.md`](../api/README.md).

## Test clients run their sockets on one thread

A test that talks to a server over a socket (`liquid::http1::request`, a liquid sync push or pull)
runs it with `lake::test::block_on_one_thread` or `lake::test::http_request` from
`tests/one_thread_io.hpp`, never with `liquid::runtime::block_on` (lake#510).

`block_on` drives its io_context from several runner threads. corosio's reactor (kqueue on macOS,
epoll on Linux) gives the runner that called `kevent()`/`epoll_wait()` the descriptor state of
each event as a raw pointer. When another runner closes that socket in the meantime, the socket
service frees the descriptor state at once, because it is not yet queued; the first runner then
queues the freed state, and the runner that pops it locks a destroyed mutex:

```text
terminate: uncaught exception: mutex lock failed: Invalid argument
  std::__throw_system_error
  std::mutex::lock
  boost::corosio::detail::reactor_descriptor_state::invoke_deferred_io()
  boost::corosio::detail::reactor_scheduler::do_one(...)
  boost::corosio::detail::reactor_scheduler::run()
  liquid::runtime::context::run(...)::$_0   (a block_on runner thread)
```

It aborted `SchemaConformance.EveryToolAndRequiredOutputUsesHttpContract` in about one run in 25
when eight copies ran at once, mid-test and not at shutdown, and passed alone. The lake binary
does not take this path: each of its io_contexts that owns sockets (the API server's io thread,
the outbound HTTPS client, Zetta's native transport) is run by exactly one thread, and nothing in
`src/` opens a socket inside `block_on`. SIGTERM does not reach it either: `ApiServer::stop()`
stops and joins those threads before it flushes the metering and audit recorders. The race itself
is corosio's and is to be fixed there.

The runner's terminate handler (`tests/test_main.cpp`) prints the uncaught exception and the stack
of the thread that threw, so an abort like this one is diagnosable from the CTest log.

## Issue #138 bounded-read sweep

The sweep examined every `list_audit_events`, registry `audit`, customer lookup, recovery-delivery
claim, provisioning-log read, application-webhook read, and bounded list followed by a search in
`tests/`, including helpers used by parameterized tests. No coverage-producing writes were removed.

| Test or helper | Disposition |
|---|---|
| `PostgresCapabilities.OrganizationAndPolicyRoundTrip` | Exact deletion request plus repository object/outcome; fresh database. The earlier object-only first-row assertion was still order-dependent. |
| `PostgresCapabilities.SearchFiltersRepositoryCandidates` | Exact search request filter replaces the shared `reader` history scan. |
| `PostgresAudit.RegistryEventsPersistAndVerify` | Exhaust all audit pages before/after; retain exact event-count delta and full signature verification. |
| `PostgresSync.PushAndPullUsePostgresControlPlane` | Exhaust audit pages before checking push/pull outcomes. |
| `PostgresSync.EnforcesBranchProtectionAtReferenceUpdates` | Exhaust both histories; retain refusal details and the negative no-new-branch-event assertion. |
| `IdentityExit.PostgresProvesIdentityPkiAndDynamicSecrets` | Exhaust certificate and full-chain histories; retain exact lease-object scope and complete verification. |
| `PostgresTransparency.CapabilitiesPersistSignedAuditOutcomes` | Exact read request; exhaust history for every required action and signed-chain verification. |
| `TokenAttenuation.ScopeLifetimeAndRecursiveRevocation`, `WildcardStaticAndDeviceAuthority`, `InvalidLifetimesAndExpiredAncestors`, `ApplicationPoolsReturnOnlyDerivedSessionBearer` (all transport/store parameters) | Shared call helper filters by request; teardown exhausts history and retains secret-absence/signature assertions. The application-webhook count remains scoped to a fresh application with exactly three issued sessions; an explicit non-full-page assertion prevents silent truncation. |
| `personal_customer_test_support.hpp` | Query user ID before limiting, require exactly one customer. Applies to provisioning, identity exit, offline recovery, token applications, sync-registry and all drive/share scenarios that call the helper. |
| `PostgresOfflineRecovery.GrantsRecoveryPathsAndRepositoryKeyCustody` | Claim successive queue pages until the exact recovery ID is found; fail at an empty queue. Both email and administrator paths retain completion checks. |
| `PostgresIdentity.SsoFederationAndScimDeactivation` | Check the provider's newest log immediately after deactivation, including external user ID; no history search. Later delete operations remain covered. |
| `PostgresApplications.CapabilitiesPersistKeysUsersOrganizationsAndWebhooks` | Capture the application's newest webhook event immediately after organization creation and retain its ID for retry; no history search. |
| `Scim.UserUpdateGroupReplacementAndDeactivationRevokeSessions` | Check the newest provider log immediately after deactivation; private memory fixture. |
| `Repositories.LifecycleUsesPolicyAndAuditRegistry` | Exact deletion request; private memory fixture. |
| `Sso.SamlAndOidcProviderAssertionsRequireFederationPolicy`, `Sso.SignedSamlAssertionIsVerifiedBeforeSignIn` | Exact denied request; private memory fixtures. |
| `ScimHttp.RecordedClientSessionProvisionsUsersGroupsAndRevokesAccess` | Exhaust audit pages; private memory fixture. |
| `Audit.RegistryRecordsEachCapabilityOutcomeAndDetectsDeletedRows`, `Audit.DeniedAuthorizationAndJsonLinesExportAreRecorded`, `Audit.IdentityRotatesAndRetentionFloorIsEnforced` | Exhaust all histories, including rotation/reopen, preserving counts, export, retention and tamper checks; private memory fixtures. |
| `SyncRegistry.DeclaresTransportPolicyAndRecordsDenial`, `SyncRegistry.RevokedOfflineGrantRefusesPublicationWithChangeAndReason` | Exhaust audit pages; private memory fixtures. |
| `SyncHttp.PushPullShareCustomerStoreAndTransferOnlyMissingChunks`, `SyncHttp.PushWithoutWriteAccessIsRejectedAndAudited` | Exhaust audit pages; private memory fixtures. |
| `Identity.PasswordSessionsTokensAndRevocation`, `OAuth.AuthorizationCodePkceOidcAndCachedJwks`, `Search.RegisteredCapabilityReturnsAuthorizedHits` | Exhaust audit pages; private memory fixtures. |
| `TransparencyRegistry.ReplicationCapabilityConvergesTwoInstancesAndRecordsAudit`, `TransparencyRegistry.CapabilitiesServeMetadataAndRecordAudit`, `TransparencyTimestampRegistry.CapabilitiesTimestampRenewReadAndAudit`, `TransparencyTimestamp.OwnerLoopRenewsDueEvidenceThroughRegistry` | Exhaust audit pages; private memory fixtures. |
| `PlatformFixture.SyncCreatesEnvironmentLatestPhaseAndAuditEvent`, `PlatformFixture.PlatformReducersProduceTheSameResultForEveryInputOrder` | Exhaust audit pages; private memory fixtures, including each reducer ordering. |
| Agent-session audit helper (`AgentSessions.MintsAttributesUsesAndExpiresSessionKey`, `PostgresAgentSessions.PersistsLoggedSessionAndDurableAttribution`) | Already scoped by the minted session object; now exhausts that scope too. |

Bounded lists retained as list/ordering coverage, rather than arbitrary history searches:
`PostgresProvisioning.OwnsOneCustomerAndCanCreateFirstRepository` and
`HttpResponseIsolation.FreshSignUpOwnsFirstRepositoryWithZeroInitialMetering` test authorized
customer visibility in private fixtures; `Customers.IndividualLifecycleWithGeneratedHandle`
checks listing its private customers. `PostgresCapabilities.DriveShareLinksRoundTrip`,
`PostgresMetering.AggregatesHourlyUsageAndRecordsBuildOnce`,
`PostgresCallers.PersistsKubernetesAndFederatedCredentialMetadata`, its portable
`Callers.RegistryRecordsFederatedAndCloudExchanges` counterpart, and SCIM user/group lists assert
exact cardinality/order inside their fresh repository, customer/hour, or provider scope.
`AccountRecovery.CodesCoolingAndTwoAdministratorApprovalRestoreSignIn` consumes the one expected
item in each private in-memory queue. These do not search a bounded global collection.

The personal-customer helper's original callers are:
`PostgresProvisioning.OwnsOneCustomerAndCanCreateFirstRepository`,
`PostgresProvisioning.SessionFailureRollsBackUserCustomerAndGrantThenRetrySucceeds`,
`IdentityExit.PostgresProvesIdentityPkiAndDynamicSecrets`,
`PostgresOfflineRecovery.GrantsRecoveryPathsAndRepositoryKeyCustody`,
`SyncRegistry.RevokedOfflineGrantRefusesPublicationWithChangeAndReason`,
`OfflineGrant.CachedReadSurvivesRevocationUntilExplicitExpiry`,
`RepositoryRecovery.MissingContactIsPlainAndWrappedKeyIsContactOnly`, and
`TokenAttenuation.ApplicationPoolsReturnOnlyDerivedSessionBearer` (all parameters).
The shared drive setup also uses it for
`DriveScenario.UserSyncsSharesAndSeesHistoryEndToEnd`,
`DriveScenario.ConcurrentOfflineEditsExposeAndResolveFirstClassConflict`,
`DriveScenario.SyncedFolderPullsAsRepositoryAndBuildsProject`,
`SharedLinkScenario.SignedOutRecipientTraversesPreviewsAndDownloadsExactBytes`,
`SharedLinkScenario.ScopeAndRevocationRefuseEveryRead`,
`SharedLinkScenario.ExpiryAfterListingStopsPreviewAndDownload`, and
`SharedLinkScenario.DescendantPolicyCanRefuseTheLinkWithoutChangingItsRoot`.

## Issue #138 verification

Native macOS with Homebrew PostgreSQL 17.7, builds limited to six jobs and one CTest suite at a
time. The initial clean static run passed; its optional Hugging Face client was then enabled
with the CI-pinned `huggingface_hub==1.30.0` in a worktree-local virtual environment.

The full static contamination run registered a throwaway test immediately before
`PostgresCapabilities.OrganizationAndPolicyRoundTrip`. It wrote and verified 3,000 signed events
in the caller's database; both tests passed in that order. PostgreSQL independently reported
`3000|1|3000` for count/minimum sequence/maximum sequence. The throwaway test was removed and both
builds were refreshed. The final shared suite passed against that same populated base database,
which still contained exactly those 3,000 events afterward.

```text
ctest --test-dir build --output-on-failure  (with the throwaway test)
100% tests passed, 0 tests failed out of 234

ctest --test-dir build-shared --output-on-failure  (final source)
100% tests passed, 0 tests failed out of 233
```

The existing `SearchBenchmark.BuildAndQueryTenThousandFiles` is disabled in both suites. No
PostgreSQL or optional-client cases skipped in these two runs. The four new regressions also
passed together with shuffle seed 138, and the organization/policy case passed twice in one
process. `lake version` and a launched server's `/v1/server_info` passed the binary smoke check.

## AddressSanitizer

A segfault, bus error or abort in any test or in the `lake` binary means an AddressSanitizer run of
the whole suite, even when the crash does not come back. Build in its own directory and run every
test under it:

```sh
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLAKE_BUILD_TESTS=ON \
  -DLAKE_LIQUID_SOURCE_DIR=<liquid> -DLAKE_ZETTA_SOURCE_DIR=<zetta> \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address
cmake --build build-asan -j 4
ASAN_OPTIONS=detect_leaks=0 LAKE_TEST_DB_URI=... ctest --test-dir build-asan --output-on-failure \
  > asan.log 2>&1
grep -c 'ERROR: AddressSanitizer' asan.log   # must be 0
```

macOS's ASan does not detect leaks, hence `detect_leaks=0`. Every report is fixed or filed with its
stack. The first run (lake#1168, a null control store in the owner loop) found a
heap-use-after-free in `InMemoryStore::upsert_scim_group` (lake#1175) and a container-overflow in
`run_search_jobs` (lake#1176).

`RegistryStore.ALargeLayerIsStreamedNeverHeldWhole` checks the push and the streamed pull under
ASan and skips only its held-memory bound. ASan's macOS malloc zone reports `size_in_use` as every
byte ever allocated: in a probe, 64 MiB freed still read 64 MiB and 1 GiB allocated and freed read
1088 MiB, with the quarantine at its default or at zero, where the system allocator reads 0. The
304 MiB pull read as 6.2 to 6.7 GiB of growth under ASan and 8 MiB in the normal build.
