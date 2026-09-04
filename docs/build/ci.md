# Continuous integration and image publishing

GitHub Actions has two workflows that keep the repository buildable and publish deployment
images. The Dockerfile remains the canonical Linux toolchain recipe; CI installs the same Ubuntu
24.04 packages, LLVM 19 channel and CMake 3.31.5 installer.

## Build and test workflow

[`ci.yml`](../../.github/workflows/ci.yml) runs for every push and pull request. It checks out all
submodules recursively, starts a Postgres 16 service, configures Release with
`LAKE_BUILD_TESTS=ON`, builds with clang-19, applies `db/schema.sql`, then runs:

```sh
ctest --test-dir build --output-on-failure
```

`lake-postgres-tests` reads `LAKE_TEST_DB_URI` and uses unique fixture keys to round-trip every
registered data capability through the production Postgres store; the audit integration test in
`lake-tests` reads the same variable and skips without it. The rest of the test binary
uses `InMemoryStore`, including policy inheritance, delegation, registry metadata and renderer
coverage.

For the same proof locally:

```sh
docker run --name lake-postgres -e POSTGRES_PASSWORD=postgres -e POSTGRES_DB=lake_test \
  -p 55416:5432 -d postgres:16
PGPASSWORD=postgres psql -h 127.0.0.1 -p 55416 -U postgres -d lake_test -f db/schema.sql
LAKE_TEST_DB_URI='postgresql://postgres:postgres@127.0.0.1:55416/lake_test' \
  ctest --test-dir build --output-on-failure
docker rm -f lake-postgres
```

The workflow caches `build/_deps`, which contains the CMake FetchContent downloads for Boost and
GoogleTest. libliquid supplies nlohmann/json, Capy, Corosio, Boost.HTTP, and its own transitive
dependencies; lake's adapter and libliquid's sync client share that one Boost.HTTP target. Its key
includes the hash of `CMakeLists.txt`; a missing cache is valid and CMake downloads the dependencies
again.

Postgres-backed identity coverage uses `LAKE_TEST_DB_URI`. To run it locally, start the documented
test database and apply the schema before the test command:

```sh
docker run --rm -d --name lake-test-postgres -e POSTGRES_PASSWORD=postgres \
  -e POSTGRES_DB=lake_test -p 5432:5432 postgres:16
until docker exec lake-test-postgres pg_isready -U postgres -d lake_test; do sleep 1; done
docker exec -i lake-test-postgres psql -U postgres -d lake_test < db/schema.sql
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

The workflow can also be dispatched manually with the required `tag` input. Use that path only to
retry an unpublished tag, selecting the matching Git tag as the workflow ref:

```sh
gh workflow run image.yml --repo oramasearch/lake --ref <git-tag> -f tag=<git-tag>
```

Do not dispatch an already-published tag. Create and push a new Git tag for a new image.

## Required credentials

Steve must create these Actions secrets before the workflows can complete their external work:

| Secret | Scope and use |
|---|---|
| `ORAMA_SUBMODULES_TOKEN` | Organization-level Actions secret available to `oramasearch/lake`; a fine-grained token with read access to the organization's private repositories, passed to recursive `actions/checkout`. |
| `DOCKERHUB_USERNAME` | Repository Actions secret containing the Docker Hub account allowed to push `oramasearch/lake`. |
| `DOCKERHUB_TOKEN` | Repository Actions secret containing that account's Docker Hub access token with push permission. |

Until `ORAMA_SUBMODULES_TOKEN` exists, CI stops during checkout. Until the two Docker Hub secrets
exist, a tag build cannot log in or publish. Neither secret is created by this repository or its
workflows.

## Verify and change

Validate workflow syntax locally with `actionlint .github/workflows/*.yml`. After merging, a push
or pull request must show the `ci` check passing. Push a new Git tag and inspect the `image`
workflow before changing a `Lake` resource to that tag.

When the Dockerfile toolchain changes, make the corresponding package, LLVM and CMake changes in
`ci.yml` in the same change. Keep the image workflow restricted to `linux/amd64` and immutable
tags.
