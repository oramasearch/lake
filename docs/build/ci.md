# Continuous integration and image publishing

GitHub Actions has two workflows that keep the repository buildable and publish deployment
images. The Dockerfile remains the canonical Linux toolchain recipe; CI installs the same Ubuntu
24.04 packages, LLVM 19 channel, checksum-verified OpenSSL 3.6.3 source build, and CMake 3.31.5
installer.

## Build and test workflow

[`ci.yml`](../../.github/workflows/ci.yml) runs lake's **full suite on Linux** for every pull
request and every push to `main` (lake#547). It is one job, `ci`, on `ubuntu-24.04`:

1. loads one read-only deploy key per private repository (below) and checks out all submodules;
2. fetches `oramasearch/zetta` at `ZETTA_REF` and `oramasearch/liquid` at `LIQUID_REF`, the
   commits the bridges pin (Zetta `5f70b30`, liquid `8955519`);
3. installs the Dockerfile toolchain (LLVM 19, CMake 3.31.5, OpenSSL 3.6.3 built from the
   checksum-verified source);
4. restores, or else builds, the **Zetta server binary** at `ZETTA_REF` (below);
5. starts a **Postgres 16 service container**, configures Release with `LAKE_BUILD_TESTS=ON`,
   builds with clang-19 through ccache and applies `db/schema.sql`;
6. runs every ctest test with `LAKE_TEST_DB_URI` and `LAKE_TEST_ZETTA_BINARY` set:

```sh
ctest --test-dir build --output-on-failure --timeout 300 --parallel 2 -E "$HEAVY_TESTS" --output-junit ctest-results.xml
ctest --test-dir build --output-on-failure --timeout 600 --parallel 1 -R "$HEAVY_TESTS" --output-junit ctest-results-heavy.xml
```

CI supplies every dependency a test can ask for, so nothing may skip:

| Variable | What CI gives it | Tests |
|---|---|---|
| `LAKE_TEST_DB_URI` | the Postgres 16 service | the Postgres stores, scenarios and sync |
| `LAKE_TEST_ZETTA_BINARY` | the cached Zetta server | `WorkspaceStoreZetta.*`, which start their own node |
| `LAKE_TEST_ZETTA_URL`, `LAKE_TEST_ZETTA_TOKEN` | a node the job starts on `127.0.0.1:48000` | `RequestRecorderZetta.*`, `WorkspaceRecordsZetta.*` |
| `LAKE_TEST_LIQUID` | the liquid CLI built at `LIQUID_REF`, cached per pin | `InstanceOrigin.BrowserToTerminal` |
| `LAKE_TEST_BROWSER`, `LAKE_TEST_PUPPETEER` | the runner's Chrome and `puppeteer-core` 24 | `InstanceOrigin.BrowserToTerminal` |

That includes the **real-Zetta tests** (`WorkspaceStoreZetta.*`, which start the Zetta binary) and
the **UI token gates** (`lake-ui-token-gates`, `tools/ui/test-token-gates.sh`). A last step writes
the totals (passed, failed, skipped, real-Zetta tests run, token gates) to the run summary and fails
the job if any test skipped or the token gates did not pass: without their dependency those tests
skip rather than fail, so a green run must prove they ran.

A hung test fails by name after five minutes (`--timeout 300`), far inside the job's 300-minute
limit, so the job reports it instead of being killed. Tests run two at a time: the runner has two
CPUs and 7 GiB of memory. `HEAVY_TESTS` (`RegistryStore.ALargeLayerIsStreamedNeverHeldWhole`, a
304 MiB layer, and `ModelSync.MultiGigabyteRevisionsDeduplicateAndResumePull`) run one at a time
after the rest, with a ten-minute timeout.

The first two Linux runs (2026-10-08) lost the whole job about six minutes into the tests, both
times while `RegistryStore.ALargeLayerIsStreamedNeverHeldWhole` was running: the Test step got
`SIGTERM` (exit 143) and no later step ran, which is how a runner ends when its agent dies, not how
a failing or timed-out test ends. To leave a trace and to keep the runner alive:

- a first step deletes preinstalled toolchains CI does not use (.NET, Android, GHC, CodeQL, Swift),
  and a step after the build deletes both compiler caches (already saved) and Zetta's submodules,
  so the tests start with far more than the runner's original 12 GiB free;
- a sampler prints memory, swap, free disk and the three largest processes into the Test step's
  log every ten seconds, so the log shows what ran out even if the job dies;
- the tests run with `oom_score_adj` 1000, so an out-of-memory kill takes a test, which ctest
  reports by name, rather than the runner's agent.

The summary step runs even when tests fail; if ctest did not finish it says so and fails. A
`Diagnostics` step prints memory, disk and any out-of-memory or no-space kernel message, and the
`ctest-log` artifact keeps `LastTest*.log` and the JUnit files for 14 days. A newer push to a pull request cancels its older run; runs on `main` always
finish.

### The Zetta binary

The real-Zetta tests need a `zetta` built from Zetta dev `5f70b30` or later. CI builds it from
`ZETTA_REF` with Zetta's own image recipe (clang-19, `ZETTA_BUILD_TESTS=OFF`, target `zetta`,
stripped) and keeps it in the Actions cache under
`Linux-zetta-bin-<ZETTA_REF>-clang19-v1`. A cache hit skips Zetta's dependencies, submodules and
build entirely, so the build runs once per Zetta pin; Zetta's ccache is kept as well, so a pin bump
recompiles only what changed. No registry credential is involved: pulling the private
`oramasearch/zetta:dev-*` image instead would need a Docker Hub read token as a secret.

