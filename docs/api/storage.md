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

**Train → Data** (`/train/data`) ends with a **Storage** panel. It shows whether or not the space's
workspace is ready: stores do not need the workspace.

- **The table:** name, type (*Volume*, *Bucket*, …), provider (*cluster disk*, …), size in GiB, the
  status with the operator's message under it (for a volume: the claim binds when a service first
  mounts it), the endpoint, and the actions. A store with no status yet reads *Waiting for the
  operator*; a Ready one whose connection has not reached lake's mount reads *Connecting*; a
  withdrawn one reads *Deleting* until the operator has removed it, then it leaves the list. The
  page checks again every 3 seconds while any store is changing.
- **Show connection** (once Ready): answers with the Data page itself, showing every field the
  operator delivered (for a volume: `claim`, `namespace`, `size_gi`, `storage_class`) once. It is
  never put in a URL, a redirect or the list, and lake keeps no copy.
- **Delete:** inside a *Delete* disclosure, a warning that deleting a store deletes the data in it,
  and a field where the person types the store's name. The operator deletes the resource (for a
  volume, the claim and its data), which cannot be recovered.
- **New Storage:** one *Type and provider* select holding every pair (`value="volume:cluster"`);
  unavailable pairs are `disabled` and their label says why (*Bucket — Cloudflare R2 — not yet:
  needs R2 enabled on Orama's Cloudflare account …*). Then a name (lowercase letters, digits and
  `-`, starting with a letter, at most 31 characters) and a size in GiB (1–100, default 10). A store
  is private: nothing on the page or in the API makes one public in phase 1.

Form routes, each carrying `customer_id`: `POST /train/data/store-create` (`store`, `name`,
`size_gi`), `POST /train/data/store-delete` (`store_id`, `confirm`), and
`POST /train/data/store-connection` (`store_id`). A failure returns to the Data page with the
reason.

## Capabilities

All are in lake's registry (REST, OpenAPI, MCP; the *Storage* group in `llms.txt`, the skill and the
MCP page) and authorized on the customer's policy object.

| Capability | Kind | Action on the customer | What it does |
|---|---|---|---|
| `list_store_providers` | read | `read` (checked by the handler, as every `list_` capability) | `{providers: [{type, provider, label, available, unavailable_reason?}]}`, the provider table `control::store_providers` in order: the available pair first |
| `list_stores` | read | `read` (checked by the handler) | `{stores: [...]}`, oldest first; each store carries `id, customer_id, type, provider, name, slug, size_gi, region, public, desired, generation, phase, message, endpoint, observed_generation, created_at, connected` and never a credential. A withdrawn store the operator has marked `Deleted` is history and is left out |
| `create_store` | action | `write` | `{type, provider, name, size_gi?, region?}`; `size_gi` 1–100 (default 10). A pair that is not in the table, or not available, is `invalid_input` naming the reason; a live name is `conflict`. The slug is `st-` and the first 12 hex digits of the id, never reused |
| `delete_store` | action | `admin` | `{store_id, confirm}`, `confirm` equal to the store's name; marks it absent and bumps its generation |
| `store_connection` | action | `admin` | A Ready store's connection: what the operator wrote to `<slug>.json` beside the services' connections. Refused (`workspace_not_ready`) before Ready, while the connection is still reaching lake, and for a withdrawn store |

Audit kinds: `store.create`, `store.delete`, `store.connection`, each with the customer, store id,
type, provider and name. The Postgres store sends `NOTIFY lake_services, '<store id>'` in the
transaction of every create and withdrawal, so the operator reconciles at once.

The worked flow *Create a volume* in `llms.txt` and the skill: `list_store_providers` →
`create_store` (`type: "volume"`, `provider: "cluster"`) → `list_stores` until `Ready` →
`store_connection`.

## Configuration

Stores use the services configuration: `LAKE_SERVICE_DIR` (the mounted Secret
`lake-<lake>-services`) holds each Ready store's `<slug>.json`. Without it the Data page says
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
- `Workspaces.DataPageShowsStorageWithTypedDeleteAndRefreshesWhileChanging`: the panel before the
  workspace is ready, the disabled options with their reasons, typed-confirmation delete, the 3 s
  refresh only while a store is changing, and the connection on the response page.
- `Workspaces.EveryNewFeatureIsInOpenApiAndWorksOverMcp`: the five capabilities in OpenAPI and as
  MCP tools, a volume created over MCP.
- `PostgresStores.CreateListWithdrawAndNotifyFollowTheContract`: the store against a real database
  — `spec` as `{size_gi, region, public}`, conflict, withdrawal, a `LISTEN lake_services` connection
  receiving each id, and `store_status` joined in with `Deleted` history hidden.

On a deployment with the operator's store reconciler: on Train → Data, create a volume, see it
Ready with *Show connection*, and delete it by typing its name.

## Changing it

- **A new pair becomes available:** set `available = true` and clear its reason in
  `control::store_providers` (`include/lake/control/store.hpp`) once the operator reconciles it;
  the capabilities, the select and the docs read the table. Move it up among the available ones.
- **A new type or provider key:** the schema's `store_type_check` / `store_provider_check` must list
  it first — a schema change, copied byte-identically into the operator.
