# Workspace desired-state contract

Refs lake#265 and lake#311. Lake owns workspace identity and desired state; the operator owns
namespace provisioning and observations. This is distinct from a managed Zetta service.

Migration `0039_workspaces` creates `workspaces` with `id` (UUID primary key), `customer_id`,
`name`, `environment`, `desired`, `generation`, `phase`, `observed_generation`, `message`,
`namespace`, `zetta_service_id`, `created_at` and `updated_at`. Environment is a lowercase
DNS label of at most 47 characters, leaving room for `-ws-` and twelve UUID hex digits.
Desired state is `running`, `stopped` or `deleted`. The initial observation is `Pending`,
generation 1 and observed generation 0. Status fields are never accepted by Lake's API.

An AFTER INSERT/UPDATE/DELETE trigger sends `NOTIFY lake_workspaces` with the row UUID,
including changes made by the operator. The operator also relists. The guard is
`workspaces_guard`, using the existing `lake-schema` content-hashed schema Job convention.
The operator must vendor this schema and update its guard to `workspaces_guard` before rolling
out Lake. The merged operator#217 workspace controller alone does not supply this migration.

Creation atomically writes a workspace and its primary Zetta service, with separate UUIDs.
`workspaces.zetta_service_id` points to the service; `services.workspace_id` points back.
The primary service slug is `ws-<first 12 workspace UUID hex digits>`. Only desired metadata
is written: no namespace or Kubernetes resource is provisioned by Lake. Services created
through the API also require an explicit owning `workspace_id` for the operator's join.

Changes to name, environment or desired state advance generation; an identical update does
not. Deleted workspaces cannot be revived. Stopping withdraws the primary Zetta service;
running restores that primary service; deletion withdraws every service belonging to the
workspace so the services reconciler cannot recreate them after namespace deletion. Other
services are not automatically revived. Observations remain unchanged until the operator
reports them. An environment change asks the operator to replace the namespace and can lose data.

## Existing services and routing

Every existing Zetta service becomes a workspace with environment `prod`. An original service
named `ws-<customer UUID prefix>` receives that customer's UUID as its workspace ID, matching
the operator's legacy-cluster cleanup name. Other Zettas receive their service UUID as the
workspace ID. Primary service slugs are aligned to the workspace UUID so the workspace and
services controllers reconcile the same cluster. Old non-default `svc-*` Zetta resources, if
any, need cleanup during rollout; they are not matched by operator#217's `ws-*` legacy cleanup.
The migration explicitly remaps task/app/build IDs and assignment records in its transaction,
temporarily disabling only their routing triggers, then restores immutability and the new
foreign keys. The services assignment record preserves the old service ID to workspace ID map.
Pre-cutover `/w/<service-id>` links must use the recorded workspace ID after this migration.
An absent service becomes a deleted workspace. Namespace observations begin empty so the
operator reconciles the new namespace contract. Other services are assigned only if exactly
one non-deleted workspace exists for their customer; ambiguous services remain unassigned.

`workspace_assignments` retains migration 0038's `sole_default` / `needs_selection` records
for tasks, apps and builds. Migration 0039 adds `workspace_contract_prod` for Zetta services
and `sole_workspace` / `needs_selection` for other services. Objects created after 0038 receive
`workspace_contract_remap` records if they have no earlier assignment record. Warnings report unresolved IDs.
Inspect this ledger before serving traffic; there is no default-workspace routing fallback.
Reapplying the schema preserves the recorded assignments. Tasks, apps and builds now have
foreign keys to `workspaces(id)` and retain their immutable, customer-bound routing triggers.

All workspace callers resolve `workspaces.zetta_service_id`, then the service's endpoint and
mounted credential. A workspace UUID is not treated as a service UUID. Missing, malformed,
foreign or deleted IDs are refused; stopped or unready workspaces do not reach a data plane.

## Dev-cluster cutover

Steve explicitly approved automatic replacement on 2026-09-29: this cluster is a development
environment with no production data. Operator#217 deleting the legacy `prod-ws-*` clusters,
volumes and credentials is intended. There is no approval guard or data-preserving adoption
requirement. The replacement namespace is `<environment>-ws-<first 12 workspace UUID hex digits>`.

Roll out the operator's schema Job with this schema and guard, inspect assignment warnings,
then roll out the workspace-aware Lake/operator/client versions. Expect empty replacement
Zettas and newly projected credentials. No deployment or cluster deletion is performed by
this change's native verification. The event cutover and remaining client work are described
in [the data-plane design](../architecture/workspace-data-plane.md).
