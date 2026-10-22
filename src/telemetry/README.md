# Telemetry implementation

`has_active_agent_certificate` forwards the customer, TAAP identity and issuance instant to the
wrapped store and preserves its result or error. Agent delegation registration therefore applies
through the instrumented store too. See [agent session keys](../../docs/api/agents.md).

The network Store wrapper forwards all twenty connection, egress-rule, tunnel-grant and peer
operations with per-operation labels, preserving actors, scope, records and errors. See
[network.md](../../docs/control-plane/network.md).

Agent claim duration, credential-checked lease renewal, and retry-notice acknowledgement pass
through the Store wrapper with their original results and errors. See
[agent leases](../../docs/api/agent-bus.md).

`metrics.cpp` instruments and forwards Store operations. Stream ownership and customer-scoped
agent-task enumeration and prior-conversation lookup preserve the wrapped store's results and
errors; the metrics layer adds no authorization or topic state. See
`docs/control-plane/streams.md` for those contracts.

The same wrapper forwards protected-app resolution, session authorization, code creation,
exchange, and cleanup without changing their results. Forward-auth timing remains visible through
the HTTP `Server-Timing` regression described in `docs/serving/http.md`.

It forwards the cloud order calls (lake#831) under their own operation labels too.

It also forwards the organization self-service store calls (`create_organization_space`,
`convert_personal_space`, `list_user_memberships`, `list_invitee_invitations`) under their own
operation labels.

`list_repositories` forwards its optional workspace filter (lake#315); `transfer_repository` is no
longer a store operation.

The forge record, platform, metering, stream owner, opportunity, application and agent task
content store operations are gone (lake#341, lake#358, lake#359 and the others of lake#311), and so
are the legacy reads the one-shot copy used: migration `0070` dropped their tables.

The secret store operations are replaced (lake#328-#332): the wrapper forwards the mount routes,
the customer KEK operations (`wrap_secret_data_key`, `unwrap_secret_data_key`,
`customer_kek_state`, `rotate_customer_kek`, `retire_customer_kek`), the lease routes
(`record_secret_lease_route`, `get_secret_lease_route`, `reschedule_secret_lease`,
`delete_secret_lease_route`, `claim_due_secret_leases`, lake#362), each under its own operation
label.

The wrapper forwards `assign_legacy_audit_events` and `legacy_audit_events` (lake#317), the copy's
reads of the central audit events about a workspace's data, each under its own operation label.
The server reports the workspace audit's flushes as `lake_audit_workspace_*`
([telemetry](../../docs/serving/telemetry.md)).

The wrapper forwards the workspace deletion operations (lake#514): `delete_workspace_cascade`,
`get_workspace_deletion`, `pending_workspace_deletions` and `complete_workspace_deletion`. The
owner loop's resume pass counts `lake_workspace_deletions_completed_total` and
`lake_workspace_deletion_errors_total`.

The store decorator forwards email recovery's trusted client address and owner cancellation
(lake#968, LSR-102) so instrumented production stores enforce the same budgets and ownership
checks as direct stores. See [recovery.md](../../docs/api/recovery.md#email-recovery-budgets).
The instrumented store forwards `apply_cloud_order_subscription` and records its normal call/error metrics (lake#972); signed entitlement updates therefore use the same observability as Checkout updates. See [orders](../../docs/api/orders.md#recurring-entitlement-lsr-105-lake972).
