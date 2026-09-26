# Design and status

[overview.md](overview.md) describes Lake’s place in the product; [status.md](status.md) uses the
implemented, exercised locally, deployed, and exercised-in-deployment vocabulary. The dated
[production record](../deployment/production-2026-09-08.md) is the source for current deployment
facts. Token attenuation and ancestor revocation are part of the identity status and contract in
[../api/identity.md](../api/identity.md).

Verify changes with the full PostgreSQL-backed static and shared CTest suites. Keep the identity
contract in `docs/api/identity.md` updated with changes to token derivation.

Personal customer ownership on sign-up is specified in [../api/provisioning.md](../api/provisioning.md)
and reflected in [status.md](status.md).

The customer landing journey and viewer navigation are recorded in
[../ui/front-door.md](../ui/front-door.md) and reflected in [status.md](status.md).

Indexed PostgreSQL session authentication and the legacy-session migration are specified in
[../api/auth.md](../api/auth.md) and recorded separately from deployment in [status.md](status.md).

The design for lake as Orama's cloud (navigation by Repositories, Design, Train and Ship, and the
new home page) is [cloud.md](cloud.md), with a static mock in [ui/mocks/cloud/](ui/mocks/cloud/README.md).
It is not implemented.

- [`zetta-parity.md`](zetta-parity.md) — every Zetta capability reachable from lake; the pipeline as the central object (plan, 2026-09-26)
