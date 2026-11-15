# Managed services: the lake ↔ operator contract

**Status: implemented in lake 2026-09-25 (liquid-bridge decision 46); the operator side is orama-operator `docs/lake/services.md`.** A customer runs any number of
services on the Lake's cluster — Zetta clusters and Postgres clusters today. lake records what the
customer wants in its **control Postgres**; the Orama operator reads it, creates one Kubernetes
resource per service (an `OramaZettaCluster`, or a CloudNativePG `Cluster` that the CloudNativePG
operator then runs), and writes back what it observed. lake never talks to Kubernetes, and the
operator never decides what a customer may have.

A customer's workspace is now an explicit [workspace contract row](workspaces.md), with a
primary Zetta service referenced by `zetta_service_id`. `is_default` remains compatibility
metadata, not a data-plane routing choice. Migration 0039 replaces the earlier default-service
workspace model and decision 45's request stream.

## Tables (migration `0024_services`, `db/schema.sql`)

`services` — written by lake only.

| Column | Meaning |
|---|---|
| `id` | uuid, the service's identity everywhere (label `orama.cloud/service-id`) |
| `customer_id` | the owning customer |
| `workspace_id` | owning `workspaces.id`; added by migration 0039 for namespace routing |
| `kind` | `zetta`, `postgres`, `kafka`, `clickhouse`, `tidb`, `cassandra`, `valkey` (migration `0025_service_catalog`) `sqlite` (migration `0026_sqlite_services`), `duckdb` or `polars` (migration `0027_analytics_services`) |
| `name` | the customer's name for it, `^[a-z][a-z0-9-]{0,30}$`, unique per customer among live services |
| `slug` | Kubernetes suffix, `svc-<12 hex of id>`, or `ws-<12 hex of workspace id>` for a primary workspace Zetta |
| `is_default` | legacy compatibility flag (Zetta only; at most one live per customer), not workspace identity |
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

1. **Connect** with pgx using the CloudNativePG application Secret `lake-<lake>-controldb-app`, key
   `uri`, and read every service joined to its workspace namespace and status.
2. **Present** services: create or update `<lake>-<slug>` in the workspace namespace, controlled by
   the Lake, labelled `orama.cloud/lake`, `orama.cloud/service-id`, `orama.cloud/service-kind`,
   annotated with the customer id.
   - `zetta`: an `OramaZettaCluster` built as the workspace reconciler builds it today, image
     `spec.services.zettaImage` (falling back to `spec.workspaces.image`), `instances` replicas,
     `storage_gi` volume.
   - `postgres`: a CloudNativePG `Cluster` with `instances`, `storage_gi`, and `initdb` database
     `app` owned by `app`.
   - **Legacy cutover:** operator#217 deletes the old `ws-*` cluster, volumes and credentials
     in the Lake namespace before isolated recreation. This is explicitly approved for the dev
     cluster; see [the workspace migration](workspaces.md). Ownership checks still apply.
