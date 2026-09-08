# Design and status

[overview.md](overview.md) describes Lake’s place in the product; [status.md](status.md) tracks
implemented behavior separately from deployed production. Token attenuation and ancestor
revocation are part of the identity status and contract in [../api/identity.md](../api/identity.md).

Verify changes with the full PostgreSQL-backed static and shared CTest suites. Keep the identity
contract in `docs/api/identity.md` updated with changes to token derivation.

Personal customer ownership on sign-up is specified in [../api/provisioning.md](../api/provisioning.md)
and reflected in [status.md](status.md).
