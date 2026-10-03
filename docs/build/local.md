# Local build, test and run

## Requirements

- CMake 3.28 or newer and Ninja.
- Clang 17 or newer. On macOS `CMakeLists.txt` selects Homebrew LLVM
  (`/opt/homebrew/opt/llvm/bin/clang++`) and the Xcode SDK before `project()`; on Linux it takes
  the newest `clang++-NN` on the path. Pass `-DLAKE_USE_SYSTEM_COMPILER=ON` or set
  `CMAKE_CXX_COMPILER` to choose another compiler.
- OpenSSL 3 and libpq from Homebrew (`brew install openssl@3 libpq`). On macOS the build adds
  `/opt/homebrew/opt/openssl@3` and `/opt/homebrew/opt/libpq` to `CMAKE_PREFIX_PATH`.
- Node, because the test suite (`LAKE_BUILD_TESTS`, on by default) runs the UI token-gate and
  contrast checks in `tools/ui/`.
- A checkout of `zetta-bridge`. `LAKE_ZETTA_SOURCE_DIR` is required and must name its
  `modules/zetta` directory; lake compiles the client transport and its runtime event
  implementation from it and links them to its own Boost.HTTP, Beast2, Capy and Corosio targets,
  so the binary carries one networking stack. `CLAUDE.md` names the one authorized checkout.
  The target also compiles that checkout's `src/core/runtime.cpp`, which supplies the transport's
  synchronized asynchronous event definitions; it does not link the engine or change the pin.
- Submodules. Clone with `--recurse-submodules` or run `git submodule update --init --recursive`
  before configuring: libliquid (library-only, pinned to liquid `main`), Boost.HTTP, Beast2 and
  libpqxx are vendored under `modules/`. Boost and GoogleTest arrive through FetchContent on the
  first configure. [`dependencies.md`](dependencies.md) records every direct pin and the private
  patch-carrier update procedure.

## Build

```sh
git clone --recurse-submodules git@github.com:oramasearch/lake.git
cd lake
cmake -S . -B build -G Ninja \
  -DLAKE_ZETTA_SOURCE_DIR=/path/to/zetta-bridge/modules/zetta
cmake --build build -j 6
```

The configure step is Release by default and exports `compile_commands.json`. GitHub Actions
repeats this build with the Dockerfile's Ubuntu 24.04, clang-19 and CMake 3.31.5 toolchain on
every push and pull request; see [`ci.md`](ci.md).

Lake's own targets and its test binaries compile with `-Werror=return-type`. A coroutine that
returns `liquid::runtime::task<T>` carries a `result<T>`, so its promise has `return_value` and no
`return_void`: one that flows off its end leaves the result unset, and the coroutine awaiting it
reads and destroys uninitialized memory. That is how `report_on_asking_turn` aborted
`AgentRuns.AgentTurnReachesLibraryNativeAndPythonAgents` in about half of its runs (lake#476). End
every such coroutine with an explicit `co_return`.

### Debug builds and thread stacks

A Debug build is the same configure with `-DCMAKE_BUILD_TYPE=Debug` (use a separate build
directory, such as `build-debug`). Run the full suite against it when a change touches coroutines,
threads or anything liquid runs; the unoptimised code shows problems that Release hides.

Every thread lake starts — the HTTP server's `io_context` thread, the store-root owner thread
(which opens and warms liquid stores), the outbound HTTP client's thread, the agent-bus loop in `main.cpp`, and the writer threads in `test_shared_store.cpp` — is a
`lake::Thread` (`include/lake/thread.hpp`) with an explicit 8 MiB stack, not a `std::thread`.
`std::thread` takes the platform default for a secondary thread, which is 8 MiB on glibc Linux but
512 KiB on macOS. Those threads run request handling and, through `liquid::runtime::block_on`, a
whole nested liquid runtime, so their demand is lake's and liquid's to set (lake#478).

In a Debug build, liquid's `chunk_index::compact` reserves 516 KiB of stack in one frame: its two
65,537-entry `std::uint32_t` fanout arrays live in the heap coroutine frame, but at `-O0` the
coroutine's ramp function also reserves stack for them. A push or drive write whose index flush
compacted therefore hit the macOS guard page at `compact`'s entry, as `EXC_BAD_ACCESS (code=2)`,
which the shell reports as `Bus error`. Release keeps the frames on the heap (the ramp is 64
bytes), and Linux threads had 8 MiB, so the Release image was never exposed.

The size comes from measurement. Each thread's high-water mark was read from its untouched stack
pages after the thread finished, over the full suite with 64 MiB stacks (2026-10-02, macOS arm64,
Homebrew Clang):

| Build | Threads measured | Peak stack |
|---|---|---|
| Debug | 378 | 546,064 bytes (533 KiB) |
| Release | 387 | 68,560 bytes (67 KiB) |

8 MiB is fifteen times the Debug peak and equals the glibc default the Linux image already ran
with, so the deployed behaviour does not change. Raise it only from a new measurement; a frame
that needs more belongs on the heap.

## Test

```sh
ctest --test-dir build --output-on-failure
```

CTest registers four entries: `lake-tests` (the portable suite plus Postgres-backed cases that
skip when `LAKE_TEST_DB_URI` is absent), `lake-postgres-tests` (the focused control-store cases),
`lake-ui-token-gates` and `lake-ui-contrast`. `tests/README.md` says what each covers;
`ctest --test-dir build -N` lists the discovered cases.

Postgres-backed tests connect only through `LAKE_TEST_DB_URI` and need a `LAKE_INSTANCE_KEK`.
The configured role needs `CREATEDB`: the test runner creates a private database per case,
loads `db/schema.sql`, and drops only that database. Create a base connection database against
local Postgres and run the suite with both variables set:

```sh
createdb lake_test
psql -d lake_test -f db/schema.sql
LAKE_INSTANCE_KEK=$(openssl rand -hex 32) \
LAKE_TEST_DB_URI="postgresql://$USER@127.0.0.1:5432/lake_test" \
  ctest --test-dir build --output-on-failure
```

Tests use the shared per-test database fixture and independent KEK values; see
[`testing.md`](testing.md). The Docker-based equivalent that CI uses, and the `huggingface_hub`
virtual environment for the model-client integration, are in [`ci.md`](ci.md).

## Run

```sh
./build/lake version
./build/lake --listen 127.0.0.1:4900 --node dev --memory-store --admin-token t
curl http://127.0.0.1:4900/v1/server_info
```

`--memory-store` is the in-memory control plane for demos and tests. Against Postgres, pass
`--control-db postgresql://...` (or set `LAKE_CONTROL_DB_URI`) to a database with `db/schema.sql`
applied, together with `--instance-kek`; a store-backed instance also wants `--store-root` for
repository data. Every flag and environment variable, with its default and the page that owns it,
is in [`../serving/configuration.md`](../serving/configuration.md). SIGINT and SIGTERM stop the
server cleanly; the routes it serves are in [`../serving/http.md`](../serving/http.md).

## Verify and change

A change to the toolchain or to a required dependency updates this page, the `Dockerfile`,
[`ci.md`](ci.md) and [`container-image.md`](container-image.md) in the same change. Before
finishing any change, `cmake --build build && ctest --test-dir build` must be green and the
binary smoke-tested by hand, as `CLAUDE.md` requires.
