# Public API types

`web_ui.hpp` declares what lake still renders on the server since the console became the web app
(lake#561, [web-app.md](../../../docs/ui/web-app.md)): an identity application's hosted sign-in
pages, the stylesheet and fonts they link, the Docs guides `list_docs` and `get_doc` read, a handler
failure's page, and the pure helpers capabilities share (`web_ui_escape`, `web_ui_space_label`,
`web_ui_source_units`). `web_app.hpp` declares the embedded app and which requests it answers.

`Capability::post_alias` gives read capabilities a JSON POST route as well as GET, preserving
read policy and identical OpenAPI/MCP schemas. See `docs/api/streams.md` and `docs/ui/streams.md`
for the Streams contracts and `docs/api/experiments.md` for the Experiments operations.

`transcription.hpp` declares speech-to-text's pure helpers (audio decoding, caption cleaning,
OpenAI's request and answer) and `openai_transcriber`. `registry.hpp` declares the `Transcriber`
injected dependency on `ApiContext` and `ServerOptions` (lake#561, `docs/api/transcripts.md`).

`mcp_client.hpp` is Lake as an MCP client (decision 60, lake#652, `docs/api/mcp-connections.md`):
`McpClient` speaks Streamable HTTP and HTTP+SSE over an injected `McpHttp` (the server's transport,
`make_mcp_http`, or a test's), with `McpLimits` bounding every exchange; `SseParser`,
`parse_mcp_url`, `resolve_mcp_url` and `mcp_public_address` are the pure parts. `ApiContext` and
`ServerOptions` carry the transport (`mcp_http`) and `mcp_allow_private_addresses`.
`mcp_connections.hpp` registers a workspace's MCP connection capabilities and answers what Lake's
own MCP endpoint re-exports (`exported_mcp_tools`, `call_exported_mcp_tool`).
`agent_stream_triggers.hpp` (lake#677) registers the capabilities that start an agent from a
workspace stream, removes an agent's triggers, and declares `AgentStreamWorker`. `registry.hpp`'s
`acting_agent` names the agent whose credential made a call.
`model_gateway.hpp` declares the model gateway's pure helpers (`prepare_model_request`,
`model_usage`, `model_provider_failure`) and `openai_model_provider`. `registry.hpp` declares the
`ModelProvider` injected dependency on `ApiContext`, and `server.hpp` the provider and its URL on
`ServerOptions` (lake#248, `docs/api/model-gateway.md`).

`ServerOptions::external_origin` is trusted instance configuration for repository terminal
instructions (`repository_external_origin`). It takes precedence over `https://` plus the trusted
identity/platform `public_host`; with neither value, lake names no origin. See
[configuration.md](../../../docs/serving/configuration.md) and
[forge.md](../../../docs/ui/forge.md).

`workspace_deletion.hpp` declares the workspace deletion cascade (lake#514): `WorkspaceContent`,
the injected dependency through which the HTTP server lets a deletion reach the instance registry,
the space index's search segments and the workspace's own search index (lake#381), and the
extension point for closing a workspace's own store (lake#380);
the dry-run plan; and the idempotent `finish_workspace_deletion`/`resume_workspace_deletions`.
`ApiContext::workspace_content` carries it. See
[workspaces.md](../../../docs/api/workspaces.md#deletion).

`server.hpp`'s `ApiServer::begin_drain` is the drain's first step at SIGTERM, and
`ServerOptions::preopen_workspace_stores` turns on the start-up pre-open of workspace stores; both
are the rollout hand-off of workspace stores (lake#536,
[workspace-store.md](../../../docs/architecture/workspace-store.md#rollout-hand-off-lake536)).

`ApiContext` carries trusted issuer session ID, kind, scopes and expiry from the common credential
resolver. Token request JSON cannot supply those fields. Keep HTTP and PostgreSQL types out of
these public headers.

`ServerOptions::apps_reserved_names` carries the operator's comma-separated additions to the Ship
App name policy. The HTTP adapter copies it into `ApiContext`; `create_app` combines it with the
built-in platform names before any desired-state write. See
[web-apps.md](../../../docs/api/web-apps.md).

`ServerOptions::methodology_runtime_image` carries `LAKE_METHODOLOGY_RUNTIME_IMAGE` the same way
for workspace methodologies (lake#655, [methodologies.md](../../../docs/api/methodologies.md)).
`ServerOptions::apps_runtime_image` carries `LAKE_APPS_RUNTIME_IMAGE`. The HTTP adapter copies it
into `ApiContext`, and `build_app` records it as the generated Dockerfile's
`APP_RUNTIME_IMAGE` build argument. See [builds.md](../../../docs/api/builds.md).

`ServerOptions::max_registry_blob_bytes` carries `LAKE_MAX_REGISTRY_BLOB_BYTES`. It raises the
server parser ceiling for OCI upload traffic only; body sinks retain the 64 MiB general limit.
See [the HTTP body-limit contract](../../../docs/serving/http.md).

`metering_recorder.hpp` declares `MeteringRecorder`, the bounded in-process buffer of repository
transfer and build records on their way to the workspace Zettas (lake#348, lake#349).
`ApiContext::metering_recorder` points at the server's; `ServerOptions::metering_flush_interval`
and `metering_max_pending` configure it. A null recorder (no workspaces) records nothing.
`audit_routing.hpp` declares the audit classification (decision 52, lake#317): `audit_log_of` for
every registered capability, `audit_route` for one call and `workspace_audit_route` for an event
recorded outside the registry, and `record_audit`. `audit_recorder.hpp` declares `AuditRecorder`,
the bounded queue of workspace audit events on their way to each workspace's chain;
`ApiContext::audit_recorder` points at the server's (null: no workspaces, every event central),
`ApiContext::audit_workspace_id` carries the workspace a call acted on, and
`ServerOptions::audit_flush_interval` (one second) and `audit_max_pending` (65,536) configure it.
`audit_routing.hpp`'s `acted_workspace` is the one lookup of the workspace a call acted on, shared
by the audit route and the request series. `usage_recorder.hpp` declares `UsageRecorder`, the bounded
per-workspace `lake_usage` counter and `lake_storage` readings (lake#564, `ApiContext::usage_recorder`,
`ServerOptions::usage_flush_interval`, `usage_max_pending`, `usage_sample_interval`), and
`usage_dimensions()`, each dimension's unit, whether the price list may rate it and whether it has
GPU classes (lake#643), `add_transfer` for bytes moved outside repository sync, and (lake#658) the
metering contract's tables: `capability_meters()`, `calls_only_capabilities()`,
`metered_workload_kinds()` and `metered_store_types()`; `capabilities.hpp` declares `sample_usage`,
`record_workspace_transfer` and `queue_repository_builds`, and `images.hpp`'s `ImageBytes` splits
image bytes by workspace.
`registry.hpp`'s `Transcription` carries the tokens a speech-to-text model reports. See
[metering](../../../docs/api/metering.md#usage-over-time). `billing.hpp`'s `UsageQuantities` and `bill_lines` price a GPU class at its own rate (lake#643).
`billing.hpp` declares billing (lake#564,
decision 56): the `PaymentProvider` interface and its manual implementation, `billing_enabled`
(`LAKE_BILLING`), `register_billing_capabilities` (called by `ApiServer` only where
`ServerOptions::billing` is on), `billing_registered`, and the pure `price_of` and `bill_lines`. See
[billing](../../../docs/api/billing.md). Since lake#620 (decision 57) it also declares
`PaymentProviders` (the provider in use: `fixed_payment_providers`, or
`environment_payment_providers`, Stripe from the keys lake was started with, lake#628), `PaymentStatus` and
`payment_status_json`, and `receive_payment_notice`, the webhook's logic. `stripe.hpp` declares
lake's own Stripe client: `stripe_payment_provider` over an injected `StripeHttp`, the
`Stripe-Signature` check (`verify_stripe_signature`), `parse_stripe_event`, `stripe_form` and
`stripe_cents`. `request_recorder.hpp` declares `RequestRecorder`, the
bounded per-workspace `lake_requests` counts on their way to each workspace Zetta's time series
(decision 54 S9, lake#384); `registry.hpp`'s `RequestAttribution` (`ApiContext::request_attribution`)
is what one HTTP request's capability calls resolved: its outermost capability, its workspace and
its request id. `ServerOptions::request_series_flush_interval` (ten seconds) and
`request_series_max_pending` (16,384) configure it. See
[telemetry.md](../../../docs/serving/telemetry.md#the-workspace-request-series).
`ServerOptions::secret_lease_expiry_interval` (ten seconds; zero: every tick) paces the owner
loop's dynamic-lease expiry pass (lake#362), which runs only with a store, workspaces and an
instance KEK.
`sealed_delivery.hpp` (lake#625) declares the sealed-secret envelope (`seal_for_cluster`,
`sealing_kid`, `decode_sealing_public_key`), the `lake-secret://` reference (`LakeSecretRef`,
`parse_lake_secret_ref`, `lake_secret_ref`) and `register_sealed_delivery_capabilities`
(`mint_sealed_secret`). See [secrets](../../../docs/api/secrets.md#projections).

`forge.hpp` declares `BranchProtectionRecords` and `load_branch_protection`: the rules, reviews and
releases a push needs, read from the repository's workspace before the push (lake#341), and
`evaluate_branch_protection` over that snapshot.

Verify changes with the full PostgreSQL-backed static and shared CTest suites. Keep the identity
contract in `docs/api/identity.md` updated with changes to token derivation.

The registry owns the response envelope and its success/error schemas, and
`audience_input_schema` owns the input schema each audience is shown. REST/OpenAPI
(`openapi_document`) and MCP (`mcp_tools`) render both from there, so their schemas are
byte-identical; `mcp.hpp` also carries the tool-list digest and session identifier behind
`tools.listChanged` (lake#441, [`docs/api/mcp.md`](../../../docs/api/mcp.md)). REST and MCP use that
shared contract, and input validation recursively checks object fields and array items with JSON
pointer errors. See [`docs/api/README.md`](../../../docs/api/README.md) for the envelope decision
and HTTP conformance proof.

`agent_proposal_digest` (`capabilities.hpp`) is the digest of an agent's proposal as lake stored
it: `approve_agent_proposal` and `agent_turn`'s confirmed ApplicationAgent and ExperimentsAgent
turns compare it with the one the person reviewed (lake#561).

`datasets.hpp` is pure: workspace dataset naming, CSV checks, the `<database>/<name>` id and the
engine-result classification the capabilities share. See
[workspaces.md](../../../docs/api/workspaces.md).

`WorkspaceCallTiming` records each workspace call's Zetta capability and duration for the
`zetta_<capability>` entries of `Server-Timing`. See
[performance.md](../../../docs/ui/performance.md).

`repository_workspace` (`capabilities.hpp`) checks the workspace a new repository is created in
(lake#315). `evaluate_branch_protection` (`forge.hpp`) takes the repository, not its id, so the
evaluation knows the repository's workspace. See
[repositories.md](../../../docs/ui/repositories.md#the-owning-workspace).

`apps.hpp` declares the asynchronous application identity helpers: `application_session_result`
and `checked_application_session` check a session issued in an application's pool against the
user's record in the application's workspace before returning it, and `sign_up_password_session`
claims a pool user's email there before the account exists (lake#353). See
[apps.md](../../../docs/api/apps.md#where-users-and-organizations-live-lake353-lake354).
`apps.hpp` declares `configured_application`, which reads an application's settings record from
its workspace onto the routing row the store returns (lake#351), and `configured`, which unwraps
its completed answer; the identity capabilities and hosted pages use them.

`images.hpp` declares the tenant image helpers of lake#383 phase A (splitting `<space>/<name>`,
a build target's image, the legacy owner rule, the image byte count) and `register_image_capabilities`.
`WorkspaceContent` (`workspace_deletion.hpp`) also exposes the instance registry's legacy tenant
tags and blob reads for the one-shot copy, and since lake#383 phase B the registry's file of a
blob (`blob_file`) and the digests the instance's own repositories use (`instance_digests`) for
the `oci_blobs` copy. `ImageBytes::stored_bytes` is the image blobs already counted in a workspace
store's usage. `metering_usage` counts image bytes from the tags in
each routed workspace (`tenant_image_bytes`), replacing the former
`ApiContext::registry_space_bytes` callable. See [metering.md](../../../docs/api/metering.md).

`capabilities.hpp` declares `register_volume_capabilities` (migration `0069`,
[volumes.md](../../../docs/api/volumes.md)).
`sync_store.hpp` adds `WorkspaceBlob`, `put_workspace_blob` and `read_workspace_blob` (a tenant
image blob as an object of its workspace's liquid store, lake#383 phase B),
`WorkspaceStoreProvision` and `provision_workspace_store` (move_workspace_store `provision`),
`admit_workspace_stream` and `workspace_blob_present`, and
`WorkspaceStoreTransfer` and the `SyncStore` operations of the workspace store
move (`transfer_to_workspace_store`, `switch_workspace_store`, `create_repository_content`,
`close_workspace_store`, lake#380); `capabilities.hpp` adds `create_repository_row`;
`server.hpp` adds `ServerOptions::object_transport` and `workspace_store_lease_ttl`, and
`registry_upload_max_age` (lake#383 phase B: abandoned OCI uploads are removed after it) and
`registry_stream_idle_timeout` (a pull whose client takes no bytes for it is ended).

`register_deletion_capabilities` (`capabilities.hpp`, lake#645) registers `delete_dataset`,
`delete_model`, `delete_run`, `delete_index` and `delete_ask_conversation`
([deletion.md](../../../docs/api/deletion.md)).
