# Contributing to lake

Thank you for helping. This document says how to build and test lake, how a change gets in,
and the one thing every commit needs: a sign-off.

## Before you start

- For a bug, open an issue with what you did, what you expected and what happened, and the
  version or commit.
- For a larger change (a new feature, a new dependency, a change to an interface), open an issue
  first so the approach can be agreed before you write it.
- **Security issues are never reported in a public issue.** See [SECURITY.md](SECURITY.md): write
  to **security@orama.com**.

## Build and test

You need CMake 3.28 or newer, Ninja, Clang 17 or newer (Homebrew LLVM on macOS, `clang++-NN` on
Linux), OpenSSL 3, libpq and Node. On macOS: `brew install cmake ninja llvm openssl@3 libpq node`.

lake builds against two sibling checkouts that are not submodules: [zetta](https://github.com/oramasearch/zetta)
(required, `LAKE_ZETTA_SOURCE_DIR`) and [liquid](https://github.com/oramasearch/liquid)
(`LAKE_LIQUID_SOURCE_DIR`, default `../liquid`). Boost and GoogleTest are fetched by CMake on the
first configure.

```sh
git clone --recurse-submodules https://github.com/oramasearch/lake.git
cd lake
cmake -S . -B build -G Ninja \
  -DLAKE_ZETTA_SOURCE_DIR=/path/to/zetta \
  -DLAKE_LIQUID_SOURCE_DIR=/path/to/liquid
cmake --build build
ctest --test-dir build --output-on-failure
```

In an existing checkout, run `git submodule update --init --recursive` first. A Debug build is
the same configure with `-DCMAKE_BUILD_TYPE=Debug` in its own build directory; run the suite
against it when a change touches coroutines or threads.

The web app is in `frontend/`:

```sh
cd frontend
npm ci
npm run typecheck
npm test
npm run build
```

More detail, including the Postgres-backed tests, is in [docs/build/local.md](docs/build/local.md).
A crash in a test (segmentation fault, bus error, abort) is investigated with an AddressSanitizer
build, not rerun until it passes.

## Making a change

1. Fork the repository and branch from `dev`. `main` is the last release; it moves only when a
   release is cut.
2. Keep one change per pull request, with the tests that prove it. A change in behavior updates
   the documentation it touches in the same pull request.
3. Write commit messages in the [Conventional Commits](https://www.conventionalcommits.org/) form,
   for example `fix(api): reject an empty name`.
4. Sign off every commit (next section).
5. Open the pull request against `dev`. Start its description with what changes, why, and how.

A maintainer reviews it. Pull requests are merged with a merge commit, so every commit you sign
stays in the history.

## Sign your commits (Developer Certificate of Origin)

lake takes contributions under the [Developer Certificate of Origin](https://developercertificate.org/)
(DCO). There is no contributor license agreement to sign. Your contribution is licensed under the
project's license, the MIT License (see [LICENSE](LICENSE)).

By adding a `Signed-off-by` line to a commit, you certify the DCO below for that commit. Git adds
the line for you with `-s`:

```sh
git commit -s -m "fix(api): reject an empty name"
```

which records:

```
Signed-off-by: Your Name <you@example.com>
```

The name and address are the ones in your `git config user.name` and `user.email`. Use your real
name; anonymous contributions cannot be accepted. To sign commits you have already made:

```sh
git commit --amend -s --no-edit        # the last commit
git rebase --signoff dev               # every commit on your branch
```

A pull request with an unsigned commit is not merged until it is signed.

```
Developer Certificate of Origin
Version 1.1

Copyright (C) 2004, 2006 The Linux Foundation and its contributors.

Everyone is permitted to copy and distribute verbatim copies of this
license document, but changing it is not allowed.


Developer's Certificate of Origin 1.1

By making a contribution to this project, I certify that:

(a) The contribution was created in whole or in part by me and I
    have the right to submit it under the open source license
    indicated in the file; or

(b) The contribution is based upon previous work that, to the best
    of my knowledge, is covered under an appropriate open source
    license and I have the right under that license to submit that
    work with modifications, whether created in whole or in part
    by me, under the same open source license (unless I am
    permitted to submit under a different license), as indicated
    in the file; or

(c) The contribution was provided directly to me by some other
    person who certified (a), (b) or (c) and I have not modified
    it.

(d) I understand and agree that this project and the contribution
    are public and that a record of the contribution (including all
    personal information I submit with it, including my sign-off) is
    maintained indefinitely and may be redistributed consistent with
    this project or the open source license(s) involved.
```

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

## License headers

Every source file starts with the line `SPDX-License-Identifier: MIT`, in that file's comment
syntax, for example `// SPDX-License-Identifier: MIT`. Add it to a new file yourself, or run:

```sh
python3 tools/spdx-headers.py           # adds the line where it is missing
python3 tools/spdx-headers.py --check   # lists files without it
```

Code copied from another project keeps its own license header and is listed in
[THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES). Say so in the pull request, with where it came from
and its license; it must be compatible with the MIT License.

## Code of conduct

Everyone taking part follows the [Code of Conduct](CODE_OF_CONDUCT.md). Report unacceptable
behavior to **conduct@orama.com**.
