# Services: a customer's Zetta and Postgres clusters

**Status: implemented 2026-09-25 (liquid-bridge decision 46).** A customer runs any number of
managed services on the Lake's cluster: **Zetta** clusters (the engine behind Design, Train and
Ship) and **Postgres** clusters. lake records what the customer wants; the Orama operator creates
and deletes the clusters and reports back. The contract between them — tables, phases, Secrets,
adoption — is [`../control-plane/services.md`](../control-plane/services.md).

## What a person sees

**Ship → Services** (`/services`) lists every service of the selected space: name, kind, size
(instances × storage), the operator's phase and message, and the in-cluster address. The
customer's workspace is marked *(workspace)*. The page refreshes every 15 seconds while a service
is changing. A service the operator reports Ready shows *Connecting* until its connection has
reached lake's mount, which the kubelet does up to a minute after the operator writes it.

- **New service:** kind, name (lowercase letters, digits and `-`, starting with a letter), 1–3
  instances, 1–100 GiB of storage. The page shows how many services the space may run.
- **Show connection** (Postgres, once Ready): answers with the page itself, showing the
  connection URI once. It is never put in a URL, a redirect or the list.
- **Delete:** type the service's name. The operator deletes the cluster and its data, which cannot
  be recovered; the row shows *Deleting* until it is gone.

## Capabilities

All are in lake's registry (REST, OpenAPI, MCP) and authorized on the customer's policy object.

| Capability | Kind | Action on the customer | What it does |
|---|---|---|---|
| `list_services` | read | `read` (checked by the handler, as every `list_` capability) | `{services: [...], limit}`; each service carries `id, kind, name, slug, default, instances, storage_gi, desired, generation, phase, message, endpoint, observed_generation, created_at` and never a credential |
| `create_service` | action | `write` | `{kind, name, instances?, storage_gi?}`; `conflict` for a live name, `quota_exceeded` at `LAKE_SERVICES_PER_CUSTOMER` |
| `delete_service` | action | `admin` | `{service_id, confirm}`, `confirm` equal to the service name; marks it absent |
| `service_connection` | action | `admin` | A Ready Postgres service's `host, port, database, user, password, uri`. A Zetta service's admin token is never returned: lake calls Zetta for the customer through `workspace_query` / `workspace_act`, whose allow-lists keep Zetta's administrative surface out of reach |

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

- Addresses are inside the cluster only. A Postgres service is reachable from the customer's other
  services, for example as a Zetta connection on Train → Data, not from the internet.
- A service cannot be resized or renamed.
- Design, Train and Ship use the default Zetta service only.
