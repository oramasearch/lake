# Managed services: the lake ↔ operator contract

**Status: implemented in lake 2026-09-25 (liquid-bridge decision 46); the operator side is orama-operator `docs/lake/services.md`.** A customer runs any number of
services on the Lake's cluster — Zetta clusters and Postgres clusters today. lake records what the
customer wants in its **control Postgres**; the Orama operator reads it, creates one Kubernetes
resource per service (an `OramaZettaCluster`, or a CloudNativePG `Cluster` that the CloudNativePG
operator then runs), and writes back what it observed. lake never talks to Kubernetes, and the
operator never decides what a customer may have.

The customer's **workspace** — the Zetta cluster behind Design, Train and Ship
([`../api/workspaces.md`](../api/workspaces.md)) — is the customer's **default Zetta service**.
This supersedes decision 45's request stream in `orama_control.workspaces` on the data-plane
Zetta.

## Tables (migration `0024_services`, `db/schema.sql`)

`services` — written by lake only.

| Column | Meaning |
|---|---|
| `id` | uuid, the service's identity everywhere (label `orama.cloud/service-id`) |
| `customer_id` | the owning customer |
| `kind` | `zetta` or `postgres` |
| `name` | the customer's name for it, `^[a-z][a-z0-9-]{0,30}$`, unique per customer among live services |
| `slug` | Kubernetes suffix, `svc-<12 hex of id>`, or `ws-<12 hex of customer id>` for a default workspace; never reused |
| `is_default` | the customer's workspace (Zetta only; at most one live per customer) |
| `spec` | JSON object: `instances` (1–3, default 1), `storage_gi` (1–100, default 10) |
| `desired` | `present` or `absent` (deletion requested) |
| `generation` | incremented by lake on every change to `spec` or `desired` |

`service_status` — written by the operator only, one row per service, upserted on change.

| Column | Meaning |
|---|---|
| `service_id` | the service |
| `phase` | `Provisioning`, `Ready`, `Failed`, `Deleting`, `Deleted` |
| `message` | human-readable detail; for a quota refusal exactly `service quota reached (<n>)` |
| `endpoint` | in-cluster address: `http://…:4800` for Zetta, `<host>:5432` for Postgres |
| `observed_generation` | the `generation` the phase describes |

A row with `desired = 'absent'` and phase `Deleted` is history: lake hides it and the operator
skips it.

## What the operator does

For each `Lake` with `spec.services.enabled` (or the deprecated `spec.workspaces.enabled`):

1. **Connect** with pgx using the CloudNativePG application Secret `<lake>-controldb-app`, key
   `uri`, and read every service joined to its status.
2. **Present** services: create or update `<lake>-<slug>` in the Lake's namespace, controlled by
   the Lake, labelled `orama.cloud/lake`, `orama.cloud/service-id`, `orama.cloud/service-kind`,
   annotated with the customer id.
   - `zetta`: an `OramaZettaCluster` built as the workspace reconciler builds it today, image
     `spec.services.zettaImage` (falling back to `spec.workspaces.image`), `instances` replicas,
     `storage_gi` volume.
   - `postgres`: a CloudNativePG `Cluster` with `instances`, `storage_gi`, and `initdb` database
     `app` owned by `app`.
   - **Adoption:** a `<lake>-ws-…` `OramaZettaCluster` created under decision 45 (controlled by the
     Lake, `orama.cloud/workspace-id` equal to the slug, no service-id label) is adopted by the
     default service with that slug, so an existing workspace keeps its data. Anything else already
     holding the name fails the service with `… exists and is not this Lake's service`.
3. **Credentials:** copy each Ready service's connection into the Secret `lake-<lake>-services`
   (controlled by the Lake), key `<slug>.json`:
   - zetta: `{"kind":"zetta","endpoint":…,"token":<admin token>}`
   - postgres: `{"kind":"postgres","host":…,"port":5432,"database":"app","user":"app","password":…,"uri":…}`
     from CloudNativePG's `<cluster>-app` Secret.
   The Secret is mounted in the lake pod at `/var/run/lake/services`, with
   `LAKE_SERVICE_DIR=/var/run/lake/services`.
4. **Status:** upsert `service_status` on every phase, message or endpoint change. Ready needs the
   resource Ready *and* the credential written.
5. **Absent** services: phase `Deleting`; for Zetta delete the data PVCs, then the cluster; for
   Postgres delete the CloudNativePG `Cluster` (its PVCs are owned by it); remove the Secret key;
   write `Deleted` once nothing remains. Only a resource that passes the ownership rule above is
   ever deleted.
6. **Quota:** `spec.services.maxServices` (0 means no limit; falls back to
   `spec.workspaces.maxWorkspaces`) caps the live services on the Lake. A new service over it is
   `Failed` / `service quota reached (<n>)` and starts once there is room. Existing services are
   never refused.

## What lake does

- Capabilities `list_services`, `create_service`, `delete_service` and `service_connection`
  ([`../api/services.md`](../api/services.md)), authorized on the customer. lake caps each
  customer at `LAKE_SERVICES_PER_CUSTOMER` live services (default 5).
- The `/services` page lists every service of a customer with its phase and endpoint, creates and
  deletes them.
- Design, Train and Ship call the customer's default Zetta service at its `endpoint` with the token
  from `$LAKE_SERVICE_DIR/<slug>.json`, read per call and never stored.

## Verify

- lake: `ctest --test-dir build -R 'Services|Workspaces'`.
- operator: `go test ./internal/controller/ -run Services`.
- On AKS: create a service on `/services`, then
  `kubectl -n lake get oramazettacluster,clusters.postgresql.cnpg.io -l orama.cloud/lake=prod`
  shows `prod-<slug>`, and the page turns Ready.

## Known limits

- Endpoints are in-cluster only: nothing outside the cluster reaches a customer's Postgres or
  Zetta. A Postgres service is usable today as a Zetta connection source and from other services.
- `spec` cannot be changed after creation (no resize).
- The operator connects with lake's own database credential; see decision 46 for why a narrower
  role was not worth it yet.
