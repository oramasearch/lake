# Contributing to Lake

Thank you for helping. This page says how to build Lake, run its tests, send a change, keep the
server's shape, and cut a release.

## Build and test

Lake is C++26 (CMake 3.28 or newer, Ninja, Clang 17 or newer) with a React web app under
`frontend/`. [`docs/build/local.md`](docs/build/local.md) lists the requirements, the native build,
the test suite with and without Postgres, and how to run the binary.
[`docs/build/testing.md`](docs/build/testing.md) covers the test runner's options, and
[`docs/build/layout.md`](docs/build/layout.md) the source tree.

```sh
git clone --recurse-submodules <this repository>
cd lake
cmake -S . -B build -G Ninja -DLAKE_ZETTA_SOURCE_DIR=/path/to/zetta
cmake --build build
ctest --test-dir build --output-on-failure
```

**One networking checkout.** Lake compiles Zetta's client transport from one Zetta checkout, the
one `LAKE_ZETTA_SOURCE_DIR` names, and links it to its own Boost.HTTP, Beast2, Capy and Corosio
targets, so the binary carries one networking stack. Use the Zetta commit this release pins, never
an unrelated clone; a second copy of the networking stack is not allowed.

PostgreSQL tests use `lake::test::postgres_uri()` for a fresh database per test; the database
`LAKE_TEST_DB_URI` names belongs to the caller and is not modified.

## Sending a change

- Open an issue first for anything larger than a small fix, so the approach can be agreed.
- Branch from `dev` and open the pull request against `dev`. `main` holds the last release.
- Write commit messages as [Conventional Commits](https://www.conventionalcommits.org/)
  (`fix(api): ...`, `feat(ui): ...`, `docs: ...`).
- Every behaviour change comes with a test that fails without it, and the documentation for the
  folder it touches (each folder's `README.md`, and the pages under `docs/`) in the same change.
  `README.md` is the front door only; status goes in `docs/design/status.md`.
- A schema change takes the next free migration number ([`db/README.md`](db/README.md)); two
  migrations never share a number. Migrations never run in the lake server process.
- Every environment variable lake reads is declared in `environment_variables()`
  (`src/api/manifest.cpp`), and every Stripe event it handles in `stripe_webhook_events`.
- Format only the lines you change (`git clang-format`, from `.clang-format`).
- **Before you finish:** `cmake --build build && ctest --test-dir build` is green, and the binary is
  **smoke-tested by hand**: start it (`./build/lake --memory-store --listen 127.0.0.1:4900`) and
  exercise what you changed through the API or the web app.
- If a test or binary crashes (segmentation fault, bus error, abort), rebuild with
  AddressSanitizer and run the suite under it before calling it a flake.

## How the server is built

The rules that keep the server small and the same shape as Zetta's:

- **Which translation units may include what.** `include/lake/api/server.hpp` exposes `ApiServer`
  and `ServerOptions` and **no HTTP type**: no Beast2, `boost::http` or Boost.JSON name may appear
  in a public header. Only two translation units name third parties: `src/api/http_server.cpp`
  (Beast2, `boost::http`, Boost.JSON) and `src/control/postgres_store.cpp` (libpqxx).
  nlohmann::json is the API document type.
- **One adapter TU.** `src/api/http_server.cpp` is the only HTTP adapter. Every response goes
  through `send_text`. Routes are registered in `ApiServer::start()`, ending with a **trailing 404
  handler**, which stays last (without it an unmatched request hangs).
- **One registry.** Every capability is registered from `src/api/capabilities.cpp` or its
  area-specific registration file; REST, OpenAPI and MCP are renderings of it and never
  special-case an entry. `Registry::call` is the only path that authorizes and validates
  ([`docs/api/README.md`](docs/api/README.md)).
- **One auth check.** The API bearer and the web app's session cookie carry the same secret and are
  resolved by the same code (`context_for`); never add a second path.
- **The web app calls only `/v1` capabilities**, so it can never do what the API cannot; do not add
  a server page or an HTML form handler for it ([`docs/ui/web-app.md`](docs/ui/web-app.md)).
- **Pure renderers.** The few server-rendered pages (hosted sign-in pages, the region picker and
  the order sheet) are pure functions in `src/api/web_ui.cpp`: input in, HTML out, forms and no
  client JavaScript beyond what a page strictly needs.
- `std::expected` at every public boundary; no exceptions across public APIs.

## Cutting a release

A release is a tag on `main`. The pages and their conventions are in
[`docs/releases/`](docs/releases/README.md).

1. On a release branch, move the version in all three places: `version_string()` in
   `src/version.cpp`, `current_version()` in `include/lake/version.hpp` and
   `project(lake VERSION ...)` in `CMakeLists.txt`. Tests read the version from
   `lake::version_string()`; never write the number into a test. `ReleaseVersion.*`
   (`tests/test_release_version.cpp`) fails when the three disagree or when the version has no page
   in `docs/releases/` linked from its `README.md`.
2. Write `docs/releases/vX.Y.Z.md` and link it from `docs/releases/README.md`: what the release
   includes, what it does not, known issues (a security defect first), and how it was verified.
3. Prove it: build, then the full suite with `LAKE_TEST_DB_URI` set, and put the counts on the page.
   A failure is fixed or listed as a known issue, never left out.
4. Merge the release pull request into `main` with a merge commit, tag the merge commit `vX.Y.Z`,
   and publish the GitHub release from the page.
5. Build the image from the tag and give it the same tag.

## Security

Do not report a vulnerability in a public issue. See `SECURITY.md` for how to report one.

## Licence

By contributing you agree that your contribution is licensed under the repository's licence.
The copyright holder is named in [`COPYRIGHT`](COPYRIGHT).
