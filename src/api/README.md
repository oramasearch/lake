# API implementation

The single capability registry defines REST, OpenAPI and MCP behavior. `create_token` validates
issuer scopes, lifetime and static authority in `capabilities.cpp`. The common HTTP context
carries authenticated session metadata; adapters do not decide delegation policy. Application
sign-in results may include a JWT; API-token issuance retains only its derived session bearer.

Verify changes with the full PostgreSQL-backed static and shared CTest suites. Keep the identity
contract in `docs/api/identity.md` updated with changes to token derivation.
