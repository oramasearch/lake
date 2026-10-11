# Volumes: workspace storage and snapshots

Decision 54 S13 ([lake#512](https://github.com/oramasearch/lake/issues/512)) moves each
workspace's liquid store, tenant images and search index onto its workspace Zetta's volume
([lake#380](https://github.com/oramasearch/lake/issues/380)). That volume has to grow with the
workspace, and on a one-node workspace it is the only copy, so it is snapshotted on a schedule.
Lake's own store volume (the transparency log, TUF and the instance registry) is snapshotted the
same way.

Lake holds only desired state and the operator's observations, in its control PostgreSQL
(migration `0069_workspace_volumes`, [schema](../control-plane/schema.md#workspace-volumes-migration-0069_workspace_volumes)).
The Orama operator does the work: it grows each claim of the workspace's `OramaZettaCluster`
in place ([orama-operator#259](https://github.com/oramasearch/orama-operator/issues/259)) and takes
CSI `VolumeSnapshot`s through a `VolumeSnapshotClass` with `deletionPolicy: Retain`
([orama-operator#260](https://github.com/oramasearch/orama-operator/issues/260)). It never talks to
a workspace Zetta (decision 51).

## Capabilities

The three are for the operator credential and the instance administrator only; anyone else,
including an account's owner, is answered `unauthorized`. They are control plane, so their audit
events stay in the central log (decision 52, [audit.md](audit.md#which-log)).

| Capability | Kind | Input | What it does |
|---|---|---|---|
| `set_workspace_storage` | action | `workspace_id`, `storage_gi` (1 to 32767) | Sets the desired size of each of the workspace Zetta's volumes, in GiB, and advances the workspace's `generation`. Answers the workspace row. The same size changes nothing. A smaller size is `invalid_input` at `/storage_gi` (volumes never shrink; the `enforce_workspace_storage_growth` trigger refuses it too). A missing, unknown or deleted workspace is `invalid_input`. Audit object `workspace/<id>/storage`, with the previous and new size. |
| `request_volume_snapshot` | action | `target`, optional `schedule`, `keep` (1 to 100) | Records a one-off request (`requested_at` = now) for `target`: `store` or a live workspace id. `schedule` and `keep` change the target's policy. A schedule is the operator's five standard cron fields in UTC (minute 0-59, hour 0-23, day of month 1-31, month 1-12, day of week 0-6; each a comma list of `*`, `n` or `a-b`, optionally `/step`); one that can never fire, such as `0 0 31 2 *`, is refused, as the operator would find no next time for it. Answers the policy with its status. An unknown or deleted target, or a bad schedule or keep, is `invalid_input`. Audit object `volume/<target>/snapshot`. |
| `volume_snapshots` | read | optional `target` | Every policy (`store` first, then each workspace, including deleted workspaces, whose policy and last status stay with their retained snapshots) with the operator's `status`, or `target`'s alone (`invalid_input` when it has none). |

A store that cannot answer is `store_unavailable` (503) on all three, never `invalid_input`; only a
missing, malformed or unknown id is the caller's error. A deleted workspace is `invalid_input` for
`set_workspace_storage` and `request_volume_snapshot`, and is still listed by `volume_snapshots`.

A workspace row ([workspaces.md](workspaces.md#capabilities)) carries `storage_gi`,
`storage_gi_observed` (the smallest claim capacity the operator sees, in GiB), `storage_phase`
(`Pending`, `Resizing`, `Ready` or `Failed`) and `storage_reason`, a short code anyone who may read
the workspace sees (`pending`, `resizing`, `ready`, `failed`, or empty). `storage_message`, the
operator's own text, can carry Kubernetes API errors, so it is returned only to the operator
credential and the instance administrator, on `get_workspace`, `list_workspaces` and
`set_workspace_storage`. A policy is:

```json
{
  "target": "store",
  "schedule": "0 3 * * *",
  "keep": 7,
  "requested_at": "2026-10-03T19:00:00.000000Z",
  "created_at": "2026-10-03T18:00:00.000000Z",
  "updated_at": "2026-10-03T19:00:00.000000Z",
  "status": {
    "snapshot_name": "lake-prod-store-20261003t190012z",
    "ready_to_use": true,
    "snapshot_at": "2026-10-03T19:00:12.000000Z",
    "message": "",
    "updated_at": "2026-10-03T19:00:40.000000Z"
  }
}
```

`status` is null until the operator has written one. `requested_at` is empty until a request.

## What the operator does with it

- **Storage.** It creates a workspace's `OramaZettaCluster` at `storage_gi`. On a larger value it
  patches `spec.resources.requests.storage` of each claim, waits for `status.capacity`, and keeps
  the cluster's own `storage.size` in step so new replicas are resized by the same patch. A value
  below the claims' size is ignored and reported in `storage_message`; a storage class without
  expansion is `storage_phase: Failed` with the reason.
- **Snapshots.** Every workspace gets the default policy (daily at 03:00 UTC, keep 7) when it is
  created, and the migration adds `store` and every existing workspace. The operator takes a
  snapshot of each of the target's claims when its schedule comes due, when it has none yet, and
  when `requested_at` is newer than its last one; it names each `<claim>-<UTC timestamp>`, labels
  it with the target, deletes the oldest beyond `keep`, and writes `volume_snapshot_status`.
- **Deleted workspaces.** Deleting a workspace leaves its snapshots: the class retains each
  snapshot's contents, and the policy and status rows stay. Removing them is a separate, explicit
  action. A deleted workspace takes no new requests.

Restoring a workspace from a snapshot is a documented operator runbook
(orama-operator `docs/lake/volume-snapshots.md`), not automated.

## Not yet

`move_workspace_store` does not yet refuse `copy` until a snapshot of the workspace's volume and of
the store volume taken after the request is ready to use. That check belongs to lake#380's change
and is wired once it merges.
