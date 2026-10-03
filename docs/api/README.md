# The `/v1` API and MCP

lake#452: [services.md](services.md) documents `list_services`' shared `status` and
`status_reason`; [web-apps.md](web-apps.md) the public acknowledgement `create_app` requires;
[workspaces.md](workspaces.md) the two `create_project` name refusals.

[experiments.md](experiments.md) documents eleven training study, trial, run-metric, comparison
and bounded-live-read capabilities, with identical REST POST/MCP schemas and confirmed actions,
plus the `agent_turn` contract used for ExperimentsAgent proposals. These names are distinct from
the solutions holdout `get_experiment` operation. `start_study` leaves a missing objective and
budgets to Zetta under `from_defaults`, and every experiment error keeps Zetta's `path`.

[streams.md](streams.md) documents all nine topic/message operations on both REST POST and MCP,
with closed input/output schemas, space permissions, system-topic refusal and bounded live reads.
Read tools advertise `readOnlyHint`; writes conservatively advertise `destructiveHint`. Streams reads
retain GET and add POST without changing their read classification.

The [sync protocol](sync.md) also backs the repository page's configured-origin clone/push
instructions. The [private-instance proof](../deployment/private-install.md) executes those
instructions against the existing registry operations.

The [workspace contract](workspaces.md) records how solution-backed Project pages consume
`get_experiment.balance.allocations`, scope-filtered `list_ledger` actions, bounded pending A1
decision rows and the ROI reason without adding a workspace read.
It also records the one workspace action a Project Agent pipeline change turn may run: its
`compile_intent` check, confined to lake's check graph on the turn's project and hidden from
every pipeline list (lake#438).

**Shape.** lake exposes JSON capabilities three ways, the way Zetta does: REST under `/v1`, an
OpenAPI 3.1 document at `GET /v1/openapi.json`, and MCP at `POST /mcp`. One capability registry
(`include/lake/api/registry.hpp`) is the single source all three read: a capability is a name,
a kind (`read` → `GET /v1/<name>`, input in the query string; `action` → `POST /v1/<name>`,
input as the JSON body), an input schema, an output schema, an authorization object and action,
an object resolver, and a handler. REST and MCP go through the same `Registry::call` or coroutine
`Registry::call_async`, so they
authorize, validate and fail identically. Both render each input schema from
`audience_input_schema`, so a capability's REST and MCP schemas are byte-identical for every
caller (lake#441, [`mcp.md`](mcp.md)); MCP declares `tools.listChanged` and tells a session that
began on an older tool list.

Binary liquid-sync capabilities use the same registry for validation, policy, execution, and
audit, while the one HTTP adapter streams their bodies on repository-scoped routes. OpenAPI renders
those routes and media types. MCP omits them because its tool arguments are JSON.

**Envelope decision (REST and MCP).** The advertised success schema describes the complete
JSON response: `{ "ok": true, "value": <capability value>, "duration_ms": <number> }`.
The registry owns both `ApiResult::envelope()` and `success_envelope_schema()`. Each capability
continues to declare its value schema once; the registry nests that schema under `value`, with
`ok`, `value`, and `duration_ms` required. OpenAPI's JSON `200` response schema and MCP's
`outputSchema` use this same generated schema. MCP returns that complete envelope as
`structuredContent` and as its text content, without changing the value's shape.

Failures use `{ "ok": false, "error": { "code", "message", "path" }, "duration_ms" }`,
also serialized and described by the registry. [`errors.md`](errors.md) lists every code a caller
can receive with its REST status (`error_codes()` is the one table; `invalid_input` 400,
`unauthenticated` 401 with `reason` and `help`, `unauthorized` 403, `not_found` 404, `conflict`
409, `store_unavailable` 503, …); any other code is answered as `store_error`. `path` is set
whenever the input is at fault. [`naming.md`](naming.md) gives the one name each concept has across
capabilities (`customer_id`, `run_id`, `model_id`, `index_id`) and the deprecated aliases still
accepted. An empty query key (`/v1/server_info?`) is ignored. MCP capability
failures are successful JSON-RPC exchanges over HTTP 200 with `isError: true` and the same
error envelope; the advertised success `outputSchema` applies when `isError: false`.
Database `ERROR:` and `DETAIL:` diagnostics are logged server-side and replaced before an
`ApiResult` is constructed. Capability handlers may provide a more specific product message and
JSON pointer, such as repository-name conflicts at `/name`; REST and MCP therefore expose the
same actionable error without schema names, SQL text or tenant identifiers from the database.

Registry input validation descends through declared object properties and array `items`, checking
types, required fields, `enum` values, numeric bounds (`minimum`, `maximum`, `exclusiveMinimum`,
`exclusiveMaximum`), and `additionalProperties: false` before authorization-object resolution
or handler execution. Invalid nested input reports `invalid_input` and a JSON pointer such as
`/files/0/content`; pointer tokens escape `~` and `/`. Top-level input still rejects undeclared fields, and existing optional top-level null values
retain their absent-value interpretation. File encoding validation remains with the capability's
byte decoder; `contentEncoding` documents the encoding in the schema.

**Auth.** Operator bearer or row-resolved user credential, see [`auth.md`](auth.md). Browser sign-in through Clerk, when configured, ends in the same row-resolved session; see [`clerk.md`](clerk.md). `server_info`,
the lake `identity`, and the identity sign-up/sign-in entry capabilities document their public use.
The [identity contract](identity.md) includes response isolation: a completed sign-in cannot
send its session cookie or redirect through another request, including a new client connection.
Repository reads are the deliberate public-read exception: anonymous calls return only a public
repository and its content; all other repository visibility levels remain protected.

[`export.md`](export.md) defines the signed `lake_export/1` portable repository archive and its
import validation.

[`github-import.md`](github-import.md) defines recorded GitHub migration input and its explicit
fidelity report.

[`platform.md`](platform.md) defines the operator-control mirror, its total row-ordering guarantee,
environment/domain mutations, and Loki-compatible log reads.

[`registry.md`](registry.md) defines the OCI Distribution, npm, Python, and debuginfod protocol
routes, build-push credentials, exact-repository app pull credentials, and exact-byte integration
coverage.
OCI blob `PATCH` and final `PUT` requests are the only routes admitted above the general 64 MiB
request-body limit. They stream to disk up to `LAKE_MAX_REGISTRY_BLOB_BYTES`, validate chunk
ranges, and verify the digest from the completed file; [`builds.md`](builds.md) and
[`../serving/http.md`](../serving/http.md) own the operational contract.

[`services.md`](services.md) defines a customer's managed services, and [`storage.md`](storage.md)
a customer's stores (buckets, filesystems, network filesystems and volumes; a cluster volume in
phase 1, lake#196), with their capabilities and the Train → Data → Storage panel.
[`web-apps.md`](web-apps.md) defines customer-scoped Ship app creation, direct-zone hosts,
platform-reserved names, deployment, lifecycle, authentication, environment, attach/detach
bindings, logs, events and the read-only app registry pull token. It is separate from identity
[applications](apps.md).
[`builds.md`](builds.md) defines asynchronous in-cluster image builds, configured Ship App runtime
bases, cancellation, status, public digest pull references, retained progress and build-scoped
credentials.

## Operational routes

| Route | Contract |
|---|---|
| `GET /healthz`, `GET /readyz`, `GET /metrics` | Liveness and readiness are unauthenticated; metrics uses the same bearer or session cookie as capabilities. These are HTTP operational routes, not capabilities, so they are not rendered into OpenAPI or MCP. See [`../serving/telemetry.md`](../serving/telemetry.md). |
| `GET /apps/authorize`, `GET /v1/apps/auth`, `GET /w/<workspace_id>/v1/apps/auth` | Lake-protected Ship app browser handoff and nginx forward-auth. They use Lake sessions, host-only app cookies, and one-use codes rather than registry capabilities. See [`web-apps.md`](web-apps.md). |
| `/scim/v2/Users`, `/scim/v2/Users/:id`, `/scim/v2/Groups`, `/scim/v2/Groups/:id` | SCIM 2.0 list/get/create/PATCH/delete resources. The adapter renders SCIM media types and calls the registry capabilities authenticated by the provider bearer. See [`sso.md`](sso.md). |
| `/v2/*`, npm package paths, `/legacy/`, `/simple/*`, `/packages/*`, `/buildid/*` | Native artifact registry and debug-information protocols. Publication uses the administrator bearer; published artifacts are public reads. See [`registry.md`](registry.md). |

## Capabilities

| Capability | Kind | Input | Value | Page |
|---|---|---|---|---|
| `server_info` | read, **no auth** | — | `{name, node, version, revision, api, store}`; `revision` is the embedded full source commit or `unknown` | [../deployment/production-2026-09-08.md](../deployment/production-2026-09-08.md) |
| `list_users` | read | `limit` (≤1000, default 100) | `{items: [user]}` | [customers.md](customers.md) |
| `get_user` | read | `id` | user | [customers.md](customers.md) |
| `create_user` | action | `taap_id?`, `email?`, `display_name?` | user | [customers.md](customers.md) |
| `list_organizations` | read | `limit` | `{items: [organization]}` | [customers.md](customers.md) |
| `get_organization` | read | `id` | organization | [customers.md](customers.md) |
| `create_organization` | action | `taap_id?`, `display_name?`, `customer_id?` | organization | [organizations.md](organizations.md) |
| `list_customers` | read | `limit` | `{items: [customer]}` | [customers.md](customers.md) |
| `get_customer` | read | `customer_id` (`id` deprecated) | customer | [customers.md](customers.md) |
| `create_customer` | action | `customer_type`, `user_id?`, `organization_id?`, `name?` | customer | [customers.md](customers.md) |
| `list_repositories` | read | `customer_id?`, `limit` | `{items: [repository]}` | [repositories.md](repositories.md) |
| `get_repository` | read | `id` | repository | [repositories.md](repositories.md) |
| `list_environments`, `get_environment`, `list_deployments`, `environment_logs` | read | repository id, optional environment, paging and log cursor fields | mirrored platform state or Loki-compatible log page | [platform.md](platform.md) |
| `set_environment_env`, `unset_environment_env` | action (`write`) | repository, environment, key and plain value where setting | appended control-plane row | [platform.md](platform.md) |
| `create_domain`, `delete_domain` | action (`deploy`) | repository, environment, host and TLS mode where creating | pending operator request | [platform.md](platform.md) |
| `repository_refs`, `repository_tree`, `repository_file` | read | repository id plus ref or path where required | published refs, tree entries, or one file/blob | [repositories.md](repositories.md) |
| `list_changes`, `get_change`, `change_diff`, `list_conflicts` | read | repository id plus change id where required | change metadata, per-path diffs with text hunks, or conflicts with kind, region, explanation, proposal, and reasons | [repositories.md](repositories.md) |
| `resolve_conflict` | action (`write`) | repository id, ref, path, conflict id, resolution kind, and optional base64 blob | conflict/value resolution record and new change | [repositories.md](repositories.md) |
| `list_artifacts`, `get_artifact` | read | repository id; optional action/output/content filters, cursor and limit; or result id | a result page, or one result with outputs, timing, cache state, verified attestations and trace | [repositories.md](repositories.md) |
| `reindex_artifacts` | action (`write`) | repository id | registry-derived artifact-member index counts | [registry.md](registry.md) |
| `drive_browse`, `drive_preview`, `drive_history` | read | repository id, ref, and optional or required path | authorized folder children, format-registry preview, or path changes | [drive.md](drive.md) |
| `restore_drive_version` | action | repository id, ref, path, change id | new restoring change | [drive.md](drive.md) |
| `share_drive_folder`, `drive_effective_access`, `drive_shared_with_me` | action/read | folder, recipient, role and evidence; folder; or no input | live grant, direct/inherited access panel, or direct live folders for the signed-in identity | [sharing.md](sharing.md) |
| `create_drive_share_link`, `resolve_drive_share_link`, `revoke_drive_share_link` | action/read | folder, `workspace_id`, role, expiry; token, ref, optional descendant path; or `workspace_id` and link id | scoped link, authorized folder contents, or revoked link | [sharing.md](sharing.md) |
| `preview_drive_share_link`, `download_drive_share_link` | read, bearer link | token, ref, file path | scoped format-registry preview or exact base64 bytes; expiry and revocation checked on every call | [sharing.md](sharing.md) |
| `get_model`, `get_dataset`, `resolve_model_file`, `resolve_dataset_file` | read | repository id and revision, or, without `revision`, a workspace model or dataset id with `customer_id` and `workspace_id`; file path where required | compatible model/dataset metadata, the workspace's trained model or dataset schema, storage/provenance, or exact file | [models.md](models.md) |
| `prepare_repository_upload`, `stage_repository_file`, `commit_repository_files` | action | repository id plus upload plan, verified staged bytes, or file operations | upload modes, unique stored bytes, or immutable committed revision | [models.md](models.md) |
| `attach_model_provenance`, `verify_model_provenance` | action | model and dataset revisions plus canonical liquid attestations, or model revision | attached or re-verified dataset-to-model training chain | [models.md](models.md) |
| `create_repository` | action | `customer_id`, `workspace_id`, `name`, `description?`, `visibility?` | repository, with its `workspace_id` | [repositories.md](repositories.md) |
| `rename_repository`, `transfer_repository`, `set_repository_visibility`, `delete_repository` | action | repository id plus the lifecycle field where applicable; `transfer_repository` is always refused (a repository stays in its workspace) | repository | [repositories.md](repositories.md) |
| `refs` | read | `repository_id` | `{items: [repository ref]}` | [sync.md](sync.md) |
| `find_missing`, `push`, `pull`, `upload_chunks`, `read_chunk` | binary read/action | repository id plus liquid sync body, digest, or range | binary liquid response; push reports every reference outcome and refusal reason | [sync.md](sync.md) |
| `create_issue`, `update_issue`, `list_issues`, `create_issue_comment`, `list_issue_comments` | read/action | repository id, issue fields, state, or issue id | durable Markdown issue and discussion records | [forge.md](forge.md) |
| `create_review`, `update_review`, `list_reviews`, `create_review_comment`, `list_review_comments`, `map_review_anchors` | read/action | repository id, liquid change, review state, anchor, or text hunks | change review and cursor-compatible anchored discussion records | [forge.md](forge.md) |
| `create_release`, `list_releases` | read/action | repository id, tag, change, and build-result IDs | tag release retaining verified attestation evidence | [forge.md](forge.md) |
| `create_webhook`, `list_webhooks`, `deliver_webhook`, `retry_webhook_delivery`, `list_webhook_deliveries` | read/action | repository id, subscription, endpoint, event, payload, or delivery ID | signed delivery attempts and retained retry log | [forge.md](forge.md) |
| `set_branch_protection`, `list_branch_protections` | read/action | repository id, ref pattern, review/verification/fast-forward requirements, and allowed pushers | ref policy records and policy-object binding; enforcement waits on liquid #177 | [forge.md](forge.md) |
| `export_repository`, `import_repository`, `list_exports` | read/action | repository or destination customer and workspace, archive, and optional Git projection | signed portable archive, recreated repository, or prior export records | [export.md](export.md) |
| `import_github_repository` | action | destination customer and workspace, local Git directory, and recorded GitHub responses | imported repository and durable fidelity report | [github-import.md](github-import.md) |
| `list_teams`, `get_team`, `create_team` | read/action | organization or team identifiers and fields | team(s) | [organizations.md](organizations.md) |
| `list_memberships`, `get_membership`, `create_membership`, `set_member_role`, `remove_member` | read/action | scope (omit for your own), membership id, principal, role, TAAP evidence | membership(s) or removal confirmation | [organizations.md](organizations.md) |
| `list_invitations`, `create_invitation`, `accept_invitation` | read/action | scope (omit for your own), invitee, role, expiry or accepting principal | invitation or membership | [organizations.md](organizations.md) |
| `create_organization_space`, `convert_personal_space`, `join_organization` | action | organization name and handle, your personal space, or an invitation addressed to you | organization space or membership | [organizations.md](organizations.md) |
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
| `initialize_tuf` | action | threshold-signed root metadata | immutable root metadata version | [transparency.md](transparency.md) |
| `publish_tuf_metadata` | action | signed targets, snapshot, and timestamp metadata | appended role versions | [transparency.md](transparency.md) |
| `rotate_tuf_root` | action | next root signed by old and new thresholds | immutable successor root | [transparency.md](transparency.md) |
| `tuf_metadata` | read | role and optional version | exact encoded and parsed role metadata | [transparency.md](transparency.md) |
| `transparency_log_configuration` | read | — | effective mode, witnesses, quorum, and aggregator | [transparency.md](transparency.md) |
| `publish_transparency_checkpoint` | action | — | signed current checkpoint including UTC Unix-second `issued_at`, after mode requirements | [transparency.md](transparency.md) |
| `transparency_checkpoint` | read | — | current signed checkpoint | [transparency.md](transparency.md) |
| `append_transparency_entries` | action | canonical TAAP attestations | appended indexes, tree size, and root | [transparency.md](transparency.md) |
| `transparency_tile` | public read | level, tile index, and exact width | immutable base64url entry or interior-hash tile | [transparency.md](transparency.md) |
| `transparency_inclusion_proof` | public read | leaf index and checkpoint tree size | compact audit path bound to a complete signed checkpoint record | [transparency.md](transparency.md) |
| `transparency_proof_bundle` | public read | leaf index | identity, trust configuration, current checkpoint, all committed tiles, and one inclusion proof for offline verification | [transparency.md](transparency.md) |
| `transparency_consistency_proof` | public read | old and new checkpoint tree sizes | compact append-only audit path bound to the complete new checkpoint record | [transparency.md](transparency.md) |
| `transparency_identity_scan` | public read | TAAP identity | checkpoint-verified matching attestations and scan measurements | [transparency.md](transparency.md) |
| `list_agent_library` | read | none | the Agent Library: every specialized agent Orama ships, whether this lake runs it, and `internal` for Orama's own tooling (never offered in Ask Orama) | [train-agents.md](train-agents.md) |
| `list_datasets` | read | customer | The workspace dataset catalogue with stable `<database>/<name>` ids (the form `get_dataset` takes) and `kind` (`data` or engine `result`) | [workspaces.md](workspaces.md) |
| `list_runs` | read | customer, optional project/solution and limit | newest matching runs with status, reached stage and final error line | [workspaces.md](workspaces.md) |
| `launch_agent`, `list_agent_launches`, `stop_agent_launch`, `schedule_agent`, `list_agent_schedules`, `unschedule_agent` | action / read | customer, agent, mode, input, cron or trigger | run, follow and stop Python agents on agent-runtime, and give them cron schedules (each a CronJob) | [agent-launches.md](agent-launches.md) |
| `agent_turn` | action | customer, agent, message, optional conversation/kind/context | one conversation turn with any agent: library, native or Python; a reused library-agent conversation includes its preceding same-agent, same-kind turn; ExperimentsAgent uses proposal, status and explanation kinds | [agent-launches.md](agent-launches.md) |
| `receive_agent_tasks`, `claim_agent_task`, `publish_agent_event`, `receive_agent_events` | read / action | agent, task, sequence | renewable claims, bounded reclaim/retry, progress and first-terminal-write-wins results over workspace Zetta pub/sub; delegated turns reported on the asking turn | [agent-bus.md](agent-bus.md) |
| `mint_agent_session_key` | action | customer, durable and dispatcher identities, scopes, lifetime | response-only private seed plus logged durable-identity delegation | [agents.md](agents.md) |
| `verify_agent_session_drip` | action | customer, session, canonical body and TAAP signature | verified durable attribution, scopes and log commitment | [agents.md](agents.md) |
| `revoke_agent_session` | action | customer, agent session | the key with `revoked_at`; it never verifies again | [agents.md](agents.md) |
| `set_customer_credential_policy` | action | customer, `allow_non_expiring_credentials` | the customer's opt-in to keys and tokens that never expire | [agents.md](agents.md) |
| `timestamp_checkpoint` | action | — | RFC 4998 evidence record for the current checkpoint signature | [transparency.md](transparency.md) |
| `renew_timestamp_evidence` | action | checkpoint digest | evidence record with a new archive timestamp over its predecessor | [transparency.md](transparency.md) |
| `timestamp_evidence` | read | checkpoint digest | verified evidence record, token summaries, and renewal deadline | [transparency.md](transparency.md) |
| `export_transparency_replication_batch` | action | source tree-size cursor | verified log suffix with base/head commitments and current checkpoint | [replication.md](replication.md) |
| `replicate_transparency_log` | action | source Lake URL | applied tree size, root, checkpoint sequence, and catch-up state | [replication.md](replication.md) |
| `replicate_repository` | action | repository, source instance, source Lake URL | durable liquid cursor, transfer totals, and ref count | [replication.md](replication.md) |
| `sign_up_password`, `sign_in_password`, `sign_in_taap`, `verify_mfa` | action, public entry | password, one-use TAAP assertion, or MFA proof | browser session + secret | [identity.md](identity.md) |
| `register_application`, `list_applications`, `get_application`, `update_application_theme`, `rotate_application_signing_key` | action/read | customer, application registration, theme, or application id | isolated pool registration and key metadata | [apps.md](apps.md) |
| `application_jwks`, `issue_application_jwt` | public read / protected action | application, its `workspace_id` (JWT) and active user ids | Ed25519 JWKS or short-lived audience-bound JWT | [apps.md](apps.md) |
| `begin_application_authorization`, `exchange_application_authorization_code` | protected/public action | application, exact redirect, state, nonce, PKCE challenge; then code and verifier | one-use authorization result, then access and ID tokens | [apps.md](apps.md) |
| `list_application_users`, `suspend_application_user`, `delete_application_user`, `export_application_user` | read/action | application, its `workspace_id`, and user ids | pool profiles, suspension, deletion, or export, from the application's workspace Zetta (lake#353) | [apps.md](apps.md) |
| `list_application_organizations`, `create_application_organization`, `update_application_organization`, `delete_application_organization` | read/action | application, its `workspace_id`, organization, TAAP identity, name, metadata | application organization lifecycle in the application's workspace Zetta (lake#354) | [apps.md](apps.md) |
| `create_application_webhook`, `list_application_webhooks`, `list_application_webhook_events`, `retry_application_webhook` | action/read | application, endpoint, subscriptions, event and webhook ids | signed lifecycle delivery log | [apps.md](apps.md) |
| `enroll_mfa`, `disable_mfa` | action | authenticated user; disable takes a fresh TOTP or recovery code | one-use setup URI and recovery codes, or confirmation | [identity.md](identity.md) |
| `begin_passkey_registration`, `register_passkey` | action | WebAuthn client data and attestation object | creation options, then confirmation with a lake-signed TAAP binding stored on the device | [identity.md](identity.md) |
| `begin_passkey_sign_in`, `sign_in_passkey` | action, public entry | pool, then WebAuthn assertion | request options, then browser session + secret | [identity.md](identity.md) |
| `register_oauth_provider` | action, operator | issuer, client id, secret, scopes | confirmation | [identity.md](identity.md) |
| `start_oauth` | action, public entry | pool, provider, device name, identity document for a new user | authorization URL | [identity.md](identity.md) |
| `create_token`, `list_sessions`, `revoke_session` | action/read | name/scopes or session id | tokens bounded by issuer scopes and expiry, with parent revocation | [identity.md](identity.md) |
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
| `search` | read | `query`, `customer?`, `repository?`, `ref?`, `change?`, `history?`, `limit?` | `{mode, scanned_changes, items: [ranked hit]}` | [search.md](search.md) |
| `metering_usage`, `set_storage_quota` | read/action | customer, time range, or quota bytes | hourly repository transfer and build compute from the workspace Zettas, live storage, pricing, the customer quota, and populated cold-read proof | [metering.md](metering.md) |
| `create_secret_mount`, `list_secret_mounts`, `get_secret_mount` | action/read | customer, `workspace_id`, organization, mount fields | mount metadata (in the mount's workspace Zetta) | [secrets.md](secrets.md) |
| `put_secret`, `read_secret`, `secret_metadata` | action/read | mount, name, value/version/metadata fields | versioned value or history | [secrets.md](secrets.md) |
| `delete_secret_versions`, `undelete_secret_versions`, `destroy_secret_versions` | action | mount, name, `versions[]` | updated history | [secrets.md](secrets.md) |
| `lease_dynamic_secret`, `renew_dynamic_secret`, `revoke_dynamic_secret` | action | mount, role and bounded lifetime; then lease id | response-only generated credential or updated lease metadata | [secrets.md](secrets.md) |
| `create_encryption_key`, `list_encryption_keys`, `get_encryption_key`, `rotate_encryption_key` | action/read | mount and key name | named-key metadata only | [encryption.md](encryption.md) |
| `encrypt`, `decrypt`, `rewrap`, `sign`, `verify`, `hmac`, `derive_encryption_key` | action | mount, key and base64url payload fields | protected result and key version | [encryption.md](encryption.md) |
| `rotate_customer_kek` | action | `customer_id` | KEK version and rewrapped-key count | [encryption.md](encryption.md) |
| `create_pki_authority` | action | `customer_id` | customer root certificate and intermediate chain | [pki.md](pki.md) |
| `issue_x509_svid` | action | customer, SPIFFE ID, optional lifetime | short-lived X.509 SVID, private key, and chain | [pki.md](pki.md) |
| `issue_jwt_svid` | action | customer, SPIFFE ID, audience, optional lifetime | short-lived ES256 JWT SVID | [pki.md](pki.md) |
| `issue_client_certificate`, `issue_device_certificate` | action | customer, subject, optional lifetime | short-lived client or device X.509 certificate, private key, and chain | [pki.md](pki.md) |
| `issue_agent_certificate`, `rotate_agent_certificate`, `revoke_agent_certificate`, `check_agent_certificate` | action/read | customer and TAAP key; then certificate id and optional replacement key | short-lived public-key certificate or enforced status | [pki.md](pki.md) |
| `issue_ssh_user_certificate`, `issue_ssh_host_certificate` | action | customer, Ed25519 public key, key ID, principals, optional lifetime | short-lived OpenSSH certificate and CA public key | [pki.md](pki.md) |
| `acme_new_account`, `acme_new_order`, `acme_order`, `acme_authorization`, `acme_http01_challenge`, `acme_finalize_order`, `acme_certificate`, `acme_http01_response` | ACME transport | RFC 8555 JWS, account, order, authorization, challenge and CSR fields | customer-scoped ACME state and certificate chain | [pki.md](pki.md) |
| `begin_root_recovery`, `complete_root_recovery` | action | customer, configured quorum; then ceremony and shares | recovery shares, then rehearsal confirmation | [break-glass.md](break-glass.md) |
| `exchange_kubernetes_attestation` | action, public entry | customer, organization, projected token, workload TAAP ID, audiences | durable caller ID and short-lived workload credential | [callers.md](callers.md) |
| `exchange_jwt_attestation`, `exchange_cloud_attestation` | action, public entry | customer, federation assertion or AWS/Azure evidence, workload TAAP ID | durable caller ID and short-lived workload credential | [callers.md](callers.md) |

MCP: [`mcp.md`](mcp.md). The full request/response schemas are in `/v1/openapi.json`, generated
from the registry.

Each operation carries **`x-lake-action`**: `read` or `write`, the action lake authorizes the call
as. It is what the call does, apart from how it is sent: an action (POST) can be a read, like
`workspace_query` (its input is a JSON object) or `predict`. An agent that may only read decides
from it, not from the method. Ask Orama's turn credential carries every capability whose action is
`read`.

The binary sync operations use `/v1/repositories/<repository-id>/sync/*` rather than the generic
`/v1/<capability>` mapping. Their OpenAPI operations are generated from registry entries, and the
complete wire contract, including Liquid #192's server-derived per-reference authorization,
outcome context, and branch-protection enforcement, is mirrored in [`sync.md`](sync.md) and
[`forge.md`](forge.md). Forge records (issues, reviews, comments, releases, webhooks, exports and
branch protection rules) live in the workspace Zetta of the repository's workspace (lake#341): a
repository without a workspace is `conflict` at `/repository_id`, and a push reads its branch
protection there before it starts ([forge.md](forge.md#where-forge-records-live)).

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
   `properties`, `required`, `additionalProperties: false`; an output schema for the value. The registry generates the advertised success envelope.
3. Name the authorization object and action and provide the resolver that maps validated input to
   the object id. `Registry::call` rejects protected capabilities missing this metadata.
   A candidate-returning capability may instead declare its batched authorization and invoke
   `authorize_batch` before it returns any candidate.
4. Register it in `src/api/capabilities.cpp`. JSON capabilities appear on REST, in OpenAPI and as
   MCP tools automatically. Binary capabilities declare their transport so OpenAPI can render the
   repository route and MCP can omit a body it cannot represent; `test_api.cpp` proves both cases.
5. Tests against `InMemoryStore` and the Postgres fixture; a page under `docs/api/` in the same
   change. Request examples in that page (JSON bodies with `customer_id`/`workspace_id`, `curl`
   commands) are validated against the registry by `tests/test_examples.cpp` (lake#439), so write
   them complete, with every required field.

**Verify.** `ctest --test-dir build`, then
`./build/lake --listen 127.0.0.1:4901 --memory-store --admin-token t` and `curl` the routes
(see [`../deployment/aks.md`](../deployment/aks.md) for the production checks).

## Schema conformance proof

`SchemaConformance.EveryToolAndRequiredOutputUsesHttpContract` starts the production HTTP server
with an isolated in-memory control store and a real Liquid repository. It enumerates `tools/list`, calls every advertised tool
over `/mcp`, and captures the actual responses. An independent Python `jsonschema` validator checks
every successful `structuredContent` against that tool's HTTP-advertised `outputSchema`; failure
results validate against the registry's error envelope from HTTP-fetched OpenAPI. The fixture
requires successful calls for every tool with required value fields, including metering, conflict
resolution, shared-file reads, and transparency proofs. Adding required output fields without a
successful fixture fails the coverage check. The discovery pass also exercises no-argument calls;
stateful tools without fixtures can return their normal validation or authorization errors.

The same proof validates actual REST successes and malformed upload errors, rejects an unwrapped
metering value and a value missing `currency`, and checks nested upload paths over both transports.
It reports the tool count, successful tool count, required-output coverage, and schema errors.
Run it through the full native Postgres-backed CTest suite; see
[`../build/testing.md`](../build/testing.md). To change an output, update its registry value schema
and success fixture together, retaining the shared envelope construction.

Personal-customer sign-up behavior, naming, quotas, metering, and atomic retry semantics are in
[provisioning.md](provisioning.md). All transports use the existing `sign_up_password` entry.

[auth.md](auth.md) specifies indexed PostgreSQL session authentication, operator precedence and
the `0022_session_lookup` rollout. Password hashing and live revocation guarantees are preserved.
