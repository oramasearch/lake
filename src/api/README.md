# API implementation

`store_owner.cpp` holds the exclusive owner lock on `LAKE_STORE_ROOT`, with a bounded wait for a
predecessor. `ApiServer::start` binds first; its owner thread takes the lock and opens the customer
stores (each with liquid's `exclusive_owner`), while the `store_root` middleware holds every request
but probes, metrics and static assets until the root is open and `/readyz` reports the hand-off.
`stop` drains in-flight requests, closes the stores and releases the lock (lake#467, lake#479,
[store-volume.md](../../docs/deployment/store-volume.md#the-hand-off)).
lake#452: `/w/<workspace>` and `/w/<workspace>/` redirect to the workspace's home; the selectors
label the viewer's personal space *Personal space* and workspaces by name, and wrap onto their own
row at phone width; `workspace_act create_project` says whether a name is missing or taken;
`create_app` requires `confirm_public` (or a person's acknowledgement on the confirmed
ApplicationAgent turn) for a public app; `list_services` answers the page's `status` and
`status_reason`. See [workspace selection](../../docs/ui/workspace-selection.md),
[web apps](../../docs/api/web-apps.md) and [services](../../docs/api/services.md).

Agent-task capability calls renew the current claim through the registry. Agent events are
accepted atomically before workspace publication; only a Running task accepts them, and the
first terminal write wins. Apps, Experiments and Project panels show the persisted retry notice
(lake's `notice`, from the task's row); the agent's own message is one `get_record` of the task's
workspace record (lake#375). See [agent leases](../../docs/api/agent-bus.md).

A delegated `agent_turn` from a task credential names its asking task (`parent_task_id`), and
`publish_agent_event` reports the delegated task's progress on that asking task (lake#451). The Ask
page renders the running turn it already reads (agent, latest message, attempts, elapsed time) and
builds *Talk to* with `control::talk_to_agents`, which leaves out internal library agents. See
[ask-orama.md](../../docs/api/ask-orama.md) and [delegated turns](../../docs/api/agent-bus.md#delegated-turns).

What was written for an agent task lives in its workspace (lake#374, lake#375,
[agent-bus.md](../../docs/api/agent-bus.md#where-a-task-lives-lake374-lake375)).
`agent_task_records_api.cpp` assembles a task from both halves (`load_agent_task_content`,
`agent_task_with_content`) and writes a new task's request and each accepted event's message and
answer (`write_agent_task_request`, `write_agent_task_event`). `agent_turn` writes the request
before the row and routes a delegated turn by `parent_task_id`; `claim_agent_task` became
asynchronous and reads the request just before its one-statement claim; `publish_agent_event`
records the event after the store accepted it; `get_agent_task`, `receive_agent_events`, a
conversation's next turn, the confined compile check, `settle_solution`, Opportunities' settle and
`create_app`'s public acknowledgement read the record. The Apps and Experiments working panels read
it for the running message, and the project page takes the build's message from `get_agent_task`.
`copy_workspace_records` also copies `agent_tasks`.

Page reads share authentication and customer selection within each GET, and batch independent
workspace and local reads with `when_all`. Data uses a projected dataset/database list from `system.datasets`; dataset
details own metadata and the bounded sample. `Server-Timing` reports actual outbound calls and
their sequential rounds, and names each call by its Zetta capability (`zetta_<capability>`). Ask
Orama, Train → Agents and Search Indexes defer the panels that wait for slow Zetta reads to JSON
panel routes (`/ask/agents`, `/train/agents/list`, `/ship/indexes/datasets`, lake#446); the page
script fills them in, and `?inline=1` renders Agents and Indexes in place without JavaScript.
Startup warms existing repository indexes for Metering without
caching usage. `metering_capabilities.cpp` is asynchronous: its summary reads every workspace's
datasets, runs, and repository transfer and build records concurrently (lake#348, lake#349), and
`http_server.cpp` injects `registry_space_bytes` so registry image bytes are counted where the
registry storage lives. `metering_recorder.cpp` is the `MeteringRecorder`: the sync capabilities in
`capabilities.cpp` add each request's bytes to it without I/O, `ApiServer::drain` flushes it to the
workspace Zettas every `metering_flush_interval`, and `ApiServer::stop` flushes it once more after
in-flight requests finish. Contracts and regression proof: [page performance](../../docs/ui/performance.md)
and [metering](../../docs/api/metering.md).

The audit split by subject (decision 52, lake#317) lives in three files. `audit_routing.cpp` is the
one classification: two sorted tables name every registered capability central or workspace, and
`audit_route` sends a call's event to the workspace it acted on (the validated `workspace_id`, or
the workspace of the repository, mount, application, app, build or task its input names) or to the
central chain; `Registry::call`'s `finish_call` and the push and branch protection events in
`capabilities.cpp` record through `record_audit`. `audit_recorder.cpp` is the `AuditRecorder`: a
workspace event is queued without I/O, `ApiServer::drain` flushes it into the workspace's chain
every `audit_flush_interval`, and `ApiServer::stop` flushes it once more. `audit_logs.cpp` reads
both logs for `audit`, `my_audit`, `export_audit`, `verify_audit` and the `/activity` and `/audit`
pages, every workspace concurrently in one round. See [audit](../../docs/api/audit.md#which-log).

`datasets.cpp` holds the workspace dataset rules lake applies on top of Zetta (lake#447): engine
result databases, SQL-queryable names, CSV row checks with line numbers, and the one
`<database>/<name>` id. `workspace_query`/`workspace_act` apply them to `list_datasets`,
`get_dataset`, `analyze_dataset`, `upload_dataset` and `create_dataset_from_query`; the Data page
and the dataset pickers use them to keep engine results apart. See
[`workspaces.md`](../../docs/api/workspaces.md) and [load data](../../docs/guide/load-data.md).

Solution catalogue details and solution-backed Project pages call the registry's bounded
`list_runs` read. It filters by project or solution, completes retained summaries through
concurrent `get_run` calls, and derives the stage and final error line. `http_server.cpp` starts
the run read with the existing project/catalogue work and reuses it for solution state;
`web_ui.cpp` renders lifecycle status, active-run refresh, readable policy/experiment/monitor/ROI
summaries and the collapsed correctness certificate. Experiment allocations come from
`balance.allocations`; scoped `list_ledger` action entries supply decision/action/held counts and
latest actions, while the bounded decision rows supply pending A1 approval controls. The five
operating-state reads remain concurrent. See
[`workspaces.md`](../../docs/api/workspaces.md).

`experiment_capabilities.cpp` registers eleven space-scoped study/run operations using the
closed contracts in `experiment_schemas.hpp`. `web_ui_experiments.cpp` is their pure page renderer;
embedded, hashed chart assets call only registered capabilities. The page's ExperimentsAgent box
uses `agent_turn`; the HTTP adapter hashes the stored proposal and sends the edited study only in a
confirmed turn. Questions and absent proposals get a reply form that reuses the task conversation;
Apps and Design pipeline questions use the same follow-up contract. Workspace action aliases retain
the same confirmation, scope and promotion checks. Their failures keep Zetta's `error.path`;
`start_study` requires `objective` and `budgets` only without `from_defaults` (lake#463).
The study page carries its latest trials' curves and readable analysis tables from the server.
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

Train → Runs, a run page and Ship → Models also read `list_studies` (lake#449): a trial run is
*Trial N of study <name>*, a study's model names its study, and a promoted model is marked. A run
report leads with its held-out score and row count from `get_run_report.model`, with Zetta's
caveat on that figure beside it.

Copy-ready external calls on Ship → Models and the search index page name
`WorkspacePage::workspace_id`, which `serve_workspace_page` sets from the `/w/<workspace_id>` path;
every data-plane capability requires it (lake#439). Without one the page shows `<workspace id>`.
`markdown.cpp` joins a list item's continuation lines before inline markup, so a Docs code span may
wrap. `tests/test_examples.cpp` validates every such example against the registry.

OCI blob upload patches and final puts stream request buffers into the upload file. The adapter
checks the configured completed-blob limit and `Content-Range`, hashes the completed file without
loading it into memory, and leaves the common 64 MiB body limit on every other route. See
[the HTTP contract](../../docs/serving/http.md) and [image builds](../../docs/api/builds.md).

The single capability registry defines REST, OpenAPI and MCP behavior. `create_token` validates
issuer scopes, lifetime and static authority in `capabilities.cpp`. The common HTTP context
carries authenticated session metadata; adapters do not decide delegation policy. Application
sign-in results may include a JWT; API-token issuance retains only its derived session bearer.

`route_guard.hpp` wraps every route handler registered by `http_server.cpp`: an exception is
logged with the route and request id and answered 500 (HTML or the JSON envelope), never a
dropped connection. It names Boost.HTTP types, so it stays in `src/api/` rather than `include/`.
Renderers read optional JSON fields with `json_text`, which treats a null as empty; `value(key,
std::string())` throws on a present null. See [handler exceptions](../../docs/serving/http.md#handler-exceptions).

Verify changes with the full PostgreSQL-backed static and shared CTest suites. Keep the identity
contract in `docs/api/identity.md` updated with changes to token derivation.

`registry.cpp` owns `ApiResult::envelope()` and the advertised success/error envelope schemas.
REST and MCP use those definitions; recursive input validation rejects malformed array items,
values outside an `enum` and numbers outside their bounds before capability handlers run.
`audience_input_schema` is the one input schema `rest.cpp` and `mcp.cpp` render, and `mcp.cpp`
issues the tool-list `Mcp-Session-Id` that `http_server.cpp` uses to send
`notifications/tools/list_changed` after an upgrade (lake#441, [`docs/api/mcp.md`](../../docs/api/mcp.md)). The contract and HTTP conformance proof are in
[`docs/api/README.md`](../../docs/api/README.md).
For an authenticated agent-task credential, the registry supplies the immutable task workspace
when an allowed capability has a `workspace_id` property and the caller omitted it. Explicit IDs
still pass normal task-scope authorization. `http_server.cpp` serves the matching agent OpenAPI (rendered for `SchemaAudience::AgentTask`, as
that credential's `tools/list` is),
and routes unprefixed Zetta-shaped task calls through the same registry path. See
[`docs/api/agent-bus.md`](../../docs/api/agent-bus.md).
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
`audit` and `my_audit` return the newest events first, name each principal (`principal_name`) and
a workspace call's inner `capability`; `my_audit` leaves out successful reads unless
`include_reads` is set (lake#445). Their contract and regression coverage are in
[audit.md](../../docs/api/audit.md).

The common request context compares operator bearers in constant time before any session lookup.
User bearers and cookies use the same indexed PostgreSQL resolver; see `docs/api/auth.md`.

`web_app_capabilities.cpp` owns Ship App reads and desired-state mutations. The common registry
keeps REST and MCP customer policy, `x-lake-action`, audit, lifecycle bus events, Loki reads and
Secret-reference validation identical. Creation also applies the built-in and configured reserved
name policy before deriving the direct-zone host. Operator hash credentials are restricted in the
registry to source repository/revision reads, customer workspace reads, or one app's event
publication. See `docs/api/web-apps.md`.

`http_server.cpp` also owns the non-registry protected-app routes. `/apps/authorize` validates an
exact HTTPS app destination against the Lake browser session, and `/v1/apps/auth` exchanges its
one-use code or verifies the host-only app session. The workspace-prefixed form avoids the generic
workspace lookup and uses the store's single-query app authorization path. See
`docs/api/web-apps.md` and `docs/serving/http.md`.

ApplicationAgent app turns are assembled in `workspace_capabilities.cpp`: read tools on the first
turn, repository/App writes only when the reviewed proposal is present. `http_server.cpp` and
`web_ui.cpp` display and hash that stored proposal before starting the confirmed turn.
ExperimentsAgent turns use the workspace adapter and grant raw study operations by turn kind;
`create_study` and `promote_trial` appear only with their reviewed confirmation. The adapter sends
those actions through the canonical experiment capabilities.
Project Agent pipeline change and build turns also carry `workspace_act` and `compile_intent`;
the `workspace_act` handler refuses any action an agent turn's credential does not name and
confines the Project Agent's compile check to lake's check graph on the turn's project (lake#438).
`without_project_agent_checks` hides those check graphs from every pipeline list and from the
pick after a build.
`build_capabilities.cpp` owns customer-scoped build creation, app image builds, status,
cancellation and retained Zetta progress. Build source and event credentials are rechecked against
the current row on every call. App builds require `LAKE_APPS_RUNTIME_IMAGE`, use it through the
generated Dockerfile's `APP_RUNTIME_IMAGE` argument, and never fall back to an unqualified image.
Successful build JSON replaces the internal tagged target with the public host and immutable
digest. Pending builds keep rendering their internal target until that public image exists. The
HTTP registry adapter accepts Basic user `app` only for a current pull hash and only for that app's
exact image repository.
See `docs/api/builds.md`.

Organization self-service (`create_organization_space`, `convert_personal_space`,
`join_organization`, and the scope-less `list_memberships`/`list_invitations` modes) is registered
in `capabilities.cpp`; `http_server.cpp` adapts the Organizations page forms
(`/organizations/create`, `/organizations/convert`, `/invitations/accept`, `/organization/teams`)
to them. See `docs/api/organizations.md` and `docs/ui/settings.md`.
`workspace_capabilities.cpp` scopes launches and schedules to the calling workspace, reports a
waiting launch's reason through `launch_json`, and checks a `create_agent` or `update_agent`
definition before it reaches Zetta, which would drop unknown fields such as `content`. Zetta's
`invalid_argument` is reported as `invalid_input`. `web_ui.cpp` shows a run lake timed out as
Failed, with its reason (lake#442). See `docs/api/agent-launches.md`.
The store capabilities (`list_stores`, `create_store`, `delete_store`, `store_connection`) take
the workspace and see only its stores (lake#465). See `docs/api/storage.md`.

`workspace_records_api.hpp` opens one workspace's records through `call_workspace`, so record
calls are owner-checked, refused when the workspace is not ready and named in Server-Timing.
`workspace_record_capabilities.cpp` registers the operator-only `copy_workspace_records` (plan,
copy, verify) for each retired table still to be dropped: `opportunities`, `stream_owners`,
`drive_share_links` and the application webhook events and deliveries. `ask_messages` left the
list when migration `0050` dropped its table (lake#483). Ask Orama and the Agent Factory
(`http_server.cpp`) and `get_ask_conversation` read and write conversations in the page's or
input's workspace. `settle_discover`, `list_opportunities`, `dismiss_opportunity` and
`create_projects_from_opportunities` keep Design → Opportunities in the workspace the scan ran in
(lake#376); the Opportunities page reads them concurrently with the methodology registry. The record store capabilities stay out of
the `workspace_query`/`workspace_act` allow-lists, so only lake reaches its `lake_*` collections
(lake#370, `docs/architecture/workspace-records.md`). `stream_capabilities.cpp` reads and claims
topic creators in the topic's workspace (`lake_stream_owners`, lake#379), and
`copy_workspace_records` also copies the retired `stream_owners` rows.
(lake#370, `docs/architecture/workspace-records.md`). Drive share links (`drive_capabilities.cpp`,
lake#350) live in the workspace named by `workspace_id` on create, list (`drive_effective_access`)
and revoke, and by the bearer token on resolve, preview and download; the sharing pages carry the
workspace in their URLs and forms (`http_server.cpp`, `web_ui.cpp`). `copy_workspace_records` also
copies `drive_share_links`.
(lake#370, `docs/architecture/workspace-records.md`). `app_capabilities.cpp` records application
lifecycle events and delivery attempts in the application's workspace (lake#356, lake#357):
`register_application` requires `workspace_id`, the synchronous identity callers record an event
through `liquid::runtime::block_on`, and `list_application_webhook_events` and
`retry_application_webhook` are asynchronous and take the application's `workspace_id`.
`copy_workspace_records` also copies `auth_app_webhook_events` and `auth_app_webhook_deliveries`.
The applications page registers into a chosen workspace, and the event log page reads it.
An application's users and organizations live in the same workspace (lake#353, lake#354):
`app_capabilities.cpp` reads and writes them there, the user and organization capabilities take the
application's `workspace_id`, and `application_session_result` checks every session issued in an
application's pool against the user's record before returning it, revoking it otherwise.
`sign_up_password_session` claims a pool user's email before the account exists. These, the
identity capabilities in `capabilities.cpp` (sign-up, sign-in, TOTP, passkey, SSO, MFA, tokens) and
recovery in `offline_recovery_capabilities.cpp` are asynchronous, with no `block_on`; the hosted
pages call them through `call_async`. `copy_workspace_records` also copies `auth_app_users` and
`auth_app_organizations`.
Repositories belong to one workspace (lake#315, part 1). `create_repository`,
`import_repository` (`export_capabilities.cpp`) and `import_github_repository` require
`workspace_id`, checked by `repository_workspace` (declared in `capabilities.hpp`);
`list_repositories` takes an optional `workspace_id`; `transfer_repository` is refused. The push
path passes the repository itself to `sync_change_authorizer` and `evaluate_branch_protection`
(`forge.hpp`), so each decision and its audit record carry the workspace. `delete_workspace`
refuses a workspace that holds a live repository. `http_server.cpp`'s `/repository/create` form and
hub `/api/repos/create` (body `workspace_id` or `X-Lake-Workspace-Id`) pass the workspace, and
`web_ui.cpp` shows it on the repository list and home. See `docs/api/repositories.md`.
Forge records live in the workspace of their repository's workspace (lake#341):
`forge_records_api.hpp` declares `RepositoryForge` (create, list, get and conditional update of one
repository's records; a repository without a workspace is `conflict` at `/repository_id`), and every
forge capability, `export_repository`, `import_repository` and `import_github_repository` is
asynchronous over it. The `push` capability is asynchronous too: it reads the rules, and the
reviews and releases they ask for, once with `load_branch_protection` (`forge.hpp`) and hands that
snapshot to `sync_change_authorizer`, because the push runs synchronously inside the repository
store; a snapshot that could not be read refuses every reference update. `serve_sync_post` and the
repository pages call the registry asynchronously; the home page and change view render without
releases or reviews, saying why, when the workspace cannot be read. `copy_workspace_records` also
copies `forge_records`.
`app_capabilities.cpp` keeps an application's signing keys and webhooks in its workspace (lake#352,
lake#355): `register_application` is asynchronous and writes the first key ring there before the
application row, `rotate_application_signing_key`, `create_application_webhook` and
`list_application_webhooks` are asynchronous and take the application's `workspace_id`,
`application_jwks` is asynchronous and finds the workspace from the application id alone, and the
token issuers read the current key with `application_signing_key` on their asynchronous path, with
no `block_on`. The `POST /applications` and `POST /application/webhooks` page actions and the raw
JWKS route call the capabilities asynchronously. `copy_workspace_records` also
copies `auth_app_signing_keys` and `auth_app_webhooks`.
Secrets live in the workspace of their mount (lake#328-#332): `secret_records_api.hpp` declares
`MountSecrets` (one mount's records, opened through the mount's route: put, read, metadata, version
changes, named keys and transit), `create_secret_mount`, `list_secret_mounts` and
`rotate_customer_kek`, which rewraps every data key of the space's workspaces before forgetting the
replaced KEK. Every version and key-version number is claimed `if_absent` and every transition
written at the revision read. `secrets_capabilities.cpp` is asynchronous over it; the policy
decision reads only PostgreSQL, so a refused caller reaches no workspace. `copy_workspace_records`
copies the five secrets collections, and the `/secret-mounts`, `/secrets` and `/encryption-keys`
pages call the registry asynchronously (`/secret-mounts` lists one workspace).
Dynamic leases follow (lake#362): `MountSecrets::lease`, `renew_lease` and `revoke_lease` keep each
lease as a record of its mount's workspace (route first, then the record `if_absent`; renewal and
revocation rewrite it at the revision read), `MountSecrets::open_lease` finds the mount through the
lease's route, and `expire_secret_leases` is one pass of the expiry loop: it claims due routes from
the control store and settles each lease in its own workspace only. The server's tick runs it every
`secret_lease_expiry_interval`; `copy_workspace_records` copies `secrets_dynamic_leases`.
`app_capabilities.cpp` keeps an application's settings in its workspace (lake#351):
`register_application` writes them there with the first key ring, `get_application` and
`list_applications` are asynchronous and fill them from the workspace (`configured_application`,
one `list_records` per workspace for a listing), and `update_application_theme` takes the
application's `workspace_id` and writes conditionally. `capabilities.cpp` reads the allowed origins
from there on the asynchronous path (`begin_passkey_registration`, `begin_passkey_sign_in` and
`start_oauth` became asynchronous). `http_server.cpp` reads them for the hosted and custom-domain
identity pages, the password and OAuth forms' redirects, and the application pages, which read the
settings concurrently with their listing (`application_page_reads`). `copy_workspace_records` also
copies `auth_applications`.
Environments and the platform mirror live in the workspace of their repository (lake#358,
lake#359): every platform capability in `platform_capabilities.cpp` is asynchronous and opens the
repository's own workspace (a repository without one is `conflict` at `/repository_id`). An
environment's policy object is found in PostgreSQL from the repository and the name, so a refused
caller reaches no workspace, and `list_environments`/`list_deployments` answer an empty list
without a workspace call to a caller who may read no environment of the repository. The
`/deployments`, `/environments` and `/environment` pages call the registry asynchronously, and the
owner loop hands `PlatformService::sync` an opener over the workspace caller.
`copy_workspace_records` copies `platform_mirror` and then `platform_environments`.
`app_capabilities.cpp` issues and redeems application authorization codes in the application's
workspace (lake#363): `begin_application_authorization` writes the code there, and
`exchange_application_authorization_code` reads the routing row for the workspace before it redeems
the code. `copy_workspace_records` also copies `auth_app_authorization_codes`
(`copy_application_authorization_codes`: live codes only, each written at most once).
