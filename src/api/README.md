# API implementation

The single capability registry defines REST, OpenAPI and MCP behavior. `create_token` validates
issuer scopes, lifetime and static authority in `capabilities.cpp`. The common HTTP context
carries authenticated session metadata; adapters do not decide delegation policy. Application
sign-in results may include a JWT; API-token issuance retains only its derived session bearer.

Verify changes with the full PostgreSQL-backed static and shared CTest suites. Keep the identity
contract in `docs/api/identity.md` updated with changes to token derivation.

`registry.cpp` owns `ApiResult::envelope()` and the advertised success/error envelope schemas.
REST and MCP use those definitions; recursive input validation rejects malformed array items
before capability handlers run. The contract and HTTP conformance proof are in
[`docs/api/README.md`](../../docs/api/README.md).
The sign-up registry contract includes personal customer provisioning, with atomic ownership and
retry semantics shared by all transports. See `docs/api/provisioning.md`.
The sign-up registry contract includes personal customer provisioning. Its form redirects to
repositories, whose create form calls `create_repository` through the same registry. See
`docs/api/provisioning.md` and `docs/ui/identity.md`.

The landing page and all shared-shell renderers receive `WebUiViewer` from the common credential
resolver. Global and repository navigation omit destinations unavailable to that viewer; customer
authentication redirects use `/sign-in`. OpenAPI describes account tokens. The behavior and
verification are in [front-door.md](../../docs/ui/front-door.md).

Audit query and export schemas accept exact `request_id` and append-order `after_id` filters.
Their contract and regression coverage are in [audit.md](../../docs/api/audit.md).
