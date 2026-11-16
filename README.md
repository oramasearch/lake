# lake

**The workspace where you build, train and ship AI on your own data.**

lake is the server behind Orama Lake's console. It gives each account workspaces for its data,
projects, pipelines, runs and models, and adds what a team needs around them: accounts and sign-in,
agents, apps and services, version-controlled repositories, search, secrets and audit. Everything
it does is one registry of capabilities, served as REST, OpenAPI and MCP, and the web console
calls only that API, so an agent can do anything a person can.

Part of [Orama Lake](https://github.com/oramasearch/lake-platform), the sovereign AI stack.
MIT-licensed.

## What it does

- **Workspaces on their own engine**: each workspace has its own Zetta for datasets, projects,
  pipelines, runs and models, with Design, Train and Ship pages in the console.
  [Docs](docs/control-plane/workspaces.md)
- **One API, three renderings**: every capability is a REST call, an OpenAPI operation and an MCP
  tool, authorized, validated and audited in one place. [Docs](docs/api/README.md)
- **Sign-in and identity**: password, passkeys and TOTP, OpenID Connect and SAML single sign-on,
  SCIM provisioning, scoped API tokens, and identity pools for your own applications.
  [Docs](docs/api/identity.md)
- **Agents**: library agents and your own, started from the console, a schedule or a stream, each
  with a credential bound to its workspace. [Docs](docs/api/agent-launches.md)
- **Apps and services**: deploy web and data apps and managed databases into a workspace.
  [Docs](docs/api/web-apps.md)
- **Repositories, drives and packages**: liquid repositories with reviews, issues and branch rules;
  drives with sharing and history; OCI, npm and Python registries. [Docs](docs/api/repositories.md)
- **Search, secrets and audit**: repository search, a secret store with rotation and dynamic leases,
  a private certificate authority, and a signed, hash-linked audit log with a transparency log.
  [Docs](docs/api/secrets.md)
- **Usage and billing**: usage per workspace over time and limits; billing and prepaid balances are
  off unless `LAKE_BILLING` turns them on. [Docs](docs/api/metering.md)

What is implemented, tested and deployed is in [the implementation status](docs/design/status.md);
every release, with what it includes and its known issues, is in the
[release notes](docs/releases/README.md).

## Quickstart

To run the whole stack at once (lake, its engine, the agents and the operator), use the
[Orama Lake install guide](https://github.com/oramasearch/lake-platform/blob/main/docs/install/README.md).

To run lake alone from source, build it (below) and start it with the in-memory control plane:

```sh
./build/lake version
./build/lake --listen 127.0.0.1:4900 --node dev --memory-store --admin-token t
```

In another terminal:

```sh
curl -sS http://127.0.0.1:4900/v1/server_info
```

It answers the version and revision. Open `http://127.0.0.1:4900/` in a browser for the console.

### Build from source

You need CMake 3.28 or newer, Ninja, Clang 17 or newer (Homebrew LLVM on macOS), OpenSSL 3, libpq
and Node. lake builds against two checkouts that are not its submodules, zetta and liquid, at the
commits [lake-platform](https://github.com/oramasearch/lake-platform) pins; cloning the platform
gets all three:

```sh
git clone --recurse-submodules https://github.com/oramasearch/lake-platform.git
cd lake-platform/modules/liquid-bridge/modules/lake
cmake -S . -B build -G Ninja \
  -DLAKE_ZETTA_SOURCE_DIR=$PWD/../../../zetta-bridge/modules/zetta \
  -DLAKE_LIQUID_SOURCE_DIR=$PWD/../liquid
cmake --build build -j 6
ctest --test-dir build --output-on-failure
```

Without `LAKE_TEST_DB_URI` the PostgreSQL-backed tests skip; with it, and `LAKE_TEST_ZETTA_BINARY`
for the tests that start a Zetta, the whole suite runs. Every option is in
[`docs/build/local.md`](docs/build/local.md) and [`docs/build/testing.md`](docs/build/testing.md).

## How it fits the stack

lake is the control plane and the console. It keeps accounts, policy and audit in PostgreSQL, keeps
each workspace's data in that workspace's Zetta, stores repositories with liquid's library, and
hands what should run (workspaces, apps, services, builds) to the operator as desired state.

| Piece | What it does | Relation to lake |
|---|---|---|
| [lake-platform](https://github.com/oramasearch/lake-platform) | The whole stack, its install and its skill | Pins and ships lake |
| [zetta](https://github.com/oramasearch/zetta) | The engine for data, search and machine learning | Runs each workspace; lake calls it |
| [liquid](https://github.com/oramasearch/liquid) | Version control, builds, packages and deploys | lake embeds its store and serves its sync |
| [orama-operator](https://github.com/oramasearch/orama-operator) | Runs the stack on Kubernetes | Applies lake's schema and runs what lake asks for |

## Known limits

- The container image is built on Ubuntu, which bundles GPL and LGPL components; a static image is
  [lake#1334](https://github.com/oramasearch/lake/issues/1334).
- Agent session keys do not yet meet liquid v0.3.0's delegation rules
  ([lake#1346](https://github.com/oramasearch/lake/issues/1346)), and a new pod needs its liquid
  identity custody configured ([lake#1352](https://github.com/oramasearch/lake/issues/1352)).
- An unknown HTTP method on a routed path closes the connection without an answer
  ([lake#1341](https://github.com/oramasearch/lake/issues/1341)).
- The rest are in the [open issues](https://github.com/oramasearch/lake/issues) and each release's
  known issues.

## Documentation

- [`docs/README.md`](docs/README.md): one row per topic.
- [`docs/guide/README.md`](docs/guide/README.md): the guides people read in the console.
- [`docs/api/README.md`](docs/api/README.md): every capability; `GET /v1/openapi.json` on a running
  lake is the generated reference.
- [`docs/serving/configuration.md`](docs/serving/configuration.md): every environment variable;
  `lake manifest` prints them from the binary.
- [`docs/serving/http.md`](docs/serving/http.md): every route.
- [`docs/releases/README.md`](docs/releases/README.md): the release notes.

## Community

- Bugs and requests: [issues](https://github.com/oramasearch/lake/issues).
- Security reports: privately, per [SECURITY.md](SECURITY.md). Please don't open a public issue.
- Conduct: [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).

## Contributing

Contributions are welcome. [CONTRIBUTING.md](CONTRIBUTING.md) says how to build, test and open a pull
request (against `dev`), and how contributions are licensed ([CLA.md](CLA.md)).

## License

MIT; see [LICENSE](LICENSE) and [COPYRIGHT](COPYRIGHT). The components lake includes keep their own
licences, listed in [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES).
