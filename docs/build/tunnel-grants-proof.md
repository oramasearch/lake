# Tunnel grants native proof

Work order: [lake#730](https://github.com/oramasearch/lake/issues/730).
Branch: `issue-730-tunnel-grants`, based on `issue-728-network-capabilities`.
Contract: [tunnel credentials](../api/tunnel-grants.md), operator secure-networking section 11.1.

## Environment and commands

Native macOS arm64, Homebrew Clang 22.1.8, CMake/Ninja, local PostgreSQL. No containers.
Submodules were already initialized. No Git metadata writes or load checks were run under the
sandbox rules; the coordinator owns review, commit, upstream integration and PR publication.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DLAKE_LIQUID_SOURCE_DIR=/Users/stevesperandeo/dev/oramasearch/codex-work/liquid \
  -DLAKE_ZETTA_SOURCE_DIR=/Users/stevesperandeo/dev/homer6/orama-work/modules/zetta-bridge/modules/zetta
cmake --build build -j 6
cmake -S . -B build-shared -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
  -DLAKE_LIQUID_SOURCE_DIR=/Users/stevesperandeo/dev/oramasearch/codex-work/liquid \
  -DLAKE_ZETTA_SOURCE_DIR=/Users/stevesperandeo/dev/homer6/orama-work/modules/zetta-bridge/modules/zetta
cmake --build build-shared -j 6
createdb -h 127.0.0.1 lake_issue730_20261014_tunnel_csr
export LAKE_TEST_SKIP_STALE_SWEEP=1
export LAKE_INSTANCE_KEK=3333333333333333333333333333333333333333333333333333333333333333
export LAKE_TEST_DB_URI=postgresql://stevesperandeo@127.0.0.1:5432/lake_issue730_20261014_tunnel_csr
ctest --test-dir build \
  -R 'NetworkCapabilities|NetworkStore|PostgresNetwork|TunnelCredentials|SchemaConformance|AuditRouting' \
  --output-on-failure
ctest --test-dir build --output-on-failure -j 6
ctest --test-dir build-shared --output-on-failure -j 6
```

The KEK above is fixture material, not a deployed key. Only the fresh administrative database
was created manually; test fixtures manage their own per-test databases and skip stale cleanup.
One suite runs at a time; all builds use at most six jobs.

Debug reuses downloaded sources, with separate build products (zsh):

```sh
typeset -a tunnel_sources
for tunnel_source_dir in "$PWD"/build/_deps/*-src; do
  tunnel_source_name="${tunnel_source_dir:t}"
  tunnel_source_name="${tunnel_source_name%-src}"
  tunnel_sources+=("-DFETCHCONTENT_SOURCE_DIR_${(U)tunnel_source_name}=$tunnel_source_dir")
done
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_SHARED_LIBS=OFF \
  -DLAKE_LIQUID_SOURCE_DIR=/Users/stevesperandeo/dev/oramasearch/codex-work/liquid \
  -DLAKE_ZETTA_SOURCE_DIR=/Users/stevesperandeo/dev/homer6/orama-work/modules/zetta-bridge/modules/zetta \
  "${tunnel_sources[@]}"
cmake --build build-debug -j 6
ctest --test-dir build-debug --output-on-failure -j 6
```

## Results

Final focused static proof:

```text
100% tests passed, 0 tests failed out of 43
```

Final static Release full suite:

```text
100% tests passed, 0 tests failed out of 1063
```

1,064 registered: 1,044 passed, 19 skipped, one disabled. No full-suite failures or isolated reruns.
Final shared Release full suite:

```text
100% tests passed, 0 tests failed out of 1063
```

The shared build has the same 1,044 passed, 19 skipped and one disabled, with no failures or
isolated reruns.

Final Debug full suite:

```text
100% tests passed, 0 tests failed out of 1063
```

Debug also has 1,044 passed, 19 skipped and one disabled. Its existing thousand-change history
scan completed successfully. No host-contention exception or isolated rerun was needed in any
final full suite. All 43 focused cases ran without skips.

Manual binary smoke:

```sh
./build/lake version
./build/lake --listen 127.0.0.1:49730 --node tunnel-smoke --memory-store \
  --admin-token tunnel-smoke-token --store-root build/tunnel-smoke-store
curl -fsS -H 'Authorization: Bearer tunnel-smoke-token' \
  http://127.0.0.1:49730/v1/server_info | jq '{ok, version: .value.version}'
curl -fsS -H 'Authorization: Bearer tunnel-smoke-token' \
  http://127.0.0.1:49730/v1/openapi.json | \
  jq '.paths["/v1/issue_network_tunnel_certificate"] | keys'
curl -fsS -H 'Authorization: Bearer tunnel-smoke-token' -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' http://127.0.0.1:49730/mcp | \
  jq '.result.tools[] | select(.name == "issue_network_tunnel_certificate") | {name, annotations}'
```

```text
lake 0.1.0
{"ok":true,"version":"0.1.0"}
["post"]
{"name":"issue_network_tunnel_certificate","annotations":{
  "destructiveHint":false,"idempotentHint":false,"readOnlyHint":false}}
```

The smoke server drained and exited cleanly after interruption. Its token is a local test value.
The final rebuilt binary repeated this smoke successfully. All three binaries report `lake 0.1.0`.

After all suites completed, only the fresh administrative database created above was removed:

```sh
dropdb -h 127.0.0.1 lake_issue730_20261014_tunnel_csr
```

No pre-existing database was dropped. The documented `createdb` command recreates the empty
administrative database for a rerun; per-test data is disposable fixture data.

Formatting uses `.clang-format`, changed-line-only formatting for existing C++ files and full
formatting for new C++ files. `git diff --check` and formatter dry checks pass. Migration 0084
is unchanged; 0085 consists only of the two approved ALTERs and its migration marker.

## Coverage and handoff

The focused cases cover recipient-only REST/MCP credentials, the CSR public key and sole grant
SAN, client-only non-CA usage, expiry/12-hour bounds, malformed/weak/invalid-signature CSRs,
operator/other-admin/other-user refusal, cross-scope refusal, missing/stale relay trust, CA
pinning, cross-profile grant-SAN refusal, admin-only grant/revoke, generation/revocation,
expired-grant refusal and signed audit.
Provider tests cover scope, limits, permissions, invalid output and the explicit pending default.
PostgreSQL checks include column ownership/defaults, existing-row preservation and migration replay.

Linux proof deferred: `/Users/stevesperandeo/dev/oramasearch/codex-work/tools/linux-proof.sh`
does not exist on this host. Image builds and container-based suites are deferred: no Docker on
this host. Real session history is pending zetta#1281. Relay end-to-end grant/connect/revoke/
session-ends proof and operator schema adoption belong to the coordinator, as agreed.

No PR was opened and no issue was commented on or closed. Proposed commit:
`Add recipient-bound tunnel certificates and grant trust`.
Proposed PR title: `Add recipient-only tunnel certificates and grant trust`.
PR base: `issue-728-network-capabilities`; reference `Refs #730` rather than closing the issue.
