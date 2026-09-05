# The `/v1` API and MCP

**Shape.** lake exposes JSON capabilities three ways, the way Zetta does: REST under `/v1`, an
OpenAPI 3.1 document at `GET /v1/openapi.json`, and MCP at `POST /mcp`. One capability registry
(`include/lake/api/registry.hpp`) is the single source all three read: a capability is a name,
a kind (`read` → `GET /v1/<name>`, input in the query string; `action` → `POST /v1/<name>`,
input as the JSON body), an input schema, an output schema, an authorization object and action,
an object resolver, and a handler. REST and MCP go through the same `Registry::call`, so they
authorize, validate and fail identically.

Binary liquid-sync capabilities use the same registry for validation, policy, execution, and
audit, while the one HTTP adapter streams their bodies on repository-scoped routes. OpenAPI renders
those routes and media types. MCP omits them because its tool arguments are JSON.

**Envelope.** Every response is `{ ok, value | error{code, message, path}, duration_ms }`.
Status by error code: `invalid_input` 400, `unauthorized` / `mfa_required` 403, `not_found` /
`unknown_capability` 404, `conflict` / `unrecoverable` 409, `store_unavailable` 503, anything else
500. MCP
`tools/call` returns the same
envelope as `structuredContent` (and as text), with `isError` set.

**Auth.** Operator bearer or row-resolved user credential, see [`auth.md`](auth.md). `server_info`,
the lake `identity`, and the identity sign-up/sign-in entry capabilities document their public use.
Repository reads are the deliberate public-read exception: anonymous calls return only a public
repository and its content; all other repository visibility levels remain protected.

[`export.md`](export.md) defines the signed `lake_export/1` portable repository archive and its
import validation.

[`github-import.md`](github-import.md) defines recorded GitHub migration input and its explicit
fidelity report.

## Operational routes

| Route | Contract |
|---|---|
| `GET /healthz`, `GET /readyz`, `GET /metrics` | Liveness and readiness are unauthenticated; metrics uses the same bearer or session cookie as capabilities. These are HTTP operational routes, not capabilities, so they are not rendered into OpenAPI or MCP. See [`../serving/telemetry.md`](../serving/telemetry.md). |
| `/scim/v2/Users`, `/scim/v2/Users/:id`, `/scim/v2/Groups`, `/scim/v2/Groups/:id` | SCIM 2.0 list/get/create/PATCH/delete resources. The adapter renders SCIM media types and calls the registry capabilities authenticated by the provider bearer. See [`sso.md`](sso.md). |

## Capabilities

