# The `/v1` API and MCP

**Shape.** lake exposes every capability three ways, the way Zetta does: REST under `/v1`, an
OpenAPI document at `/v1/openapi.json`, and MCP at `POST /mcp` so agents use the same
capabilities directly, with their TAAP identity attributed. The server-rendered web UI is the
fourth view of the same thing.

**What exists.**

| Capability | REST | MCP | Notes |
|---|---|---|---|
| server info | `GET /v1/server_info` | — | name, node, version, api |

**Next.** The control-plane entities from [`../control-plane/schema.md`](../control-plane/schema.md):
`users`, `organizations`, `customers` (list, get, create), backed by the Postgres the operator
provisions; `/v1/openapi.json` generated from the capability registry; `POST /mcp` serving the
same registry as tools. Tracked as lake issue #10. Each capability gets a page in this folder when
it lands: request/response, errors, auth, and how to verify it.

**Rules.** No HTTP type outside `src/api/http_server.cpp`; capabilities are plain functions on a
registry the REST, MCP and OpenAPI adapters all read; `std::expected` at every boundary; every
error is JSON `{"error": "<snake_case>"}` with the right status.
