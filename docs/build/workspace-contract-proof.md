# Workspace contract verification

Native macOS verification of the lake#265 contract correction in lake#311 / PR #385.
The [contract](../control-plane/workspaces.md) and [navigation guide](../ui/workspace-selection.md)
describe the implementation. No deployment, formatter or container tooling was run.

## Commands

Both existing Release build configurations use the authorized read-only Zetta checkout and
the Liquid source checkout recorded in [the initial proof](workspace-data-plane-proof.md).
Builds use at most six jobs. Test suites run sequentially. These two fresh local base databases
were created for this correction; PostgreSQL fixtures create isolated databases and keys per case.

```sh
createdb -h 127.0.0.1 issue311_contract_static_20260929
createdb -h 127.0.0.1 issue311_contract_shared_20260929
cmake --build build -j 6
cmake --build build-shared -j 6

export LAKE_INSTANCE_KEK=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef01234567
LAKE_TEST_DB_URI=postgresql://stevesperandeo@127.0.0.1:5432/issue311_contract_static_20260929 \
  ctest --test-dir build --output-on-failure
LAKE_TEST_DB_URI=postgresql://stevesperandeo@127.0.0.1:5432/issue311_contract_shared_20260929 \
  ctest --test-dir build-shared --output-on-failure
```

The displayed KEK is public, test-only input; fixtures generate independent keys.

## Results

Static:

```text
100% tests passed, 0 tests failed out of 434
Total Test time (real) = 292.55 sec
```

Shared:

```text
100% tests passed, 0 tests failed out of 434
Total Test time (real) = 349.82 sec
```

`lake-ui-token-gates`, `lake-ui-contrast` and `lake-experiment-charts` passed in both suites.
No host-contention exceptions were needed for the final runs.

After both proofs, `git fetch origin && git rebase origin/main` reported the branch up to date
at `438bb9048896dc1c31e74d0dba4a0660d55257dc`. `git submodule update --init --recursive`
completed without changes. No incoming source, headers, CMake lists or submodule pointers
required another configure/build/proof cycle.

Both binaries printed `lake 0.0.1`. There are 435 registered cases, with the same one disabled
benchmark and fourteen optional/fixture skips listed in the initial proof. PostgreSQL workspace,
app, build, migration and credential tests run against the local server.

Coverage includes distinct workspace/service UUIDs, desired-state generation, status-field
rejection, typed-name deletion, every-change notifications, all three foreign keys, legacy naming
remaps and ambiguity reporting, cross-workspace event isolation, REST/MCP schemas, explicit form
IDs, selector persistence, control-page selection, and navigation ownership / `aria-current`.
The initial diagnostic failures were corrected fixture IDs, an internal service-name length,
an app-service caller's missing ID, and asynchronous notification receipt. A diagnostic rerun
overlapped relinking and briefly could not find its test executable; final suites run only after
their own builds complete. These are not reported as host-contention exceptions.

## Files in this correction

- Schema and storage: `db/schema.sql`; `include/lake/control/{store,memory_store,zetta_workspace}.hpp`;
  `src/control/{memory_store,postgres_store,zetta_workspace,agent_bus}.cpp`; `src/telemetry/metrics.cpp`.
- APIs and UI: `src/api/{workspace_capabilities,registry,stream_capabilities,web_app_capabilities,
  agent_docs,http_server,web_ui}.cpp`.
- Tests: `tests/{stream_fixture,experiment_fixture}.hpp`;
  `tests/{test_api,test_postgres,test_workspaces,test_http_server}.cpp`.
- Docs: `docs/api/{agent-bus,builds,services,streams,web-apps,workspaces}.md`;
  `docs/control-plane/{builds,schema,services,streams,web-apps,workspaces}.md`;
  `docs/architecture/workspace-data-plane.md`; `docs/ui/workspace-selection.md`;
  `docs/build/{workspace-data-plane-proof,workspace-contract-proof}.md`;
  `docs/README.md`, root `README.md` and `CLAUDE.md`.

## Deferred and open questions

Linux proof deferred: `/Users/stevesperandeo/dev/oramasearch/codex-work/tools/linux-proof.sh`
does not exist. Image builds and container integrations are deferred: no Docker on this host.
Operator schema/guard adoption, deployment and agent-client rollout remain separate-repository
work, as do the audited non-event data migrations and upstream polling changes. The operator's
automatic legacy `prod-ws-*` deletion is approved dev-cluster behavior, not an approval gate.
Unresolved deployed assignments and any old non-default `svc-*` Zettas must be inspected during
rollout as described in the contract. No deployed database or cluster was changed by this proof.
There is no outstanding Lake implementation decision.
