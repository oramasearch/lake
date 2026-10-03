# Continuous integration and image publishing

GitHub Actions has two workflows that keep the repository buildable and publish deployment
images. The Dockerfile remains the canonical Linux toolchain recipe; CI installs the same Ubuntu
24.04 packages, LLVM 19 channel, checksum-verified OpenSSL 3.6.3 source build, and CMake 3.31.5
installer.

## Build and test workflow

> **Disabled since 2026-09-26** (Steve), with `gh workflow disable ci --repo oramasearch/lake`. The
> checkout, toolchain and timeout fixes below work: the first Linux runs built lake and ran all
> 287 tests. 284 pass. The other three (`SharedLinkScenario.ExpiryAfterListingStopsPreviewAndDownload`,
> `ModelSync.MultiGigabyteRevisionsDeduplicateAndResumePull`,
> `PostgresForgeScenario.DripsReviewRebaseAndGraphEvidenceLandEndToEnd`) hang or fail on a lost read
> in the HTTP client on Linux's epoll backend. The server answers 200 and the client never
> receives the response. That needs liquid#317 (block_on waits for its coroutine) and very
> likely an update of the `oramasearch/corosio` fork, 221 commits behind upstream `develop`.
> Re-enable with `gh workflow enable ci --repo oramasearch/lake` once they pass.

[`ci.yml`](../../.github/workflows/ci.yml) runs for every push and pull request. It loads one read-only
deploy key per private repository (below), checks out all submodules recursively, fetches the
pinned `oramasearch/zetta` and `oramasearch/liquid` commits, starts a Postgres 16 service,
configures Release with `LAKE_BUILD_TESTS=ON` and `LAKE_ZETTA_SOURCE_DIR=_external/zetta`, builds with clang-19, applies `db/schema.sql`, then
runs:

```sh
ctest --test-dir build --output-on-failure
```

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

`modules/liquid` is a library-only source submodule pinned to liquid `origin/main`. Initialise it
with the rest of the module tree (`git submodule update --init --recursive`) before configuring a
consumer build. Its `LIQUID_BUILD_CLI` and `LIQUID_BUILD_TESTS` options stay off for lake.

Zetta remains an external source dependency. `LAKE_ZETTA_SOURCE_DIR` must name the one authorized
`zetta-bridge/modules/zetta` checkout. Lake builds a client-only `zetta-api` from that directory,
including the runtime event implementation used by transport completion waits, and links it to
Lake's existing Boost.HTTP, Beast2, Capy, and Corosio targets, so the binary contains one networking
stack. CI fetches only the pinned `oramasearch/zetta` commit (the one `zetta-bridge` pins), without
its submodules, because only its client headers, transport and runtime implementation are compiled;
the image workflow supplies the same directory as the `zetta_source` named build context. Bump the
ref in both workflows when the bridge bumps Zetta, and the liquid ref when liquid-bridge bumps
liquid. Both workflows pin liquid `df2ae4d` (liquid main after liquid#322, the store owner lock
and `owner_wait` that lake#479 uses); lake does not compile against an older liquid.

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
