# API implementation

Page reads share authentication and customer selection within each GET, and batch independent
workspace and local reads with `when_all`. Data uses a projected name/database list; dataset
details own metadata and the bounded sample. `Server-Timing` reports actual outbound calls and
their sequential rounds. Startup warms existing repository indexes for Metering without
caching usage. Contracts and regression proof: [page performance](../../docs/ui/performance.md)
and [metering](../../docs/api/metering.md).

`experiment_capabilities.cpp` registers eleven space-scoped study/run operations using the
closed contracts in `experiment_schemas.hpp`. `web_ui_experiments.cpp` is their pure page renderer;
embedded, hashed chart assets call only registered capabilities. The page's ExperimentsAgent box
uses `agent_turn`; the HTTP adapter hashes the stored proposal and sends the edited study only in a
confirmed turn. Workspace action aliases retain the same confirmation, scope and promotion checks.
See [API](../../docs/api/experiments.md) and [UI](../../docs/ui/experiments.md) contracts.

`stream_capabilities.cpp` registers the nine Streams REST/MCP operations, enforces space and
creator policy, pages one `list_topics` result per Zetta, filters shared bus topics and proxies
non-consuming browse through service credentials. The shared agent views use durable space tasks
without per-topic Zetta reads. `web_ui_streams.cpp` renders the Train pages; its controls call
those same capabilities. Read `post_alias` routes preserve read classification, and MCP tools
carry read/destructive annotations. See `docs/api/streams.md` and `docs/ui/streams.md`.

Repository terminal instructions receive a validated explicit `ServerOptions::external_origin` or
the HTTPS fallback derived from trusted `public_host`. Request and proxy headers cannot select
their URL. `web_ui.cpp` quotes each shell argument and escapes the resulting commands for HTML;
missing configuration suppresses commands. Existing registry sync operations implement clone and
push. See [forge.md](../../docs/ui/forge.md) and the native
[private-install proof](../../docs/deployment/private-install.md).

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

The common request context compares operator bearers in constant time before any session lookup.
User bearers and cookies use the same indexed PostgreSQL resolver; see `docs/api/auth.md`.

`web_app_capabilities.cpp` owns Ship App reads and desired-state mutations. The common registry
keeps REST and MCP customer policy, `x-lake-action`, audit, lifecycle bus events, Loki reads and
Secret-reference validation identical. Operator hash credentials are restricted in the registry to
source repository/revision reads, customer workspace reads, or one app's event publication. See
`docs/api/web-apps.md`.

ApplicationAgent app turns are assembled in `workspace_capabilities.cpp`: read tools on the first
turn, repository/App writes only when the reviewed proposal is present. `http_server.cpp` and
`web_ui.cpp` display and hash that stored proposal before starting the confirmed turn.
ExperimentsAgent turns use the workspace adapter and grant raw study operations by turn kind;
`create_study` and `promote_trial` appear only with their reviewed confirmation. The adapter sends
those actions through the canonical experiment capabilities.
`build_capabilities.cpp` owns customer-scoped build creation, app image builds, status,
cancellation and retained Zetta progress. Build source and event credentials are rechecked against
the current row on every call. See `docs/api/builds.md`.
