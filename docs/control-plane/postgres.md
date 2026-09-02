# Control-plane Postgres

**Decision.** lake's control plane is Postgres for now; the data plane is Zetta
(bridge `docs/decisions.md` §6).

**What exists.** For every `Lake`, the operator provisions a CloudNativePG `Cluster` named
`lake-<name>-controldb` in the Lake's namespace, through the same unstructured path
`OramaCloudV1` uses for its control DB. It bootstraps a database and owner both named `lake`.

| `spec.postgres` | Default | Meaning |
|---|---|---|
| `enabled` | `true` | `false` tears the cluster down (disable is not "leave running"), **deleting its data** |
| `instances` | `1` | CNPG instances; 2+ adds streaming replicas |
| `storageSize` | `50Gi` | per-instance PVC; a Lake is a production control plane, start with headroom. CNPG grows the PVC in place when the StorageClass allows expansion (Azure disk CSI does) |
| `storageClassName` | cluster default | |

**How lake reaches it.** CNPG generates the Secret `lake-<name>-controldb-app` (keys include
`uri`, `password`, `host`, `port`, `dbname`, `user`). The operator injects
`LAKE_CONTROL_DB_URI` and `LAKE_CONTROL_DB_PASSWORD` into the lake container from it as
**optional** references, so the pod starts before the database is bootstrapped; treat an empty
value as "not yet". The URI has the form
`postgresql://lake:<pw>@lake-<name>-controldb-rw.<namespace>:5432/lake`.

**Readiness.** The operator does not watch the CNPG `Cluster` (neither does `OramaCloudV1`); the
Lake requeues every 20 s until `status.readyInstances ≥ 1`, and shows it as the `DB` column
(`ready/desired`). A cluster without the CNPG operator degrades to "not ready" rather than
crashing the manager.

**Schema.** Applied by the operator through a `PostgresSchema`; see [`schema.md`](schema.md).
`Lake/prod` has migrations `0001` and `0002` applied.

**Verify.**

```sh
kubectl get lake <name>                                   # DB 1/1, SCHEMA Applied
kubectl get clusters.postgresql.cnpg.io -n <namespace>    # Cluster in healthy state
kubectl exec -n <namespace> lake-<name>-controldb-1 -c postgres -- psql -U postgres -d lake -c '\dt'
```

**lake's side.** `main.cpp` builds the Postgres store from `LAKE_CONTROL_DB_URI` (or
`--control-db`); `src/control/postgres_store.cpp` is the one TU that names libpqxx. Each call is one
connection (reused) and one transaction, inline on the io thread. `server_info` reports
`"store": "postgres" | "memory" | "none"`.

**Not yet.** Backups (CNPG can ship WAL and base backups to blob storage), a second instance,
connection pooling or offloading the blocking calls.