3. **Credentials:** copy each Ready service's connection into the Secret `lake-<lake>-services`
   (controlled by the Lake), key `<slug>.json`:
   - zetta: `{"kind":"zetta","endpoint":…,"token":<admin token>}`
   - postgres: `{"kind":"postgres","host":…,"port":5432,"database":"app","user":"app","password":…,"uri":…}`
     from CloudNativePG's `<cluster>-app` Secret.
   Each connection is also its own Secret, `lake-<lake>-<slug>-connection`, in its workspace
   namespace, read only by the operator. How lake reads a connection depends on the image
   ([Connections on demand](#connections-on-demand)): an image whose `lake manifest` lists
   `LAKE_SERVICE_CONNECTION_URL` asks the operator for one connection at a time and gets no mount;
   an older image gets the aggregate Secret mounted at `/var/run/lake/services`, with
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

## The catalog (decision 47)

Each kind is run by one upstream operator, installed from the operator's `helmfile.yaml` and
watching every namespace. The Orama operator writes that operator's resource, named
`<lake>-<slug>` in the Lake's namespace with the labels and ownership above, and never runs the
database itself. `instances` is 1–3 and `storage_gi` 1–100 for every kind.

| Kind | Upstream operator (chart, pinned) | Resource the Orama operator writes | `instances` means | Ready when | Credential (`<slug>.json`, besides `kind` and `endpoint`) | `endpoint` |
|---|---|---|---|---|---|---|
| `zetta` | the Orama operator itself | `OramaZettaCluster` | nodes | cluster Ready | `token` | `http://<name>-zetta.<ns>.svc.cluster.local:4800` |
| `postgres` | CloudNativePG (`cnpg/cloudnative-pg`) | `postgresql.cnpg.io/v1 Cluster`, `initdb` database `app` owned by `app` | instances | ready instances = instances | `host, port, database, user, password, uri` | `<name>-rw.<ns>.svc.cluster.local:5432` |
| `kafka` | Strimzi 1.2.0 (`strimzi/strimzi-kafka-operator`) | `kafka.strimzi.io` `Kafka` in KRaft mode with one `KafkaNodePool` (roles controller and broker, `instances` replicas, persistent-claim `storage_gi`), an internal listener on 9092 with TLS (Strimzi's cluster CA) and SCRAM-SHA-512 whose NetworkPolicy peer is the owning Lake only (the workspace's own pods reach it through the workspace's policies), `authorization: simple`, the entity operator, and a `KafkaUser` `<name>-app` with SCRAM-SHA-512 and ACLs limited to the cluster's non-internal topics, its groups and transactional ids, with no cluster administration (the user's name is its login and must be unique in the namespace; [orama-operator#770](https://github.com/oramasearch/orama-operator/issues/770)) | brokers | the `Kafka` `Ready` condition, the `KafkaUser` Secret and the cluster CA Secret exist | `bootstrap, username, password, sasl_mechanism` (`SCRAM-SHA-512`), `security_protocol` (`SASL_SSL`), `ca_certificate` (the cluster CA, PEM) | `<name>-kafka-bootstrap.<ns>.svc.cluster.local:9092` |
| `clickhouse` | Altinity 0.27.4 (`altinity/altinity-clickhouse-operator`) | `clickhouse.altinity.com/v1 ClickHouseInstallation`: one shard of `instances` replicas, a volume claim template of `storage_gi`, user `app` with a generated password (SHA-256 in the spec, the plain password only in the credential), and, when `instances` > 1, a `ClickHouseKeeperInstallation` of one node the installation points at | replicas | the installation's status is `Completed` | `host, http_port` (8123), `native_port` (9000), `user, password, database` (`default`) | `clickhouse-<name>.<ns>.svc.cluster.local:8123` (the installation's own service); Ready also waits for the Keeper when there is one |
| `tidb` | PingCAP TiDB Operator v1.6.6 (`pingcap/tidb-operator`, stable 1.x; its CRDs applied from the v1.6.6 release first) | `pingcap.com/v1alpha1 TidbCluster`: PD, TiKV and TiDB with `instances` replicas each, TiKV and PD storage of `storage_gi`, `tidb.initializer` with `createPassword`, which reads the root password from the Secret `<name>-init` (key `root`) the Orama operator creates, and `pvReclaimPolicy: Delete` | replicas of each component | PD, TiKV and TiDB report `Normal` and all their members ready | `host, port` (4000), `user` (`root`), `password`, `uri` (`mysql://…`) | `<name>-tidb.<ns>.svc.cluster.local:4000` |
| `cassandra` | K8ssandra Operator 1.33.0 (`k8ssandra/k8ssandra-operator`, cluster-scoped, needs cert-manager v1.21.2) | `k8ssandra.io/v1alpha1 K8ssandraCluster`, one datacenter of `instances` nodes with `storage_gi` (its datacenter object and Cassandra cluster are named after the slug so two services never collide; the Cassandra datacenter name is `dc1`), Apache Cassandra images only | nodes | the datacenter is `Ready` | `host, port` (9042), `datacenter` (`dc1`), `user, password` (K8ssandra's superuser Secret) | `<slug>-<slug>-service.<ns>.svc.cluster.local:9042` |
| `valkey` **preview** | valkey-io/valkey-operator 0.6.0 (`valkey/valkey-operator`; its README says *not ready for production*, `v1alpha1`) | `valkey.io/v1alpha1 ValkeyCluster`: 3 shards (cluster mode needs three), `instances - 1` replicas per shard, `storage_gi` per node, an ACL user `app` with a generated password, and the built-in `default` user disabled (left alone it accepts unauthenticated clients) | replicas per shard + 1 | the `ValkeyCluster` `Ready` condition | `host, port` (6379), `user, password`, `cluster` (`true`) | the cluster's client service on 6379 |
| `sqlite` | the Orama operator itself, image `oramasearch/lake-sqlite` (orama-operator `cmd/lake-sqlite`) | a `StatefulSet` of one `lake-sqlite` pod with a `storage_gi` volume holding one SQLite database in WAL mode, a `Service`, and a Secret `<name>-credentials` holding a generated bearer token. `instances` must be 1: SQLite has one writer | 1 (always) | the pod is Ready (its `/healthz` opens the database) | `token`, `database` (`main`) | `http://<name>.<ns>.svc.cluster.local:8080` |
| `duckdb` | the Orama operator itself, image `oramasearch/lake-duckdb` (private repo `oramasearch/lake-duckdb`) | a `StatefulSet` of one `lake-duckdb` pod owning one DuckDB file on a `storage_gi` volume, a `Service`, and a Secret `<name>-credentials` holding a generated bearer token. The engine is locked down: `enable_external_access=false`, no extension install or autoload, `lock_configuration=true`, memory and thread limits, a statement timeout | 1 (always) | the pod is Ready (`/healthz` runs `SELECT 1`) | `token`, `database` (`main`) | `http://<name>.<ns>.svc.cluster.local:8080` |
| `polars` | the Orama operator itself, image `oramasearch/lake-polars` (private repo `oramasearch/lake-polars`, Rust, the open-source `polars` crate) | a `StatefulSet` of `instances` `lake-polars` workers sharing nothing, each with a `storage_gi` volume of Parquet datasets, a `Service`, and a Secret `<name>-credentials` holding a generated bearer token | workers (1–3); datasets are per worker in v1, so 1 is the useful value until shared storage (lake#196) | every worker Ready | `token` | `http://<name>.<ns>.svc.cluster.local:8080` |

Exact field names follow each operator's pinned CRD; where the table and the CRD disagree, the CRD
wins and `docs/lake/services.md` in the operator says what was chosen. Every credential also carries
`endpoint`. Each resource's `app.kubernetes.io/instance` label is its own name, not the Lake's:
TiDB builds its pod selectors from that label. SQLite, DuckDB and Polars have no
upstream operator we would use (lake `.claude/skills` → liquid-bridge `/lake-service`, step 1), so
the Orama operator runs each directly behind a small web service of ours in its own private repo:
`lake-sqlite`, `lake-duckdb`, `lake-polars`. `lake-duckdb` has the same API as `lake-sqlite` below.
`lake-polars` is analytics compute, not a database: `POST /v1/datasets/<name>` uploads CSV, JSONL
or Parquet (stored as Parquet), `GET /v1/datasets` lists them, `DELETE /v1/datasets/<name>` removes
one, and `POST /v1/query` `{sql}` runs Polars SQL over them (row cap, timeout, memory limit) and
answers `{columns, rows}`. For `lake-sqlite`, a small web service we wrote
(decision 47, amended). Its API: `POST /v1/query` `{sql, params}` answers `{columns, rows}`;
`POST /v1/exec` answers `{rows_affected, last_insert_id}`; `POST /v1/batch` `{statements: [{sql,
params}]}` runs them in one transaction; `GET /healthz`. Every call but `/healthz` needs
`Authorization: Bearer <token>`. There is no replication; the volume is the only copy until
backups exist.

Deletion deletes the resource and, for kinds whose volumes the upstream operator does not own
(Kafka, TiDB, Cassandra and Valkey keep their PVCs by default), the PVCs carrying that resource's
labels, before writing `Deleted`. Only the Orama operator's own resources are ever touched.

## What lake does

- Capabilities `list_services`, `create_service`, `set_service_stores`, `delete_service` and
  `service_connection`
  (every kind but Zetta, whose admin token lake never hands out)
  ([`../api/services.md`](../api/services.md)), authorized on the customer. lake caps each
  customer at `LAKE_SERVICES_PER_CUSTOMER` live services when it is set; the default, `0`, is no
  limit.
- The `/services` page lists every service of a customer with its phase and endpoint, creates and
  deletes them.
- **`NOTIFY lake_services, '<service id>'`** in the same transaction as every `services` insert and
  every withdrawal (`desired = 'absent'`), so it is delivered on commit. The operator `LISTEN`s and
  reconciles the Lake at once instead of on its next poll (lake#197); the poll remains the
  fallback, so a missed notification only delays.
- **Storage a service mounts** (lake#666): `create_service` and `set_service_stores` write the
  service's rows of `store_mounts` (migration `0080_store_mounts`) in the transaction that bumps
  its `generation` and `NOTIFY`s `lake_services`; `withdraw_service` removes them. Only the kinds
  whose pod template the operator writes itself (`sqlite`, `duckdb`, `polars`,
  `control::store_mounting_service_kinds`) may mount; lake refuses the others. The operator adds
  each to the StatefulSet's pod template ([storage.md](storage.md#mounts)).
- Design, Train and Ship call the customer's default Zetta service at its `endpoint` with the token
  from its connection ([Connections on demand](#connections-on-demand)), asked of the operator and
  kept in memory for 30 s, or, on an older operator, read per call from
  `$LAKE_SERVICE_DIR/<slug>.json`. Never stored.

## Connections on demand

[orama-operator#773](https://github.com/oramasearch/orama-operator/issues/773). The aggregate
Secret `lake-<lake>-services` put every tenant's database password in one file tree inside the
lake process, so one path traversal or file read there read them all. lake now asks the operator
for one connection when it needs one instead.

| Variable | Set by | What it is |
|---|---|---|
| `LAKE_SERVICE_CONNECTION_URL` | operator | The control plane's `POST /v1/service_connection`, an https URL. Set, it enables managed services and workspaces and the mount is not read |
| `LAKE_SERVICE_CONNECTION_TOKEN_FILE` | operator | The file holding this Lake's own credential (`lake-<lake>-connections`, prefix `lake_conn_`). It grants only this operation, for this Lake's services; it is neither the admin token nor the deploy token |
| `LAKE_SERVICE_CONNECTION_CA_FILE` | operator | The control plane's CA certificate (PEM), the only root the call trusts |

- **The call.** `POST <url>` with `{"service":"<slug>"}`, `Authorization: Bearer <token>` and the
  CA as the only trusted root, within 10 s. The answer is `{"service":"<slug>","connection":{…}}`,
  the same JSON as `<slug>.json`. An answer for another slug is not used. Both files are read on
  every call, so a rotated credential or CA is used at once. Without the CA file nothing is sent:
  the call never falls back to the system roots.
- **Memory only.** A connection is kept in memory for 30 s (`service_connection_ttl`), and an
  answer that there is none (`404`) for 5 s, so listing a page of services does not ask once per
  row per request. Nothing is written to disk, logged, or put in an error message.
- **Failures.** When the operator cannot be reached or fails (a transport error, a `5xx`), a
  connection fetched in the last 5 minutes (`service_connection_stale_limit`) is still used, so a
  roll of the operator does not take every workspace offline; otherwise there is none and the
  service shows as not connected. A refused credential (`401`, `403`) drops what was kept.
- **The switch.** The operator renders these variables, and stops mounting `lake-<lake>-services`,
  only for an image whose `lake manifest` `environment` lists `LAKE_SERVICE_CONNECTION_URL`
  (`status.imageManifest.serviceConnectionsOnDemand`). An image on an older operator still gets
  `LAKE_SERVICE_DIR` and reads the mount as before. **Roll the operator first**
  ([orama-operator#1164](https://github.com/oramasearch/orama-operator/pull/1164)), then the lake
  image.
- **Code.** `WorkspaceService::connection` (`src/control/zetta_workspace.cpp`) asks over the
  exchange the server installs (`set_connection_http`, the outbound client with
  `OutboundRequest::ca_bundle`, `src/api/http_server.cpp`); `src/main.cpp` reads the variables and
  refuses to start with the URL but without the two files, or with a URL that is not https.

## Verify

- lake: `ctest --test-dir build -R 'Services|Workspaces|ServiceConnections'`.
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