| Capability | Kind | Input | Value | Page |
|---|---|---|---|---|
| `server_info` | read, **no auth** | — | `{name, node, version, api, store}` | — |
| `list_users` | read | `limit` (≤1000, default 100) | `{items: [user]}` | [customers.md](customers.md) |
| `get_user` | read | `id` | user | [customers.md](customers.md) |
| `create_user` | action | `taap_id?`, `email?`, `display_name?` | user | [customers.md](customers.md) |
| `list_organizations` | read | `limit` | `{items: [organization]}` | [customers.md](customers.md) |
| `get_organization` | read | `id` | organization | [customers.md](customers.md) |
| `create_organization` | action | `taap_id?`, `display_name?`, `customer_id?` | organization | [organizations.md](organizations.md) |
| `list_customers` | read | `limit` | `{items: [customer]}` | [customers.md](customers.md) |
| `get_customer` | read | `id` | customer | [customers.md](customers.md) |
| `create_customer` | action | `customer_type`, `user_id?`, `organization_id?`, `name?` | customer | [customers.md](customers.md) |
| `list_repositories` | read | `customer_id?`, `limit` | `{items: [repository]}` | [repositories.md](repositories.md) |
| `get_repository` | read | `id` | repository | [repositories.md](repositories.md) |
| `repository_refs`, `repository_tree`, `repository_file` | read | repository id plus ref or path where required | published refs, tree entries, or one file/blob | [repositories.md](repositories.md) |
| `list_changes`, `get_change`, `change_diff`, `list_conflicts` | read | repository id plus change id where required | change metadata, per-path diffs with text hunks, or conflict values | [repositories.md](repositories.md) |
| `list_artifacts`, `get_artifact` | read | repository id; optional action/output/content filters, cursor and limit; or result id | a result page, or one result with outputs, timing, cache state, verified attestations and trace | [repositories.md](repositories.md) |
| `create_repository` | action | `customer_id`, `name`, `description?`, `visibility?` | repository | [repositories.md](repositories.md) |
| `rename_repository`, `transfer_repository`, `set_repository_visibility`, `delete_repository` | action | repository id plus the lifecycle field where applicable | repository | [repositories.md](repositories.md) |
| `refs` | read | `repository_id` | `{items: [repository ref]}` | [sync.md](sync.md) |
| `find_missing`, `push`, `pull`, `upload_chunks`, `read_chunk` | binary read/action | repository id plus liquid sync body, digest, or range | binary liquid response | [sync.md](sync.md) |
| `create_issue`, `update_issue`, `list_issues`, `create_issue_comment`, `list_issue_comments` | read/action | repository id, issue fields, state, or issue id | durable Markdown issue and discussion records | [forge.md](forge.md) |
| `create_review`, `update_review`, `list_reviews`, `create_review_comment`, `list_review_comments`, `map_review_anchors` | read/action | repository id, liquid change, review state, anchor, or text hunks | change review and cursor-compatible anchored discussion records | [forge.md](forge.md) |
| `create_release`, `list_releases` | read/action | repository id, tag, change, and build-result IDs | tag release retaining verified attestation evidence | [forge.md](forge.md) |
| `create_webhook`, `list_webhooks`, `deliver_webhook`, `retry_webhook_delivery`, `list_webhook_deliveries` | read/action | repository id, subscription, endpoint, event, payload, or delivery ID | signed delivery attempts and retained retry log | [forge.md](forge.md) |
| `set_branch_protection`, `list_branch_protections` | read/action | repository id, ref pattern, review/verification/fast-forward requirements, and allowed pushers | ref policy records and policy-object binding; enforcement waits on liquid #177 | [forge.md](forge.md) |
| `export_repository`, `import_repository`, `list_exports` | read/action | repository or destination customer, archive, and optional Git projection | signed portable archive, recreated repository, or prior export records | [export.md](export.md) |
| `import_github_repository` | action | destination customer, local Git directory, and recorded GitHub responses | imported repository and durable fidelity report | [github-import.md](github-import.md) |
| `list_teams`, `get_team`, `create_team` | read/action | organization or team identifiers and fields | team(s) | [organizations.md](organizations.md) |
| `list_memberships`, `create_membership` | read/action | scope, principal, role, TAAP evidence | membership(s) | [organizations.md](organizations.md) |
| `list_invitations`, `create_invitation`, `accept_invitation` | read/action | scope, invitee, role, expiry or accepting principal | invitation or membership | [organizations.md](organizations.md) |
| `list_objects`, `get_object`, `create_object` | read/action | graph identifiers, type, parent, `inherit` | object(s) | [policy.md](policy.md) |
| `list_grants`, `create_grant` | read/action | object, principal, role, TAAP evidence | grant(s) | [policy.md](policy.md) |
| `effective_access` | read | `object_id` | `{items: [effective access]}` | [policy.md](policy.md) |
| `entity_effective_access` | read | `object_type` (`repository`, `organization`, or `team`), `id` | `{object, items: [effective access]}` | [policy.md](policy.md) |
| `identity` | read, **no auth** | — | lake TAAP identity document | [audit.md](audit.md) |
| `rotate_identity` | action | — | replacement lake identity document | [audit.md](audit.md) |
| `audit` | read | `principal?`, `object?`, `outcome?`, `from?`, `to?`, `limit?` | `{items: [audit event]}` | [audit.md](audit.md) |
| `export_audit` | read | audit filters | `{jsonl}` | [audit.md](audit.md) |
| `verify_audit` | read | — | `{verified: true}` | [audit.md](audit.md) |
| `set_audit_retention` | action | `customer_id`, `days` (≥400) | `{customer_id, days}` | [audit.md](audit.md) |
| `sign_up_password`, `sign_in_password`, `verify_mfa` | action, public entry | password credentials; verify also takes a TOTP or recovery code | browser session + secret | [identity.md](identity.md) |
| `enroll_mfa`, `disable_mfa` | action | authenticated user; disable takes a fresh TOTP or recovery code | one-use setup URI and recovery codes, or confirmation | [identity.md](identity.md) |
| `begin_passkey_registration`, `register_passkey` | action | WebAuthn client data and attestation object | creation options, then confirmation with a lake-signed TAAP binding stored on the device | [identity.md](identity.md) |
| `begin_passkey_sign_in`, `sign_in_passkey` | action, public entry | pool, then WebAuthn assertion | request options, then browser session + secret | [identity.md](identity.md) |
| `register_oauth_provider` | action, operator | issuer, client id, secret, scopes | confirmation | [identity.md](identity.md) |
| `start_oauth` | action, public entry | pool, provider, device name, identity document for a new user | authorization URL | [identity.md](identity.md) |
| `create_token`, `list_sessions`, `revoke_session` | action/read | name/scopes or session id | token/session data | [identity.md](identity.md) |
| `list_devices`, `revoke_device` | read/action | device id for revocation | device data, including passkey credential IDs and signed bindings | [identity.md](identity.md) |
| `issue_offline_grant`, `list_offline_grants`, `revoke_offline_grant`, `check_cached_read` | action/read | device, repository-read scopes, duration, or signed credential | bounded cached-read grant or verification | [offline.md](offline.md) |
| `recover_with_code`, `begin_email_recovery`, `begin_admin_recovery`, `approve_admin_recovery`, `complete_recovery` | action | recovery proof, organization/user, or one-use token | recovery state or fresh browser session | [recovery.md](recovery.md) |
| `claim_recovery_deliveries`, `list_recovery_notifications` | action/read | operator limit or authenticated user | queued mail work or per-device notices | [recovery.md](recovery.md) |
| `configure_recovery_contact`, `repository_recovery_status`, `recover_repository_key` | action/read | repository, contact TAAP identity, opaque wrapped key | custody status or contact-wrapped key | [recovery.md](recovery.md) |
| `register_sso_provider`, `list_sso_providers`, `test_sso_provider` | action/read | organization, protocol configuration, or provider id | secret-free provider configuration | [sso.md](sso.md) |
| `create_federation_issuer`, `list_federation_issuers` | action/read | organization, issuer, claim | acceptable issuer-claim policy rows | [sso.md](sso.md) |
| `sign_in_sso` | action, public entry | provider assertion identity and claims | browser session + secret | [sso.md](sso.md) |
| `upsert_scim_user`, `upsert_scim_group`, `list_sso_provisioning_logs` | action/read | SCIM resource mapping or provider id | SCIM mapping or provisioning outcomes | [sso.md](sso.md) |
| `scim_list_*`, `scim_get_*`, `scim_upsert_*`, `scim_patch_*`, `scim_delete_*` | action, provider entry | provider bearer plus filter, resource, PATCH operations, or resource id | SCIM user/group resource, list response, or deletion confirmation | [sso.md](sso.md) |
| `search` | read | `query`, `customer?`, `repository?`, `ref?`, `limit?` | `{items: [ranked hit]}` | [search.md](search.md) |
| `create_secret_mount`, `list_secret_mounts`, `get_secret_mount` | action/read | customer, organization, mount fields | mount metadata | [secrets.md](secrets.md) |
| `put_secret`, `read_secret`, `secret_metadata` | action/read | mount, name, value/version/metadata fields | versioned value or history | [secrets.md](secrets.md) |
| `delete_secret_versions`, `undelete_secret_versions`, `destroy_secret_versions` | action | mount, name, `versions[]` | updated history | [secrets.md](secrets.md) |
| `create_encryption_key`, `list_encryption_keys`, `get_encryption_key`, `rotate_encryption_key` | action/read | mount and key name | named-key metadata only | [encryption.md](encryption.md) |
| `encrypt`, `decrypt`, `rewrap`, `sign`, `verify`, `hmac`, `derive_encryption_key` | action | mount, key and base64url payload fields | protected result and key version | [encryption.md](encryption.md) |
| `rotate_customer_kek` | action | `customer_id` | KEK version and rewrapped-key count | [encryption.md](encryption.md) |

