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
| `repository_refs` | read | `id` | `{items: [{name, change}]}` |
| `repository_tree` | read | `id`, `ref` | `{items: [{path, kind, blob, mode}]}` |
| `repository_file` | read | `id`, `ref`, `path` | `{path, blob, contents, size}` |
| `list_changes` | read | `id` | `{items: [{id, parents, author, message, occurred_at}]}` |
| `get_change` | read | `id`, `change_id` | change identity and metadata |
| `change_diff` | read | `id`, `change_id` | `{items: [{path, old_path, kind, changes, old_mode, new_mode, old_blob, new_blob, line_hunks, old_conflicts, new_conflicts}]}` |
| `list_conflicts` | read | `id` | `{items: [{path, blobs}]}` |
| `list_artifacts` | read | `id`, `action?`, `output?`, `content?`, `cursor?`, `limit?` | `{items: [result], next_cursor, done}` |
| `get_artifact` | read | `id`, `result_id` | one result with outputs, timings, cache/divergence state, attestations and trace |
| `refs` | read | `repository_id` | `404 not_found`, `409 conflict` (tombstoned) |
| repository sync routes | binary read/action | repository URL and liquid protocol body | liquid protocol errors; see [`sync.md`](sync.md) |
| `audit` | read | audit filters | [`audit.md`](audit.md), including the lifecycle capability calls |

Every lifecycle capability carries the repository or customer authorization object and its
`write` action. `Registry::call` invokes `authorize(principal, object, action)` before the
handler and appends its signed outcome to `audit_events` through `audit::record`.

`list_repositories` resolves every candidate repository policy object and makes one
`authorize_batch(..., "read", ...)` call before returning the permitted set. Repository content
capabilities authorize the repository before opening its libliquid data. The other candidate
lists follow their policy-object or scoped parent authorization paths, and the registry records
every successful or denied call.

## Repository data and change pages

`repository_refs`, `repository_tree`, `repository_file`, `list_changes`, and `list_conflicts`
read libliquid directly through the lake repository-store adapter. Tree output is recursive;
every file links to its backing blob identity and rendered contents. Conflict records remain
first-class values containing a path and the competing blob references.

`change_diff` compares the selected change with its first parent through libliquid's public
`repository::diff` API. A root change is compared with an empty tree. The result retains every
per-path status, old/new mode and blob digest, move origin, conflict digest, and optional text hunk.
Lake reads the hunk ranges from their blobs so the API and page show escaped removed and added
lines. A change without visible hunks produces the page's empty state.

## Artifact data

`list_artifacts` maps its filters and opaque cursor directly to libliquid's paged
`repository::results` query. The page size is limited to 1–1000. `get_artifact` loads exactly one
result, its output blob records, action-phase timing, cache/divergence state, worker, and timestamps.
It verifies the linked SLSA provenance against the signed result/action, verifies direct result
attestations such as `reproducibility/1` through the registered TAAP profile, and carries each
`sbom/1` report's verification outcome. It scans retained trace pages for the trace containing the
result and returns its wall time, action/dependency totals, and cache summary.

Both capabilities authorize the repository object before opening its customer data. The result
list is marked as candidate-returning and uses the same batched repository authorization path as
the other lists. Anonymous callers may reach it only for a public repository.

## Public repositories

An anonymous caller may call `list_repositories` and the repository read capabilities only for a
repository marked `public`. The repository list omits every other visibility level. A direct read
of a private or organization repository returns `unauthorized`; the HTML routes turn that result
into a sign-in redirect. A signed-in caller continues through the repository policy object.

## Lifecycle

Rename changes only the repository slug. Transfer moves the repository record to another customer
after checking the destination's name namespace. liquid #160 and #165 publish the sync client,
server, and wire contract. Lake exposes that contract at
`/v1/repositories/<repository-id>/sync/*`; [`sync.md`](sync.md) records the routes, binary bodies,
authorization, and atomic-rejection guarantees.

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
sync contract and lake adapter are documented in [`sync.md`](sync.md). Every repository is opened
with `repository_options.store` over the one caller-owned store at
`LAKE_STORE_ROOT/<customer-id>/store`; it does not get a private `.liquid-store`.

## Verify

Run `ctest --test-dir build --output-on-failure`. The repository lifecycle tests create two
customers, change visibility, rename, transfer, tombstone, verify policy denial, and verify the
signed audit stream contains the deletion outcome. For Postgres, apply `db/schema.sql` through
the operator’s `PostgresSchema` before running the integration test. `SyncHttp.*` proves push,
incremental chunk transfer, pull, atomic bad-signature rejection, policy denial, and audit records.
`PostgresSync.*` repeats the exchange with the production control store.

## Change

Lifecycle changes update the `0006_repository_lifecycle` section of `db/schema.sql`, the
byte-identical operator schema asset, this page, `docs/control-plane/schema.md`, and the repository
page renderer in one change. Do not shorten the retention window without a migration and a
documented collection policy.
