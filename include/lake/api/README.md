# Public API types

`ExperimentsPage` contains capability values, resolved display permissions, ExperimentsAgent task
state, reply availability and the stored proposal digest for the pure study/run/comparison
renderer. `AppsPage` carries the equivalent ApplicationAgent reply state. Experiment asset
declarations expose embedded bytes and hashed URLs without filesystem or HTTP dependencies. See
[the UI contract](../../../docs/ui/experiments.md) and
[registered API operations](../../../docs/api/experiments.md).

`Capability::post_alias` gives read capabilities a JSON POST route as well as GET, preserving
read policy and identical OpenAPI/MCP schemas. `StreamsPage` holds only capability values and
resolved UI permissions, including list cursors and Agent event filters; `web_ui_streams` is a
pure renderer. See `docs/api/streams.md` and `docs/ui/streams.md` for the contracts.

`ServerOptions::external_origin` is trusted instance configuration for repository terminal
instructions. The HTTP adapter validates it and supplies it to `RepositoryHomeData`. It takes
precedence over `https://` plus the trusted identity/platform `public_host`; with neither value,
commands are omitted. See
[configuration.md](../../../docs/serving/configuration.md) and
[forge.md](../../../docs/ui/forge.md).

`workspace_deletion.hpp` declares the workspace deletion cascade (lake#514): `WorkspaceContent`,
the injected dependency through which the HTTP server lets a deletion reach the instance registry
and the search segments, and the extension point for closing a workspace's own store (lake#380);
the dry-run plan; and the idempotent `finish_workspace_deletion`/`resume_workspace_deletions`.
`ApiContext::workspace_content` carries it. See
[workspaces.md](../../../docs/api/workspaces.md#deletion).

`ApiContext` carries trusted issuer session ID, kind, scopes and expiry from the common credential
resolver. Token request JSON cannot supply those fields. Keep HTTP and PostgreSQL types out of
these public headers.

`ServerOptions::apps_reserved_names` carries the operator's comma-separated additions to the Ship
App name policy. The HTTP adapter copies it into `ApiContext`; `create_app` combines it with the
built-in platform names before any desired-state write. See
[web-apps.md](../../../docs/api/web-apps.md).

`ServerOptions::apps_runtime_image` carries `LAKE_APPS_RUNTIME_IMAGE`. The HTTP adapter copies it
into `ApiContext`, and `build_app` records it as the generated Dockerfile's
`APP_RUNTIME_IMAGE` build argument. See [builds.md](../../../docs/api/builds.md).

`ServerOptions::max_registry_blob_bytes` carries `LAKE_MAX_REGISTRY_BLOB_BYTES`. It raises the
server parser ceiling for OCI upload traffic only; body sinks retain the 64 MiB general limit.
See [the HTTP body-limit contract](../../../docs/serving/http.md).

`ApiContext::registry_space_bytes` is an injected callable the HTTP adapter sets when registry
storage exists: it returns the unique bytes the OCI registry holds below `<space>/`. The
`metering_usage` summary calls it, so the registry layout stays inside the adapter. See
[metering.md](../../../docs/api/metering.md).

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
`web_ui.hpp`'s `AuditPageView` carries each event's workspace, the queued events and the workspaces
that could not be read.
`ServerOptions::secret_lease_expiry_interval` (ten seconds; zero: every tick) paces the owner
loop's dynamic-lease expiry pass (lake#362), which runs only with a store, workspaces and an
instance KEK.

`forge.hpp` declares `BranchProtectionRecords` and `load_branch_protection`: the rules, reviews and
releases a push needs, read from the repository's workspace before the push (lake#341), and
`evaluate_branch_protection` over that snapshot. `web_ui.hpp`'s `RepositoryHomeData` and
`ChangeReviewData` carry why releases or reviews could not be read.

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

`WebUiViewer` is presentation state, passed explicitly to shared-shell page renderers. Its
operator flag comes from the resolved credential and does not authorize a capability. Boolean
renderer arguments remain compatible and mean an ordinary account, never an operator. See
[front-door.md](../../../docs/ui/front-door.md).

`AppsPage` carries the selected ApplicationAgent task, working state, and exact proposal digest to
the pure Ship Apps renderer. Authorization and digest verification remain in the HTTP adapter.

`OrganizationsPageData` gives the Organizations renderer the viewer's memberships, the invitations
addressed to them and their personal space; see [settings.md](../../../docs/ui/settings.md).
`datasets.hpp` is pure: workspace dataset naming, CSV checks, the `<database>/<name>` id and the
engine-result classification shared by the capabilities and the pages. See
[workspaces.md](../../../docs/api/workspaces.md).

`WorkspaceCallTiming` records each workspace call's Zetta capability and duration for the
`zetta_<capability>` entries of `Server-Timing`. `web_ui_deferred_panels` names the JSON routes a
page fills a slow panel from; `web_ui_your_agents` and `web_ui_new_index` render two of them, and
`AskPage::defer_agents` marks the deferred agent picker. See
[performance.md](../../../docs/ui/performance.md) (lake#446).

`repository_workspace` (`capabilities.hpp`) checks the workspace a new repository is created in
(lake#315). `evaluate_branch_protection` (`forge.hpp`) takes the repository, not its id, so the
evaluation knows the repository's workspace. `RepositoryHomeData::workspace` and
`web_ui_repositories`' workspaces let the repository pages name the owning workspace. See
[repositories.md](../../../docs/ui/repositories.md#the-owning-workspace).

`apps.hpp` declares the asynchronous application identity helpers: `application_session_result`
and `checked_application_session` check a session issued in an application's pool against the
user's record in the application's workspace before returning it, and `sign_up_password_session`
claims a pool user's email there before the account exists (lake#353). See
[apps.md](../../../docs/api/apps.md#where-users-and-organizations-live-lake353-lake354).
`apps.hpp` declares `configured_application`, which reads an application's settings record from
its workspace onto the routing row the store returns (lake#351), and `configured`, which unwraps
its completed answer; the identity capabilities and hosted pages use them.
