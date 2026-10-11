# Storage: a customer's buckets, filesystems, network filesystems and volumes

**Status: phase 1 implemented 2026-09-26 (lake#196).** A customer's **stores** follow the
managed-services model ([`services.md`](services.md)): lake records the store the customer wants,
the Orama operator makes it exist and reports back. The contract between them — the type ×
provider model, the `stores` and `store_status` tables (migration `0028_stores`), the `NOTIFY`, the
connection in the services Secret — is [`../control-plane/storage.md`](../control-plane/storage.md).

Phase 1 offers one pair, a **cluster volume** (`volume` / `cluster`: a PersistentVolumeClaim on
the StorageClass the Lake's services use). Every other pair is listed, not offered, with the reason
it waits; phases 2–4 add them.

## What a person sees

**Data → Drives → Storage** (`/drives?tab=storage`, lake#656; it was a panel at the end of Data →
Datasets until then, and `/train/data#storage` still leads there) lists the workspace's stores. It
shows whether or not the workspace is ready: stores do not need the workspace. The Lake Console
says *storage* for a store and *drive* for where people work with files; "store" is the API's word.
The page is described in [../ui/drives.md](../ui/drives.md#storage).

- **The figures:** how many stores (or how many are changing), how many open as a drive, the
  volumes, and the GiB asked for in all.
- **The table:** name, type (*Volume*, *Bucket*, …), provider (*cluster disk*, …), size in GiB,
  this month's volume time (`usage_over_time`, dimension `volumes`, by object, linking to Usage),
  the status with the operator's message under it (for a volume: the claim binds when a service first
  mounts it), the endpoint, and the actions. A store with no status yet reads *Waiting for the
  operator*; a Ready one whose connection has not reached lake's mount reads *Connecting*; a
  withdrawn one reads *Deleting* until the operator has removed it, then it leaves the list. The
  page checks again every 3 seconds while any store is changing.
- **Open as a drive** (a Ready bucket or filesystem): a drive with `storage: store` over it
  ([drives.md](drives.md#store-drives)), browsed in place. A volume reads *Shown, not browsed*:
  its contents belong to the service that mounts it. No bucket or filesystem provider is offered
  or browsable yet ([lake#664](https://github.com/oramasearch/lake/issues/664)). Which services and
  apps mount a store, and its used space, are not recorded yet
  ([lake#666](https://github.com/oramasearch/lake/issues/666)).
- **Show connection** (once Ready): answers with the Data page itself, showing every field the
  operator delivered (for a volume: `claim`, `namespace`, `size_gi`, `storage_class`) once. It is
  never put in a URL, a redirect or the list, and lake keeps no copy.
- **Delete:** a dialog with a warning that deleting a store deletes the data in it,
  and a field where the person types the store's name. The operator deletes the resource (for a
  volume, the claim and its data), which cannot be recovered.
- **New Storage:** one *Type and provider* select holding every pair (`value="volume:cluster"`);
  unavailable pairs are `disabled` and their label says why (*Bucket — Cloudflare R2 — not yet:
  needs R2 enabled on Orama's Cloudflare account …*). Then a name (lowercase letters, digits and
  `-`, starting with a letter, at most 31 characters) and a size in GiB (1–100, default 10). A store
  is private: nothing on the page or in the API makes one public in phase 1.

A store belongs to the workspace it was created in (lake#465, migration `0044_store_workspaces`):
the panel lists the current workspace's stores, and the operator places each store in its
workspace's namespace.

The web app's Storage tab calls `create_store`, `delete_store` (with `confirm`) and
`store_connection` under the workspace's `/w/<id>/` path; a failure is shown with its reason. The
server form routes were retired in lake#561.

## Capabilities

All are in lake's registry (REST, OpenAPI, MCP; the *Storage* group in `llms.txt`, the skill and the
MCP page) and authorized on the customer's policy object. Every capability except
`list_store_providers` also requires `workspace_id`, supplied by the `/w/<id>/` URL or an agent
task's workspace when the input leaves it out, and must name a live workspace of the account. A store
of another workspace is `not_found`.

| Capability | Kind | Action on the customer | What it does |
|---|---|---|---|
| `list_store_providers` | read | `read` (checked by the handler, as every `list_` capability) | `{providers: [{type, provider, label, available, unavailable_reason?}]}`, the provider table `control::store_providers` in order: the available pair first |
| `list_stores` | read | `read` (checked by the handler) | `{stores: [...]}`, oldest first; each store carries `id, customer_id, workspace_id, type, provider, name, slug, size_gi, region, public, desired, generation, phase, message, endpoint, observed_generation, created_at, connected` and never a credential. A withdrawn store the operator has marked `Deleted` is history and is left out |
| `create_store` | action | `write` | `{type, provider, name, size_gi?, region?}`, recorded in the workspace; `size_gi` 1–100 (default 10). A pair that is not in the table, or not available, is `invalid_input` naming the reason; a live name is `conflict`. The slug is `st-` and the first 12 hex digits of the id, never reused |
| `delete_store` | action | `admin` | `{store_id, confirm}`, `confirm` equal to the store's name; marks it absent and bumps its generation |
| `store_connection` | action | `admin` | A Ready store's connection: what the operator wrote to `<slug>.json` beside the services' connections. Refused (`workspace_not_ready`) before Ready, while the connection is still reaching lake, and for a withdrawn store |

Audit kinds: `store.create`, `store.delete`, `store.connection`, each with the customer, workspace,
store id, type, provider and name. The Postgres store sends `NOTIFY lake_services, '<store id>'` in the
transaction of every create and withdrawal, so the operator reconciles at once.

The worked flow *Create a volume* in `llms.txt` and the skill: `list_store_providers` →
`create_store` (`type: "volume"`, `provider: "cluster"`) → `list_stores` until `Ready` →
`store_connection`.

## Configuration

Stores use the services configuration: `LAKE_SERVICE_DIR` (the mounted Secret
`lake-<lake>-services`) holds each Ready store's `<slug>.json`. Without it the Storage tab says
Design, Train and Ship are not available, and the store capabilities answer `store_unavailable`.

## Verify

```sh
ctest --test-dir build -R 'Stor'
```

- `Workspaces.StoresAreNamedPerCustomerAndOnlyAvailablePairsAreCreated`: the provider table, the
  slug, private by default, name conflict, unavailable and unknown pairs refused, another customer
  refused, withdrawal and a `Deleted` store leaving the list.
- `Workspaces.StoreCapabilitiesRefuseUnavailablePairsAndCheckTheName`: authorization on the
  customer, an S3 bucket refused with its reason, size bounds, a mismatched `confirm` deleting
  nothing, and the connection only once Ready and delivered.
- `Workspaces.EveryNewFeatureIsInOpenApiAndWorksOverMcp`: the five capabilities in OpenAPI and as
  MCP tools, a volume created over MCP.
- `PostgresStores.CreateListWithdrawAndNotifyFollowTheContract`: the store against a real database
  — the routing trigger refusing a store without a workspace, the workspace filter, `spec` as
  `{size_gi, region, public}`, conflict, withdrawal, a `LISTEN lake_services` connection receiving
  each id, and `store_status` joined in with `Deleted` history hidden.
- `PostgresStores.MigrationAssignsTheSoleWorkspaceAndRecordsTheRest`: migration 0044 run twice over
  stores without a workspace — the sole workspace assigned, a two-workspace account recorded as
  `needs_selection`, and the workspace immutable afterwards.
- `OperatorContract.*`: the operator's own SQL against lake's schema (see
  [`../control-plane/storage.md`](../control-plane/storage.md#verify)).

On a deployment with the operator's store reconciler: on Data → Drives → Storage, create a volume, see it
Ready with *Show connection*, and delete it by typing its name.

## Changing it

- **A new pair becomes available:** set `available = true` and clear its reason in
  `control::store_providers` (`include/lake/control/store.hpp`) once the operator reconciles it;
  the capabilities, the select and the docs read the table. Move it up among the available ones.
- **A new type or provider key:** the schema's `store_type_check` / `store_provider_check` must list
  it first — a schema change, copied byte-identically into the operator.
