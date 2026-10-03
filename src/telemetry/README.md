# Telemetry implementation

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

It also forwards the organization self-service store calls (`create_organization_space`,
`convert_personal_space`, `list_user_memberships`, `list_invitee_invitations`) under their own
operation labels.

`list_repositories` forwards its optional workspace filter (lake#315); `transfer_repository` is no
longer a store operation.

The forge record store operations are gone (lake#341): the wrapper forwards only
`assign_legacy_forge_records` and `legacy_forge_records`, for the one-shot copy.

The platform store operations are gone (lake#358, lake#359): the wrapper forwards only
`assign_legacy_platform_environments`, `legacy_platform_environments`,
`assign_legacy_platform_mirror` and `legacy_platform_mirror`, for the one-shot copy.

The secret store operations are replaced (lake#328-#332): the wrapper forwards the mount routes,
the customer KEK operations (`wrap_secret_data_key`, `unwrap_secret_data_key`,
`customer_kek_state`, `rotate_customer_kek`, `retire_customer_kek`), the lease routes
(`record_secret_lease_route`, `get_secret_lease_route`, `reschedule_secret_lease`,
`delete_secret_lease_route`, `claim_due_secret_leases`, lake#362) and the `legacy_secret_*` and
`legacy_dynamic_secret_leases` reads, each under its own operation label.

The wrapper forwards `assign_legacy_audit_events` and `legacy_audit_events` (lake#317), the copy's
reads of the central audit events about a workspace's data, each under its own operation label.
The server reports the workspace audit's flushes as `lake_audit_workspace_*`
([telemetry](../../docs/serving/telemetry.md)).
