# Repositories

A repository is owned by a customer (the tenant) and named within it: the address is
`<customer handle>/<name>`. Schema: [`../control-plane/schema.md`](../control-plane/schema.md)
(migration `0002_repositories`).

| Field | |
|---|---|
| `id` | uuid v4 |
| `customer_id` | the owning customer; deleting the customer deletes its repositories |
| `name` | slug, same format as the handle (lowercase, digits, inner hyphens, 1–256); unique per customer |
| `description` | free text, default empty |
| `visibility` | `private` (default) or `public` |
| `created_at`, `updated_at` | timestamps |

| Capability | Kind | Input | Errors |
|---|---|---|---|
| `list_repositories` | read | `customer_id?` (narrows to one tenant), `limit?` | — |
| `get_repository` | read | `id` | `404 not_found` |
| `create_repository` | action | `customer_id`, `name`, `description?`, `visibility?` | `400 invalid_input` (unknown customer, bad name, bad visibility), `409 conflict` (name taken within the customer) |

```sh
H='Authorization: Bearer <token>'
curl -H "$H" -H 'content-type: application/json' \
  -d '{"customer_id":"<customer id>","name":"liquid","description":"the tool","visibility":"public"}' \
  https://lake.oramadev.net/v1/create_repository
curl -H "$H" 'https://lake.oramadev.net/v1/list_repositories?customer_id=<customer id>&limit=20'
```

The UI lists them at `/repositories` ([`../ui/README.md`](../ui/README.md)). What a repository
*contains* — the liquid store, drips, changes — arrives with liquid Phase 1; today it is the
control-plane record only.
