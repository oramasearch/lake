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
