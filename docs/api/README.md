# The `/v1` API and MCP

**Shape.** lake exposes every capability three ways, the way Zetta does: REST under `/v1`, an
OpenAPI 3.1 document at `GET /v1/openapi.json`, and MCP at `POST /mcp`. One capability registry
(`include/lake/api/registry.hpp`) is the single source all three read: a capability is a name,
a kind (`read` → `GET /v1/<name>`, input in the query string; `action` → `POST /v1/<name>`,
input as the JSON body), an input schema, an output schema, an authorization object and action,
an object resolver, and a handler. REST and MCP go through the same `Registry::call`, so they
authorize, validate and fail identically.

**Envelope.** Every response is `{ ok, value | error{code, message, path}, duration_ms }`.
Status by error code: `invalid_input` 400, `unauthorized` / `mfa_required` 403, `not_found` /
`unknown_capability` 404, `conflict` 409, `store_unavailable` 503, anything else 500. MCP
`tools/call` returns the same
envelope as `structuredContent` (and as text), with `isError` set.

**Auth.** Operator bearer or row-resolved user credential, see [`auth.md`](auth.md). `server_info`,
the lake `identity`, and the identity sign-up/sign-in entry capabilities document their public use.

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
| `create_organization` | action | `taap_id?`, `display_name?`, `customer_id?` | organization | [organizations.md](organizations.md) |
| `list_customers` | read | `limit` | `{items: [customer]}` | [customers.md](customers.md) |
| `get_customer` | read | `id` | customer | [customers.md](customers.md) |
| `create_customer` | action | `customer_type`, `user_id?`, `organization_id?`, `name?` | customer | [customers.md](customers.md) |
| `list_repositories` | read | `customer_id?`, `limit` | `{items: [repository]}` | [repositories.md](repositories.md) |
| `get_repository` | read | `id` | repository | [repositories.md](repositories.md) |
| `create_repository` | action | `customer_id`, `name`, `description?`, `visibility?` | repository | [repositories.md](repositories.md) |
| `rename_repository`, `transfer_repository`, `set_repository_visibility`, `delete_repository` | action | repository id plus the lifecycle field where applicable | repository | [repositories.md](repositories.md) |
| `refs`, `find_missing`, `push`, `pull` | read/action, reserved | repository id and the liquid sync protocol body | liquid ref heads, object selection, or metadata stream | [sync.md](sync.md) |
| `list_teams`, `get_team`, `create_team` | read/action | organization or team identifiers and fields | team(s) | [organizations.md](organizations.md) |
| `list_memberships`, `create_membership` | read/action | scope, principal, role, TAAP evidence | membership(s) | [organizations.md](organizations.md) |
| `list_invitations`, `create_invitation`, `accept_invitation` | read/action | scope, invitee, role, expiry or accepting principal | invitation or membership | [organizations.md](organizations.md) |
| `list_objects`, `get_object`, `create_object` | read/action | graph identifiers, type, parent, `inherit` | object(s) | [policy.md](policy.md) |
| `list_grants`, `create_grant` | read/action | object, principal, role, TAAP evidence | grant(s) | [policy.md](policy.md) |
| `effective_access` | read | `object_id` | `{items: [effective access]}` | [policy.md](policy.md) |
| `identity` | read, **no auth** | — | lake TAAP identity document | [audit.md](audit.md) |
| `rotate_identity` | action | — | replacement lake identity document | [audit.md](audit.md) |
| `audit` | read | `principal?`, `object?`, `outcome?`, `from?`, `to?`, `limit?` | `{items: [audit event]}` | [audit.md](audit.md) |
| `export_audit` | read | audit filters | `{jsonl}` | [audit.md](audit.md) |
| `verify_audit` | read | — | `{verified: true}` | [audit.md](audit.md) |
| `set_audit_retention` | action | `customer_id`, `days` (≥400) | `{customer_id, days}` | [audit.md](audit.md) |
| `sign_up_password`, `sign_in_password`, `verify_mfa` | action, public entry | password credentials; verify also takes a TOTP or recovery code | browser session + secret | [identity.md](identity.md) |
| `enroll_mfa`, `disable_mfa` | action | authenticated user; disable takes a fresh TOTP or recovery code | one-use setup URI and recovery codes, or confirmation | [identity.md](identity.md) |
| `begin_passkey_registration`, `register_passkey` | action | WebAuthn client data and attestation object | creation options, then confirmation | [identity.md](identity.md) |
| `begin_passkey_sign_in`, `sign_in_passkey` | action, public entry | pool, then WebAuthn assertion | request options, then browser session + secret | [identity.md](identity.md) |
| `register_oauth_provider` | action, operator | issuer, client id, secret, scopes | confirmation | [identity.md](identity.md) |
| `start_oauth` | action, public entry | pool, provider, device name, identity document for a new user | authorization URL | [identity.md](identity.md) |
| `create_token`, `list_sessions`, `revoke_session` | action/read | name/scopes or session id | token/session data | [identity.md](identity.md) |
| `list_devices`, `revoke_device` | read/action | device id for revocation | device data | [identity.md](identity.md) |

MCP: [`mcp.md`](mcp.md). The full request/response schemas are in `/v1/openapi.json`, generated
from the registry.

The sync row documents the reserved capability names and their published wire contract. It is not
registered until libliquid exposes the server-facing repository/store binding and codec described
in [`sync.md`](sync.md).

OAuth providers return the browser to `GET /v1/oauth/<provider>/callback`. This callback is the
documented provider-facing route outside the registry; it consumes one-use state and issues the
same `lake_session` cookie returned by the identity capabilities.

## Adding a capability

1. A thin handler over the `Store` (`include/lake/control/store.hpp`): no business logic in the
   HTTP layer, no HTTP type outside `src/api/http_server.cpp`.
2. `snake_case` name, verb-first for actions, noun-first for reads; input schema with
   `properties`, `required`, `additionalProperties: false`; an output schema for the value.
3. Name the authorization object and action and provide the resolver that maps validated input to
   the object id. `Registry::call` rejects protected capabilities missing this metadata.
4. Register it in `src/api/capabilities.cpp`. It appears on REST, in OpenAPI and as an MCP tool
   automatically; `test_api.cpp`'s OpenAPI test proves it.
5. Tests against `InMemoryStore` and the Postgres fixture; a page under `docs/api/` in the same
   change.

**Verify.** `ctest --test-dir build`, then
`./build/lake --listen 127.0.0.1:4901 --memory-store --admin-token t` and `curl` the routes
(see [`../deployment/aks.md`](../deployment/aks.md) for the production checks).