### Caching

| Cache | Key | Saves |
|---|---|---|
| Zetta binary | `ZETTA_REF` | the whole Zetta build |
| liquid CLI | `LIQUID_REF` and the OpenSSL version | building the CLI |
| Zetta ccache | `ZETTA_REF`, falling back to the newest | recompiling Zetta after a pin bump |
| OpenSSL `/opt/openssl` | version and checksum | the OpenSSL source build |
| `build/_deps` | hash of `CMakeLists.txt` | the Boost and GoogleTest downloads |
| lake ccache | branch and run, falling back to the branch's newest, then `main`'s | recompiling unchanged lake sources |

Each cache is restored and saved by separate `actions/cache/restore` and `actions/cache/save`
steps, saved as soon as what it holds is built: `actions/cache` alone saves only when the whole job
succeeds, so a run with a failing test would rebuild Zetta every time. ccache runs without
`CCACHE_BASEDIR`, which would rewrite `__FILE__` to a relative path and break the tests that find
their fixtures next to their source file (`GitHubImport.*`, the drive scenarios).

A cache written by a pull request is visible only to that pull request; one written on `main` is
visible to every pull request, so the first run after a pin bump lands on `main` warms it for all.

### History: disabled from 2026-09-26 to 2026-10-08

Steve disabled the workflow on 2026-09-26 (`gh workflow disable ci`), so no pull request since then
ran on Linux and lake#545 merged a change that broke every connection there (lake#546). The first
Linux runs had built lake and run 287 tests; three
(`SharedLinkScenario.ExpiryAfterListingStopsPreviewAndDownload`,
`ModelSync.MultiGigabyteRevisionsDeduplicateAndResumePull`,
`PostgresForgeScenario.DripsReviewRebaseAndGraphEvidenceLandEndToEnd`) hung or failed on a lost
read on Linux's epoll backend (lake#176). The deploy keys and secrets were never the cause.

The native toolchain also installs `python3-jsonschema` for the HTTP response conformance test.
CI selects `/usr/bin/python3`, which owns that distribution package. CMake checks the import
before configuring test targets.

The workflow installs pinned `huggingface_hub` 1.30.0 in a Python virtual environment before the
test step. The model/dataset client integration therefore runs in CI; a local environment without
the package reports an explicit skip while the direct C++ HTTP coverage still runs.

