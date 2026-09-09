# Test fixture isolation

## What exists

Lake test processes create one randomly suffixed directory below the platform temporary root. Every
filesystem fixture is a named child of that directory, and the process removes the directory at
exit. The helper uses `std::filesystem::temp_directory_path`, so platform selection and `TMPDIR`
remain effective. Failure to create the process directory reports the selected root and the
filesystem error.

Postgres-backed tests call `lake::test::postgres_uri()` from `tests/postgres_fixture.hpp`.
The shared GoogleTest runner lazily creates a UUID-named database from `template0`, loads
`db/schema.sql`, and drops that database after the test fixture is destroyed. Repeated calls in
one test return the same connection string, including when the test restarts a server or opens a
second store. A missing `LAKE_TEST_DB_URI` returns null so existing skip behavior is preserved.

`LAKE_TEST_DB_URI` supplies the administrative connection settings. Its database is never
modified, copied, or dropped. Its role needs `CREATEDB`. The runner generates a fresh random
`LAKE_INSTANCE_KEK` per test; fixtures with explicit cryptographic test keys may retain those
keys within their private database. There is no cross-test persisted identity. Database creation,
schema loading and cleanup errors fail the test; they do not turn into skips. A process killed
before teardown can leave its UUID-named database behind; remove only databases whose ownership
you have established.

We chose per-test databases because isolation covers every append-only table, sequences and
advisory locks automatically. A test cannot accidentally omit a tenant or request filter on a
write and contaminate another case. The implementation also works when several tests run in one
executable, with shuffle or repeat, rather than relying on CTest's usual one-case-per-process
execution. Existing HTTP/token fixtures retain their nested schemas inside the private database.

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
