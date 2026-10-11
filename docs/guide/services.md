# Services and Storage

Lake runs databases, streams and engines for you on its own cluster: Zetta, Postgres, Kafka,
ClickHouse, TiDB, Cassandra, Valkey, SQLite, DuckDB and Polars. Each service is its own, with its
data kept apart from every other customer's. **Ship → Services** creates them, shows their state
and hands you their connection. **Storage** (on **Data → Datasets**) provisions volumes your
services mount.

Lake records what you ask for; the Orama operator creates and deletes the clusters and reports
back. Calls on this page name `customer_id`, the id of the account that owns your workspaces (yours,
or your organization's), and, where a service or store is created, `workspace_id`.

## Concepts

| Term | What it is |
|---|---|
| **Service** | One managed database, stream or engine of a kind from the catalog, created in a workspace. |
| **Zetta** | Orama's data and AI engine. Every workspace runs on one of its own, the engine behind Design, Train and Ship; it is listed on Services marked as the workspace, and is created and deleted with the workspace, not here. You can also start a Zetta service of your own. |
| **Kind** | `zetta`, `postgres`, `kafka`, `clickhouse`, `tidb`, `cassandra`, `valkey`, `sqlite`, `duckdb` or `polars`. |
| **Connection** | What a client needs to reach a Ready service: a Postgres URI, Kafka's bootstrap and SASL settings, and so on. Shown on request, once, and never put in a page address or a list. |
| **Store** | A volume (a cluster disk) for your services, created in a workspace. |
| **Connect a service** | Make one of your Ready Postgres services a connection of your workspace, so Train can pull datasets from it. |

## Ship → Services

The page opens with figures (**Services**, against your limit when the lake sets one; **Ready**;
**Changing**, checked every 3 seconds; **Failed**), then **Your Services**: a table with a search, a
filter by kind, and per service its name and kind, size (instances and storage), status with the
operator's message, its in-cluster address and its actions. Below it, the **Service Catalog**
shows every kind as a card.

- REST: `GET /v1/list_services?customer_id=<customer id>`
- REST: `GET /v1/list_service_kinds`
- MCP: `list_services`, `list_service_kinds`.

### Create a Service

1. Open **Ship → Services** and press **New service**, or press a kind's card in the **Service
   Catalog**.
2. Choose the **Kind** (Zetta is first), and fill in **Name** (lowercase letters, digits and dashes,
   starting with a letter), **Instances** (1 to 3) and **Storage (GiB)** (1 to 100).
3. Press **Create service**. The service is listed and its status follows the operator: waiting for
   the operator, the operator's own steps, *Connecting* while its connection reaches lake, then
   **Ready**.

A service is created in the workspace in the page address; with no workspace chosen, the drawer asks
you to choose or create one first.

- REST: `POST /v1/create_service`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "kind": "postgres", "name": "crm", "instances": 1, "storage_gi": 10}
```

- MCP: `create_service` with the same input. `instances` defaults to 1 and `storage_gi` to 10. A
  SQLite service has exactly one instance.

### Show a Service's Connection

Once a service (other than Zetta) is Ready, press **Show connection** on its row. The dialog shows
every field the operator delivered, each with a copy button. Close it and the connection is gone
from the page.

- REST: `POST /v1/service_connection` (an action, so the password never rides a URL):

```json
{"customer_id": "<customer id>", "service_id": "<service id>"}
```

- MCP: `service_connection`.

A Zetta service's admin token is never shown. Lake reaches your workspace's engine for you through
`workspace_query`, `workspace_act` and `workspace_sql`.

Addresses are inside the cluster only: a service is reachable from your other services, your apps
and your workspace, not from the internet.

### Use a Postgres Service as a Data Source

1. Open **Data → Datasets**.
2. Under **Connect One of Your Services**, press **Connect** next to a Ready Postgres service (or
   **Refresh** to renew an existing connection). Nothing is typed: the workspace uses the credential
   lake already holds.
3. Pull tables from the new connection into datasets ([Datasets and Loading Data](/docs/load-data)).

- REST: `POST /v1/connect_service`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "service_id": "<service id>"}
```

