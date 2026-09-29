# Workspace Data-plane Proof

This records the initial events-first implementation. The subsequent explicit workspace-table
correction has its own [native proof record](workspace-contract-proof.md).

Native macOS verification for [lake#311](https://github.com/oramasearch/lake/issues/311).
The [design and audit](../architecture/workspace-data-plane.md) records the implementation scope
and every owning-repository follow-up; [navigation](../ui/workspace-selection.md) records the UI contract.

## Commands

Both configurations use Release, Homebrew LLVM, the authorized read-only Zetta source tree and
the existing Liquid checkout. No formatter or container tooling was run. Builds use six jobs;
CTest suites run one at a time.

```sh
createdb -h 127.0.0.1 issue311_static_20260929
createdb -h 127.0.0.1 issue311_shared_20260929

cmake -S . -B build -G Ninja -DBUILD_SHARED_LIBS=OFF \
  -DLAKE_ZETTA_SOURCE_DIR=/Users/stevesperandeo/dev/homer6/orama-work/modules/zetta-bridge/modules/zetta \
  -DLAKE_LIQUID_SOURCE_DIR=/Users/stevesperandeo/dev/oramasearch/codex-work/liquid
cmake --build build -j 6

cmake -S . -B build-shared -G Ninja -DBUILD_SHARED_LIBS=ON \
  -DLAKE_ZETTA_SOURCE_DIR=/Users/stevesperandeo/dev/homer6/orama-work/modules/zetta-bridge/modules/zetta \
  -DLAKE_LIQUID_SOURCE_DIR=/Users/stevesperandeo/dev/oramasearch/codex-work/liquid
cmake --build build-shared -j 6

export LAKE_INSTANCE_KEK=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef01234567
LAKE_TEST_DB_URI=postgresql://stevesperandeo@127.0.0.1:5432/issue311_static_20260929 \
  ctest --test-dir build --output-on-failure
LAKE_TEST_DB_URI=postgresql://stevesperandeo@127.0.0.1:5432/issue311_shared_20260929 \
  ctest --test-dir build-shared --output-on-failure
```

The displayed KEK is a public test-only value. Database fixtures create their own private databases
and independent keys; neither database name belongs to a pre-existing database. Liquid's AppImage
runtime download failed its pinned-hash check. Configure used cached runtime
files with exactly those expected hashes, staged inside each build tree, without changing a pin:

```text
runtime-x86_64  1cc49bcf1e2ccd593c379adb17c9f85a36d619088296504de95b1d06215aebbf
runtime-aarch64 7d5d772b7c32f0c84caf0a452a3072a5709027d7eac5856feb89a7a7a8881372
```

## Results

Static output:

```text
100% tests passed, 0 tests failed out of 432
Total Test time (real) = 278.91 sec
```

Shared output:

```text
100% tests passed, 0 tests failed out of 432
Total Test time (real) = 286.66 sec
```

`lake-ui-token-gates`, `lake-ui-contrast` and `lake-experiment-charts` all passed in both runs.
There are 433 registered cases: one disabled benchmark and fourteen expected skips described
below. No host-contention exceptions were used for these final runs.

After both proofs, `git fetch origin && git rebase origin/main` reported the branch up to date
against `438bb9048896dc1c31e74d0dba4a0660d55257dc`. `git submodule update --init --recursive`
completed without changes. No incoming source, header, CMake or submodule changes required a rebuild.

Both `./build/lake version` and `./build-shared/lake version` printed `lake 0.0.1`.
A separate native static server on `127.0.0.1:49117`, using an in-memory control store and a
worktree-local store directory, answered `GET /v1/server_info` with HTTP 200 and:

```json
{"ok":true,"value":{"api":"v1","name":"lake","node":"issue311-smoke","revision":"unknown","store":"memory","version":"0.0.1"}}
```

The response's variable duration field is omitted above. The smoke server was stopped afterwards.

## Coverage and Limits

Coverage includes required/unknown/foreign IDs, immutable persisted routing, recorded ambiguous
legacy assignments, workspace isolation within one customer and across customers, task-credential
isolation, a synthetic week of bounded dispatch, topic retention settings, app/build cleanup,
dispatch-only bus cutover, REST/MCP schemas, global selection, canonical links, active navigation,
cookie redirects, same-page switching and explicit form fields.

Existing optional skips are the externally supplied Liquid CLI browser-to-terminal scenario,
the optional Python Hugging Face client, and twelve PostgreSQL variants whose fixtures expressly
support memory-only operator status writes. The search benchmark is disabled by default. The
PostgreSQL store, migration, application, build and credential tests run against local Postgres.

Linux proof deferred: the configured off-host lane helper does not exist on this host.
Image builds and container integrations are deferred: no Docker on this host.
Agent-runtime/agent deployment, operator schema and event-client adoption, upstream 60-second
polls and the deployed idle-receive-rate measurement remain linked rollout follow-ups. Remaining
workspace-data custody migrations are deliberately not implemented here. Billing design remains open.
