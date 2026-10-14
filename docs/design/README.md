# Design and status

Secure-networking store and capability support and its separate operator rollout are tracked in
[status.md](status.md) and specified in the [control-plane](../control-plane/network.md) and
[API](../api/network.md) contracts.
Recipient-only [tunnel credentials](../api/tunnel-grants.md), migration 0085 and the pending
zetta#1281 history provider have source-tree support; live relay termination remains coordinator-owned.

Agent-task lease recovery is tracked separately from deployment in [status.md](status.md).
The cascading workspace deletion (lake#514) has its own row in [status.md](status.md).
The [agent bus contract](../api/agent-bus.md) documents retries and shared-credential limits.

Train → Experiments implementation, local proof and deployment acceptance are tracked separately
in [status.md](status.md); see its [API](../api/experiments.md) and [UI](../ui/experiments.md) contracts.

[overview.md](overview.md) describes Lake’s place in the product; [status.md](status.md) uses the
implemented, exercised locally, deployed, and exercised-in-deployment vocabulary. The dated
[2026-10-01 record](../deployment/dev-2026-10-01.md) of the dev deployment is the source for
current deployment facts, and `tools/docs/check-status.py` fails when the page calls a
capability undeployed that the newest observed OpenAPI lists (lake#448). Token attenuation and ancestor revocation are part of the identity status and contract in
[../api/identity.md](../api/identity.md).

Verify changes with the full PostgreSQL-backed static and shared CTest suites. Keep the identity
contract in `docs/api/identity.md` updated with changes to token derivation.

Personal customer ownership on sign-up is specified in [../api/provisioning.md](../api/provisioning.md)
and reflected in [status.md](status.md).

The customer landing journey and viewer navigation are recorded in
[../ui/front-door.md](../ui/front-door.md) and reflected in [status.md](status.md).

Train → Streams is tracked separately from deployment in [status.md](status.md), with its
[API](../api/streams.md), [UI](../ui/streams.md) and ownership contracts linked there.

Ship Apps direct-zone hosts and reserved-name validation are tracked separately from deployment in
[status.md](status.md), with the [API contract](../api/web-apps.md) linked there.

Indexed PostgreSQL session authentication and the legacy-session migration are specified in
[../api/auth.md](../api/auth.md) and recorded separately from deployment in [status.md](status.md).

The design for lake as Orama's cloud (navigation by Repositories, Design, Train and Ship, and the
new home page) is [cloud.md](cloud.md), with a static mock in [ui/mocks/cloud/](ui/mocks/cloud/README.md).
It is not implemented.

- [`zetta-parity.md`](zetta-parity.md) — every Zetta capability reachable from lake; the pipeline as the central object (plan, 2026-09-26)

[status.md](status.md) records repository workspaces (lake#315 part 1, migration `0051`) as
implemented and not yet deployed.

[status.md](status.md) records forge records in the repository's workspace Zetta (lake#341, migration
`0054`) as implemented and not yet deployed.

[status.md](status.md) records repository environments and the platform mirror in the repository's
workspace Zetta (lake#358, lake#359, migration `0060`) as implemented and not yet deployed.

[status.md](status.md) records the audit split by subject (decision 52, lake#317, lake#318,
migration `0063`) as implemented and not yet deployed.