- MCP: `connect_service`. `name` names the connection (default: the service's name). Other kinds
  are refused: the workspace reads Postgres today.

### Bind a Service to an App

An app reaches a service through a binding: on the app's **Services** tab, **Attach service**, or
**Create and attach** for a new one ([Apps](/docs/apps#bindings-workspace-data-and-services)).

### Delete a Service

1. Press **Delete** on the service's row.
2. Type the service's name in the field and press **Delete <name>**. When a Datasets connection uses
   the service, the dialog says so.

The service and all of its data are deleted, which cannot be recovered. The row reads *Deleting*
until the operator has removed it.

- REST: `POST /v1/delete_service`:

```json
{"customer_id": "<customer id>", "service_id": "<service id>", "confirm": "crm"}
```

- MCP: `delete_service`. A `confirm` that is not the exact name deletes nothing.

Your workspace's own engine is not deleted here: deleting the workspace deletes it
([Workspaces](/docs/workspaces)).

## Storage

**Data → Datasets** ends with the **Storage** card, *Buckets, Filesystems and Volumes*: storage for
your services, private to the workspace. It lists each store's name, type, provider, size, status
with the operator's message, and its actions. It works whether or not the workspace is ready.

Today one kind is offered: a **volume on the cluster disk**. Buckets and shared filesystems are
listed in the form and disabled, each with the reason; they cannot be created.

- REST: `GET /v1/list_stores?customer_id=<customer id>&workspace_id=<workspace id>`
- REST: `GET /v1/list_store_providers?customer_id=<customer id>`
- MCP: `list_stores`, `list_store_providers`.

### Create a Volume

1. On **Data → Datasets**, in the **Storage** card, press **New storage**.
2. Keep **Type and provider** on *Volume — cluster disk*, fill in **Name** (lowercase letters,
   digits and `-`, starting with a letter, at most 31 characters) and **Size (GiB)** (1 to 100,
   default 10).
3. Press **Create storage**. The store reads *Waiting for the operator*, then its status; a volume's
   claim binds when a service first mounts it.

- REST: `POST /v1/create_store`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "type": "volume", "provider": "cluster", "name": "reports-data", "size_gi": 10}
```

- MCP: `create_store`. A store is private; nothing makes one public.

### Show a Store's Connection

Once the store is Ready, press **Show connection**: the dialog shows what a service in your workspace
mounts to use it (for a volume: `claim`, `namespace`, `size_gi`, `storage_class`). It is not shown
again unless you ask.

- REST: `POST /v1/store_connection`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "store_id": "<store id>"}
```

- MCP: `store_connection`.

### Delete a Store

Press **Delete** on the store's row, type its name and press **Delete <name>**. Deleting a store
deletes the data in it, which cannot be recovered.

- REST: `POST /v1/delete_store`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "store_id": "<store id>", "confirm": "reports-data"}
```

- MCP: `delete_store`.

## Limits

- Instances 1 to 3 (SQLite exactly 1); storage 1 to 100 GiB per service and per store.
- A lake's operator may cap the number of services per account; the figures then say how many your
  workspaces may run at once, and past it `create_service` answers `quota_exceeded`.
- A service cannot be resized or renamed.
- There are no backups yet for any kind. A SQLite service has one instance and no replication, so
  its volume is its only copy; it is reached over HTTP with a bearer token, not as a SQLite file.
- Valkey is a preview: its operator is not ready for production use.
- Design, Train and Ship use the workspace's own engine only.
- Only volumes on the cluster disk can be created as storage.

## Permissions

| Step | Needs |
|---|---|
| See services, kinds and stores | `read` on the account that owns the workspace |
| Create a service or store, connect a service to the workspace | `write` on the account |
| Show a connection, delete a service or store | `admin` on the account |

## Troubleshooting

| You see | What it means | What to do |
|---|---|---|
| *Waiting for the operator* | The operator has not reported on it yet. | Wait; the page checks again every 3 seconds. |
| *Connecting* | The service is Ready, and its connection is on its way to lake. | Wait for **Ready**; **Show connection** appears then. |
| *Unassigned* | The service belongs to no workspace, so the operator no longer reconciles it. | Ask the lake's administrator to assign or remove it. |
| *Deleting* for a long time | The operator has not finished removing it; the row says how long it has waited. | Ask the lake's administrator. |
| *Failed* | The operator could not create it; its message is under the status. | Delete it and create it again, or ask the lake's administrator. |
| `conflict` | A live service or store already has that name. | Choose another name. |
| `quota_exceeded` | The account runs as many services as the lake allows. | Delete one you no longer need. |
| `workspace_not_ready` on a store's connection | The store is not Ready yet, or is being deleted. | Wait for **Ready**. |
| `invalid_input` on `create_store` | The type and provider pair is not offered. | Use `volume` and `cluster`. |
| `store_unavailable` | Services are not configured on this lake. | Ask the lake's operator. |
| *The workspace can't read … yet* next to a service | Only Postgres can be connected to the workspace today. | Use a Postgres service as a data source. |

## Related Pages

- [Datasets and Loading Data](/docs/load-data): connections and pulling tables.
- [Apps](/docs/apps): bind a service to an app.
- [Workspaces](/docs/workspaces): your workspace and its engine.
- [Usage and Metering](/docs/usage): what your services use.
- [Grants and Effective Access](/docs/access): who may create and delete.
