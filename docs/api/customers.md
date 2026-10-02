# Users, organizations, customers

The control-plane entities from [`../control-plane/schema.md`](../control-plane/schema.md).
Users and organizations are identity; the **customer is the tenant** and owns everything.

## Shapes

| Entity | Fields |
|---|---|
| user | `id` (uuid v4), `taap_id` (string or null, unique), `email` (string or null, unique case-insensitively), `display_name`, `created_at`, `updated_at` |
| organization | `id`, `taap_id` (string or null, unique), `display_name`, `created_at`, `updated_at` |
| customer | `id`, `customer_type` (`individual` \| `organization`), `user_id` (uuid or null), `organization_id` (uuid or null), `name` (the handle), `created_at`, `updated_at` |

## Rules `create_customer` enforces (and the database enforces again)

- `customer_type: individual` needs exactly `user_id`; `organization` needs exactly
  `organization_id` → otherwise `400 invalid_input`.
- The referenced user or organization must exist → `400 invalid_input`.
- One customer per user and per organization → `409 conflict`.
- `name` is the handle: lowercase letters, digits, inner hyphens, 1–256 chars → otherwise
  `400 invalid_input`; one global namespace shared by individuals and organizations →
  `409 conflict`. Omit `name` and lake generates `{adjective}-{noun}-{number}` (48 × 48 × 999
  combinations), retrying up to 8 times on a collision before answering `409`.

## Examples

```sh
H='Authorization: Bearer <token>'
curl -H "$H" -H 'content-type: application/json' \
  -d '{"email":"ada@example.com","display_name":"Ada"}' https://lake.oramadev.net/v1/create_user
curl -H "$H" -H 'content-type: application/json' \
  -d '{"customer_type":"individual","user_id":"<user id>"}' https://lake.oramadev.net/v1/create_customer
curl -H "$H" 'https://lake.oramadev.net/v1/list_customers?limit=5'
curl -H "$H" 'https://lake.oramadev.net/v1/get_customer?id=<customer id>'
```

`get_customer` also accepts `customer_id`, the name every other capability uses for a space;
passing both with different values answers `400`.

Response (success): `{"ok":true,"value":{...},"duration_ms":0.4}`. Failure:
`{"ok":false,"error":{"code":"conflict","message":"...","path":""},"duration_ms":0.2}`.

## Store

`Store` (`include/lake/control/store.hpp`) has two implementations: `InMemoryStore`
(tests, `--memory-store`) and the Postgres store (`src/control/postgres_store.cpp`, the one TU
that names libpqxx). The Postgres store maps SQL errors to the same codes: unique violation →
`conflict`, check / foreign-key / data errors → `invalid_input`, a broken connection →
`store_unavailable`. Calls run inline on the server's io thread, one statement each; a pool or
offload is the follow-up if latency shows.

## Self-service ownership

An ordinary Lake password sign-up now creates its individual customer and a customer-level
`admin` grant atomically. It uses a generated permanent handle and the configured storage quota.
`create_customer` remains operator-only; the public sign-up capability performs the bootstrap
inside the account transaction. See [provisioning.md](provisioning.md).

A person can also start an organization with its own `organization` customer, or turn their
personal space into one (`create_organization_space`, `convert_personal_space`). Conversion is
the one change of `customer_type`: the row keeps its id and handle. See
[organizations.md](organizations.md#self-service-an-organization-a-person-runs).
