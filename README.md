# lake

lake is the hosted service around liquid: forge, drive, platform, registry and transparency log,
identity, secrets and PKI, and search, served by one C++26 binary with a server-rendered web UI.
The control plane is Postgres, the data plane is Zetta, and repository data goes through
libliquid, consumed as a library. The operator deploys it as a `Lake` custom resource. liquid is
the tool; lake is where it collects. The longer statement, the six surfaces and where lake sits
in the project are in [`docs/design/overview.md`](docs/design/overview.md).

## Status

The control surface serves in production: `Lake/prod` on AKS at https://lake.oramadev.net runs
the capability registry as REST `/v1`, OpenAPI and MCP, the Postgres control plane, and the
server-rendered pages behind the operator sign-in, with the state and runbook in
[`docs/deployment/aks.md`](docs/deployment/aks.md). The source tree goes further than the deployed
image: identity, SSO, forge pages, repository sync, search, secrets and PKI, drive and models,
metering, the transparency log, replication and platform mirroring are implemented and tested
here and wait on a new image, the operator's schema copy and volume support before they run in
production. [`docs/design/status.md`](docs/design/status.md) lists each area with its migration
and contract page.

## Build and run

Requirements on macOS: CMake 3.28 or newer, Ninja, Homebrew LLVM (Clang 17 or newer), OpenSSL 3
and libpq (`brew install llvm cmake ninja openssl@3 libpq`), Node for the UI checks in the test
suite, and a checkout of `zetta-bridge` for `LAKE_ZETTA_SOURCE_DIR`. Clone with submodules; the
HTTP stack and libliquid are vendored under `modules/`.

```sh
git clone --recurse-submodules git@github.com:oramasearch/lake.git
cd lake
cmake -S . -B build -G Ninja \
  -DLAKE_ZETTA_SOURCE_DIR=/path/to/zetta-bridge/modules/zetta
cmake --build build -j 6
ctest --test-dir build --output-on-failure
```

Without `LAKE_TEST_DB_URI` the Postgres-backed cases skip. To run them against a local Postgres,
create a database, apply the schema, and set the two variables:

```sh
createdb lake_test
psql -d lake_test -f db/schema.sql
LAKE_INSTANCE_KEK=$(openssl rand -hex 32) \
LAKE_TEST_DB_URI="postgresql://$USER@127.0.0.1:5432/lake_test" \
  ctest --test-dir build --output-on-failure
```

Run the server with the in-memory control plane and check it answers:

```sh
./build/lake version
./build/lake --listen 127.0.0.1:4900 --node dev --memory-store --admin-token t
curl http://127.0.0.1:4900/v1/server_info
```

`--control-db` (or `LAKE_CONTROL_DB_URI`) selects a Postgres control plane instead, with
`--instance-kek` and `--store-root` for repository data. Every flag and environment variable is in
[`docs/serving/configuration.md`](docs/serving/configuration.md); the build in more detail, the
test targets and CI are in [`docs/build/local.md`](docs/build/local.md) and
[`docs/build/ci.md`](docs/build/ci.md).

## Documentation

[`docs/README.md`](docs/README.md) indexes one folder per topic. Every change to the system lands
its page there in the same change; this file is only the front door. The most useful pages:

- [`docs/design/overview.md`](docs/design/overview.md): what lake is and what it is built on.
- [`docs/design/status.md`](docs/design/status.md): what the source tree implements, by area.
- [`docs/api/README.md`](docs/api/README.md): the capability registry rendered as REST, OpenAPI
  and MCP, with every capability and how to add one.
- [`docs/serving/http.md`](docs/serving/http.md): the route table and the rules that keep the
  server correct.
- [`docs/serving/configuration.md`](docs/serving/configuration.md): flags and environment.
- [`docs/ui/README.md`](docs/ui/README.md): the server-rendered pages and the session cookie.
- [`docs/control-plane/schema.md`](docs/control-plane/schema.md): `db/schema.sql`, the tenant
  model and how the schema reaches a Lake.
- [`docs/deployment/aks.md`](docs/deployment/aks.md): the production instance and its runbook.
- [`docs/build/layout.md`](docs/build/layout.md): the source tree.

The product specification is `oramasearch/liquid` `docs/features/18-lake-service.md`. lake is
part of the liquid project; [`oramasearch/liquid-bridge`](https://github.com/oramasearch/liquid-bridge)
holds the cross-module map and the action plan.

## Repository rules

[`CLAUDE.md`](CLAUDE.md) is the instruction file for every contributor and agent: the rules that
keep the server Zetta-shaped, what lake is built on, what never changes, and what to do before
finishing a change. [`AGENTS.md`](AGENTS.md) restates the non-negotiables for Codex.

## Licence

No licence is declared in this repository, and `CLAUDE.md` says it stays private. The licence
decision for the liquid project is tracked in
[oramasearch/liquid-bridge#2](https://github.com/oramasearch/liquid-bridge/issues/2).
