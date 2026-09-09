# Indexed session authentication proof

## Reproduce

Use native macOS, Homebrew PostgreSQL 17 and the setup in [local.md](local.md). Build with at most
six jobs and run one suite at a time. The test uses `postgres_uri()` and its own database, never
existing application data. Enable the optional model client with the CI-pinned Python dependency.

```sh
cmake --build build -j 6
build/tests/lake-tests --gtest_filter='PostgresSessionAuthentication.*'
ctest --test-dir build --output-on-failure
ctest --test-dir build-shared --output-on-failure
```

`PopulationDoesNotIncreaseRequestCost` creates a password account and six API tokens through the
production store, giving seven active rows. It performs three sequential loopback HTTP
`GET /v1/server_info` requests for the operator and the latest token, adds ten tokens, and repeats
identical reads with seventeen rows. Both token populations have the same ancestor depth. Timing
uses a steady clock and includes the entire HTTP response. Each larger-population median must be
at most 1.5 times its smaller-population median plus 20 milliseconds of scheduling tolerance.

`IndexedReadsStayBoundedAtSevenThousandSessions` adds synthetic unrelated session rows directly
through SQL, avoiding thousands of unrelated password sign-ups. It samples the same HTTP endpoint
and checks PostgreSQL's actual session tuple counters for nine production authentication calls:
three valid bearers, three wrong secrets for an existing lookup, and three unknown lookup UUIDs.
Closing the measured store flushes its connection's counters. The test requires zero sequential
session tuples and at most 24 indexed tuples, so a scan cannot pass by merely using faster hashes.
No query-planner setting forces an index.

## Before

Source `d9d03d90ad92160c51714134217355844e58156d`, with only the measurement test added. Native
Release static build; PostgreSQL 17.7, OpenSSL 3.6.3. The host's starting load was
`{ 66.46 29.96 29.22 }`; these are diagnostic loopback samples, not production percentiles.

```text
sessions=7 bearer=operator seconds= 0.768636 0.777630 0.738807
sessions=7 bearer=session seconds= 0.824203 0.791051 0.757098
sessions=17 bearer=operator seconds= 1.974757 1.903692 1.870610
sessions=17 bearer=session seconds= 1.855378 1.847617 1.851834
sessions=17 bearer=anonymous seconds= 0.003725 0.002811 0.002782
```

The measurement regression failed both growth assertions before the fix, as intended: operator
median 1.903692 seconds exceeded 1.172954, and session median 1.851834 exceeded 1.206576.

## After

Same native toolchain and fixture; starting host load `{ 29.83 28.03 28.60 }`.
All six new authentication tests passed.

```text
sessions=7 bearer=operator seconds= 0.002830 0.003431 0.001215
sessions=7 bearer=session seconds= 0.002635 0.002109 0.002124
sessions=17 bearer=operator seconds= 0.001371 0.001088 0.001172
sessions=17 bearer=session seconds= 0.002221 0.002277 0.002300
sessions=17 bearer=anonymous seconds= 0.001358 0.001346 0.001439
sessions=7000 bearer=operator seconds= 0.007214 0.003390 0.001550
sessions=7000 bearer=session seconds= 0.002990 0.001888 0.001754
sessions=7000 auth_calls=9 sequential_rows=0 indexed_rows=9
[  PASSED  ] 6 tests.
```

The larger real population no longer adds per-session password work. The 7,000-row test also
proves bounded database reads independently of timing noise. Its synthetic rows are used only
for lookup scaling; the 7/17 measurements use credentials issued by the production store.

## Security and lifecycle coverage

`OnlyTheRandomSecretAuthenticates` refuses the stored digest, public selector, digest-as-secret,
mixed credentials and malformed bearers. It checks the persisted password Argon2 policy and both
correct and incorrect password sign-in. `OperatorPrecedesEvenAMatchingSession` configures a real
session bearer as the operator credential and proves HTTP bearer/cookie requests never update its
`last_used_at`. This catches accidental session resolution even when the operator would eventually
be accepted.

`ScopeExpiryAndRevocationApplyOnNextRequest` proves allowed and refused scopes, then session
revocation, expiry and ancestor-device revocation on the next HTTP request, including the session
cookie. `MigrationRevokesLegacySessionsAndIsRepeatable` removes the lookup column to model the
previous schema, stores a real legacy Argon2 bearer, applies the migration, signs in again and
reapplies the complete schema twice. A reopened store accepts only the replacement credential.
Existing `TokenAttenuation.*` cases preserve recursive ancestor, expiry, static authority and
application-pool restrictions across REST/MCP and both stores.

The implementation and deployment contract are in [authentication](../api/auth.md) and
[storage migration](../control-plane/schema.md#session-lookup-migration). Update the populations,
thresholds and migration fixture alongside any format or query change and retain the before/after
output when rerunning this proof.

## Full native proof before rebase

Both `LAKE_TEST_DB_URI` and `LAKE_INSTANCE_KEK` were set; the fixture created a fresh database
for every PostgreSQL case. The optional `huggingface_hub==1.30.0` client was installed. No
PostgreSQL or optional-client cases skipped.

```text
ctest --test-dir build --output-on-failure
100% tests passed, 0 tests failed out of 239
Total Test time (real) = 175.57 sec

The following tests did not run:
131 - SearchBenchmark.BuildAndQueryTenThousandFiles (Disabled)

lake version
lake 0.0.1
GET /v1/server_info: HTTP 200 ok=true
```

The complete documentation link check reports zero broken links. Linux proof deferred: the
host's prescribed `linux-proof.sh` runner is absent. No container proof was run.
