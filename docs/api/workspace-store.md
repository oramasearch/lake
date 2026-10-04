# Moving a Workspace's Repositories into Its Store

Refs [lake#380](https://github.com/oramasearch/lake/issues/380). The design is
[architecture/workspace-store.md](../architecture/workspace-store.md); the decisions are liquid-bridge
decision 54 S1-S3 and the [plan](../architecture/workspace-storage-plan.md#lake380-the-liquid-store).

**Status: implemented in the source tree; not deployed; no workspace on dev is switched.** It needs liquid
`8955519` or later (liquid#332, write-once index runs).

`move_workspace_store` is an operator capability (operator bearer only; every other caller is
`unauthorized`; `src/api/workspace_store_capabilities.cpp`). It moves one workspace's repositories
from its space's liquid store on lake's volume (`LAKE_STORE_ROOT/<space>/store`) into the workspace's
own encrypted store in its workspace Zetta. It is not an MCP tool for people or agents in practice:
the registry renders it everywhere, but only the operator credential passes.

```json
POST /v1/move_workspace_store
{"workspace_id": "<uuid>", "mode": "plan" | "copy" | "verify" | "switch"}
```

Every answer carries `workspace_id`, `customer_id`, `mode`, `store_moved_at` (null until the switch)
and `unassigned`: the repositories of the space without a workspace. Any unassigned repository blocks
`copy`, `verify` and `switch` with `409 conflict` (lake#513 assigns them).

| Mode | Does | Answers |
|---|---|---|
| `plan` | Reads only. Lists the workspace's repositories, live and tombstoned, with `logical_bytes` and `unique_bytes` of their refs (`size_error` when a size cannot be read). | `repositories`, `blocked` |
| `copy` | Copies each repository with liquid's `repository::transfer` (liquid#328): every reachable object keeping its digest, its lease records, then its state keys with `LIQUID_REPO` last. A rerun copies only what is missing. Refused once the workspace is switched. | `repositories` (one transfer report each), `complete` |
| `verify` | Writes nothing. Compares every reachable object and every state key in both stores. | `repositories` with `differences`, `complete` |
| `switch` | Refuses the workspace's and its space's repository requests and repository creation in the workspace with `503 store_unavailable` (`Retry-After`) while it runs, lists the workspace's repositories under that hold, copies what arrived since `copy`, verifies each repository again, creates in the workspace store each one the space store never held, and only when all are complete sets `workspaces.store_moved_at`; from then on the workspace's repositories are served from its store. Requests for other workspaces' stores are not held. A repeated switch answers `switched: true` and changes nothing. | `repositories`, `switched`, `store_moved_at` |

A transfer report has `repository_id`, `absent` (the space store never held it: nothing to copy),
`complete`, `objects`, `objects_present`, `objects_written`, `bytes_written`, `state_written`,
`leases_written`, `missing` (digests a record names that the source lacks too) and `differences`
(`subject`, `reason`).

**What the move does not do.**

- It never writes the space's store, and never removes it. The store stays on the volume read-only
  after the switch; removing it needs Steve's approval.
- An interrupted `copy` leaves no `LIQUID_REPO` for the repository in the workspace store, and lake
  reopens the workspace store before the next use, which liquid's recovery requires. Run `copy`
  again.
- The plan's precondition of a fresh snapshot of the workspace volume and of lake's store volume
  ([orama-operator#260](https://github.com/oramasearch/orama-operator/issues/260)) is not checked:
  the snapshot desired state exists (migration `0069`), but the move does not request a snapshot or
  wait for one yet. Make sure recent snapshots exist before `copy`.
- A repository created in a switched workspace is created in its store at creation. One created
  before the switch and never written is created empty in the workspace store by the switch.
- After the switch a missing workspace store, data key or repository answers `corrupt` (500); lake
  never creates an empty one in its place.

**Audit.** Each call is one central audit event (`workspace_store.move`, details `workspace_id` and
`mode`): it is an operator action on the instance, classified central in
`src/api/audit_routing.cpp`.

## Dev cutover

1. Deploy an image with this change and liquid `8955519` or later, and Zetta with the object capability on every
   workspace Zetta (dev Zetta has it). Apply migration `0067`.
2. Snapshot `lake-prod-store` and each workspace Zetta's volume.
3. For each workspace: `plan`, then `copy`, then `verify` until `complete`, then `switch`.
4. Check the workspace's repository pages, a `liquid pull`, and a push; `store_moved_at` is set.
5. Leave `LAKE_STORE_ROOT/<space>/store` in place.
