# Scoped connection credentials native proof

Work order: [lake#731](https://github.com/oramasearch/lake/issues/731).
Branch: `issue-731-scoped-credentials`, based on `issue-730-tunnel-grants`.
Contract: [scoped connection credentials](../api/scoped-credentials.md), migration `0084_network`
and [orama-operator#469](https://github.com/oramasearch/orama-operator/issues/469).

## Environment and commands

Native macOS arm64, Homebrew Clang 22.1.8, CMake/Ninja and PostgreSQL 18.3. Builds use at most
six jobs and one test suite runs at a time. The sandbox permits no Git metadata writes or load
checks; the coordinator owns commits, upstream integration and PR publication. All four required
instruction and index files were present. No container commands were run.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DLAKE_LIQUID_SOURCE_DIR=/Users/stevesperandeo/dev/oramasearch/codex-work/liquid \
  -DLAKE_ZETTA_SOURCE_DIR=/Users/stevesperandeo/dev/homer6/orama-work/modules/zetta-bridge/modules/zetta
cmake --build build -j 6
cmake -S . -B build-shared -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
  -DLAKE_LIQUID_SOURCE_DIR=/Users/stevesperandeo/dev/oramasearch/codex-work/liquid \
  -DLAKE_ZETTA_SOURCE_DIR=/Users/stevesperandeo/dev/homer6/orama-work/modules/zetta-bridge/modules/zetta
cmake --build build-shared -j 6
createdb -h 127.0.0.1 lake_issue731_20261014_scoped_credentials
export LAKE_TEST_SKIP_STALE_SWEEP=1
export LAKE_INSTANCE_KEK=4444444444444444444444444444444444444444444444444444444444444444
export LAKE_TEST_DB_URI=postgresql://stevesperandeo@127.0.0.1:5432/lake_issue731_20261014_scoped_credentials
ctest --test-dir build \
  -R 'NetworkCapabilities|NetworkStore|PostgresNetwork|SchemaConformance|AuditRouting|UsageContract|DocsStatus' \
  --output-on-failure
ctest --test-dir build --output-on-failure -j 6
ctest --test-dir build-shared --output-on-failure -j 6
```

The KEK is public fixture material. The administrative database was created for this run and is
left available for coordinator reruns. The fixtures create and remove only their own test
databases; the stale-database sweep is disabled.

## Results

Both native Release builds completed successfully. Final focused static proof:

```text
100% tests passed, 0 tests failed out of 35
```

All 35 focused tests ran without skips. Final static Release full suite:

```text
100% tests passed, 0 tests failed out of 1064
```

Final shared Release full suite:

```text
100% tests passed, 0 tests failed out of 1064
```

Each full suite registered 1,065 tests: 1,045 passed, 19 skipped and one disabled. Both include
the final removal and observed-status assertions. No failures or isolated reruns occurred.
The skipped cases require external browser, Python client, runner or real-Zetta fixtures; the
disabled case is the existing ten-thousand-file search benchmark. The Lake network, schema,
audit and usage checks all ran successfully.

## Manual binary smoke

```sh
./build/lake version
./build/lake --listen 127.0.0.1:49731 --node scoped-credential-smoke --memory-store \
  --admin-token scoped-credential-smoke-token --store-root build/scoped-credential-smoke-store
curl -fsS -H 'Authorization: Bearer scoped-credential-smoke-token' \
  http://127.0.0.1:49731/v1/server_info | jq -c '{ok, version: .value.version}'
curl -fsS -H 'Authorization: Bearer scoped-credential-smoke-token' \
  http://127.0.0.1:49731/v1/openapi.json | \
  jq -c '.paths["/v1/rotate_network_connection_credential"] | keys'
curl -fsS -H 'Authorization: Bearer scoped-credential-smoke-token' \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' http://127.0.0.1:49731/mcp | \
  jq -c '.result.tools[] | select(.name == "rotate_network_connection_credential") | {name, annotations}'
```

```text
lake 0.1.0
{"ok":true,"version":"0.1.0"}
["post"]
{"name":"rotate_network_connection_credential","annotations":{"destructiveHint":false,"idempotentHint":false,"readOnlyHint":false}}
```

The token is a local fixture value. The server drained and exited cleanly after interruption.
The shared binary also reports `lake 0.1.0`.

Changed C++ lines pass the repository formatter check, and whitespace validation is clean:

```sh
git diff --no-ext-diff -U0 -- '*.cpp' | \
  python3 /opt/homebrew/opt/llvm/share/clang/clang-format-diff.py \
  -p1 -binary /opt/homebrew/opt/llvm/bin/clang-format
git diff --check
```

Both commands return exit status zero with no output. No migrations or submodule pointers changed.

## Acceptance coverage and remaining integration proof

| Issue acceptance | Lake evidence | Operator integration still required |
|---|---|---|
| A read-only connection's app cannot write | The exact `read` and optional schema scope survives creation and rotation; shared store validation refuses credentials outside app-to-service connections. | Connect as the issued Postgres role, read successfully and observe a rejected write. |
| Rotation replaces the credential the app sees | Rotation advances desired generation without changing scope or endpoints; old status remains visible until a simulated operator observation catches up, using the same secret reference. | Reconcile the generation-only change, rewrite the workspace secret and verify the app sees the replacement and the old credential fails. |
| Removal revokes the credential | Removal sets `desired: absent`, advances generation, excludes the connection from effective rules and refuses further rotation. | Reconcile removal, drop the role and verify access ends. |

The native suites exercise Lake's control-plane and API contract. They do not run the operator or
an app with its projected credential, so they do not establish the three end-to-end acceptance
items. The operator must implement the documented generation-only rotation request in #469.
Those integration checks remain a completion gate for the combined feature.

Linux proof deferred: `/Users/stevesperandeo/dev/oramasearch/codex-work/tools/linux-proof.sh`
does not exist on this host. Image builds and container-based suites are deferred: no Docker on
this host.

## Coordinator handoff

Changes remain uncommitted. Proposed commit: `api: add scoped connection credential rotation`.
Proposed PR title: `Add scoped app connection credential rotation`.
PR base: `issue-730-tunnel-grants`; head: `issue-731-scoped-credentials`; reference `Refs #731`.
No PR was opened and no issue was commented on or closed.
