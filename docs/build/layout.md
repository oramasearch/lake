# Source tree layout

One binary, a few libraries, and the rules from `CONTRIBUTING.md` about which translation units may
name third parties. `lake-core` is HTTP-free; `src/api/http_server.cpp` and
`src/control/postgres_store.cpp` are the two files that know Beast2/Boost.JSON and libpqxx
respectively.

| Path | Role |
|---|---|
| `include/lake/api/server.hpp` | `ApiServer` / `ServerOptions` in Zetta's shape; no HTTP type appears here |
| `src/api/http_server.cpp` | the **one** translation unit that knows Beast2, `boost::http` and Boost.JSON; serves the registry as REST, OpenAPI and MCP |
| `include/lake/api/registry.hpp`, `sync_store.hpp`, `rest.hpp`, `mcp.hpp`, `capabilities.hpp` | the HTTP-free capability registry, liquid-sync boundary, and surface renderers |
| `include/lake/control/`, `src/control/` | the control-plane store, policy engine, operator-Zetta platform client/mirror, application authentication, static-secret lifecycle and encryption service: interface, in-memory, Postgres (`postgres_store.cpp` is the one TU that names libpqxx) |
| `include/lake/telemetry/metrics.hpp`, `src/telemetry/metrics.cpp` | dependency-free Prometheus registry and measured store decorator |
| `include/lake/api/web_ui.hpp`, `src/api/web_ui.cpp` | pure HTML renderers (string in, string out) |
| `include/lake/audit/`, `src/audit/` | canonical-CBOR audit signing, the lake identity lifecycle, JSON-lines export and chain verification (compiled through the Postgres TU so OpenSSL stays confined) |
| `include/lake/transparency/`, `src/transparency/` | immutable TUF metadata storage, threshold verification, cross-role binding, root rotation, checkpoint-bound Merkle entry/hash tiles and compact proofs, identity monitoring, RFC 3161 timestamping, and RFC 4998 archive renewal |
| `include/lake/control/agents.hpp`, `src/control/agents.cpp` | short-lived agent-session key minting, canonical TAAP delegation, expiry, and durable drip attribution |
| `src/main.cpp` | CLI and the owner loop (`wait_for_work` / `drain`) |
| `tests/` | GoogleTest; `ctest --test-dir build` |
| `tools/ui/` | reproducible 1440×900 rendering for the static UI visual-direction mocks, the design-token gates and the contrast check |
| `assets/fonts/`, `cmake/embed_fonts.cmake` | OFL-licensed IBM Plex WOFF2 sources and their binary embed step |
| `modules/` | vendored dependencies and private patch carriers listed in [`dependencies.md`](dependencies.md); Zetta remains an external source path selected by `LAKE_ZETTA_SOURCE_DIR` |
| `Dockerfile`, `.dockerignore` | the Linux/amd64 image ([`container-image.md`](container-image.md)); the context excludes `build/` and `.git/` |
| `db/schema.sql` | **the control-plane schema, source of truth.** Identity and application user pools, offline authorization and recovery, tenants, repositories, organization administration, SSO federation, policy, audit, static secrets and wrapped encryption keys. The operator vendors a byte-identical copy and applies it per Lake through a `PostgresSchema` ([`../control-plane/schema.md`](../control-plane/schema.md)) |
| `docs/` | one folder per topic ([`../README.md`](../README.md)) |

A new area adds its header and source under `include/lake/<area>/` and `src/<area>/`, its tests
under `tests/`, and a row here.