`lake-postgres-tests` reads `LAKE_TEST_DB_URI` and uses unique fixture keys to round-trip every
registered data capability through the production Postgres store; the audit and repository-sync
integration tests in `lake-tests` read the same variable and skip without it. The sync integration
starts lake and uses libliquid's real client against the production control store. The rest of the
test binary uses `InMemoryStore`, including policy inheritance, delegation, registry metadata and
renderer coverage.

For the same proof locally:

```sh
docker run --name lake-postgres -e POSTGRES_PASSWORD=postgres -e POSTGRES_DB=lake_test \
  -p 55416:5432 -d postgres:16
PGPASSWORD=postgres psql -h 127.0.0.1 -p 55416 -U postgres -d lake_test -f db/schema.sql
LAKE_INSTANCE_KEK=00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff \
  LAKE_TEST_DB_URI='postgresql://postgres:postgres@127.0.0.1:55416/lake_test' \
  ctest --test-dir build --output-on-failure
docker rm -f lake-postgres
```

The workflow caches `build/_deps`, which contains the CMake FetchContent downloads for Boost and
GoogleTest. libliquid supplies nlohmann/json, Capy, Corosio, Boost.HTTP, OramaCore, and its own
transitive dependencies; lake's adapter and libliquid's sync client share that one Boost.HTTP
target. Its key includes the hash of `CMakeLists.txt`; a missing cache is valid and CMake downloads
the dependencies again.

liquid is not a submodule of lake: it is a sibling module of liquid-bridge, which holds the one
pin. CI fetches it at `LIQUID_REF` with its submodules; locally, point `LAKE_LIQUID_SOURCE_DIR` at
liquid-bridge's `modules/liquid`. Its `LIQUID_BUILD_CLI` and `LIQUID_BUILD_TESTS` options stay off
for lake.

