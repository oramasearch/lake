# Contributing to Lake

Thank you for helping. This page says how to build Lake, run its tests and send a change.

## Build and test

Lake is C++ (CMake, Ninja, Clang 17 or newer) with a React web app under `frontend/`.
[`docs/build/local.md`](docs/build/local.md) lists the requirements, the native build, the test
suite with and without Postgres, and how to run the binary.
[`docs/build/testing.md`](docs/build/testing.md) covers the test runner's options, and
[`docs/build/layout.md`](docs/build/layout.md) the source tree.

```sh
git clone --recurse-submodules <this repository>
cd lake
cmake -S . -B build -G Ninja -DLAKE_ZETTA_SOURCE_DIR=/path/to/zetta
cmake --build build
ctest --test-dir build --output-on-failure
```

## Sending a change

- Open an issue first for anything larger than a small fix, so the approach can be agreed.
- Branch from `dev` and open the pull request against `dev`. `main` holds the last release.
- Write commit messages as [Conventional Commits](https://www.conventionalcommits.org/)
  (`fix(api): ...`, `feat(ui): ...`, `docs: ...`).
- Every behaviour change comes with a test that fails without it, and the documentation for the
  folder it touches (each folder's `README.md`, and the pages under `docs/`) in the same change.
- A schema change takes the next free migration number ([`db/README.md`](db/README.md)); two
  migrations never share a number.
- If a test or binary crashes (segmentation fault, bus error, abort), rebuild with
  AddressSanitizer and run the suite under it before calling it a flake.

## Security

Do not report a vulnerability in a public issue. See `SECURITY.md` for how to report one.

## Licence

By contributing you agree that your contribution is licensed under the repository's licence.
The copyright holder is named in [`COPYRIGHT`](COPYRIGHT).
