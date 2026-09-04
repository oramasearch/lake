# Repositories

A repository is owned by a customer (the tenant) and named within it: the address is
`<customer handle>/<name>`. Schema: [`../control-plane/schema.md`](../control-plane/schema.md)
(migrations `0002_repositories` and `0006_repository_lifecycle`).

| Field | |
|---|---|
| `id` | uuid v4 |
| `customer_id` | the owning customer; deleting the customer deletes its repositories |
| `name` | slug, same format as the handle (lowercase, digits, inner hyphens, 1–256); unique per customer |
| `description` | free text, default empty |
| `visibility` | `private` (default), `organization`, or `public` |
| `deleted_at`, `retention_until` | null while live; deletion tombstones the repository for 30 days before its customer store may collect unreferenced data |
| `created_at`, `updated_at` | timestamps |

| Capability | Kind | Input | Errors |
|---|---|---|---|
| `list_repositories` | read | `customer_id?` (narrows to one tenant), `limit?` | — |
| `get_repository` | read | `id` | `404 not_found` |
| `create_repository` | action | `customer_id`, `name`, `description?`, `visibility?` | `400 invalid_input` (unknown customer, bad name, bad visibility), `409 conflict` (name taken within the customer) |
| `rename_repository` | action | `id`, `name` | `404 not_found`, `409 conflict` (tombstoned or name taken) |
| `transfer_repository` | action | `id`, `customer_id` | `400 invalid_input` (unknown destination), `404 not_found`, `409 conflict` (tombstoned or destination name taken) |
| `set_repository_visibility` | action | `id`, `visibility` | `400 invalid_input`, `404 not_found`, `409 conflict` (tombstoned) |
| `delete_repository` | action | `id` | `404 not_found`, `409 conflict` (already tombstoned) |
| `audit` | read | audit filters | [`audit.md`](audit.md), including the lifecycle capability calls |

Every lifecycle capability carries the repository or customer authorization object and its
`write` action. `Registry::call` invokes `authorize(principal, object, action)` before the
handler and appends its signed outcome to `audit_events` through `audit::record`.

## Lifecycle

Rename changes only the repository slug. Transfer moves the repository record to another customer
after checking the destination's name namespace. liquid #160 publishes the sync API and
wire-format document. Lake cannot register its push or pull endpoint until libliquid exposes a
repository over the required customer store and the documented protocol codec at a public boundary;
[`sync.md`](sync.md) records that required follow-up.

Delete is a tombstone, not an immediate erase: `deleted_at` and `retention_until` are stored on the
repository record. The collector may reclaim data after the documented 30-day retention period,
but only when no remaining repository root leases it. A tombstoned repository cannot be renamed,
transferred, or have visibility changed.

The repository detail page at `/repository?id=<id>` presents current lifecycle state. The signed
audit page at `/audit` exposes the same capability outcomes. All mutations remain capabilities,
so REST, OpenAPI, and MCP expose the same operations.

```sh
H='Authorization: Bearer <token>'
curl -H "$H" -H 'content-type: application/json' \
  -d '{"customer_id":"<customer id>","name":"liquid","description":"the tool","visibility":"public"}' \
  https://lake.oramadev.net/v1/create_repository
curl -H "$H" 'https://lake.oramadev.net/v1/list_repositories?customer_id=<customer id>&limit=20'
curl -H "$H" -H 'content-type: application/json' \
  -d '{"id":"<repository id>","visibility":"organization"}' \
  https://lake.oramadev.net/v1/set_repository_visibility
curl -H "$H" -H 'content-type: application/json' \
  -d '{"id":"<repository id>","name":"liquid-next"}' \
  https://lake.oramadev.net/v1/rename_repository
```

The UI lists them at `/repositories` ([`../ui/README.md`](../ui/README.md)); the detail page
displays the tombstone state. Repository content uses the libliquid customer store. The published
sync contract is mirrored in [`sync.md`](sync.md); its lake endpoints await the public
server-facing bindings described there.

## Verify

Run `ctest --test-dir build --output-on-failure`. The repository lifecycle tests create two
customers, change visibility, rename, transfer, tombstone, verify policy denial, and verify the
signed audit stream contains the deletion outcome. For Postgres, apply `db/schema.sql` through
the operator’s `PostgresSchema` before running the integration test.

## Change

Lifecycle changes update the `0006_repository_lifecycle` section of `db/schema.sql`, the
byte-identical operator schema asset, this page, `docs/control-plane/schema.md`, and the repository
page renderer in one change. Do not shorten the retention window without a migration and a
documented collection policy.
