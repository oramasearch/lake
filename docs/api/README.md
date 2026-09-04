# The `/v1` API and MCP

**Shape.** lake exposes every capability three ways, the way Zetta does: REST under `/v1`, an
OpenAPI 3.1 document at `GET /v1/openapi.json`, and MCP at `POST /mcp`. One capability registry
(`include/lake/api/registry.hpp`) is the single source all three read: a capability is a name,
a kind (`read` → `GET /v1/<name>`, input in the query string; `action` → `POST /v1/<name>`,
input as the JSON body), an input schema, an output schema and a handler. REST and MCP go through
the same `Registry::call`, so they authorize, validate and fail identically.

**Envelope.** Every response is `{ ok, value | error{code, message, path}, duration_ms }`.
Status by error code: `invalid_input` 400, `unauthorized` 403, `not_found` / `unknown_capability`
404, `conflict` 409, `store_unavailable` 503, anything else 500. MCP `tools/call` returns the same
envelope as `structuredContent` (and as text), with `isError` set.

**Auth.** Bearer token, see [`auth.md`](auth.md). Everything except `server_info` requires it.

## Operational routes

| Route | Contract |
|---|---|
| `GET /healthz`, `GET /readyz`, `GET /metrics` | Liveness and readiness are unauthenticated; metrics uses the same bearer or session cookie as capabilities. These are HTTP operational routes, not capabilities, so they are not rendered into OpenAPI or MCP. See [`../serving/telemetry.md`](../serving/telemetry.md). |

## Capabilities

| Capability | Kind | Input | Value | Page |
|---|---|---|---|---|
| `server_info` | read, **no auth** | — | `{name, node, version, api, store}` | — |
| `list_users` | read | `limit` (≤1000, default 100) | `{items: [user]}` | [customers.md](customers.md) |
| `get_user` | read | `id` | user | [customers.md](customers.md) |
| `create_user` | action | `taap_id?`, `email?`, `display_name?` | user | [customers.md](customers.md) |
| `list_organizations` | read | `limit` | `{items: [organization]}` | [customers.md](customers.md) |
| `get_organization` | read | `id` | organization | [customers.md](customers.md) |
| `create_organization` | action | `taap_id?`, `display_name?` | organization | [customers.md](customers.md) |
| `list_customers` | read | `limit` | `{items: [customer]}` | [customers.md](customers.md) |
| `get_customer` | read | `id` | customer | [customers.md](customers.md) |
| `create_customer` | action | `customer_type`, `user_id?`, `organization_id?`, `name?` | customer | [customers.md](customers.md) |
| `list_repositories` | read | `customer_id?`, `limit` | `{items: [repository]}` | [repositories.md](repositories.md) |
| `get_repository` | read | `id` | repository | [repositories.md](repositories.md) |
| `create_repository` | action | `customer_id`, `name`, `description?`, `visibility?` | repository | [repositories.md](repositories.md) |
| `rename_repository` | action | `id`, `name` | repository | [repositories.md](repositories.md) |
| `transfer_repository` | action | `id`, `customer_id` | repository | [repositories.md](repositories.md) |
| `set_repository_visibility` | action | `id`, `visibility` | repository | [repositories.md](repositories.md) |
| `delete_repository` | action | `id` | tombstoned repository | [repositories.md](repositories.md) |
| `list_repository_audit_events` | read | `repository_id`, `limit?` | `{items: [audit_event]}` | [repositories.md](repositories.md) |
| `refs` | read | `repository_id` | `{items: [ref]}` | [sync.md](sync.md) |
| `find_missing` | action | `repository_id`, `digests` | `{digests: [digest]}` | [sync.md](sync.md) |

MCP: [`mcp.md`](mcp.md). The full request/response schemas are in `/v1/openapi.json`, generated
from the registry.

`push` and `pull` are not registered until liquid #160 publishes the shared framing and public
import/export API. Their contract and the required follow-up are recorded in [`sync.md`](sync.md).

## Adding a capability

1. A thin handler over the `Store` (`include/lake/control/store.hpp`): no business logic in the
   HTTP layer, no HTTP type outside `src/api/http_server.cpp`.
2. `snake_case` name, verb-first for actions, noun-first for reads; input schema with
   `properties`, `required`, `additionalProperties: false`; an output schema for the value.
3. Register it in `src/api/capabilities.cpp`. It appears on REST, in OpenAPI and as an MCP tool
   automatically; `test_api.cpp`'s OpenAPI test proves it.
4. Tests against `InMemoryStore`; a page under `docs/api/` in the same change.

**Verify.** `ctest --test-dir build`, then
`./build/lake --listen 127.0.0.1:4901 --memory-store --admin-token t` and `curl` the routes
(see [`../deployment/aks.md`](../deployment/aks.md) for the production checks).