MCP: [`mcp.md`](mcp.md). The full request/response schemas are in `/v1/openapi.json`, generated
from the registry.

The binary sync operations use `/v1/repositories/<repository-id>/sync/*` rather than the generic
`/v1/<capability>` mapping. Their OpenAPI operations are generated from registry entries, and the
complete wire contract is mirrored in [`sync.md`](sync.md).

OAuth providers return the browser to `GET /v1/oauth/<provider>/callback`. This callback is the
documented provider-facing route outside the registry; it consumes one-use state and issues the
same `lake_session` cookie returned by the identity capabilities.

SCIM clients use the canonical `/scim/v2` resources. Their adapter translates HTTP methods and
SCIM envelopes into the `scim_*` capabilities; provider authentication, validation, store calls,
and audit recording remain inside the registry path.

## Adding a capability

1. A thin handler over the `Store` (`include/lake/control/store.hpp`): no business logic in the
   HTTP layer, no HTTP type outside `src/api/http_server.cpp`.
2. `snake_case` name, verb-first for actions, noun-first for reads; input schema with
   `properties`, `required`, `additionalProperties: false`; an output schema for the value.
3. Name the authorization object and action and provide the resolver that maps validated input to
   the object id. `Registry::call` rejects protected capabilities missing this metadata.
   A candidate-returning capability may instead declare its batched authorization and invoke
   `authorize_batch` before it returns any candidate.
4. Register it in `src/api/capabilities.cpp`. JSON capabilities appear on REST, in OpenAPI and as
   MCP tools automatically. Binary capabilities declare their transport so OpenAPI can render the
   repository route and MCP can omit a body it cannot represent; `test_api.cpp` proves both cases.
5. Tests against `InMemoryStore` and the Postgres fixture; a page under `docs/api/` in the same
   change.

**Verify.** `ctest --test-dir build`, then
`./build/lake --listen 127.0.0.1:4901 --memory-store --admin-token t` and `curl` the routes
(see [`../deployment/aks.md`](../deployment/aks.md) for the production checks).
