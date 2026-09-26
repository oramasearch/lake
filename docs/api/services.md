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
(instances × storage), the operator's phase and message, and the in-cluster address. Zetta
services come first; the customer's workspace is marked *(workspace)*. The page refreshes every 3
seconds while a service is changing. A service the operator reports Ready shows *Connecting* until
its connection has reached lake's mount; the operator makes kubelet refresh the mount as soon as it
writes the connection (orama-operator#183), so this lasts seconds, not the minute it used to.

- **Expected start times.** The kind menu says how long each kind usually takes, from *Create*
  to usable, and a starting service shows that time, the operator's current step (*waiting for the
  Postgres instances*, *delivering the connection*, …), how long ago it started, and a note once it
  has taken more than twice as long. The times are `control::service_kinds[].ready_seconds`: the
  median of `tools/e2e/start-times.py` runs (lake#197), which follows each phase: the operator
  noticing the row (lake `NOTIFY`s it), the pods' own Ready transition, the operator's Ready, and
  lake reading the connection. The measured table is below under *Start times*; re-measure when
  node size, images or operators change.
- **New service:** kind (Zetta first and preselected, then the rest by name; no kind is marked
  preview — decision 47), name (lowercase letters, digits and `-`, starting with a letter), 1–3
  instances, 1–100 GiB of storage. A space may run any number of services unless the operator
  of the lake sets `LAKE_SERVICES_PER_CUSTOMER`; the page states a limit only when one is set.
- **Show connection** (every kind but Zetta, once Ready): answers with the page itself, showing
  every field the operator delivered for that kind (a Postgres URI, Kafka's bootstrap and SASL
  settings, and so on) once. It is never put in a URL, a redirect or the list.
- **Delete:** type the service's name. When a Train → Data connection uses the service, the form says so. The operator deletes the cluster and its data, which cannot
  be recovered; the row shows *Deleting* until it is gone.

## Capabilities

All are in lake's registry (REST, OpenAPI, MCP) and authorized on the customer's policy object.

| Capability | Kind | Action on the customer | What it does |
|---|---|---|---|
| `list_service_kinds` | read | the caller | The catalog, Zetta first: each kind's key, label and `ready_seconds` |
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
| `LAKE_SERVICES_PER_CUSTOMER` | optional | Live services per customer; default `0`, no limit (Steve, 2026-09-26) |

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

## Start times

Measured with `tools/e2e/start-times.py <base-url> <token-file> <customer-id> <runs> <kind…>`
(kubectl and lake's API, no browser): it creates each kind, follows `list_services` every second,
reads the pods' Ready transition from Kubernetes, deletes the service, and prints each phase from
*Create* — `seen` (the operator's first status), `pods` (last pod Ready), `ready` (the operator's
Ready), `connected` (lake reads the connection) — with the operator's messages, then the median and
p90 per kind. `ready_seconds` is the median of `connected`.

Measured 2026-09-26 on lake.oramadev.net (`3dbbfcd`, operator `main-375debd`), one AKS node,
one service at a time, instances 1, 1 GiB. Seconds from *Create*:

| Kind | Operator saw it | Pods Ready | Operator Ready | Usable (`connected`) | `ready_seconds` | Before lake#197 |
|---|---|---|---|---|---|---|
| Zetta | 1.7 | 38.5 | 41.4 | 41.4 | 30 (Steve, 2026-09-26) | — |
| Postgres | 1.7 | 40.2 | 43.7 | 43.7 | 45 | ~2 min |
| SQLite | 1.7 | 21.5 | 24.0 | 24.0 | 25 | 52–123 |
| DuckDB | 1.7 | 23.5 | 26.5 | 26.5 | 30 | 62–113 |
| Polars | 1.7 | 32.2 | 34.0 | 34.0 | 35 | 123–124 |
| Valkey | 1.7 | — | 40.6 | 41.9 | 45 | ~1.3 min |
| Kafka | 2.4 | 86.6 | 90.2 | 90.2 | 95 | 2 min |
| ClickHouse | 2.4 | — | 71.9 | 71.9 | 75 | 2.5 min |
| TiDB | 1.9 | 98.4 | 101.4 | 101.4 | 105 | 3 min |
| Cassandra | — | — | — | — | 420 (not yet measured) | 8 min |

The operator now sees a new service within about 2 s (lake `NOTIFY`), and lake reads the
connection as soon as the operator reports Ready (it makes kubelet refresh lake's mount), so what
remains is the service's own start. The script finds pods by name prefix; Valkey's and
ClickHouse's operators name theirs differently, so their pod column is empty.

**One sample each.** Rounds 2 and 3 were stopped: the Cassandra run needed more than the one
node had free, AKS added a node, and a scale-up is not a service's start time. Cassandra is left
at 420 s until it is measured on a node with room for it. Re-measure (three runs, median) when the
cluster is quiet, and whenever node size, images or operators change.

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