Zetta remains an external source dependency. `LAKE_ZETTA_SOURCE_DIR` must name the one authorized
`zetta-bridge/modules/zetta` checkout. Lake builds a client-only `zetta-api` from that directory,
including the runtime event implementation used by transport completion waits, and links it to
Lake's existing Boost.HTTP, Beast2, Capy, and Corosio targets, so the binary contains one networking
stack. CI fetches the pinned `oramasearch/zetta` commit (the one `zetta-bridge` pins); it compiles
the client from it and, on a cache miss, builds the Zetta server from the same commit with its
submodules. The image workflow supplies the same directory as the `zetta_source` named build
context. Bump `ZETTA_REF` in `ci.yml` and the ref in `image.yml` when the bridge bumps Zetta, and
`LIQUID_REF` and `image.yml`'s liquid ref when liquid-bridge bumps liquid. Both workflows pin Zetta
`5f70b30` and liquid `8955519` (the bridge pins on 2026-10-08); lake does not compile against a
liquid older than `df2ae4d` (liquid#322, the store owner lock and `owner_wait` that lake#479 uses).

Lake's plain environment-value check compiles libliquid's existing private detector source into
`lake-core`. The detector has no exported shared-library API, so this keeps static and shared builds
on the exact inline scanner implementation without adding a different scanner implementation.
Shared test builds expose libliquid's internal conformance helpers because existing Lake tests call
them directly; production builds retain libliquid's hidden C++ visibility.

Postgres-backed identity coverage uses `LAKE_TEST_DB_URI`. To run it locally, start the documented
test database and apply the schema before the test command:

```sh
docker run --rm -d --name lake-test-postgres -e POSTGRES_PASSWORD=postgres \
  -e POSTGRES_DB=lake_test -p 5432:5432 postgres:16
until docker exec lake-test-postgres pg_isready -U postgres -d lake_test; do sleep 1; done
docker exec -i lake-test-postgres psql -U postgres -d lake_test < db/schema.sql
LAKE_INSTANCE_KEK=00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff \
  LAKE_TEST_DB_URI=postgresql://postgres:postgres@127.0.0.1:5432/lake_test \
  ctest --test-dir build --output-on-failure -R PostgresIdentity
docker stop lake-test-postgres
```

## Tagged image workflow

[`image.yml`](../../.github/workflows/image.yml) runs when any Git tag is pushed. It builds only
`linux/amd64` and pushes these Docker Hub tags:

```text
oramasearch/lake:<git-tag>
oramasearch/lake:sha-<short-sha>
```

`latest` is rejected. Git tags used for images must also be valid Docker tag strings. Deployments
reference the Git-tag image; the SHA tag provides a second immutable source identifier.

The workflow passes the complete checked-out commit and a UTC creation timestamp to the Dockerfile.
The resulting OCI image labels are `org.opencontainers.image.revision` and
`org.opencontainers.image.created`. Lake embeds the same revision in `server_info`; a deployment
record can therefore compare its source revision with one public API response. Images published
before this change lack those fields and need a clearly marked manually maintained source record.

The Linux/amd64 image is verified by this native CI worker or the AKS builder. Apple Silicon developer machines do not need to emulate the
amd64 image recipe; that emulation is not a release proof.

The image link also verifies that the provider-bound audit implementation is compiled exactly once
through the control-store translation unit.

The workflow can also be dispatched manually with the required `tag` input. Use that path only to
retry an unpublished tag, selecting the matching Git tag as the workflow ref:

```sh
gh workflow run image.yml --repo oramasearch/lake --ref <git-tag> -f tag=<git-tag>
```

Do not dispatch an already-published tag. Create and push a new Git tag for a new image.

## Credentials

**Private repositories: one read-only deploy key each** (set up 2026-09-25). CI reads six private
repositories: lake's submodules `oramasearch/corosio`, `oramasearch/beast2` and
`oramasearch/oramacore-cpp`, liquid's `oramasearch/automerge-cpp`, and `oramasearch/liquid` and
`oramasearch/zetta` themselves. Each has a deploy key titled *lake CI (read-only)*; its private
half is the lake Actions secret `DEPLOY_KEY_<REPO>` (`DEPLOY_KEY_COROSIO`, `DEPLOY_KEY_BEAST2`,
`DEPLOY_KEY_ORAMACORE_CPP`, `DEPLOY_KEY_AUTOMERGE_CPP`, `DEPLOY_KEY_LIQUID`, `DEPLOY_KEY_ZETTA`).
`webfactory/ssh-agent` (MIT, pinned by commit) loads them, maps each to its repository by the
key's comment `git@github.com:oramasearch/<repo>.git`, and rewrites both `git@` and `https://`
URLs of that repository onto it. A deploy key reads one repository and nothing else, which is why
it is used instead of a personal token with organization-wide reach.

To add a private dependency: `ssh-keygen -t ed25519 -N "" -C "git@github.com:oramasearch/<repo>.git"
-f <repo>` in a scratch directory, `gh repo deploy-key add <repo>.pub --repo oramasearch/<repo>
--title "lake CI (read-only)"`, `gh secret set DEPLOY_KEY_<REPO> --repo oramasearch/lake < <repo>`,
add the secret to the `ssh-private-key` list in both workflows, and delete the local key files. To
rotate one, delete its deploy key and repeat.

**Docker Hub** (image workflow only):

| Secret | Scope and use |
|---|---|
| `DOCKERHUB_USERNAME` | Repository Actions secret containing the Docker Hub account allowed to push `oramasearch/lake`. |
| `DOCKERHUB_TOKEN` | Repository Actions secret containing that account's Docker Hub access token with push permission. |

Until the two Docker Hub secrets exist, a tag build cannot log in or publish; images are built on
the cluster's BuildKit instead (`.claude/skills/liquid-dev` in liquid-bridge).

## Verify and change

Validate workflow syntax locally with `actionlint .github/workflows/*.yml`. After merging, a push
or pull request must show the `ci` check passing. Push a new Git tag and inspect the `image`
workflow before changing a `Lake` resource to that tag.

When the Dockerfile toolchain changes, make the corresponding package, LLVM and CMake changes in
`ci.yml` in the same change. Keep the image workflow restricted to `linux/amd64` and immutable
tags.
