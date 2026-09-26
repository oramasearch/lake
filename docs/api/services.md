# Services: a customer's managed Zetta, Postgres, Kafka, ClickHouse, TiDB, Cassandra, Valkey and SQLite

**Status: implemented 2026-09-25 (liquid-bridge decision 46).** A customer runs any number of
managed services on the Lake's cluster: **Zetta** clusters (the engine behind Design, Train and
Ship), **Postgres**, **Kafka**, **ClickHouse**, **TiDB**, **Cassandra**, **Valkey** as a
preview, and **SQLite** behind `lake-sqlite`, a small web service of ours (liquid-bridge
decision 47, amended). Each kind is run by its own upstream operator that the Orama
operator drives; which one, and what a service's connection holds, is the catalog table in
[`../control-plane/services.md`](../control-plane/services.md). lake records what the customer wants; the Orama operator creates
and deletes the clusters and reports back. The contract between them — tables, phases, Secrets,
adoption — is [`../control-plane/services.md`](../control-plane/services.md).

## What a person sees

**Ship → Services** (`/services`) lists every service of the selected space: name, kind, size
(instances × storage), the operator's phase and message, and the in-cluster address. The
customer's workspace is marked *(workspace)*. The page refreshes every 15 seconds while a service
is changing. A service the operator reports Ready shows *Connecting* until its connection has
reached lake's mount, which the kubelet does up to a minute after the operator writes it.

- **Expected start times.** The kind menu says how long each kind usually takes to become
  Ready, and a starting service shows that time, how long ago it started, and a note once it has
  taken more than twice as long. The times are `control::service_kinds[].ready_minutes`, rounded up
  from what was measured on lake.oramadev.net on 2026-09-26 on one node:

  | Kind | Measured | Shown |
  |---|---|---|
  | Postgres | ~2 min | about 2 minutes |
  | Zetta | ~1–2 min | about 2 minutes |
  | Kafka | 2 min | about 3 minutes |
  | ClickHouse | 2.5 min | about 3 minutes |
  | TiDB | 3 min | about 4 minutes |
  | Cassandra | 8 min | about 10 minutes |
  | Valkey | 1.3 min | about 2 minutes |
  | SQLite | not yet measured | about 1 minute |

  Re-measure and update the table when node size, images or operators change.
- **New service:** kind (Valkey is marked *preview, not for production*: its upstream operator
  says so), name (lowercase letters, digits and `-`, starting with a letter), 1–3
  instances, 1–100 GiB of storage. The page shows how many services the space may run.
- **Show connection** (every kind but Zetta, once Ready): answers with the page itself, showing
  every field the operator delivered for that kind (a Postgres URI, Kafka's bootstrap and SASL
  settings, and so on) once. It is never put in a URL, a redirect or the list.
- **Delete:** type the service's name. The operator deletes the cluster and its data, which cannot
  be recovered; the row shows *Deleting* until it is gone.

## Capabilities

All are in lake's registry (REST, OpenAPI, MCP) and authorized on the customer's policy object.

| Capability | Kind | Action on the customer | What it does |
|---|---|---|---|
| `list_services` | read | `read` (checked by the handler, as every `list_` capability) | `{services: [...], limit}`; each service carries `id, kind, name, slug, default, instances, storage_gi, desired, generation, phase, message, endpoint, observed_generation, created_at` and never a credential |
| `create_service` | action | `write` | `{kind, name, instances?, storage_gi?}`, `kind` one of `postgres, zetta, kafka, clickhouse, tidb, cassandra, valkey, sqlite` (a SQLite service has exactly one instance); `conflict` for a live name, `quota_exceeded` at `LAKE_SERVICES_PER_CUSTOMER` |
| `delete_service` | action | `admin` | `{service_id, confirm}`, `confirm` equal to the service name; marks it absent |
| `service_connection` | action | `admin` | A Ready service's connection: every field the operator delivered for its kind except `kind` (the catalog table lists them). A Zetta service's admin token is never returned: lake calls Zetta for the customer through `workspace_query` / `workspace_act`, whose allow-lists keep Zetta's administrative surface out of reach |

Audit kinds: `service.create`, `service.delete`, `service.connection`, each with the customer,
service id, kind and name. `start_workspace` and `delete_workspace`
([`workspaces.md`](workspaces.md)) create and delete the default Zetta service.

## Configuration

| Variable | Set by | Meaning |
|---|---|---|
| `LAKE_SERVICE_DIR` | operator | The mounted Secret `lake-<lake>-services`, one `<slug>.json` per Ready service. Without it, Services, Design, Train and Ship say they are not available |
| `LAKE_SERVICES_PER_CUSTOMER` | optional | Live services per customer, default 5; `0` means no limit |

## Verify

```sh
ctest --test-dir build -R 'Workspaces|PostgresServices'
```

`Workspaces.ServicesAreNamedPerCustomerAndCapped`, `…CapabilitiesAreAllowListedAndAuthorizedOnTheCustomer`
and `…ServicesPageListsEveryServiceWithoutCredentials` cover naming, the per-customer cap,
another customer being refused, the connection being Postgres-only and absent from lists, and the
page. `PostgresServices.CreateListAndWithdrawFollowTheContract` runs the store against a real
database: the default created once, the `ws-` slug for the first workspace and never reused, and
withdrawal bumping the generation. On a deployment: create a Postgres service on `/services`, wait
for Ready, and **Show connection**.

## Known limits

- **Valkey is a preview.** Its operator is `v1alpha1` and its upstream says it is not ready for
  production; do not put a customer commitment behind it.
- **No backups yet** for any kind; each operator's backup tooling is future work (decision 47).
- **SQLite has one instance and no replication:** its volume is the only copy until backups
  exist. It is reached over `lake-sqlite`'s HTTP API with a bearer token, not the SQLite file format.
- Addresses are inside the cluster only. A Postgres service is reachable from the customer's other
  services, for example as a Zetta connection on Train → Data, not from the internet.
- A service cannot be resized or renamed.
- Design, Train and Ship use the default Zetta service only.
